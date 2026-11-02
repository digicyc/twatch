#include "steps_app.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <ImuDrv.hpp>  // SensorStepCounter
#include <Preferences.h>
#include <time.h>

static const lv_color_t ORANGE = LV_COLOR_MAKE(0xFF, 0x98, 0x00);  // lv_palette_main(LV_PALETTE_ORANGE)
static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);   // lv_palette_main(LV_PALETTE_GREEN)
static const lv_color_t TRACK = LV_COLOR_MAKE(0x3A, 0x3A, 0x3C);   // unfilled ring, bars, and buttons
static const double STRIDE_MI = 0.762 / 1609.344;                   // an average 2.5 ft walking step
static const uint32_t SAVE_MS = 60000;  // save the total at most once a minute while walking
static const int HISTORY = 6;           // previous days kept; the chart adds today
static const int BAR_MAX = 100;         // chart height in px

static const uint32_t GOALS[] = {5000, 8000, 10000, 12000, 15000};
static const char *const GOAL_MAP[] = {"5k", "8k", "10k", "12k", "15k", ""};
static const int NUM_GOALS = sizeof(GOALS) / sizeof(GOALS[0]);
static const int DEFAULT_GOAL = 2;  // 10,000

struct DayTotal {
    uint32_t date;  // local date as YYYYMMDD, 0 for an empty slot
    uint32_t steps;
};

static SensorStepCounter *counter;
static Preferences prefs;
static uint32_t day;       // date the current count belongs to, YYYYMMDD; 0 until the clock is known
static int64_t base;       // today = base + the sensor's count, which starts at 0 on every boot
static uint32_t count;     // the sensor's latest count
static uint32_t today, saved;
static uint32_t last_save;
static DayTotal history[HISTORY];  // newest first
static int goal_index;
static bool dirty = true;  // the screen needs a refresh

static lv_obj_t *home_screen, *screen, *ring, *steps_label, *goal_label, *distance_label, *percent_label;
static lv_obj_t *bars[HISTORY + 1], *bar_days[HISTORY + 1];

// The local date `days_ago` days before now, as YYYYMMDD, and its weekday (0 = Sunday).
static uint32_t local_date(int days_ago, int *weekday = nullptr)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_mday -= days_ago;
    t.tm_hour = 12;  // mktime normalizes the day; noon keeps DST changes from moving it
    mktime(&t);
    if (weekday) {
        *weekday = t.tm_wday;
    }
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

static bool clock_set()
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year + 1900 >= 2025;
}

static uint32_t steps_on(uint32_t date)
{
    for (const DayTotal &d : history) {
        if (d.date == date) {
            return d.steps;
        }
    }
    return 0;
}

static void save()
{
    prefs.putUInt("day", day);
    prefs.putUInt("today", today);
    saved = today;
    last_save = millis();
}

// Moves the finished day into the history and starts counting the new one from zero.
static void start_day(uint32_t date)
{
    if (day) {
        memmove(&history[1], &history[0], sizeof(DayTotal) * (HISTORY - 1));
        history[0] = {day, today};
        prefs.putBytes("history", history, sizeof(history));
    }
    day = date;
    base = -(int64_t)count;
    today = 0;
    save();
    dirty = true;
}

// Formats a count with thousands separators: 12345 -> "12,345".
static void format_steps(char *buf, size_t size, uint32_t n)
{
    if (n >= 1000) {
        snprintf(buf, size, "%lu,%03lu", (unsigned long)(n / 1000), (unsigned long)(n % 1000));
    } else {
        snprintf(buf, size, "%lu", (unsigned long)n);
    }
}

static void refresh()
{
    dirty = false;
    uint32_t goal = GOALS[goal_index];
    bool met = today >= goal;
    char buf[24];

    lv_arc_set_range(ring, 0, goal);
    lv_arc_set_value(ring, met ? goal : today);
    lv_obj_set_style_arc_color(ring, met ? GREEN : ORANGE, LV_PART_INDICATOR);
    format_steps(buf, sizeof(buf), today);
    lv_label_set_text(steps_label, buf);
    format_steps(buf, sizeof(buf), goal);
    lv_label_set_text_fmt(goal_label, "of %s", buf);
    lv_label_set_text_fmt(distance_label, "%.1f mi", today * STRIDE_MI);
    lv_label_set_text_fmt(percent_label, "%lu%%", (unsigned long)((uint64_t)today * 100 / goal));

    // Bars run oldest to newest, scaled so the goal or the best day fills the chart.
    static const char *const LETTERS[] = {"S", "M", "T", "W", "T", "F", "S"};
    bool known = clock_set();
    uint32_t totals[HISTORY + 1], top = goal;
    for (int i = 0; i <= HISTORY; i++) {
        int days_ago = HISTORY - i, weekday = 0;
        uint32_t date = known ? local_date(days_ago, &weekday) : 0;
        totals[i] = days_ago == 0 ? today : known ? steps_on(date) : 0;
        top = totals[i] > top ? totals[i] : top;
        lv_label_set_text(bar_days[i], known ? LETTERS[weekday] : "-");
    }
    for (int i = 0; i <= HISTORY; i++) {
        int h = (int)((uint64_t)totals[i] * BAR_MAX / top);
        lv_obj_set_height(bars[i], h < 6 ? 6 : h);  // a stub keeps empty days visible
        lv_color_t color = totals[i] >= goal ? GREEN : i == HISTORY ? ORANGE : lv_color_hex(0x5A5A5A);
        lv_obj_set_style_bg_color(bars[i], color, 0);
    }
}

