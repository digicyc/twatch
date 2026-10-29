#include "ui.h"

lv_obj_t *make_screen()
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    return screen;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

lv_obj_t *header_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb)
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

lv_obj_t *make_scroll_column(lv_obj_t *screen)
{
    lv_obj_t *cont = lv_obj_create(screen);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), 502 - 104);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(cont, 24, 0);
    lv_obj_set_style_pad_bottom(cont, 24, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    return cont;
}
