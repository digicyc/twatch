#include "gps_app.h"
#include "ui.h"
#include <LilyGoLib.h>
#include <Preferences.h>
#include <sys/time.h>

static const uint32_t FIX_MAX_AGE_MS = 5000;  // older positions are from before the GPS lost its fix
static const uint32_t CLOCK_SYNC_INTERVAL_MS = 3600000;  // re-set the clock hourly while there's a fix
static const int MAX_BARS = 12;               // satellites shown, strongest first; fills the card
static const int BAR_MAX_HEIGHT = 100;        // px for a C/N0 of FULL_CN0 or more
static const int FULL_CN0 = 50;               // dB-Hz
static const lv_color_t GREEN = LV_COLOR_MAKE(0x4C, 0xAF, 0x50);   // lv_palette_main(LV_PALETTE_GREEN)
static const lv_color_t ORANGE = LV_COLOR_MAKE(0xFF, 0x98, 0x00);  // lv_palette_main(LV_PALETTE_ORANGE)
static const lv_color_t DIM = LV_COLOR_MAKE(0x5A, 0x5A, 0x5A);

enum { ROW_LAT, ROW_LON, ROW_ALT, ROW_SPEED, ROW_HDOP, ROW_COUNT };
static const char *ROW_NAMES[ROW_COUNT] = {"Latitude", "Longitude", "Altitude", "Speed", "HDOP"};

static Preferences prefs;
static bool gps_on;          // the user's setting
static bool running = true;  // LilyGoLib starts the receiver in begin()
static bool aid_pending = true;  // send the time once the receiver is talking
static uint32_t chars_at_wake;
static GNSSSatelliteInfo sats[GNSS_MAX_SATELLITES];
static bool clock_synced;
static uint32_t clock_synced_ms;

static lv_obj_t *home_screen, *screen, *power_switch, *status_label, *values[ROW_COUNT];
static lv_obj_t *sats_count_label, *bars_cont;

static bool online()
{
    return instance.getDeviceProbe() & HW_GPS_ONLINE;
}

static bool has_fix()
{
    return running && instance.gps.location.isValid() && instance.gps.location.age() < FIX_MAX_AGE_MS;
}

GpsStatus gps_status()
{
    GpsStatus s = {gps_on, has_fix(), 0};
    if (s.fix) {
        s.satellites = instance.gps.satellites.value();
    } else if (running && online()) {
        s.satellites = instance.gps.getSatelliteCount();
    }
    return s;
}

bool gps_location(double &lat, double &lon)
{
    if (!has_fix()) {
        return false;
    }
    lat = instance.gps.location.lat();
    lon = instance.gps.location.lng();
    return true;
}

// Seconds since 1970 for a UTC date and time (newlib has no timegm).
static time_t utc_epoch(int y, int mon, int d, int h, int min, int sec)
{
    y -= mon <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long days = era * 146097L + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + h * 3600 + min * 60 + sec;
}

// Sets the clock and RTC from GPS time on the first fix after boot and hourly after, so the time is
// right without Wi-Fi. The receiver reports a time before it has a fix too, but it can be wrong.
static void sync_clock()
{
    GPS &gps = instance.gps;
    if ((clock_synced && millis() - clock_synced_ms < CLOCK_SYNC_INTERVAL_MS) || !has_fix() || !gps.date.isValid() ||
        !gps.time.isValid() || gps.date.age() > 1000 || gps.time.age() > 1000 || gps.date.year() < 2025) {
        return;
    }
    time_t t = utc_epoch(gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(), gps.time.minute(),
                         gps.time.second());
    struct timeval tv = {.tv_sec = t, .tv_usec = 0};
    settimeofday(&tv, NULL);
    struct tm utc;
    gmtime_r(&t, &utc);
    instance.rtc.setDateTime(utc);  // the RTC keeps UTC
    clock_synced = true;
    clock_synced_ms = millis();
    Serial.printf("Clock set from GPS: %s", ctime(&t));
}

// Off puts the receiver in backup mode rather than cutting its power: it keeps its last position and
// satellite data, and its UART stays usable for the time aid below, so it has a fix again ~10 s after
// being turned on (~30 s from a power cut).
static void receiver_sleep()
{
    // UBX-RXM-PMREQ: backup mode (forced) with no time limit, woken by activity on its UART RX line.
    static const uint8_t PMREQ[] = {0xB5, 0x62, 0x02, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x61, 0x6B};
    Serial1.write(PMREQ, sizeof(PMREQ));
}

