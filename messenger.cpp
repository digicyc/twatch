#include "messenger.h"
#include "mesh_ble.h"
#include <LilyGoLib.h>
#include <FFat.h>
#include <Preferences.h>
#include <esp_random.h>
#include <sys/time.h>
#include <MeshCore.h>  // lets arduino-cli find the MeshCore library for the helpers/ includes
#include <companion_radio/MyMesh.h>

// The mesh node is MeshCore's companion radio firmware (MyMesh), so the MeshCore phone app can use
// the watch as its radio over Bluetooth. The watch keeps its own chat history for the Messages app,
// fed by MyMesh's listener; MyMesh keeps the identity, contacts and channels.
//
// Radio settings (LORA_FREQ, LORA_BW, ...) are global build flags in the Makefile because the
// MeshCore library headers read them too. The phone app can change them; MyMesh saves its prefs.

static const int MAX_CHATS = 32;
static const int MAX_MESSAGES = 40;  // kept per chat
static const int MAX_SEND_ATTEMPTS = 3;
static const int MAX_QUEUED = 8;
static const char *CHAT_DIR = "/mesh/chats";
// Where the watch kept its identity and contacts before it ran MyMesh; moved on first boot.
static const char *OLD_IDENTITY_FILE = "/mesh/_main.id";
static const char *OLD_CONTACTS_FILE = "/mesh/contacts";

struct Chat {
    bool used;
    bool is_channel;
    uint8_t pub_key[PUB_KEY_SIZE];  // the contact's key; unused for the channel
    char name[32];
    uint8_t unread;
    uint32_t last_activity;
    uint32_t total;  // messages ever added; message n is msgs[n % MAX_MESSAGES]
    ChatMessage msgs[MAX_MESSAGES];
};

struct ChatFileHeader {
    uint32_t version;
    uint8_t is_channel;
    uint8_t pub_key[PUB_KEY_SIZE];
    char name[32];
    uint8_t unread;
    uint32_t last_activity;
    uint32_t count;
};
static const uint32_t CHAT_FILE_VERSION = 1;

static Chat *chats;  // MAX_CHATS entries in PSRAM; chats[0] is the Public channel
static MessengerListener listener;
static Preferences prefs;
static bool radio_ok;

// The watch's own DMs are sent one at a time, retried here because MyMesh leaves retries to the app.
struct MessageRef {
    int chat;
    uint32_t seq;
};
static MessageRef queue[MAX_QUEUED];
static int queue_len;
static struct {
    bool active;
    MessageRef msg;
    uint32_t ack;
    uint8_t attempt;
    uint32_t deadline;  // millis
} pending;

// ---------- Board glue (declared in MeshCore's target.h) ----------

uint16_t WatchBoard::getBattMilliVolts()
{
    float v = instance.getBattVoltage();
    return v < 100 ? v * 1000 : v;  // the gauge reports volts or millivolts depending on the chip
}

void WatchBoard::reboot()
{
    esp_restart();
}

// MeshCore time is the system clock, which WatchApp keeps in UTC and syncs with the RTC.
class SystemRTCClock : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override { return time(NULL); }
    void setCurrentTime(uint32_t t) override  // the phone app sets the time when it connects
    {
        struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        time_t epoch = t;
        struct tm utc;
        gmtime_r(&epoch, &utc);
        instance.rtc.setDateTime(utc);
    }
};

class HardwareRNG : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { esp_fill_random(dest, sz); }
};

static SystemRTCClock rtc_clock;
static HardwareRNG rng;
static SimpleMeshTables tables;
static CustomSX1262 lora_chip(new Module(LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, SPI));
static DataStore store(FFat, rtc_clock);
static MeshBLE ble;

WatchBoard board;
CustomSX1262Wrapper radio_driver(lora_chip, board);
SensorManager sensors;
MyMesh the_mesh(radio_driver, rng, rtc_clock, tables, store);

mesh::LocalIdentity radio_new_identity()
{
    return mesh::LocalIdentity(&rng);
}

// ---------- Chat store ----------

static uint32_t chat_count(const Chat &c)
{
    return c.total < (uint32_t)MAX_MESSAGES ? c.total : MAX_MESSAGES;
}

static ChatMessage *message_at(int chat, uint32_t seq)
{
    Chat &c = chats[chat];
    if (seq >= c.total || c.total - seq > (uint32_t)MAX_MESSAGES) {
        return NULL;  // never added, or overwritten
    }
    return &c.msgs[seq % MAX_MESSAGES];
}

