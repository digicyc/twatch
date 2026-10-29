// MeshCore-compatible LoRa messaging: identity, contacts, the Public channel, direct messages,
// and per-chat message history stored on the FFat partition.
#pragma once
#include <stdint.h>

static const int MAX_CHAT_TEXT = 160;  // MeshCore MAX_TEXT_LEN
static const int MAX_NODE_NAME = 31;   // longest name that fits in a MeshCore advert

enum MessageStatus : uint8_t {
    MSG_RECEIVED,
    MSG_QUEUED,     // outgoing DM waiting for the radio
    MSG_SENT,       // channel message transmitted, or DM sent and awaiting ACK
    MSG_DELIVERED,  // DM acknowledged by the recipient
    MSG_FAILED,     // DM never acknowledged
};

struct ChatMessage {
    uint32_t time;  // epoch seconds (sender's clock for received messages)
    uint8_t status;
    char sender[32];  // empty for our own messages
    char text[MAX_CHAT_TEXT + 1];
};

struct ChatSummary {
    const char *name;
    bool is_channel;
    uint8_t unread;
    uint32_t last_activity;  // epoch seconds, our clock
    const ChatMessage *last_message;  // NULL if none
};

// Called when a chat changes. `incoming` is true for a newly received message.
typedef void (*MessengerListener)(int chat, bool incoming);

bool messenger_init();  // false if the radio failed to start
void messenger_loop();

// Chat ids are stable while running. Fills `ids` (Public first, then most recent first).
int messenger_chats(int *ids, int max);
bool messenger_chat_summary(int chat, ChatSummary &out);
int messenger_message_count(int chat);
const ChatMessage *messenger_message(int chat, int index);  // 0 = oldest
bool messenger_send(int chat, const char *text);
void messenger_mark_read(int chat);
int messenger_unread_total();

void messenger_send_advert();  // flood advert so others can add us
const char *messenger_node_name();
void messenger_set_node_name(const char *name);
uint32_t messenger_packets_heard();
float messenger_last_rssi();
void messenger_set_listener(MessengerListener listener);
