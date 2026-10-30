// Settings app: screen brightness and timeout, and message notifications, saved across reboots.
#pragma once
#include <lvgl.h>
#include <stdint.h>

// Loads the saved settings and builds the screen. `home` is the screen to return to.
void settings_init(lv_obj_t *home);

// Shows the Settings screen.
void settings_open();

// The display brightness to use while the screen is on, 1-255.
uint8_t settings_brightness();

// How long the screen stays on without input.
uint32_t settings_screen_timeout_ms();

// Whether new messages vibrate the watch.
bool settings_message_vibrate();
