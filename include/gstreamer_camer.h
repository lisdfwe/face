#ifndef GSTREAMER_CAMERA_H
#define GSTREAMER_CAMERA_H

#include <gst/gst.h>

// 定义帧上下文，同时持有 Sample 和 MapInfo
typedef struct {
    GstSample *sample;
    GstMapInfo map_info;
    gboolean is_mapped;
} GstFrameContext;

int gst_camera_init(int w, int h, int fps);

// 修改返回值：返回上下文指针，而不是分离的 sample 和 data
int gst_get_frame(GstFrameContext **ctx_out, unsigned char **data, int *size);

// 补全后的释放函数
void gst_release_frame(GstFrameContext *ctx);

void gst_camera_deinit(void);

#endif