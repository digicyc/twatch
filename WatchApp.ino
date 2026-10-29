// WatchApp: watch face for the LilyGo T-Watch Ultra. Swipe up for apps (Alarms, Messages, GPS,
// Map, SD Card, Wi-Fi).
//
// The RTC keeps UTC. The Wi-Fi app sets it from internet time whenever Wi-Fi connects; without
// Wi-Fi, set it from the host with `make settime`, which sends "T<unix seconds>\n" over USB serial.
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <sys/time.h>
#include "alarms.h"
#include "gps_app.h"
#include "map_app.h"
#include "messages.h"
#include "messenger.h"
#include "sdcard_app.h"
#include "wifi_app.h"

// POSIX rule for America/Denver (MST/MDT).
static const char *TIME_ZONE = "MST7MDT,M3.2.0,M11.1.0";
static const uint32_t SCREEN_TIMEOUT_MS = 10000;

LV_FONT_DECLARE(font_alibaba_100);
LV_IMAGE_DECLARE(satellite_icon);
LV_IMAGE_DECLARE(wifi_icon_1);
LV_IMAGE_DECLARE(wifi_icon_2);
LV_IMAGE_DECLARE(wifi_icon_3);
LV_IMAGE_DECLARE(wifi_icon_4);

static lv_obj_t *time_label;
static lv_obj_t *ampm_label;
static lv_obj_t *weekday_label;
static lv_obj_t *date_label;
static lv_obj_t *battery_label;
static lv_obj_t *battery_bar;
static lv_obj_t *gps_box, *gps_icon, *gps_label;  // satellite icon and count, top right
static lv_obj_t *wifi_icon;                        // Wi-Fi signal, top left
static lv_timer_t *face_timer;
static lv_obj_t *home_screen;
static lv_obj_t *tileview;  // page 0 is the watch face, page 1 the app launcher
static bool screen_on = true;
// Invisible full-screen layer, shown while the display is off, that catches the touch that wakes
// it so the tap doesn't also press whatever is underneath (keyboard keys fire on press).
static lv_obj_t *wake_shield;

