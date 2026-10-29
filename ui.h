// Shared look for app screens: black screens, gray secondary text, dark cards, orange header buttons.
#pragma once
#include <lvgl.h>

static const lv_color_t GRAY = LV_COLOR_MAKE(0x9E, 0x9E, 0x9E);
static const lv_color_t CARD = LV_COLOR_MAKE(0x1C, 0x1C, 0x1E);

lv_obj_t *make_screen();
lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

// A text-only button in the top left or top right corner of a screen.
lv_obj_t *header_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb);