static void chat_path(int chat, char *path)
{
    if (chats[chat].is_channel) {
        sprintf(path, "%s/public", CHAT_DIR);
    } else {
        const uint8_t *k = chats[chat].pub_key;
        sprintf(path, "%s/%02x%02x%02x%02x%02x%02x", CHAT_DIR, k[0], k[1], k[2], k[3], k[4], k[5]);
    }
}

static void save_chat(int chat)
{
    char path[48];
    chat_path(chat, path);
    File f = FFat.open(path, "w", true);
    if (!f) {
        return;
    }
    Chat &c = chats[chat];
    ChatFileHeader h = {};
    h.version = CHAT_FILE_VERSION;
    h.is_channel = c.is_channel;
    memcpy(h.pub_key, c.pub_key, PUB_KEY_SIZE);
    memcpy(h.name, c.name, sizeof(h.name));
    h.unread = c.unread;
    h.last_activity = c.last_activity;
    h.count = chat_count(c);
    f.write((uint8_t *)&h, sizeof(h));
    for (uint32_t seq = c.total - h.count; seq < c.total; seq++) {
        f.write((uint8_t *)message_at(chat, seq), sizeof(ChatMessage));
    }
    f.close();
}

static void load_chats()
{
    File dir = FFat.open(CHAT_DIR);
    if (!dir || !dir.isDirectory()) {
        return;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        ChatFileHeader h;
        if (f.read((uint8_t *)&h, sizeof(h)) != sizeof(h) || h.version != CHAT_FILE_VERSION) {
            continue;
        }
        int slot = 0;
        if (!h.is_channel) {
            for (slot = 1; slot < MAX_CHATS && chats[slot].used; slot++) {
            }
            if (slot == MAX_CHATS) {
                break;
            }
        }
        Chat &c = chats[slot];
        c.used = true;
        c.is_channel = h.is_channel;
        memcpy(c.pub_key, h.pub_key, PUB_KEY_SIZE);
        if (!h.is_channel) {
            memcpy(c.name, h.name, sizeof(c.name));
        }
        c.unread = h.unread;
        c.last_activity = h.last_activity;
        uint32_t count = min(h.count, (uint32_t)MAX_MESSAGES);
        c.total = 0;
        for (uint32_t i = 0; i < count && f.read((uint8_t *)&c.msgs[i], sizeof(ChatMessage)) == sizeof(ChatMessage); i++) {
            c.total++;
        }
    }
}

static int find_contact_chat(const uint8_t *pub_key)
{
    for (int i = 1; i < MAX_CHATS; i++) {
        if (chats[i].used && memcmp(chats[i].pub_key, pub_key, PUB_KEY_SIZE) == 0) {
            return i;
        }
    }
    return -1;
}

// Finds or creates the chat for a contact, evicting the least recently active one if full.
static int contact_chat(const uint8_t *pub_key, const char *name)
{
    int chat = find_contact_chat(pub_key);
    if (chat < 0) {
        chat = 1;
        for (int i = 1; i < MAX_CHATS; i++) {
            if (!chats[i].used) {
                chat = i;
                break;
            }
            if (chats[i].last_activity < chats[chat].last_activity) {
                chat = i;
            }
        }
        if (chats[chat].used) {
            char path[48];
            chat_path(chat, path);
            FFat.remove(path);
        }
        memset(&chats[chat], 0, sizeof(Chat));
        chats[chat].used = true;
        memcpy(chats[chat].pub_key, pub_key, PUB_KEY_SIZE);
        chats[chat].last_activity = time(NULL);
    }
    strlcpy(chats[chat].name, name, sizeof(chats[chat].name));
    return chat;
}

static uint32_t add_message(int chat, uint32_t time, uint8_t status, const char *sender, const char *text)
{
    Chat &c = chats[chat];
    uint32_t seq = c.total++;
    ChatMessage &m = c.msgs[seq % MAX_MESSAGES];
    memset(&m, 0, sizeof(m));
    m.time = time;
    m.status = status;
    strlcpy(m.sender, sender, sizeof(m.sender));
    strlcpy(m.text, text, sizeof(m.text));
    c.last_activity = ::time(NULL);
    return seq;
}

static void notify(int chat, bool incoming)
{
    if (listener) {
        listener(chat, incoming);
    }
}


