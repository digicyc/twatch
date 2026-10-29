#include "messenger.h"
#include <LilyGoLib.h>
#include <FFat.h>
#include <Preferences.h>
#include <esp_random.h>
#include <sys/time.h>
#include <MeshCore.h>  // lets arduino-cli find the MeshCore library for the helpers/ includes
#include <helpers/BaseChatMesh.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

// Radio settings (LORA_FREQ, LORA_BW, ...) are global build flags in the Makefile because the
// MeshCore library headers read them too.

#define PUBLIC_GROUP_PSK "izOH6cXN6mrJ5e26oRXNcg=="  // MeshCore's well-known Public channel key

static const int MAX_CHATS = 32;
static const int MAX_MESSAGES = 40;  // kept per chat
static const int MAX_SEND_ATTEMPTS = 3;
static const int MAX_QUEUED = 8;
static const uint32_t CONTACTS_SAVE_DELAY_MS = 5000;
static const char *CHAT_DIR = "/mesh/chats";
static const char *CONTACTS_FILE = "/mesh/contacts";

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
static uint32_t packets_heard;
static float last_rssi;
static Preferences prefs;
static char node_name[MAX_NODE_NAME + 1];
static uint32_t contacts_dirty_at;

// Outgoing DMs are sent one at a time because BaseChatMesh tracks a single ACK timeout.
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
} pending;

// ---------- Board glue ----------

class WatchBoard : public mesh::MainBoard {
public:
    uint16_t getBattMilliVolts() override { return 0; }
    const char *getManufacturerName() const override { return "LilyGo T-Watch Ultra"; }
    void reboot() override { esp_restart(); }
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

// MeshCore time is the system clock, which WatchApp keeps in UTC and syncs with the RTC.
class SystemRTCClock : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override { return time(NULL); }
    void setCurrentTime(uint32_t t) override
    {
        struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};
        settimeofday(&tv, NULL);
    }
};

class HardwareRNG : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { esp_fill_random(dest, sz); }
};

static WatchBoard board;
static SystemRTCClock rtc_clock;
static HardwareRNG rng;
static ArduinoMillis millis_clock;
static SimpleMeshTables tables;
static StaticPoolPacketManager packet_mgr(16);
static CustomSX1262 lora_chip(new Module(LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, SPI));
static CustomSX1262Wrapper radio_driver(lora_chip, board);

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

// ---------- Mesh ----------

class WatchMesh : public BaseChatMesh {
public:
    ChannelDetails *public_channel = NULL;

    WatchMesh() : BaseChatMesh(radio_driver, millis_clock, rng, rtc_clock, packet_mgr, tables) {}

    void loadIdentity()
    {
        IdentityStore store(FFat, "/mesh");
        if (!store.load("_main", self_id)) {
            self_id = mesh::LocalIdentity(&rng);
            // 0x00 and 0xFF hashes are reserved
            for (int i = 0; i < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF); i++) {
                self_id = mesh::LocalIdentity(&rng);
            }
            store.save("_main", self_id);
        }
    }

    // Same record layout as MeshCore's simple_secure_chat example.
    void loadContacts()
    {
        File f = FFat.open(CONTACTS_FILE);
        if (!f) {
            return;
        }
        for (;;) {
            ContactInfo c = {};
            uint8_t pub_key[32], unused;
            uint32_t reserved;
            bool ok = f.read(pub_key, 32) == 32 && f.read((uint8_t *)c.name, 32) == 32 &&
                      f.read(&c.type, 1) == 1 && f.read(&c.flags, 1) == 1 && f.read(&unused, 1) == 1 &&
                      f.read((uint8_t *)&reserved, 4) == 4 && f.read(&c.out_path_len, 1) == 1 &&
                      f.read((uint8_t *)&c.last_advert_timestamp, 4) == 4 && f.read(c.out_path, 64) == 64;
            if (!ok) {
                break;
            }
            c.id = mesh::Identity(pub_key);
            if (!addContact(c)) {
                break;
            }
        }
        f.close();
    }

    void saveContacts()
    {
        File f = FFat.open(CONTACTS_FILE, "w", true);
        if (!f) {
            return;
        }
        ContactsIterator iter = startContactsIterator();
        ContactInfo c;
        uint8_t unused = 0;
        uint32_t reserved = 0;
        while (iter.hasNext(this, c)) {
            f.write(c.id.pub_key, 32);
            f.write((uint8_t *)c.name, 32);
            f.write(&c.type, 1);
            f.write(&c.flags, 1);
            f.write(&unused, 1);
            f.write((uint8_t *)&reserved, 4);
            f.write(&c.out_path_len, 1);
            f.write((uint8_t *)&c.last_advert_timestamp, 4);
            f.write(c.out_path, 64);
        }
        f.close();
    }

    void startSend()
    {
        ChatMessage *m = message_at(pending.msg.chat, pending.msg.seq);
        ContactInfo *contact = lookupContactByPubKey(chats[pending.msg.chat].pub_key, PUB_KEY_SIZE);
        uint32_t est_timeout;
        if (!m || !contact ||
            sendMessage(*contact, m->time, pending.attempt, m->text, pending.ack, est_timeout) == MSG_SEND_FAILED) {
            finishSend(MSG_FAILED);
            return;
        }
        m->status = MSG_SENT;
    }

    void finishSend(uint8_t status)
    {
        pending.active = false;
        ChatMessage *m = message_at(pending.msg.chat, pending.msg.seq);
        if (m) {
            m->status = status;
            save_chat(pending.msg.chat);
            notify(pending.msg.chat, false);
        }
    }