static void update_face(lv_timer_t *)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    if (t.tm_year + 1900 < 2025) {
        lv_label_set_text(time_label, "--:--");
        lv_label_set_text(ampm_label, "");
        lv_label_set_text(weekday_label, "Time not set");
        lv_label_set_text(date_label, "Run: make settime");
    } else {
        char buf[32];
        int hour12 = t.tm_hour % 12 ? t.tm_hour % 12 : 12;
        lv_label_set_text_fmt(time_label, "%d:%02d", hour12, t.tm_min);
        lv_label_set_text(ampm_label, t.tm_hour < 12 ? "AM" : "PM");
        strftime(buf, sizeof(buf), "%A", &t);
        lv_label_set_text(weekday_label, buf);
        strftime(buf, sizeof(buf), "%B", &t);  // newlib's strftime has no "%-d"
        lv_label_set_text_fmt(date_label, "%s %d, %d", buf, t.tm_mday, t.tm_year + 1900);
    }
    lv_obj_align_to(ampm_label, time_label, LV_ALIGN_OUT_RIGHT_TOP, 8, 6);

    int percent = (int)instance.getBatteryPercent();
    bool charging = instance.isCharging();
    lv_color_t color = charging      ? lv_palette_main(LV_PALETTE_GREEN)
                       : percent <= 15 ? lv_palette_main(LV_PALETTE_RED)
                       : percent <= 30 ? lv_palette_main(LV_PALETTE_AMBER)
                       : lv_color_white();
    lv_label_set_text_fmt(battery_label, "%s %d%%", charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL, percent);
    lv_obj_set_style_text_color(battery_label, color, 0);
    lv_bar_set_value(battery_bar, percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(battery_bar, color, LV_PART_INDICATOR);

    // GPS: hidden when off, gray while searching, green with a fix; the count is satellites
    // in view while searching and used once fixed.
    GpsStatus gps = gps_status();
    if (gps.on) {
        lv_color_t gps_color = gps.fix ? lv_palette_main(LV_PALETTE_GREEN) : lv_color_hex(0x9E9E9E);
        lv_label_set_text_fmt(gps_label, "%d", gps.satellites);
        lv_obj_set_style_text_color(gps_label, gps_color, 0);
        lv_obj_set_style_image_recolor(gps_icon, gps_color, 0);
        lv_obj_remove_flag(gps_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(gps_box, LV_OBJ_FLAG_HIDDEN);
    }

    // Wi-Fi: shown while connected, with the signal as lit arcs.
    static const lv_image_dsc_t *const WIFI_ICONS[] = {&wifi_icon_1, &wifi_icon_2, &wifi_icon_3, &wifi_icon_4};
    int level = wifi_signal_level();
    if (level) {
        lv_image_set_src(wifi_icon, WIFI_ICONS[level - 1]);
        lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_wake_shield(lv_event_t *)
{
    lv_obj_add_flag(wake_shield, LV_OBJ_FLAG_HIDDEN);
}

// Turns the display off after SCREEN_TIMEOUT_MS without input; waking it shows the same screen.
// LVGL resets its inactivity time on touch; buttons, alarms and messages reset it via
// lv_display_trigger_activity().
static void update_screen_power()
{
    bool idle = lv_display_get_inactive_time(NULL) > SCREEN_TIMEOUT_MS;
    if (idle && screen_on) {
        screen_on = false;
        lv_timer_pause(face_timer);
        instance.setBrightness(0);
        instance.sleepDisplay();
        lv_obj_remove_flag(wake_shield, LV_OBJ_FLAG_HIDDEN);
    } else if (!idle && !screen_on) {
        screen_on = true;
        // A waking touch is still on the shield and hides it when lifted; buttons and messages
        // don't touch it, so hide it now.
        lv_indev_t *touch = lv_get_touch_indev();
        if (!touch || lv_indev_get_state(touch) != LV_INDEV_STATE_PRESSED) {
            hide_wake_shield(NULL);
        }
        instance.wakeupDisplay();
        update_face(NULL);
        lv_timer_resume(face_timer);
        lv_refr_now(NULL);
        instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    }
}

// Returns to the watch face from any app or the launcher.
static void show_face()
{
    lv_tileview_set_tile_by_index(tileview, 0, 0, screen_on ? LV_ANIM_ON : LV_ANIM_OFF);
    if (lv_screen_active() != home_screen) {
        lv_screen_load_anim(home_screen, screen_on ? LV_SCREEN_LOAD_ANIM_MOVE_RIGHT : LV_SCREEN_LOAD_ANIM_NONE, 200, 0, false);
    }
}

// Adds a round icon button with a caption to the launcher grid (three per row).
static void add_app(lv_obj_t *grid, const char *symbol, const char *name, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *item = lv_obj_create(grid);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, 130, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(item, 10, 0);

    lv_obj_t *btn = lv_button_create(item);
    lv_obj_set_size(btn, 104, 104);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_42, 0);
    lv_obj_set_style_text_color(icon, lv_color_black(), 0);
    lv_obj_center(icon);

    lv_obj_t *caption = lv_label_create(item);
    lv_label_set_text(caption, name);
}

static void build_launcher(lv_obj_t *page)
{
    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 30, 0);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 110);

    add_app(grid, LV_SYMBOL_BELL, "Alarms", lv_palette_main(LV_PALETTE_ORANGE), [](lv_event_t *) {
        alarms_open();
    });
    add_app(grid, LV_SYMBOL_ENVELOPE, "Messages", lv_palette_main(LV_PALETTE_LIGHT_BLUE), [](lv_event_t *) {
        messages_open();
    });
    add_app(grid, LV_SYMBOL_GPS, "GPS", lv_palette_main(LV_PALETTE_GREEN), [](lv_event_t *) {
        gps_open();
    });
    add_app(grid, LV_SYMBOL_IMAGE, "Map", lv_palette_main(LV_PALETTE_TEAL), [](lv_event_t *) {
        map_open();
    });
    add_app(grid, LV_SYMBOL_SD_CARD, "SD Card", lv_palette_main(LV_PALETTE_DEEP_PURPLE), [](lv_event_t *) {
        sdcard_open();
    });
    add_app(grid, LV_SYMBOL_WIFI, "Wi-Fi", lv_palette_main(LV_PALETTE_BLUE), [](lv_event_t *) {
        wifi_open();
    });
}

// Handles "T<unix seconds>" lines from `make settime`.
static void handle_serial()
{
    static char line[24];
    static size_t len = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c != '\n' && c != '\r') {
            if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
            continue;
        }
        line[len] = '\0';
        len = 0;
        if (line[0] != 'T') {
            continue;
        }
        time_t epoch = strtoll(line + 1, NULL, 10);
        if (epoch < 1735689600) {  // 2025-01-01
            Serial.printf("Rejected time: %s\n", line);
            continue;
        }
        struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
        settimeofday(&tv, NULL);
        struct tm utc;
        gmtime_r(&epoch, &utc);
        instance.rtc.setDateTime(utc);
        update_face(NULL);
        Serial.printf("Time set: %s", ctime(&epoch));
    }
}