static bool has_message(int chat, uint32_t time, bool incoming, const char *text)
{
    Chat &c = chats[chat];
    for (uint32_t seq = c.total - chat_count(c); seq < c.total; seq++) {
        ChatMessage *m = message_at(chat, seq);
        if ((m->status == MSG_RECEIVED) == incoming && m->time == time && strcmp(m->text, text) == 0) {
            return true;
        }
    }
    return false;
}

static void add_and_notify(int chat, uint32_t time, uint8_t status, const char *sender, const char *text)
{
    add_message(chat, time, status, sender, text);
    bool incoming = status == MSG_RECEIVED;
    if (incoming) {
        chats[chat].unread++;
    }
    save_chat(chat);
    notify(chat, incoming);
}

// ---------- Mesh ----------

// Index of the channel shown as the watch's Public chat; the phone app can rearrange channels.
static int public_channel_idx(ChannelDetails &ch)
{
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        if (the_mesh.getChannel(i, ch) && strcmp(ch.name, "Public") == 0) {
            return i;
        }
    }
    return -1;
}

static void finish_send(uint8_t status)
{
    pending.active = false;
    ChatMessage *m = message_at(pending.msg.chat, pending.msg.seq);
    if (m) {
        m->status = status;
        save_chat(pending.msg.chat);
        notify(pending.msg.chat, false);
    }
}

static void start_send()
{
    ChatMessage *m = message_at(pending.msg.chat, pending.msg.seq);
    ContactInfo *contact = the_mesh.lookupContactByPubKey(chats[pending.msg.chat].pub_key, PUB_KEY_SIZE);
    uint32_t est_timeout;
    if (!m || !contact ||
        the_mesh.sendMessage(*contact, m->time, pending.attempt, m->text, pending.ack, est_timeout) == MSG_SEND_FAILED) {
        finish_send(MSG_FAILED);
        return;
    }
    pending.deadline = millis() + est_timeout;
    m->status = MSG_SENT;
}

static void send_timed_out()
{
    if (++pending.attempt >= MAX_SEND_ATTEMPTS) {
        finish_send(MSG_FAILED);
        return;
    }
    if (pending.attempt == MAX_SEND_ATTEMPTS - 1) {
        // Last try: forget the learned route and flood instead.
        ContactInfo *contact = the_mesh.lookupContactByPubKey(chats[pending.msg.chat].pub_key, PUB_KEY_SIZE);
        if (contact) {
            the_mesh.resetPathTo(*contact);
        }
    }
    start_send();
}

class WatchListener : public MyMesh::Listener {
public:
    void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp,
                       const char *text) override
    {
        if (txt_type != TXT_TYPE_PLAIN && txt_type != TXT_TYPE_SIGNED_PLAIN) {
            return;  // CLI traffic for the phone app
        }
        Serial.printf("DM from %s: %s\n", from.name, text);
        int chat = contact_chat(from.id.pub_key, from.name);
        // Retries of the same message arrive with the same timestamp and text.
        if (!has_message(chat, sender_timestamp, true, text)) {
            add_and_notify(chat, sender_timestamp, MSG_RECEIVED, from.name, text);
        }
    }

    void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &channel, const char *text) override
    {
        if (strcmp(channel.name, "Public") != 0) {
            return;  // other channels are only shown in the phone app
        }
        Serial.printf("Public: %s\n", text);
        // Channel messages are "<sender>: <text>".
        char sender[32] = "";
        const char *sep = strstr(text, ": ");
        if (sep && sep - text < (int)sizeof(sender)) {
            memcpy(sender, text, sep - text);
            sender[sep - text] = '\0';
            text = sep + 2;
        }
        add_and_notify(0, time(NULL), MSG_RECEIVED, sender, text);
    }

    void onQueueSizeChanged(int) override {}

    void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path) override
    {
        Serial.printf("Advert: %s (type %d)\n", contact.name, contact.type);
        if (contact.type == ADV_TYPE_CHAT) {
            int chat = contact_chat(contact.id.pub_key, contact.name);
            save_chat(chat);
            notify(chat, false);
        }
    }

    void onACKRecv(uint32_t ack) override
    {
        if (pending.active && ack == pending.ack) {
            finish_send(MSG_DELIVERED);
        }
    }
};

static WatchListener watch_listener;

