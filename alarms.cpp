#include "alarms.h"
#include <LilyGoLib.h>
#include <Preferences.h>

LV_FONT_DECLARE(font_alibaba_100);

static const uint8_t MAX_ALARMS = 8;
static const time_t SNOOZE_SECONDS = 5 * 60;
static const uint32_t RING_TIMEOUT_MS = 60 * 1000;
static const uint32_t SAMPLE_RATE = 16000;
static const lv_color_t GRAY = LV_COLOR_MAKE(0x9E, 0x9E, 0x9E);
static const lv_color_t CARD = LV_COLOR_MAKE(0x1C, 0x1C, 0x1E);

struct Alarm {
    uint8_t hour;  // 0-23, local time
    uint8_t minute;
    uint8_t enabled;
};

static Alarm alarms[MAX_ALARMS];
static uint8_t alarm_count;
static Preferences prefs;

static lv_obj_t *home_screen;
static lv_obj_t *list_screen, *list_cont, *add_btn;
static lv_obj_t *edit_screen, *edit_title, *hour_roller, *minute_roller, *ampm_roller, *delete_btn;
static lv_obj_t *ring_screen, *ring_time_label, *ring_ampm_label;
static int editing = -1;  // index of the alarm in the editor, -1 for a new alarm

static volatile bool ringing;  // read by sound_task
static uint8_t ring_hour, ring_minute;
static uint32_t ring_started_ms, last_buzz_ms;
static time_t snooze_until;

static void rebuild_list();

static void save_alarms()
{
    // Keep alarms sorted by time of day.
    for (int i = 1; i < alarm_count; i++) {
        for (int j = i; j > 0 && alarms[j - 1].hour * 60 + alarms[j - 1].minute > alarms[j].hour * 60 + alarms[j].minute; j--) {
            Alarm tmp = alarms[j];
            alarms[j] = alarms[j - 1];
            alarms[j - 1] = tmp;
        }
    }
    if (alarm_count) {
        prefs.putBytes("alarms", alarms, alarm_count * sizeof(Alarm));
    } else {
        prefs.remove("alarms");
    }
}

static int hour12(uint8_t hour)
{
    return hour % 12 ? hour % 12 : 12;
}

static lv_obj_t *make_screen()
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

// A text-only button for screen headers.
static lv_obj_t *header_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 14, 0);
    make_label(btn, text, &lv_font_montserrat_28, lv_palette_main(LV_PALETTE_ORANGE));
    lv_obj_align(btn, align, align == LV_ALIGN_TOP_LEFT ? 16 : -16, 20);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 170, 86);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_center(make_label(btn, text, &lv_font_montserrat_28, fg));
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// ---------- Ringing ----------

static void start_ringing(uint8_t hour, uint8_t minute)
{
    ring_hour = hour;
    ring_minute = minute;
    lv_label_set_text_fmt(ring_time_label, "%d:%02d", hour12(hour), minute);
    lv_label_set_text(ring_ampm_label, hour < 12 ? "AM" : "PM");
    lv_obj_align_to(ring_ampm_label, ring_time_label, LV_ALIGN_OUT_RIGHT_TOP, 8, 6);
    lv_screen_load(ring_screen);
    ring_started_ms = millis();
    last_buzz_ms = 0;
    snooze_until = 0;
    ringing = true;
    Serial.printf("Alarm ringing: %02d:%02d\n", hour, minute);
}

static void stop_ringing()
{
    ringing = false;
    lv_screen_load(home_screen);
}

static void snooze_cb(lv_event_t *)
{
    stop_ringing();
    snooze_until = time(NULL) + SNOOZE_SECONDS;
    Serial.println("Alarm snoozed");
}

static void stop_cb(lv_event_t *)
{
    stop_ringing();
    Serial.println("Alarm stopped");
}

