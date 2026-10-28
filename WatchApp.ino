// WatchApp: starter sketch for the LilyGo T-Watch Ultra, based on LilyGoLib's helloworld example.
#include <LilyGoLib.h>
#include <LV_Helper.h>

static lv_obj_t *uptime_label;

static void btn_event_cb(lv_event_t *e)
{
    static uint32_t count = 0;
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_label_set_text_fmt(lv_obj_get_child(btn, 0), "Pressed %lu", (unsigned long)++count);
    Serial.printf("Button pressed %lu times\n", (unsigned long)count);
}

void setup()
{
    Serial.begin(115200);

    instance.begin();
    beginLvglHelper(instance);

    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "Hello, T-Watch Ultra");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    uptime_label = lv_label_create(lv_screen_active());
    lv_obj_align_to(uptime_label, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn, 160, 60);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 50);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(lv_label_create(btn));
    lv_label_set_text(lv_obj_get_child(btn, 0), "Press me");

    instance.setBrightness(DEVICE_MAX_BRIGHTNESS_LEVEL);
    Serial.println("WatchApp started");
}

void loop()
{
    static uint32_t last_update = 0;
    if (millis() - last_update >= 1000) {
        last_update = millis();
        lv_label_set_text_fmt(uptime_label, "Uptime: %lus", (unsigned long)(last_update / 1000));
    }
    lv_timer_handler();
    delay(2);
}