static void goal_cb(lv_event_t *e)
{
    goal_index = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));
    prefs.putUInt("goal", GOALS[goal_index]);
    refresh();
}

static void back_cb(lv_event_t *)
{
    lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static lv_obj_t *make_card(lv_obj_t *parent, const char *title, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), height);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_color(card, CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_hor(card, 22, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *name = make_label(card, title, &lv_font_montserrat_28, lv_color_white());
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 10);
    }
    return card;
}

// A small gray caption over a big value, for the stats card.
static lv_obj_t *make_stat(lv_obj_t *card, const char *caption, lv_align_t align)
{
    lv_obj_t *name = make_label(card, caption, &lv_font_montserrat_18, GRAY);
    lv_obj_align(name, align, 0, 6);
    lv_obj_t *value = make_label(card, "", &lv_font_montserrat_28, lv_color_white());
    lv_obj_align(value, align == LV_ALIGN_TOP_LEFT ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, -6);
    return value;
}

static void build_ui()
{
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    lv_obj_t *title = make_label(screen, "Steps", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *cont = make_scroll_column(screen);

    // Today: a ring filling toward the goal, with the count inside.
    ring = lv_arc_create(cont);
    lv_obj_set_size(ring, 250, 250);
    lv_arc_set_bg_angles(ring, 135, 45);
    lv_arc_set_rotation(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ring, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ring, true, LV_PART_INDICATOR);
    lv_obj_t *icon = lv_image_create(ring);
    LV_IMAGE_DECLARE(steps_icon);
    lv_image_set_src(icon, &steps_icon);
    lv_obj_set_style_image_recolor(icon, GRAY, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -62);
    steps_label = make_label(ring, "", &lv_font_montserrat_48, lv_color_white());
    lv_obj_align(steps_label, LV_ALIGN_CENTER, 0, 2);
    goal_label = make_label(ring, "", &lv_font_montserrat_20, GRAY);
    lv_obj_align(goal_label, LV_ALIGN_CENTER, 0, 46);

    lv_obj_t *card = make_card(cont, NULL, 96);
    distance_label = make_stat(card, "Distance", LV_ALIGN_TOP_LEFT);
    percent_label = make_stat(card, "Of goal", LV_ALIGN_TOP_RIGHT);

    // The last seven days, oldest on the left, each bar over its weekday's initial.
    card = make_card(cont, "This week", 210);
    lv_obj_t *chart = lv_obj_create(card);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, LV_PCT(100), BAR_MAX + 30);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(chart, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chart, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i <= HISTORY; i++) {
        lv_obj_t *col = lv_obj_create(chart);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 30, LV_PCT(100));
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 6, 0);
        bars[i] = lv_obj_create(col);
        lv_obj_remove_style_all(bars[i]);
        lv_obj_set_size(bars[i], 26, 6);
        lv_obj_set_style_radius(bars[i], 8, 0);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
        bar_days[i] = make_label(col, "", &lv_font_montserrat_18, GRAY);
    }

    card = make_card(cont, "Daily goal", 140);
    lv_obj_t *goals = lv_buttonmatrix_create(card);
    lv_buttonmatrix_set_map(goals, GOAL_MAP);
    lv_buttonmatrix_set_button_ctrl_all(goals, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(goals, true);
    lv_buttonmatrix_set_button_ctrl(goals, goal_index, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(goals, LV_PCT(100), 48);
    lv_obj_align(goals, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(goals, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(goals, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(goals, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(goals, 6, LV_PART_MAIN);
    lv_obj_set_style_text_font(goals, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(goals, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(goals, TRACK, LV_PART_ITEMS);
    lv_obj_set_style_radius(goals, 14, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(goals, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(goals, ORANGE, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(goals, lv_color_black(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(goals, goal_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void steps_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("steps");
    day = prefs.getUInt("day", 0);
    today = saved = prefs.getUInt("today", 0);
    base = today;
    prefs.getBytes("history", history, sizeof(history));
    uint32_t goal = prefs.getUInt("goal", GOALS[DEFAULT_GOAL]);
    goal_index = DEFAULT_GOAL;
    for (int i = 0; i < NUM_GOALS; i++) {
        if (GOALS[i] == goal) {
            goal_index = i;
        }
    }
    if (instance.getDeviceProbe() & HW_BHI260AP_ONLINE) {
        counter = new SensorStepCounter(instance.sensor);
        counter->enable(1.0, 0);  // reports as the count changes
    }
    build_ui();
}

void steps_open()
{
    refresh();
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void steps_tick()
{
    if (counter && counter->hasUpdated()) {
        uint32_t c = counter->getStepCount();
        if (c < count) {
            base = today - (int64_t)c;  // the sensor restarted its count; carry on from today's total
        }
        count = c;
        uint32_t t = base + count;
        if (t != today) {
            today = t;
            dirty = true;
        }
    }

    static uint32_t last_check;
    if (millis() - last_check >= 1000) {
        last_check = millis();
        // Steps taken before the clock is set count toward the saved day.
        if (clock_set()) {
            uint32_t date = local_date(0);
            if (date != day) {
                start_day(date);
            }
        }
        if (today != saved && millis() - last_save >= SAVE_MS) {
            save();
        }
    }

    if (dirty && lv_screen_active() == screen) {
        refresh();
    }
}
