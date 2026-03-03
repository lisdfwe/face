#ifndef _FB_DISPLAY_H_
#define _FB_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

int fb_init(void);
void fb_display_fullscreen(void *rgb_data, int src_w, int src_h);
void fb_deinit(void);

#ifdef __cplusplus
}
#endif

#endif