// Messages app: chat list, chat view, quick replies, and a keyboard on top of the messenger. Incoming
// messages vibrate and wake the screen.
#pragma once
#include <lvgl.h>

// Builds the screens and listens for messenger changes; call after messenger_init().
// `home` is the screen to return to.
void messages_init(lv_obj_t *home);

// Shows the chat list.
void messages_open();