// Messages sent from the phone app also go into the watch's chats. Frame layouts are MyMesh's
// CMD_SEND_TXT_MSG and CMD_SEND_CHANNEL_TXT_MSG.
static void on_app_frame(const uint8_t *f, size_t len)
{
    char text[MAX_CHAT_TEXT + 1];
    uint32_t timestamp;
    if (f[0] == 2 && len >= 14 && f[1] == TXT_TYPE_PLAIN) {
        memcpy(&timestamp, &f[3], 4);
        ContactInfo *contact = the_mesh.lookupContactByPubKey(&f[7], 6);
        if (!contact || contact->type != ADV_TYPE_CHAT) {
            return;
        }
        strlcpy(text, (const char *)&f[13], min(len - 13 + 1, sizeof(text)));
        int chat = contact_chat(contact->id.pub_key, contact->name);
        if (!has_message(chat, timestamp, false, text)) {  // the app retries with the same timestamp
            add_and_notify(chat, timestamp, MSG_SENT, "", text);
        }
    } else if (f[0] == 3 && len >= 7 && f[1] == TXT_TYPE_PLAIN) {
        ChannelDetails ch;
        if (f[2] != public_channel_idx(ch)) {
            return;
        }
        memcpy(&timestamp, &f[3], 4);
        strlcpy(text, (const char *)&f[7], min(len - 7 + 1, sizeof(text)));
        add_and_notify(0, timestamp, MSG_SENT, "", text);
    }
}

// Moves the identity, contacts and name the watch had before it ran MyMesh into MyMesh's files.
static bool migrate_old_files()
{
    if (!FFat.exists("/identity/_main.id") && FFat.exists(OLD_IDENTITY_FILE)) {
        FFat.mkdir("/identity");
        FFat.rename(OLD_IDENTITY_FILE, "/identity/_main.id");
    }
    if (!FFat.exists("/contacts3") && FFat.exists(OLD_CONTACTS_FILE)) {
        // Same records as /contacts3 minus the trailing lastmod, gps_lat and gps_lon fields. The phone
        // app only syncs contacts with a lastmod.
        File in = FFat.open(OLD_CONTACTS_FILE);
        File out = FFat.open("/contacts3", "w", true);
        uint8_t rec[140 + 12] = {};
        uint32_t lastmod = max(time(NULL), (time_t)1);
        memcpy(rec + 140, &lastmod, 4);
        while (in && out && in.read(rec, 140) == 140) {
            out.write(rec, sizeof(rec));
        }
        in.close();
        out.close();
        FFat.remove(OLD_CONTACTS_FILE);
    }
    return !FFat.exists("/prefs.json") && !FFat.exists("/new_prefs");
}

// ---------- Public API ----------

bool messenger_init()
{
    chats = (Chat *)ps_calloc(MAX_CHATS, sizeof(Chat));
    chats[0].used = true;
    chats[0].is_channel = true;
    strcpy(chats[0].name, "Public");

    FFat.mkdir("/mesh");
    FFat.mkdir(CHAT_DIR);
    load_chats();
    prefs.begin("mesh");

    instance.setRFSwitch(false);  // built-in antenna
    if (!lora_chip.std_init()) {
        return false;
    }

    bool new_prefs = migrate_old_files();
    store.begin();
    the_mesh.begin();
    NodePrefs *node = the_mesh.getNodePrefs();
    if (new_prefs) {
        // Keep the name the watch already had (or its old default) rather than MyMesh's hex default.
        String name = prefs.getString("name", "");
        if (name.length()) {
            strlcpy(node->node_name, name.c_str(), sizeof(node->node_name));
        } else {
            snprintf(node->node_name, sizeof(node->node_name), "TWatch-%02X%02X", the_mesh.self_id.pub_key[0],
                     the_mesh.self_id.pub_key[1]);
        }
    }
    if (new_prefs || node->ble_pin == 0) {
        if (node->ble_pin == 0) {  // a fixed random PIN for pairing the phone, shown in the Bluetooth app
            node->ble_pin = 100000 + esp_random() % 900000;
            the_mesh.setBLEPin(node->ble_pin);
        }
        the_mesh.savePrefs();
    }

    char ble_name[48];
    snprintf(ble_name, sizeof(ble_name), BLE_NAME_PREFIX "%s", node->node_name);
    ble.begin(ble_name, the_mesh.getBLEPin(), on_app_frame, prefs.getBool("bt", true));
    the_mesh.startInterface(ble);
    the_mesh.setListener(&watch_listener);
    radio_ok = true;

    // Let nodes in direct range know we're here; a flood advert is sent on request.
    the_mesh.advert();
    Serial.printf("MeshCore node %s on %.3f MHz SF%d BW%.1f, Bluetooth %s PIN %06u\n", node->node_name,
                  (double)node->freq, node->sf, (double)node->bw, ble.isEnabled() ? "on" : "off", the_mesh.getBLEPin());
    return true;
}

