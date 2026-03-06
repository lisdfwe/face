#ifndef _FB_DISPLAY_H_
#define _FB_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {

    STATE_IDLE = 0,
    STATE_RUNNING,
    STATE_EXIT

} SystemState;

int fb_touch_init();

void fb_touch_deinit();

int read_touch_coords(int *x, int *y);

SystemState check_touch_event(int x, int y, SystemState state);

void fb_display_ui(uint8_t *rgb,
                   int w,
                   int h,
                   SystemState state);
void fb_clear_screen(uint32_t color);

#ifdef __cplusplus
}
#endif

#endif