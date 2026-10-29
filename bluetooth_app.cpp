#include "bluetooth_app.h"
#include "messenger.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);   // lv_palette_main(LV_PALETTE_GREEN)
static const lv_color_t ORANGE = LV_COLOR_MAKE(0xFF, 0x98, 0x00);  // lv_palette_main(LV_PALETTE_ORANGE)

static lv_obj_t *home_screen, *screen, *power_switch, *status_label, *name_label, *pin_label;

// Sets a label's text only when it changes; this display repaints the whole screen for any change.
static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *make_row(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_radius(row, 24, 0);
    lv_obj_set_style_bg_color(row, CARD, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 22, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

// A row with a gray caption over a value; returns the value label.
static lv_obj_t *add_value_row(lv_obj_t *parent, const char *caption, const lv_font_t *font)
{
    lv_obj_t *row = make_row(parent, 100);
    lv_obj_t *cap = make_label(row, caption, &lv_font_montserrat_20, GRAY);
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, 0, -20);
    lv_obj_t *value = make_label(row, "", font, lv_color_white());
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_size(value, LV_PCT(100), lv_font_get_line_height(font));
    lv_obj_align(value, LV_ALIGN_LEFT_MID, 0, 16);
    return value;
}

static void update_screen()
{
    bool on = messenger_bt_enabled(), connected = messenger_bt_connected();
    set_text(status_label, !on ? "Off" : connected ? "Phone connected" : "Waiting for phone");
    lv_obj_set_style_text_color(status_label, connected ? GREEN : GRAY, 0);
    set_text(name_label, messenger_bt_name());
    char pin[8];
    snprintf(pin, sizeof(pin), "%06u", (unsigned)messenger_bt_pin());
    set_text(pin_label, pin);
}

static void timer_cb(lv_timer_t *)
{
    if (lv_screen_active() == screen) {
        update_screen();
    }
}

static void switch_cb(lv_event_t *)
{
    messenger_bt_set_enabled(lv_obj_has_state(power_switch, LV_STATE_CHECKED));
    update_screen();
}

static void back_cb(lv_event_t *)
{
    lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

void bluetooth_init(lv_obj_t *home)
{
    home_screen = home;
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    lv_obj_t *title = make_label(screen, "Bluetooth", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *cont = make_scroll_column(screen);

    // On/off and status.
    lv_obj_t *power = make_row(cont, 110);
    lv_obj_t *name = make_label(power, "Bluetooth", &lv_font_montserrat_28, lv_color_white());
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -18);
    status_label = make_label(power, "", &lv_font_montserrat_20, GRAY);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 0, 18);
    power_switch = lv_switch_create(power);
    lv_obj_set_size(power_switch, 90, 50);
    lv_obj_set_style_bg_color(power_switch, ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(power_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    if (messenger_bt_enabled()) {
        lv_obj_add_state(power_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(power_switch, switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // What the MeshCore app shows and asks for.
    name_label = add_value_row(cont, "Device name", &lv_font_montserrat_24);
    pin_label = add_value_row(cont, "PIN", &lv_font_montserrat_32);
    lv_obj_set_style_text_letter_space(pin_label, 4, 0);

    lv_obj_t *note = make_label(cont, "Connect from the MeshCore app and enter this PIN.",
                                &lv_font_montserrat_20, GRAY);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(note, 6, 0);

    update_screen();
    lv_timer_create(timer_cb, 500, NULL);
}

void bluetooth_open()
{
    update_screen();
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