protected:
    float getAirtimeBudgetFactor() const override { return 1.0f; }
    int calcRxDelay(float score, uint32_t air_time) const override { return 0; }
    bool shouldOverwriteWhenFull() const override { return true; }

    void logRx(mesh::Packet *packet, int len, float score) override
    {
        packets_heard++;
        last_rssi = radio_driver.getLastRSSI();
        Serial.printf("TEMP-DEBUG rx type=%d len=%d rssi=%.0f snr=%.1f\n", packet->getPayloadType(), len, last_rssi, radio_driver.getLastSNR());  // TEMP-DEBUG
    }

    void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path) override
    {
        Serial.printf("Advert: %s (type %d)\n", contact.name, contact.type);
        if (contact.type == ADV_TYPE_CHAT) {
            int chat = contact_chat(contact.id.pub_key, contact.name);
            save_chat(chat);
            notify(chat, false);
        }
        contacts_dirty_at = millis();
    }

    void onContactPathUpdated(const ContactInfo &contact) override { contacts_dirty_at = millis(); }

    ContactInfo *processAck(const uint8_t *data) override
    {
        if (pending.active && memcmp(data, &pending.ack, 4) == 0) {
            finishSend(MSG_DELIVERED);
        }
        return NULL;
    }

    void onSendTimeout() override
    {
        if (!pending.active) {
            return;
        }
        if (++pending.attempt >= MAX_SEND_ATTEMPTS) {
            finishSend(MSG_FAILED);
            return;
        }
        if (pending.attempt == MAX_SEND_ATTEMPTS - 1) {
            // Last try: forget the learned route and flood instead.
            ContactInfo *contact = lookupContactByPubKey(chats[pending.msg.chat].pub_key, PUB_KEY_SIZE);
            if (contact) {
                resetPathTo(*contact);
            }
        }
        startSend();
    }

    void onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp, const char *text) override
    {
        Serial.printf("DM from %s: %s\n", from.name, text);
        int chat = contact_chat(from.id.pub_key, from.name);
        // Retries of the same message arrive with the same timestamp and text.
        Chat &c = chats[chat];
        for (uint32_t seq = c.total - chat_count(c); seq < c.total; seq++) {
            ChatMessage *m = message_at(chat, seq);
            if (m->status == MSG_RECEIVED && m->time == sender_timestamp && strcmp(m->text, text) == 0) {
                return;
            }
        }
        add_message(chat, sender_timestamp, MSG_RECEIVED, from.name, text);
        c.unread++;
        save_chat(chat);
        notify(chat, true);
    }

    void onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp, const char *text) override
    {
        Serial.printf("Public: %s\n", text);
        // Channel messages are "<sender>: <text>".
        char sender[32] = "";
        const char *sep = strstr(text, ": ");
        if (sep && sep - text < (int)sizeof(sender)) {
            memcpy(sender, text, sep - text);
            sender[sep - text] = '\0';
            text = sep + 2;
        }
        add_message(0, timestamp, MSG_RECEIVED, sender, text);
        chats[0].unread++;
        save_chat(0);
        notify(0, true);
    }

    void onCommandDataRecv(const ContactInfo &, mesh::Packet *, uint32_t, const char *) override {}
    void onCLICommandRecv(const ContactInfo &, mesh::Packet *, uint32_t, const char *, char *) override {}
    void onSignedMessageRecv(const ContactInfo &, mesh::Packet *, uint32_t, const uint8_t *, const char *) override {}
    uint8_t onContactRequest(const ContactInfo &, uint32_t, const uint8_t *, uint8_t, uint8_t *) override { return 0; }
    void onContactResponse(const ContactInfo &, const uint8_t *, uint8_t) override {}

    // Same timeouts as MeshCore's companion firmware.
    uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override
    {
        return 500 + 16.0f * pkt_airtime_millis;
    }
    uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override
    {
        return 500 + (pkt_airtime_millis * 6.0f + 250) * ((path_len & 63) + 1);
    }
};

