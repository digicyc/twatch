// Shared look for app screens: black screens, gray secondary text, dark cards, orange header buttons.
#pragma once
#include <lvgl.h>

static const lv_color_t GRAY = LV_COLOR_MAKE(0x9E, 0x9E, 0x9E);
static const lv_color_t CARD = LV_COLOR_MAKE(0x1C, 0x1C, 0x1E);

lv_obj_t *make_screen();
lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

// A text-only button in the top left or top right corner of a screen.
lv_obj_t *header_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb);

// A vertically scrolling flex column filling the screen below the header.
lv_obj_t *make_scroll_column(lv_obj_t *screen);

// The compact slide-up keyboard: QWERTY with a one-letter shift, "1#" for numbers and punctuation,
// and "#+=" from there for the remaining symbols. KB_HEIGHT px tall, at the bottom of `parent`.
// Its OK key sends LV_EVENT_READY.
static const int KB_HEIGHT = 236;
lv_obj_t *make_keyboard(lv_obj_t *parent, lv_obj_t *textarea);

// Slides the keyboard up from the bottom edge; `shift` starts it on capitals.
void keyboard_slide_up(lv_obj_t *kb, bool shift);
