#include "settings_app.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <Preferences.h>

static const lv_color_t ORANGE = LV_COLOR_MAKE(0xFF, 0x98, 0x00);  // lv_palette_main(LV_PALETTE_ORANGE)
static const int MIN_PERCENT = 10;  // dimmer than this is hard to read outdoors and looks off

static const uint16_t TIMEOUTS_S[] = {5, 10, 15, 30, 60, 120};
static const char *const TIMEOUT_MAP[] = {"5s", "10s", "15s", "30s", "1m", "2m", ""};
static const int NUM_TIMEOUTS = sizeof(TIMEOUTS_S) / sizeof(TIMEOUTS_S[0]);
static const int DEFAULT_TIMEOUT = 1;  // 10 s

static Preferences prefs;
static int brightness_percent;
static int timeout_index;
static bool message_vibrate;
static lv_obj_t *home_screen, *screen, *slider, *value_label;

uint8_t settings_brightness()
{
    return (brightness_percent * 255 + 50) / 100;
}

uint32_t settings_screen_timeout_ms()
{
    return TIMEOUTS_S[timeout_index] * 1000;
}

bool settings_message_vibrate()
{
    return message_vibrate;
}

static void show_value()
{
    lv_label_set_text_fmt(value_label, "%d%%", brightness_percent);
}

// Dragging changes the brightness live; it's saved when the finger lifts.
static void slider_cb(lv_event_t *e)
{
    brightness_percent = lv_slider_get_value(slider);
    show_value();
    instance.setBrightness(settings_brightness());
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        prefs.putUChar("bright", brightness_percent);
    }
}

static void timeout_cb(lv_event_t *e)
{
    timeout_index = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));
    prefs.putUShort("timeout", TIMEOUTS_S[timeout_index]);
}

static void vibrate_cb(lv_event_t *e)
{
    message_vibrate = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    prefs.putBool("msg_buzz", message_vibrate);
}

static lv_obj_t *make_card(lv_obj_t *parent, const char *title, int height = 140)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), height);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_color(card, CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_hor(card, 22, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *name = make_label(card, title, &lv_font_montserrat_28, lv_color_white());
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 10);
    return card;
}

static void back_cb(lv_event_t *)
{
    lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

void settings_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("settings");
    brightness_percent = constrain(prefs.getUChar("bright", 100), MIN_PERCENT, 100);
    message_vibrate = prefs.getBool("msg_buzz", true);
    uint16_t timeout_s = prefs.getUShort("timeout", TIMEOUTS_S[DEFAULT_TIMEOUT]);
    timeout_index = DEFAULT_TIMEOUT;
    for (int i = 0; i < NUM_TIMEOUTS; i++) {
        if (TIMEOUTS_S[i] == timeout_s) {
            timeout_index = i;
        }
    }

    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    lv_obj_t *title = make_label(screen, "Settings", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *cont = make_scroll_column(screen);

    lv_obj_t *card = make_card(cont, "Brightness");
    value_label = make_label(card, "", &lv_font_montserrat_24, GRAY);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, 0, 12);

    slider = lv_slider_create(card);
    lv_obj_set_size(slider, LV_PCT(94), 16);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_slider_set_range(slider, MIN_PERCENT, 100);
    lv_slider_set_value(slider, brightness_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, ORANGE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 24);  // easier to grab on a small screen
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_RELEASED, NULL);
    show_value();

    // Screen timeout: one of a few fixed times.
    card = make_card(cont, "Screen timeout");
    lv_obj_t *times = lv_buttonmatrix_create(card);
    lv_buttonmatrix_set_map(times, TIMEOUT_MAP);
    lv_buttonmatrix_set_button_ctrl_all(times, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(times, true);
    lv_buttonmatrix_set_button_ctrl(times, timeout_index, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(times, LV_PCT(100), 48);
    lv_obj_align(times, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(times, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(times, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(times, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(times, 6, LV_PART_MAIN);
    lv_obj_set_style_text_font(times, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(times, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(times, lv_color_hex(0x3A3A3C), LV_PART_ITEMS);
    lv_obj_set_style_radius(times, 14, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(times, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(times, ORANGE, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(times, lv_color_black(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(times, timeout_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Notifications: new messages still wake the screen; this only stops the buzz.
    card = make_card(cont, "Notifications", 110);
    lv_obj_t *hint = make_label(card, "Vibrate for messages", &lv_font_montserrat_18, GRAY);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_t *vibrate = lv_switch_create(card);
    lv_obj_set_size(vibrate, 90, 50);
    lv_obj_set_style_bg_color(vibrate, ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(vibrate, LV_ALIGN_RIGHT_MID, 0, 0);
    if (message_vibrate) {
        lv_obj_add_state(vibrate, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(vibrate, vibrate_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void settings_open()
{
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
