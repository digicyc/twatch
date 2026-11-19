// SD Card app: card status and space, a read/write test, a file browser, and a text viewer.
// LilyGoLib mounts the card at boot (SD library, VFS path /sd); this app remounts it when a card
// is inserted or removed while the app is open.
#pragma once
#include <lvgl.h>

// Builds the screens. `home` is the screen to return to.
void sdcard_init(lv_obj_t *home);

// Shows the SD Card screen.
void sdcard_open();
