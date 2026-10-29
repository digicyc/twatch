#include "messages.h"
#include "messenger.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const int MAX_LISTED_CHATS = 32;  // messenger.cpp keeps at most this many
static const uint32_t TOAST_MS = 2500;
static const lv_color_t ORANGE = LV_COLOR_MAKE(0xFF, 0x98, 0x00);  // lv_palette_main(LV_PALETTE_ORANGE)

static const char *CANNED_REPLIES[] = {
    "OK", "Yes", "No", "On my way", "Be there soon", "Running late",
    "Can't talk now", "Call me", "Where are you?", "Thanks!",
};

static lv_obj_t *home_screen;
static lv_obj_t *list_screen, *list_cont;
static lv_obj_t *chat_screen, *chat_title, *chat_cont;
static lv_obj_t *reply_screen, *reply_cont;
static lv_obj_t *compose_screen, *compose_title, *compose_ta, *compose_kb;
static bool editing_name;  // compose_screen is editing the watch name rather than a message
static lv_obj_t *toast;
static lv_timer_t *toast_timer;
static int open_chat = -1;  // chat shown on chat_screen

static void open_chat_view(int chat);
static void compose_open_cb(lv_event_t *);
static void name_open_cb(lv_event_t *);

// "3:42 PM" for today, "Oct 28" for earlier days, "" if the clock isn't set.
static const char *format_time(uint32_t t)
{
    static char buf[16];
    time_t when = t, now = time(NULL);
    struct tm w, n;
    localtime_r(&when, &w);
    localtime_r(&now, &n);
    if (w.tm_year + 1900 < 2025) {
        return "";
    }
    if (w.tm_year == n.tm_year && w.tm_yday == n.tm_yday) {
        snprintf(buf, sizeof(buf), "%d:%02d %s", w.tm_hour % 12 ? w.tm_hour % 12 : 12, w.tm_min, w.tm_hour < 12 ? "AM" : "PM");
    } else {
        strftime(buf, sizeof(buf), "%b %d", &w);
    }
    return buf;
}

