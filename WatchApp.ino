// WatchApp: time, date, and battery watch face for the LilyGo T-Watch Ultra.
//
// The RTC keeps UTC. Set it from the host with `make settime`, which sends "T<unix seconds>\n"
// over USB serial.
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <sys/time.h>

// POSIX rule for America/Denver (MST/MDT).
static const char *TIME_ZONE = "MST7MDT,M3.2.0,M11.1.0";

LV_FONT_DECLARE(font_alibaba_100);

static lv_obj_t *time_label;
static lv_obj_t *ampm_label;
static lv_obj_t *weekday_label;
static lv_obj_t *date_label;
static lv_obj_t *battery_label;
static lv_obj_t *battery_bar;

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

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);

    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_24, 0);
    lv_obj_align(battery_label, LV_ALIGN_TOP_MID, 0, 40);

    battery_bar = lv_bar_create(screen);
    lv_obj_set_size(battery_bar, 120, 8);
    lv_bar_set_range(battery_bar, 0, 100);
    lv_obj_set_style_bg_color(battery_bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(battery_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(battery_bar, LV_ALIGN_TOP_MID, 0, 78);

    time_label = lv_label_create(screen);
    lv_obj_set_style_text_font(time_label, &font_alibaba_100, 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, -20, -30);

    ampm_label = lv_label_create(screen);
    lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ampm_label, lv_color_hex(0x9E9E9E), 0);

    weekday_label = lv_label_create(screen);
    lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(weekday_label, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(weekday_label, LV_ALIGN_CENTER, 0, 70);

    date_label = lv_label_create(screen);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 118);

    update_face(NULL);
    lv_timer_create(update_face, 1000, NULL);

    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    Serial.println("WatchApp started");
}

void loop()
{
    handle_serial();
    lv_timer_handler();
    delay(5);
}
