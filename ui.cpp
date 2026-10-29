#include "ui.h"
#include <string.h>

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

// ---------- Keyboard ----------

// Widths are in units of 20 per row; letter keys are 2. " " entries with KB_GAP are hidden spacers
// that indent the middle row by half a key.
#define KB_CTRL(x) ((lv_buttonmatrix_ctrl_t)(x))  // lv_buttonmatrix_ctrl_t is an enum; C++ won't OR it implicitly
static const lv_buttonmatrix_ctrl_t K = KB_CTRL(2);
static const lv_buttonmatrix_ctrl_t KB_GAP = KB_CTRL(LV_BUTTONMATRIX_CTRL_HIDDEN | 1);
static const lv_buttonmatrix_ctrl_t KB_FN = KB_CTRL(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3);  // mode keys and send
static const lv_buttonmatrix_ctrl_t KB_BKSP = KB_CTRL(LV_BUTTONMATRIX_CTRL_CHECKED | 3);
static const lv_buttonmatrix_ctrl_t KB_SPACE = KB_CTRL(10);
static const char *KEY_SYMBOLS = "#+=", *KEY_NUMBERS = "123";

static const char *const KB_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "1#", ",", " ", ".", LV_SYMBOL_OK, ""};
static const char *const KB_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "1#", ",", " ", ".", LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t KB_TEXT_CTRL[] = {
    K, K, K, K, K, K, K, K, K, K,
    KB_GAP, K, K, K, K, K, K, K, K, K, KB_GAP,
    KB_FN, K, K, K, K, K, K, K, KB_BKSP,
    KB_FN, K, KB_SPACE, K, KB_FN};

static const char *const KB_SPECIAL[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "@", "#", "$", "&", "-", "+", "(", ")", "/", "?", "\n",
    KEY_SYMBOLS, "\"", ":", ";", "!", "*", "=", "'", LV_SYMBOL_BACKSPACE, "\n",
    "abc", ",", " ", ".", LV_SYMBOL_OK, ""};
// The printable ASCII the other pages lack, plus a few repeats to fill the rows.
static const char *const KB_SYMBOLS[] = {
    "[", "]", "{", "}", "<", ">", "^", "~", "`", "|", "\n",
    "_", "\\", "-", "+", "=", "*", "/", "%", "&", "#", "\n",
    KEY_NUMBERS, "%", "'", "\"", ":", ";", "!", "?", LV_SYMBOL_BACKSPACE, "\n",
    "abc", ",", " ", ".", LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t KB_SYMBOL_CTRL[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K,
    KB_FN, K, K, K, K, K, K, K, KB_BKSP,
    KB_FN, K, KB_SPACE, K, KB_FN};

// Replaces LVGL's key handler to add a shift key (LV_SYMBOL_UP) that applies to one letter, like a
// phone keyboard, and the "#+=" / "123" page keys. Everything else is LVGL's default handling.
static void keyboard_key_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target_obj(e);
    const char *key = lv_buttonmatrix_get_button_text(kb, lv_buttonmatrix_get_selected_button(kb));
    bool upper = lv_keyboard_get_mode(kb) == LV_KEYBOARD_MODE_TEXT_UPPER;
    if (key && strcmp(key, LV_SYMBOL_UP) == 0) {
        lv_keyboard_set_mode(kb, upper ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
        return;
    }
    if (key && strcmp(key, KEY_SYMBOLS) == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
        return;
    }
    if (key && strcmp(key, KEY_NUMBERS) == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_SPECIAL);
        return;
    }
    lv_keyboard_def_event_cb(e);
    if (upper && key && key[0] >= 'A' && key[0] <= 'Z' && !key[1]) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
}

lv_obj_t *make_keyboard(lv_obj_t *parent, lv_obj_t *textarea)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, LV_PCT(100), KB_HEIGHT);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, KB_LOWER, KB_TEXT_CTRL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, KB_UPPER, KB_TEXT_CTRL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, KB_SPECIAL, KB_SYMBOL_CTRL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, KB_SYMBOLS, KB_SYMBOL_CTRL);
    lv_keyboard_set_textarea(kb, textarea);
    lv_obj_set_style_bg_color(kb, lv_color_black(), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_hor(kb, 6, 0);
    lv_obj_set_style_pad_top(kb, 6, 0);
    lv_obj_set_style_pad_bottom(kb, 14, 0);  // clear of the screen's rounded corners
    lv_obj_set_style_pad_gap(kb, 5, 0);
    lv_obj_set_style_bg_color(kb, CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x3A3A3C), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, keyboard_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return kb;
}

void keyboard_slide_up(lv_obj_t *kb, bool shift)
{
    lv_keyboard_set_mode(kb, shift ? LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, kb);
    lv_anim_set_values(&a, KB_HEIGHT, 0);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t y) { lv_obj_set_style_translate_y((lv_obj_t *)obj, y, 0); });
    lv_anim_start(&a);
}