static WatchMesh the_mesh;

// ---------- Public API ----------

bool messenger_init()
{
    chats = (Chat *)ps_calloc(MAX_CHATS, sizeof(Chat));
    chats[0].used = true;
    chats[0].is_channel = true;
    strcpy(chats[0].name, "Public");

    FFat.mkdir("/mesh");
    FFat.mkdir(CHAT_DIR);

    instance.setRFSwitch(false);  // built-in antenna
    if (!lora_chip.std_init()) {
        return false;
    }

    the_mesh.begin();
    the_mesh.loadIdentity();
    the_mesh.loadContacts();
    the_mesh.public_channel = the_mesh.addChannel("Public", PUBLIC_GROUP_PSK);
    load_chats();

    prefs.begin("mesh");
    String name = prefs.getString("name", "");
    if (name.length()) {
        strlcpy(node_name, name.c_str(), sizeof(node_name));
    } else {
        snprintf(node_name, sizeof(node_name), "TWatch-%02X%02X", the_mesh.self_id.pub_key[0], the_mesh.self_id.pub_key[1]);
    }

    radio_driver.setParams(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR);
    radio_driver.setTxPower(LORA_TX_POWER);

    // Let nodes in direct range know we're here; a flood advert is sent on request.
    mesh::Packet *advert = the_mesh.createSelfAdvert(node_name);
    if (advert) {
        the_mesh.sendZeroHop(advert, 2000);
    }
    Serial.printf("MeshCore node %s on %.3f MHz SF%d BW%.1f\n", node_name, (double)LORA_FREQ, LORA_SF, (double)LORA_BW);
    return true;
}

void messenger_loop()
{
    the_mesh.loop();
    if (!pending.active && queue_len) {
        pending.msg = queue[0];
        memmove(queue, queue + 1, --queue_len * sizeof(queue[0]));
        pending.attempt = 0;
        pending.active = true;
        the_mesh.startSend();
    }
    if (contacts_dirty_at && millis() - contacts_dirty_at > CONTACTS_SAVE_DELAY_MS) {
        contacts_dirty_at = 0;
        the_mesh.saveContacts();
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
    if (!the_mesh.public_channel) {
        return false;  // the radio didn't start
    }
    Chat &c = chats[chat];
    uint32_t now = time(NULL);
    if (c.is_channel) {
        bool ok = the_mesh.sendGroupMessage(now, the_mesh.public_channel->channel, node_name, text, strlen(text));
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
    mesh::Packet *advert = the_mesh.createSelfAdvert(node_name);
    if (advert) {
        the_mesh.sendFlood(advert);
    }
}

const char *messenger_node_name()
{
    return node_name;
}

void messenger_set_node_name(const char *name)
{
    strlcpy(node_name, name, sizeof(node_name));
    prefs.putString("name", node_name);
}

uint32_t messenger_packets_heard()
{
    return packets_heard;
}

float messenger_last_rssi()
{
    return last_rssi;
}

void messenger_set_listener(MessengerListener l)
{
    listener = l;
}