static void show_toast(const char *text)
{
    lv_label_set_text(toast, text);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

// ---------- Chat list ----------

static void chat_row_clicked_cb(lv_event_t *e)
{
    open_chat_view((int)(intptr_t)lv_event_get_user_data(e));
}

static void rebuild_list()
{
    lv_obj_clean(list_cont);
    int ids[MAX_LISTED_CHATS];
    int n = messenger_chats(ids, MAX_LISTED_CHATS);
    for (int i = 0; i < n; i++) {
        ChatSummary s;
        if (!messenger_chat_summary(ids[i], s)) {
            continue;
        }
        lv_obj_t *row = lv_obj_create(list_cont);
        lv_obj_set_size(row, LV_PCT(100), 100);
        lv_obj_set_style_radius(row, 24, 0);
        lv_obj_set_style_bg_color(row, CARD, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 22, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, chat_row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ids[i]);

        lv_obj_t *name = make_label(row, s.name, &lv_font_montserrat_28, s.is_channel ? ORANGE : lv_color_white());
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_size(name, 210, lv_font_get_line_height(&lv_font_montserrat_28));  // one line, "..." if longer
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -18);

        lv_obj_t *when = make_label(row, s.last_message ? format_time(s.last_activity) : "", &lv_font_montserrat_18, GRAY);
        lv_obj_align(when, LV_ALIGN_RIGHT_MID, 0, -18);

        char preview[MAX_CHAT_TEXT + 40];
        const ChatMessage *m = s.last_message;
        if (!m) {
            strcpy(preview, "No messages yet");
        } else if (m->status != MSG_RECEIVED) {
            snprintf(preview, sizeof(preview), "You: %s", m->text);
        } else if (s.is_channel && m->sender[0]) {
            snprintf(preview, sizeof(preview), "%s: %s", m->sender, m->text);
        } else {
            strlcpy(preview, m->text, sizeof(preview));
        }
        lv_obj_t *text = make_label(row, preview, &lv_font_montserrat_20, s.unread ? lv_color_white() : GRAY);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_obj_set_size(text, s.unread ? 250 : 300, lv_font_get_line_height(&lv_font_montserrat_20));
        lv_obj_align(text, LV_ALIGN_LEFT_MID, 0, 20);

        if (s.unread) {
            lv_obj_t *badge = lv_obj_create(row);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, LV_SIZE_CONTENT, 34);
            lv_obj_set_style_min_width(badge, 34, 0);
            lv_obj_set_style_pad_hor(badge, 10, 0);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(badge, ORANGE, 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *count = make_label(badge, "", &lv_font_montserrat_20, lv_color_black());
            lv_label_set_text_fmt(count, "%d", s.unread);
            lv_obj_center(count);
            lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 20);
        }
    }
    if (n == 1) {
        lv_obj_t *hint = make_label(list_cont, "Contacts appear here when nearby MeshCore nodes advertise. "
                                    "Tap " LV_SYMBOL_WIFI " so they can find this watch.",
                                    &lv_font_montserrat_20, GRAY);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(hint, 20, 0);
    }

    // This watch's name as other nodes see it; tap to change it.
    lv_obj_t *me = lv_obj_create(list_cont);
    lv_obj_set_size(me, LV_PCT(100), 100);
    lv_obj_set_style_radius(me, 24, 0);
    lv_obj_set_style_bg_opa(me, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(me, 2, 0);
    lv_obj_set_style_border_color(me, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_pad_hor(me, 22, 0);
    lv_obj_set_style_margin_top(me, 8, 0);
    lv_obj_remove_flag(me, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(me, name_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *caption = make_label(me, "This watch", &lv_font_montserrat_20, GRAY);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 0, -18);
    lv_obj_t *name = make_label(me, messenger_node_name(), &lv_font_montserrat_28, lv_color_white());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, 260, lv_font_get_line_height(&lv_font_montserrat_28));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 16);
    lv_obj_t *edit = make_label(me, LV_SYMBOL_EDIT, &lv_font_montserrat_28, ORANGE);
    lv_obj_align(edit, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void list_back_cb(lv_event_t *)
{
    lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void advertise()
{
    messenger_send_advert();
    static char text[64];
    snprintf(text, sizeof(text), "Advertised as %s", messenger_node_name());
    show_toast(text);
}

static void advert_cb(lv_event_t *)
{
    advertise();
}

static void build_list()
{
    list_screen = make_screen();
    header_button(list_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, list_back_cb);
    header_button(list_screen, LV_SYMBOL_WIFI, LV_ALIGN_TOP_RIGHT, advert_cb);
    lv_obj_t *title = make_label(list_screen, "Messages", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);
    list_cont = make_scroll_column(list_screen);
}

// ---------- Chat view ----------

static const char *status_text(uint8_t status)
{
    switch (status) {
    case MSG_QUEUED: return "Sending...";
    case MSG_SENT: return "Sent";
    case MSG_DELIVERED: return "Delivered " LV_SYMBOL_OK;
    case MSG_FAILED: return "Failed";
    }
    return "";
}

static void add_bubble(const ChatMessage &m, bool is_channel)
{
    bool mine = m.status != MSG_RECEIVED;

    lv_obj_t *item = lv_obj_create(chat_cont);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_flex_align_t side = mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START;
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, side, side);
    lv_obj_set_style_pad_row(item, 4, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);

    if (!mine && is_channel && m.sender[0]) {
        lv_obj_t *sender = make_label(item, m.sender, &lv_font_montserrat_18, ORANGE);
        lv_obj_set_style_margin_left(sender, 12, 0);
    }

    lv_obj_t *bubble = lv_obj_create(item);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(bubble, 18, 0);
    lv_obj_set_style_pad_ver(bubble, 12, 0);
    lv_obj_set_style_radius(bubble, 22, 0);
    lv_obj_set_style_bg_color(bubble, mine ? ORANGE : CARD, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *text = make_label(bubble, m.text, &lv_font_montserrat_24, mine ? lv_color_black() : lv_color_white());
    lv_obj_set_style_max_width(text, 270, 0);

    char meta[48];
    const char *when = format_time(m.time);
    if (mine) {
        snprintf(meta, sizeof(meta), "%s%s%s", when, when[0] ? "  " : "", status_text(m.status));
    } else {
        strlcpy(meta, when, sizeof(meta));
    }
    if (meta[0]) {
        lv_obj_t *label = make_label(item, meta, &lv_font_montserrat_18,
                                     m.status == MSG_FAILED ? lv_palette_main(LV_PALETTE_RED) : GRAY);
        lv_obj_set_style_margin_hor(label, 12, 0);
    }
}

static void rebuild_chat()
{
    lv_obj_clean(chat_cont);
    ChatSummary s;
    if (!messenger_chat_summary(open_chat, s)) {
        return;
    }
    lv_label_set_text(chat_title, s.name);
    int count = messenger_message_count(open_chat);
    if (!count) {
        lv_obj_t *empty = make_label(chat_cont, s.is_channel ? "No messages yet.\nEveryone nearby can read\nthis channel."
                                                             : "No messages yet",
                                     &lv_font_montserrat_24, GRAY);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(empty, 80, 0);
    }
    for (int i = 0; i < count; i++) {
        const ChatMessage *m = messenger_message(open_chat, i);
        if (m) {
            add_bubble(*m, s.is_channel);
        }
    }
    lv_obj_update_layout(chat_cont);
    lv_obj_scroll_to_y(chat_cont, LV_COORD_MAX, LV_ANIM_OFF);
}

static void open_chat_view(int chat)
{
    open_chat = chat;
    messenger_mark_read(chat);
    rebuild_chat();
    rebuild_list();
    lv_screen_load_anim(chat_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void chat_back_cb(lv_event_t *)
{
    lv_screen_load_anim(list_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void reply_open_cb(lv_event_t *)
{
    lv_obj_scroll_to_y(reply_cont, 0, LV_ANIM_OFF);
    lv_screen_load_anim(reply_screen, LV_SCREEN_LOAD_ANIM_MOVE_TOP, 200, 0, false);
}

static void build_chat()
{
    chat_screen = make_screen();
    chat_cont = make_scroll_column(chat_screen);
    lv_obj_set_flex_align(chat_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_bottom(chat_cont, 120, 0);  // the last message can scroll clear of the reply button

    header_button(chat_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, chat_back_cb);
    chat_title = make_label(chat_screen, "", &lv_font_montserrat_32, lv_color_white());
    lv_label_set_long_mode(chat_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(chat_title, 250, lv_font_get_line_height(&lv_font_montserrat_32));
    lv_obj_set_style_text_align(chat_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(chat_title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *reply = lv_button_create(chat_screen);
    lv_obj_set_size(reply, 88, 88);
    lv_obj_set_style_radius(reply, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(reply, ORANGE, 0);
    lv_obj_set_style_shadow_width(reply, 0, 0);
    lv_obj_center(make_label(reply, LV_SYMBOL_EDIT, &lv_font_montserrat_36, lv_color_black()));
    lv_obj_align(reply, LV_ALIGN_BOTTOM_RIGHT, -28, -28);
    lv_obj_add_event_cb(reply, reply_open_cb, LV_EVENT_CLICKED, NULL);
}

// ---------- Canned replies ----------

static void reply_close()
{
    lv_screen_load_anim(chat_screen, LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
}

static void reply_chosen_cb(lv_event_t *e)
{
    const char *text = (const char *)lv_event_get_user_data(e);
    if (!messenger_send(open_chat, text)) {
        show_toast("Couldn't send");
    }
    reply_close();
}

static void reply_cancel_cb(lv_event_t *)
{
    reply_close();
}

static void build_reply()
{
    reply_screen = make_screen();
    header_button(reply_screen, LV_SYMBOL_CLOSE, LV_ALIGN_TOP_LEFT, reply_cancel_cb);
    lv_obj_t *title = make_label(reply_screen, "Reply", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    reply_cont = make_scroll_column(reply_screen);
    lv_obj_t *type = lv_button_create(reply_cont);
    lv_obj_set_size(type, LV_PCT(100), 72);
    lv_obj_set_style_radius(type, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(type, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(type, 2, 0);
    lv_obj_set_style_border_color(type, ORANGE, 0);
    lv_obj_set_style_shadow_width(type, 0, 0);
    lv_obj_center(make_label(type, LV_SYMBOL_KEYBOARD "  Type a message", &lv_font_montserrat_28, ORANGE));
    lv_obj_add_event_cb(type, compose_open_cb, LV_EVENT_CLICKED, NULL);

    for (const char *text : CANNED_REPLIES) {
        lv_obj_t *btn = lv_button_create(reply_cont);
        lv_obj_set_size(btn, LV_PCT(100), 72);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, CARD, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_center(make_label(btn, text, &lv_font_montserrat_28, lv_color_white()));
        lv_obj_add_event_cb(btn, reply_chosen_cb, LV_EVENT_CLICKED, (void *)text);
    }
}

// ---------- Compose ----------

// A compact QWERTY sized for the 410 px screen. Widths are in units of 20 per row; letter keys are 2.
// " " entries with KB_GAP are hidden spacers that indent the middle row by half a key.
static const int KB_HEIGHT = 236;
#define KB_CTRL(x) ((lv_buttonmatrix_ctrl_t)(x))  // lv_buttonmatrix_ctrl_t is an enum; C++ won't OR it implicitly
static const lv_buttonmatrix_ctrl_t K = KB_CTRL(2);
static const lv_buttonmatrix_ctrl_t KB_GAP = KB_CTRL(LV_BUTTONMATRIX_CTRL_HIDDEN | 1);
static const lv_buttonmatrix_ctrl_t KB_FN = KB_CTRL(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3);  // mode keys and send
static const lv_buttonmatrix_ctrl_t KB_BKSP = KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 3);
static const lv_buttonmatrix_ctrl_t KB_SPACE = KB_CTRL(10);

static const char *const KB_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", ",", " ", ".", LV_SYMBOL_OK, ""};
static const char *const KB_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", ",", " ", ".", LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t KB_TEXT_CTRL[] = {
    K, K, K, K, K, K, K, K, K, K,
    KB_GAP, K, K, K, K, K, K, K, K, K, KB_GAP,
    KB_FN, K, K, K, K, K, K, K, KB_BKSP,
    KB_FN, K, KB_SPACE, K, KB_FN};

static const char *const KB_SPECIAL[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "@", "#", "$", "&", "-", "+", "(", ")", "/", "?", "\n",
    "'", "\"", ":", ";", "!", "*", "=", "%", LV_SYMBOL_BACKSPACE, "\n",
    "abc", ",", " ", ".", LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t KB_SPECIAL_CTRL[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 4),
    KB_FN, K, KB_SPACE, K, KB_FN};

static void compose_close()
{
    lv_screen_load_anim(editing_name ? list_screen : chat_screen, LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, 200, 0, false);
}

static void compose_cancel_cb(lv_event_t *)
{
    compose_close();
}

static void compose_send_cb(lv_event_t *)
{
    const char *text = lv_textarea_get_text(compose_ta);
    while (*text == ' ') {
        text++;
    }
    if (!*text) {
        return;
    }
    if (editing_name) {
        char name[MAX_NODE_NAME + 1];
        strlcpy(name, text, sizeof(name));
        for (size_t len = strlen(name); len && name[len - 1] == ' '; len--) {
            name[len - 1] = '\0';
        }
        messenger_set_node_name(name);
        rebuild_list();
        advertise();  // so other nodes pick up the new name
    } else if (!messenger_send(open_chat, text)) {
        show_toast("Couldn't send");
    }
    compose_close();
}

// Replaces LVGL's key handler to add a shift key (LV_SYMBOL_UP) that applies to one letter, like a
// phone keyboard. Everything else is LVGL's default handling.
static void keyboard_key_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target_obj(e);
    const char *key = lv_buttonmatrix_get_button_text(kb, lv_buttonmatrix_get_selected_button(kb));
    bool upper = lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_TEXT_UPPER;
    if (key && strcmp(key, LV_SYMBOL_UP) == 0) {
        lv_keyboard_set_mode(kb, upper ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
        return;
    }
    lv_keyboard_def_event_cb(e);
    if (upper && key && key[0] >= 'A' && key[0] <= 'Z' && !key[1]) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
}

// Shows the compose screen with the keyboard sliding up. Shift starts on for empty text.
static void compose_open(const char *title, const char *placeholder, const char *text, uint32_t max_length)
{
    lv_label_set_text(compose_title, title);
    lv_textarea_set_placeholder_text(compose_ta, placeholder);
    lv_textarea_set_max_length(compose_ta, max_length);
    lv_textarea_set_text(compose_ta, text);
    lv_keyboard_set_mode(compose_kb, text[0] ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_screen_load_anim(compose_screen, LV_SCREEN_LOAD_ANIM_FADE_IN, 150, 0, false);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, compose_kb);
    lv_anim_set_values(&a, KB_HEIGHT, 0);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *kb, int32_t y) { lv_obj_set_style_translate_y((lv_obj_t *)kb, y, 0); });
    lv_anim_start(&a);
}

static void compose_open_cb(lv_event_t *)
{
    ChatSummary s;
    if (!messenger_chat_summary(open_chat, s)) {
        return;
    }
    editing_name = false;
    // Channel messages go out as "<node name>: <text>", and MeshCore truncates the whole thing.
    compose_open(s.name, "Message", "", s.is_channel ? MAX_CHAT_TEXT - strlen(messenger_node_name()) - 2 : MAX_CHAT_TEXT);
}

static void name_open_cb(lv_event_t *)
{
    editing_name = true;
    compose_open("Watch name", "Name", messenger_node_name(), MAX_NODE_NAME);
}

static void build_compose()
{
    compose_screen = make_screen();
    header_button(compose_screen, LV_SYMBOL_CLOSE, LV_ALIGN_TOP_LEFT, compose_cancel_cb);
    compose_title = make_label(compose_screen, "", &lv_font_montserrat_28, lv_color_white());
    lv_label_set_long_mode(compose_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(compose_title, 250, lv_font_get_line_height(&lv_font_montserrat_28));
    lv_obj_set_style_text_align(compose_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(compose_title, LV_ALIGN_TOP_MID, 0, 38);

    compose_ta = lv_textarea_create(compose_screen);
    lv_obj_set_size(compose_ta, 410 - 48, 502 - KB_HEIGHT - 96 - 10);
    lv_obj_align(compose_ta, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_text_font(compose_ta, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(compose_ta, CARD, 0);
    lv_obj_set_style_radius(compose_ta, 20, 0);
    lv_obj_set_style_border_width(compose_ta, 0, LV_STATE_ANY);
    lv_obj_set_style_text_color(compose_ta, GRAY, LV_PART_TEXTAREA_PLACEHOLDER);

    compose_kb = lv_keyboard_create(compose_screen);
    lv_obj_set_size(compose_kb, LV_PCT(100), KB_HEIGHT);
    lv_obj_align(compose_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(compose_kb, LV_KEYBOARD_MODE_TEXT_LOWER, KB_LOWER, KB_TEXT_CTRL);
    lv_keyboard_set_map(compose_kb, LV_KEYBOARD_MODE_TEXT_UPPER, KB_UPPER, KB_TEXT_CTRL);
    lv_keyboard_set_map(compose_kb, LV_KEYBOARD_MODE_SPECIAL, KB_SPECIAL, KB_SPECIAL_CTRL);
    lv_keyboard_set_textarea(compose_kb, compose_ta);
    lv_obj_set_style_bg_color(compose_kb, lv_color_black(), 0);
    lv_obj_set_style_border_width(compose_kb, 0, 0);
    lv_obj_set_style_pad_hor(compose_kb, 6, 0);
    lv_obj_set_style_pad_top(compose_kb, 6, 0);
    lv_obj_set_style_pad_bottom(compose_kb, 14, 0);  // clear of the screen's rounded corners
    lv_obj_set_style_pad_gap(compose_kb, 5, 0);
    lv_obj_set_style_bg_color(compose_kb, CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(compose_kb, lv_color_hex(0x3A3A3C), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(compose_kb, ORANGE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(compose_kb, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(compose_kb, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_set_style_radius(compose_kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(compose_kb, 0, LV_PART_ITEMS);
    lv_obj_remove_event_cb(compose_kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(compose_kb, keyboard_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(compose_kb, compose_send_cb, LV_EVENT_READY, NULL);
}

// ---------- Setup ----------

static void messenger_changed(int chat, bool incoming)
{
    if (incoming) {
        instance.vibrator();
        lv_display_trigger_activity(NULL);  // wakes the screen and restarts its timeout
    }
    if (chat == open_chat) {
        if (lv_screen_active() == chat_screen) {
            messenger_mark_read(chat);
        }
        rebuild_chat();
    }
    rebuild_list();
}

void messages_init(lv_obj_t *home)
{
    home_screen = home;
    build_list();
    build_chat();
    build_reply();
    build_compose();
    rebuild_list();

    // Toasts sit on the top layer so they show over whichever screen is active.
    toast = make_label(lv_layer_top(), "", &lv_font_montserrat_20, lv_color_white());
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(toast, 20, 0);
    lv_obj_set_style_pad_ver(toast, 12, 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -130);  // clear of the chat view's reply button
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_timer = lv_timer_create([](lv_timer_t *t) {
        lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(t);
    }, TOAST_MS, NULL);
    lv_timer_pause(toast_timer);

    messenger_set_listener(messenger_changed);
}

void messages_open()
{
    lv_screen_load_anim(list_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