void setup()
{
    Serial.begin(115200);

    // begin() loads the RTC into the system clock; TZ is still unset here, so it is read as UTC.
    instance.begin();
    setenv("TZ", TIME_ZONE, 1);
    tzset();

    beginLvglHelper(instance);

    lv_display_t *display = lv_display_get_default();
    lv_display_set_theme(display, lv_theme_default_init(display, lv_palette_main(LV_PALETTE_ORANGE),
                         lv_palette_main(LV_PALETTE_GREY), true, &lv_font_montserrat_24));

    home_screen = lv_screen_active();
    lv_obj_set_style_bg_color(home_screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(home_screen, lv_color_white(), 0);

    // Page 0 is the watch face; swipe up for page 1, the app launcher.
    tileview = lv_tileview_create(home_screen);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *face = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_BOTTOM);
    build_launcher(lv_tileview_add_tile(tileview, 0, 1, LV_DIR_TOP));

    lv_obj_t *swipe_hint = lv_label_create(face);
    lv_label_set_text(swipe_hint, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(swipe_hint, lv_color_hex(0x5A5A5A), 0);
    lv_obj_align(swipe_hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    battery_label = lv_label_create(face);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_24, 0);
    lv_obj_align(battery_label, LV_ALIGN_TOP_MID, 0, 40);

    battery_bar = lv_bar_create(face);
    lv_obj_set_size(battery_bar, 120, 8);
    lv_bar_set_range(battery_bar, 0, 100);
    lv_obj_set_style_bg_color(battery_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(battery_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(battery_bar, LV_ALIGN_TOP_MID, 0, 78);

    gps_box = lv_obj_create(face);
    lv_obj_remove_style_all(gps_box);
    lv_obj_set_size(gps_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gps_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gps_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(gps_box, 4, 0);
    lv_obj_align(gps_box, LV_ALIGN_TOP_RIGHT, -52, 42);
    gps_icon = lv_image_create(gps_box);
    lv_image_set_src(gps_icon, &satellite_icon);  // alpha-only; drawn in the recolor color
    gps_label = lv_label_create(gps_box);
    lv_obj_set_style_text_font(gps_label, &lv_font_montserrat_20, 0);

    wifi_icon = lv_image_create(face);  // mirrors the GPS icon; alpha-only, drawn in the recolor color
    lv_obj_set_style_image_recolor(wifi_icon, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_align(wifi_icon, LV_ALIGN_TOP_LEFT, 52, 42);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);

    time_label = lv_label_create(face);
    lv_obj_set_style_text_font(time_label, &font_alibaba_100, 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, -20, -30);

    ampm_label = lv_label_create(face);
    lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ampm_label, lv_color_hex(0x9E9E9E), 0);

    weekday_label = lv_label_create(face);
    lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(weekday_label, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(weekday_label, LV_ALIGN_CENTER, 0, 70);

    date_label = lv_label_create(face);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 118);

    update_face(NULL);
    face_timer = lv_timer_create(update_face, 1000, NULL);
    alarms_init(home_screen);
    gps_init(home_screen);
    sdcard_init(home_screen);
    map_init(home_screen);
    wifi_init(home_screen);
    if (!messenger_init()) {
        Serial.println("LoRa radio failed to start");
    }
    messages_init(home_screen);

    wake_shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wake_shield);
    lv_obj_set_size(wake_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(wake_shield, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(wake_shield, hide_wake_shield, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(wake_shield, hide_wake_shield, LV_EVENT_PRESS_LOST, NULL);

    // PWR button (PMU) and BOOT button wake the screen; BOOT also goes to the watch face, except
    // while an alarm rings so its Snooze/Stop screen stays up.
    instance.onEvent(POWER_EVENT, [](const DeviceEvent &event, void *) {
        if (instance.getPMUEventType(event) == PMU_EVENT_KEY_CLICKED) {
            lv_display_trigger_activity(NULL);
        }
    });
    instance.onEvent(BUTTON_EVENT, [](const DeviceEvent &, void *) {
        lv_display_trigger_activity(NULL);
        if (!alarms_ringing()) {
            show_face();
        }
    });

    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    Serial.println("WatchApp started");
}

void loop()
{
    instance.loop();  // dispatches PMU and button events
    handle_serial();
    alarms_tick();
    gps_tick();
    messenger_loop();
    update_screen_power();
    lv_timer_handler();
    delay(5);
}
