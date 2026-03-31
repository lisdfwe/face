#ifndef _V4L2_CAMERA_H
#define _V4L2_CAMERA_H

#include <stdint.h>

/* 摄像头上下文结构 */
typedef struct {
    const char *device;   /* 设备节点，如 "/dev/video0" */
    int width;            /* 图像宽度 */
    int height;           /* 图像高度 */
    int fps;              /* 目标帧率 */
} v4l2_ctx_t;

/* 摄像头信息结构 */
typedef struct {
    char driver[32];      /* 驱动名称 */
    char card[32];        /* 设备卡片名称 */
    int width;
    int height;
    uint32_t pixelformat; /* 像素格式 (如 V4L2_PIX_FMT_YUYV) */
} v4l2_info_t;

/**
 * @brief 初始化 V4L2 摄像头
 * @param ctx 配置上下文
 * @return 0 成功, -1 失败
 */
int v4l2_init(v4l2_ctx_t *ctx);

/**
 * @brief 获取一帧图像 (阻塞式)
 * @param data 输出数据指针 (指向 RGB32 缓冲区)
 * @param index 输出缓冲区索引 (用于归还)
 * @param size 输出数据大小
 * @return 0 成功, -1 失败
 */
int v4l2_get_frame(void **data, int *index, int *size);

/**
 * @brief 归还缓冲区给内核
 * @param index 缓冲区索引
 * @return 0 成功, -1 失败
 */
int v4l2_put_frame(int index);

/**
 * @brief 释放摄像头资源
 */
void v4l2_deinit(void);

/**
 * @brief 获取摄像头当前信息
 * @param info 输出信息结构
 * @return 0 成功, -1 失败
 */
int v4l2_get_info(v4l2_info_t *info);

#endif /* _V4L2_CAMERA_H */