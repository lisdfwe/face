#ifndef __V4L2_CAMERA_H__
#define __V4L2_CAMERA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 定义结构体 */
typedef struct {
    int width;
    int height;
    const char *device;
} v4l2_ctx_t;

/* 导出函数原型 */
int v4l2_init(v4l2_ctx_t *ctx);

void mjpeg_to_rgb32(unsigned char *jpeg_data, int jpeg_size,
                    unsigned char *rgb_buffer, int img_width);

int v4l2_get_frame(void **data, int *index, int *size);

int v4l2_put_frame(int index);

void v4l2_yuyv_to_rgb32(int w, int h, int line_length,
                        unsigned char *yuyv, uint32_t *rgb);

void v4l2_deinit(void);

#ifdef __cplusplus
}
#endif

#endif