static void receiver_wake()
{
    static const uint8_t WAKE[] = {0xFF, 0xFF, 0xFF, 0xFF};  // lost while it wakes; any edge will do
    Serial1.write(WAKE, sizeof(WAKE));
}

// The receiver keeps its satellite data while off but not the time, and finding the time from the
// satellites takes ~15 s. Telling it the watch's time (UBX-MGA-INI-TIME_UTC) lets it use that data
// straight away.
static void send_time_aid()
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    if (utc.tm_year + 1900 < 2025) {
        return;  // the watch doesn't know the time either
    }
    uint8_t msg[8 + 24] = {0xB5, 0x62, 0x13, 0x40, 24, 0};
    uint8_t *p = msg + 6;
    p[0] = 0x10;                // type: TIME_UTC
    p[3] = 0x80;                // leap seconds unknown
    p[4] = (utc.tm_year + 1900) & 0xFF;
    p[5] = (utc.tm_year + 1900) >> 8;
    p[6] = utc.tm_mon + 1;
    p[7] = utc.tm_mday;
    p[8] = utc.tm_hour;
    p[9] = utc.tm_min;
    p[10] = utc.tm_sec;
    p[16] = 2;                  // accuracy: 2 s
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6 + 24; i++) {
        a += msg[i];
        b += a;
    }
    msg[30] = a;
    msg[31] = b;
    Serial1.write(msg, sizeof(msg));
}

void gps_tick()
{
    GPS &gps = instance.gps;
    // Switch off only after LilyGoLib's boot-time probe finishes, or the receiver never comes online.
    if (!gps_on && running && gps.probeDone()) {
        if (online()) {
            receiver_sleep();
        } else {
            instance.powerControl(POWER_GPS, false);  // nothing answered the probe
        }
        running = false;
    } else if (gps_on && !running) {
        if (online()) {
            receiver_wake();
            aid_pending = true;
            chars_at_wake = gps.charsProcessed();
        } else {
            instance.powerControl(POWER_GPS, true);
        }
        running = true;
    }
    if (running && online()) {
        gps.loop();  // instance.loop() parses NMEA; this also drops satellites unheard for 5 s
        if (aid_pending && gps_on && gps.charsProcessed() != chars_at_wake) {
            aid_pending = false;
            send_time_aid();
        }
        sync_clock();
    }
}

// ---------- Screen ----------

static void set_value(int row, const char *fmt, ...)
{
    char buf[32];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    lv_label_set_text(values[row], buf);
}

static void update_bars()
{
    lv_obj_clean(bars_cont);
    size_t n = running && online() ? instance.gps.copySatellites(sats, GNSS_MAX_SATELLITES) : 0;
    int used = 0;
    for (size_t i = 0; i < n; i++) {
        used += sats[i].used;
    }
    lv_label_set_text_fmt(sats_count_label, "%d used / %d in view", used, (int)n);
    if (!n) {
        lv_obj_t *none = make_label(bars_cont, gps_on ? "None in view yet" : "GPS is off", &lv_font_montserrat_20, GRAY);
        return;
    }
    // Strongest first.
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && sats[j - 1].cn0 * sats[j - 1].has_cn0 < sats[j].cn0 * sats[j].has_cn0; j--) {
            GNSSSatelliteInfo tmp = sats[j];
            sats[j] = sats[j - 1];
            sats[j - 1] = tmp;
        }
    }
    for (size_t i = 0; i < n && i < (size_t)MAX_BARS; i++) {
        lv_obj_t *col = lv_obj_create(bars_cont);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, 24, LV_PCT(100));
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 4, 0);

        int cn0 = sats[i].has_cn0 ? sats[i].cn0 : 0;
        lv_obj_t *bar = lv_obj_create(col);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 16, LV_MAX(4, LV_MIN(cn0, FULL_CN0) * BAR_MAX_HEIGHT / FULL_CN0));
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_style_bg_color(bar, sats[i].used ? GREEN : DIM, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

        lv_obj_t *prn = make_label(col, "", &lv_font_montserrat_14, GRAY);
        lv_label_set_text_fmt(prn, "%d", sats[i].prn);
    }
}

