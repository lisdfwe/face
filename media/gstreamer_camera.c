#include "gstreamer_camera.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h> // 显式包含 glib

static GstElement *pipeline = NULL;
static GstElement *appsink = NULL;

// 【修复1】使用 GOnce 保证线程安全且仅初始化一次
static GOnce gst_init_once = G_ONCE_INIT;

static gpointer do_gst_init(gpointer data) {
    gst_init(NULL, NULL);
    return NULL;
}

int gst_camera_init(int w, int h, int fps)
{
    // 线程安全的初始化
    g_once(&gst_init_once, do_gst_init, NULL);

    GError *error = NULL; // 【修复2】捕获详细错误
    char pipeline_str[512];

    snprintf(pipeline_str, sizeof(pipeline_str),
        "v4l2src device=/dev/video0 io-mode=dmabuf ! "
        "video/x-raw,format=YUY2,width=%d,height=%d,framerate=%d/1 ! "
        "videoconvert ! "
        "video/x-raw,format=RGBA! "
        "appsink name=sink sync=false max-buffers=1 drop=true",
        w, h, fps);

    pipeline = gst_parse_launch(pipeline_str, &error);
    if (!pipeline) {
        // 【修复2】打印具体错误信息
        printf("[GST] Pipeline create failed: %s\n", error ? error->message : "Unknown error");
        if (error) g_error_free(error);
        return -1;
    }

    appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!appsink) {
        printf("[GST] Failed to get appsink element\n");
        gst_object_unref(pipeline);
        pipeline = NULL;
        return -1;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        printf("[GST] Failed to set pipeline to PLAYING\n");
        gst_object_unref(appsink); // 清理 appsink 引用
        gst_object_unref(pipeline);
        pipeline = NULL;
        appsink = NULL;
        return -1;
    }

    printf("[GST] Camera started (%dx%d)\n", w, h);
    return 0;
}


// 优化核心：零拷贝且安全的获取帧
int gst_get_frame(GstFrameContext **ctx_out, unsigned char **data, int *size)
{
    if (!appsink || !ctx_out || !data || !size) {
        return -1;
    }

    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (!sample) {
        return -1;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return -1;
    }

    // 分配上下文结构体
    GstFrameContext *ctx = (GstFrameContext *)g_malloc0(sizeof(GstFrameContext));
    if (!ctx) {
        gst_sample_unref(sample);
        return -1;
    }

    ctx->sample = sample;
    ctx->is_mapped = FALSE;

    // 映射内存
    if (!gst_buffer_map(buffer, &ctx->map_info, GST_MAP_READ)) {
        g_free(ctx);
        gst_sample_unref(sample);
        return -1;
    }
    
    ctx->is_mapped = TRUE;

    // 输出结果
    *ctx_out = ctx;
    *data = ctx->map_info.data;
    *size = ctx->map_info.size;

    return 0;
}

// 【补全】严谨的释放函数
void gst_release_frame(GstFrameContext *ctx)
{
    if (!ctx) return;

    // 1. 如果已映射，先取消映射 (Unmap)
    // 必须使用当初 map 时的同一个 map_info 结构体
    if (ctx->is_mapped && ctx->sample) {
        GstBuffer *buffer = gst_sample_get_buffer(ctx->sample);
        if (buffer) {
            gst_buffer_unmap(buffer, &ctx->map_info);
        }
        ctx->is_mapped = FALSE;
    }

    // 2. 释放 Sample (这会级联释放 Buffer)
    if (ctx->sample) {
        gst_sample_unref(ctx->sample);
        ctx->sample = NULL;
    }

    // 3. 释放上下文结构体本身
    g_free(ctx);
}


void gst_camera_deinit(void)
{
    if (pipeline) {
        printf("[GST] Stopping pipeline...\n");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        
        // 【修复3】必须手动 unref appsink，否则内存泄漏
        if (appsink) {
            gst_object_unref(appsink);
            appsink = NULL;
        }
        
        gst_object_unref(pipeline);
        pipeline = NULL;
        printf("[GST] Pipeline stopped.\n");
    }
}