void messenger_loop()
{
    if (!radio_ok) {
        return;
    }
    the_mesh.loop();
    if (pending.active && (long)(millis() - pending.deadline) >= 0) {
        send_timed_out();
    }
    if (!pending.active && queue_len) {
        pending.msg = queue[0];
        memmove(queue, queue + 1, --queue_len * sizeof(queue[0]));
        pending.attempt = 0;
        pending.active = true;
        start_send();
    }
}

int messenger_chats(int *ids, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_CHATS && n < max; i++) {
        if (chats[i].used) {
            ids[n++] = i;
        }
    }
    // Public stays first; the rest by most recent activity.
    for (int i = 2; i < n; i++) {
        for (int j = i; j > 1 && chats[ids[j - 1]].last_activity < chats[ids[j]].last_activity; j--) {
            int tmp = ids[j];
            ids[j] = ids[j - 1];
            ids[j - 1] = tmp;
        }
    }
    return n;
}

bool messenger_chat_summary(int chat, ChatSummary &out)
{
    if (chat < 0 || chat >= MAX_CHATS || !chats[chat].used) {
        return false;
    }
    Chat &c = chats[chat];
    out.name = c.name;
    out.is_channel = c.is_channel;
    out.unread = c.unread;
    out.last_activity = c.last_activity;
    out.last_message = c.total ? message_at(chat, c.total - 1) : NULL;
    return true;
}

int messenger_message_count(int chat)
{
    return chat_count(chats[chat]);
}

const ChatMessage *messenger_message(int chat, int index)
{
    Chat &c = chats[chat];
    return message_at(chat, c.total - chat_count(c) + index);
}

bool messenger_send(int chat, const char *text)
{
    if (!radio_ok) {
        return false;
    }
    Chat &c = chats[chat];
    uint32_t now = time(NULL);
    if (c.is_channel) {
        ChannelDetails ch;
        bool ok = public_channel_idx(ch) >= 0 &&
                  the_mesh.sendGroupMessage(now, ch.channel, the_mesh.getNodeName(), text, strlen(text));
        add_message(chat, now, ok ? MSG_SENT : MSG_FAILED, "", text);
    } else {
        if (queue_len == MAX_QUEUED) {
            return false;
        }
        queue[queue_len++] = {chat, add_message(chat, now, MSG_QUEUED, "", text)};
    }
    save_chat(chat);
    notify(chat, false);
    return true;
}

void messenger_mark_read(int chat)
{
    if (chats[chat].unread) {
        chats[chat].unread = 0;
        save_chat(chat);
    }
}

int messenger_unread_total()
{
    int total = 0;
    for (int i = 0; i < MAX_CHATS; i++) {
        total += chats[i].used ? chats[i].unread : 0;
    }
    return total;
}

void messenger_send_advert()
{
    mesh::Packet *advert = the_mesh.createSelfAdvert(the_mesh.getNodeName());
    if (advert) {
        the_mesh.sendFlood(advert);
    }
}

const char *messenger_node_name()
{
    return the_mesh.getNodeName();
}

void messenger_set_node_name(const char *name)
{
    strlcpy(the_mesh.getNodePrefs()->node_name, name, sizeof(the_mesh.getNodePrefs()->node_name));
    the_mesh.savePrefs();
    char ble_name[48];
    snprintf(ble_name, sizeof(ble_name), BLE_NAME_PREFIX "%s", name);
    ble.setName(ble_name);
}

void messenger_set_listener(MessengerListener l)
{
    listener = l;
}

bool messenger_bt_enabled()
{
    return ble.isEnabled();
}

void messenger_bt_set_enabled(bool on)
{
    if (!radio_ok) {
        return;
    }
    on ? ble.enable() : ble.disable();
    prefs.putBool("bt", on);
}

bool messenger_bt_connected()
{
    return ble.isConnected();
}

const char *messenger_bt_name()
{
    return ble.name();
}

uint32_t messenger_bt_pin()
{
    return the_mesh.getBLEPin();
}
