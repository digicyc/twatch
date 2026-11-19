// Steps app: today's steps from the motion sensor's step counter, progress toward a daily goal, an
// estimated distance, and the last seven days. Totals are saved, so they survive reboots.
#pragma once
#include <lvgl.h>
#include <stdint.h>

void steps_init(lv_obj_t *home_screen);
void steps_open();

// Call every loop: follows the step counter, starts a new day at midnight, and saves the total.
void steps_tick();