static void update_screen()
{
    GPS &gps = instance.gps;
    GpsStatus s = gps_status();
    if (!gps_on) {
        lv_label_set_text(status_label, "Off");
    } else if (!online()) {
        lv_label_set_text(status_label, gps.probeDone() ? "Receiver not detected" : "Starting...");
    } else if (s.fix) {
        lv_label_set_text_fmt(status_label, "Fix with %d satellites", s.satellites);
    } else {
        lv_label_set_text_fmt(status_label, "Searching, %d in view", s.satellites);
    }
    lv_obj_set_style_text_color(status_label, s.fix ? GREEN : GRAY, 0);

    if (s.fix) {
        double lat = gps.location.lat(), lng = gps.location.lng();
        set_value(ROW_LAT, "%.5f %c", fabs(lat), lat < 0 ? 'S' : 'N');
        set_value(ROW_LON, "%.5f %c", fabs(lng), lng < 0 ? 'W' : 'E');
        set_value(ROW_ALT, gps.altitude.isValid() ? "%.0f ft" : "--", gps.altitude.meters() * 3.28084);
        set_value(ROW_SPEED, gps.speed.isValid() ? "%.1f mph" : "--", gps.speed.kmph() * 0.621371);
        set_value(ROW_HDOP, gps.hdop.isValid() ? "%.1f" : "--", gps.hdop.hdop());
    } else {
        for (int row = 0; row < ROW_COUNT; row++) {
            lv_label_set_text(values[row], "--");
        }
    }
    update_bars();
}

static void refresh_cb(lv_timer_t *)
{
    if (lv_screen_active() == screen) {
        update_screen();
    }
}

static void switch_cb(lv_event_t *)
{
    gps_on = lv_obj_has_state(power_switch, LV_STATE_CHECKED);
    prefs.putBool("on", gps_on);
    gps_tick();
    update_screen();
}

static void back_cb(lv_event_t *)
{
    lv_screen_load_anim(home_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_color(card, CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_hor(card, 22, 0);
    lv_obj_set_style_pad_ver(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void build_screen()
{
    screen = make_screen();
    header_button(screen, LV_SYMBOL_LEFT, LV_ALIGN_TOP_LEFT, back_cb);
    lv_obj_t *title = make_label(screen, "GPS", &lv_font_montserrat_32, lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_t *cont = make_scroll_column(screen);

    // On/off and status.
    lv_obj_t *power = make_card(cont);
    lv_obj_set_height(power, 100);
    lv_obj_t *name = make_label(power, "GPS", &lv_font_montserrat_28, lv_color_white());
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -16);
    status_label = make_label(power, "", &lv_font_montserrat_20, GRAY);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(status_label, 230, lv_font_get_line_height(&lv_font_montserrat_20));
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 0, 20);
    power_switch = lv_switch_create(power);
    lv_obj_set_size(power_switch, 90, 50);
    lv_obj_set_style_bg_color(power_switch, ORANGE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(power_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(power_switch, switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Position.
    lv_obj_t *pos = make_card(cont);
    lv_obj_set_flex_flow(pos, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pos, 10, 0);
    for (int row = 0; row < ROW_COUNT; row++) {
        lv_obj_t *line = lv_obj_create(pos);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
        make_label(line, ROW_NAMES[row], &lv_font_montserrat_20, GRAY);
        values[row] = make_label(line, "--", &lv_font_montserrat_24, lv_color_white());
        lv_obj_align(values[row], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    // Satellites: one bar per satellite, height by signal strength, green if used in the fix.
    lv_obj_t *sky = make_card(cont);
    lv_obj_set_flex_flow(sky, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sky, 10, 0);
    lv_obj_t *head = lv_obj_create(sky);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    make_label(head, "Satellites", &lv_font_montserrat_24, lv_color_white());
    sats_count_label = make_label(head, "", &lv_font_montserrat_18, GRAY);
    lv_obj_align(sats_count_label, LV_ALIGN_RIGHT_MID, 0, 0);
    bars_cont = lv_obj_create(sky);
    lv_obj_remove_style_all(bars_cont);
    lv_obj_set_size(bars_cont, LV_PCT(100), BAR_MAX_HEIGHT + 24);
    lv_obj_set_flex_flow(bars_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bars_cont, 2, 0);
    lv_obj_t *legend = make_label(sky, "Bar height is signal strength. Green bars are used in the fix.",
                                  &lv_font_montserrat_16, GRAY);
    lv_obj_set_width(legend, LV_PCT(100));

    lv_timer_create(refresh_cb, 1000, NULL);
}

void gps_init(lv_obj_t *home)
{
    home_screen = home;
    prefs.begin("gps");
    gps_on = prefs.getBool("on", true);
    build_screen();
    if (gps_on) {
        lv_obj_add_state(power_switch, LV_STATE_CHECKED);
    }
}

void gps_open()
{
    update_screen();
    lv_screen_load_anim(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