// Plays a double beep pattern on the speaker while an alarm rings.
static void sound_task(void *)
{
    static int16_t beep[SAMPLE_RATE * 150 / 1000];
    static int16_t silence[SAMPLE_RATE * 100 / 1000];
    for (size_t i = 0; i < sizeof(beep) / sizeof(beep[0]); i++) {
        beep[i] = (int16_t)(12000 * sinf(2 * PI * 2000 * i / SAMPLE_RATE));
    }
    AudioOutputIf *out = instance.getAudioOutput();
    bool open = false;
    for (;;) {
        if (!ringing) {
            if (open) {
                out->write((uint8_t *)silence, sizeof(silence));
                out->close();  // also powers the speaker amplifier down
                open = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!open) {
            out->open(16, 1, SAMPLE_RATE);
            open = true;
        }
        out->write((uint8_t *)beep, sizeof(beep));
        out->write((uint8_t *)silence, sizeof(silence));
        out->write((uint8_t *)beep, sizeof(beep));
        for (int i = 0; i < 6 && ringing; i++) {
            out->write((uint8_t *)silence, sizeof(silence));
        }
    }
}

void alarms_tick()
{
    time_t now = time(NULL);
    static time_t last_minute = now / 60;
    bool new_minute = now / 60 != last_minute;
    last_minute = now / 60;

    if (ringing) {
        lv_display_trigger_activity(NULL);  // keep the screen on
        if (millis() - last_buzz_ms >= 1000) {
            last_buzz_ms = millis();
            instance.vibrator();
        }
        if (millis() - ring_started_ms > RING_TIMEOUT_MS) {
            stop_ringing();
        }
        return;
    }

    if (snooze_until && now >= snooze_until) {
        start_ringing(ring_hour, ring_minute);
        return;
    }

    struct tm t;
    localtime_r(&now, &t);
    if (!new_minute || t.tm_year + 1900 < 2025) {
        return;
    }
    for (int i = 0; i < alarm_count; i++) {
        if (alarms[i].enabled && alarms[i].hour == t.tm_hour && alarms[i].minute == t.tm_min) {
            start_ringing(alarms[i].hour, alarms[i].minute);
            return;
        }
    }
}

bool alarms_ringing()
{
    return ringing;
}

// ---------- Editor ----------

static void open_editor(int index)
{
    editing = index;
    uint8_t hour = index < 0 ? 7 : alarms[index].hour;
    uint8_t minute = index < 0 ? 0 : alarms[index].minute;
    lv_roller_set_selected(hour_roller, hour % 12, LV_ANIM_OFF);
    lv_roller_set_selected(minute_roller, minute, LV_ANIM_OFF);
    lv_roller_set_selected(ampm_roller, hour >= 12, LV_ANIM_OFF);
    lv_label_set_text(edit_title, index < 0 ? "New alarm" : "Edit alarm");
    if (index < 0) {
        lv_obj_add_flag(delete_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(delete_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_screen_load_anim(edit_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void close_editor()
{
    rebuild_list();
    lv_screen_load_anim(list_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void editor_save_cb(lv_event_t *)
{
    int index = editing < 0 ? alarm_count++ : editing;
    alarms[index].hour = lv_roller_get_selected(hour_roller) + 12 * lv_roller_get_selected(ampm_roller);
    alarms[index].minute = lv_roller_get_selected(minute_roller);
    alarms[index].enabled = 1;
    save_alarms();
    close_editor();
}

static void editor_cancel_cb(lv_event_t *)
{
    close_editor();
}

static void editor_delete_cb(lv_event_t *)
{
    for (int i = editing; i < alarm_count - 1; i++) {
        alarms[i] = alarms[i + 1];
    }
    alarm_count--;
    save_alarms();
    close_editor();
}

static lv_obj_t *make_roller(lv_obj_t *parent, const char *options, lv_roller_mode_t mode)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, mode);
    lv_obj_set_width(roller, 110);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_40, 0);
    lv_roller_set_visible_row_count(roller, 3);  // after the font, which sets the row height
    lv_obj_set_style_text_color(roller, GRAY, 0);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller, 0, 0);
    lv_obj_set_style_bg_color(roller, CARD, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 16, LV_PART_SELECTED);
    return roller;
}

static void build_editor()
{
    edit_screen = make_screen();
    header_button(edit_screen, LV_SYMBOL_CLOSE, LV_ALIGN_TOP_LEFT, editor_cancel_cb);
    header_button(edit_screen, LV_SYMBOL_OK, LV_ALIGN_TOP_RIGHT, editor_save_cb);
    edit_title = make_label(edit_screen, "", &lv_font_montserrat_28, lv_color_white());
    lv_obj_align(edit_title, LV_ALIGN_TOP_MID, 0, 34);

    static char hours[64], minutes[200];
    char *p = hours;
    for (int h = 0; h < 12; h++) {
        p += sprintf(p, h ? "\n%d" : "%d", h ? h : 12);
    }
    p = minutes;
    for (int m = 0; m < 60; m++) {
        p += sprintf(p, m ? "\n%02d" : "%02d", m);
    }

    lv_obj_t *row = lv_obj_create(edit_screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -20);

    hour_roller = make_roller(row, hours, LV_ROLLER_MODE_INFINITE);
    lv_obj_t *colon = make_label(row, ":", &lv_font_montserrat_40, lv_color_white());
    LV_UNUSED(colon);
    minute_roller = make_roller(row, minutes, LV_ROLLER_MODE_INFINITE);
    ampm_roller = make_roller(row, "AM\nPM", LV_ROLLER_MODE_NORMAL);

    delete_btn = pill_button(edit_screen, "Delete", CARD, lv_palette_main(LV_PALETTE_RED), editor_delete_cb);
    lv_obj_set_width(delete_btn, 240);
    lv_obj_align(delete_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
}

// ---------- List ----------

static void row_clicked_cb(lv_event_t *e)
{
    open_editor((int)(intptr_t)lv_event_get_user_data(e));
}

static void row_switch_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    alarms[index].enabled = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    save_alarms();
    rebuild_list();
}

static void add_cb(lv_event_t *)
{
    open_editor(-1);
}

static void back_cb(lv_event_t *)
{
    lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void rebuild_list()
{
    lv_obj_clean(list_cont);
    if (alarm_count >= MAX_ALARMS) {
        lv_obj_add_flag(add_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(add_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (!alarm_count) {
        lv_obj_t *empty = make_label(list_cont, "No alarms\nTap + to add one", &lv_font_montserrat_28, GRAY);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(empty, 100, 0);
        return;
    }
    for (int i = 0; i < alarm_count; i++) {
        lv_color_t color = alarms[i].enabled ? lv_color_white() : GRAY;

        lv_obj_t *row = lv_obj_create(list_cont);
        lv_obj_set_size(row, LV_PCT(100), 100);
        lv_obj_set_style_radius(row, 24, 0);
        lv_obj_set_style_bg_color(row, CARD, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 24, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *time = make_label(row, "", &lv_font_montserrat_44, color);
        lv_label_set_text_fmt(time, "%d:%02d", hour12(alarms[i].hour), alarms[i].minute);
        lv_obj_align(time, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *ampm = make_label(row, alarms[i].hour < 12 ? "AM" : "PM", &lv_font_montserrat_24, GRAY);
        lv_obj_align_to(ampm, time, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 90, 50);
        lv_obj_set_style_bg_color(sw, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (alarms[i].enabled) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(sw, row_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
}

static void build_list()
{
    list_screen = make_screen();
    header_button(list_screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    add_btn = header_button(list_screen, LV_SYMBOL_PLUS, LV_ALIGN_TOP_RIGHT, add_cb);
    lv_obj_t *title = make_label(list_screen, "Alarms", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    list_cont = lv_obj_create(list_screen);
    lv_obj_remove_style_all(list_cont);
    lv_obj_set_size(list_cont, LV_PCT(100), 502 - 110);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(list_cont, 24, 0);
    lv_obj_set_style_pad_bottom(list_cont, 24, 0);
    lv_obj_set_style_pad_row(list_cont, 14, 0);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_cont, LV_SCROLLBAR_MODE_OFF);
}

static void build_ring_screen()
{
    ring_screen = make_screen();
    lv_obj_t *bell = make_label(ring_screen, LV_SYMBOL_BELL, &lv_font_montserrat_48, lv_palette_main(LV_PALETTE_ORANGE));
    lv_obj_align(bell, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_t *title = make_label(ring_screen, "Alarm", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 128);

    ring_time_label = make_label(ring_screen, "", &font_alibaba_100, lv_color_white());
    lv_obj_align(ring_time_label, LV_ALIGN_CENTER, -20, 0);
    ring_ampm_label = make_label(ring_screen, "", &lv_font_montserrat_28, GRAY);

    lv_obj_t *snooze = pill_button(ring_screen, "Snooze", lv_color_hex(0x3A3A3C), lv_color_white(), snooze_cb);
    lv_obj_align(snooze, LV_ALIGN_BOTTOM_LEFT, 24, -40);
    lv_obj_t *stop = pill_button(ring_screen, "Stop", lv_palette_main(LV_PALETTE_ORANGE), lv_color_black(), stop_cb);
    lv_obj_align(stop, LV_ALIGN_BOTTOM_RIGHT, -24, -40);
}

void alarms_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("watchapp");
    if (prefs.isKey("alarms")) {
        alarm_count = min(prefs.getBytesLength("alarms") / sizeof(Alarm), (size_t)MAX_ALARMS);
        prefs.getBytes("alarms", alarms, alarm_count * sizeof(Alarm));
    }

    build_list();
    build_editor();
    build_ring_screen();
    rebuild_list();

    instance.setHapticEffects(14);  // DRV2605 "strong buzz"
    xTaskCreatePinnedToCore(sound_task, "alarm_sound", 4096, NULL, 1, NULL, 0);
}

void alarms_open()
{
    lv_screen_load_anim(list_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
