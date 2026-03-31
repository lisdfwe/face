/*
 * ============================================================================
 * 文件：media/v4l2_camera.c
 * 描述：OV5640 CSI摄像头V4L2接口 (YUYV格式)
 * 面试考点：V4L2 ioctl流程、mmap内存映射、YUYV→RGB32转换
 * ============================================================================
 */

#include "v4l2_camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define BUFFER_COUNT 4

typedef struct {
    void *start;
    size_t length;
} buffer_t;

static int fd = -1;
static buffer_t *buffers = NULL;
static int n_buffers = 0;
static unsigned char *rgb_buffer = NULL;
static int img_width, img_height;

/* YUYV到RGB32转换 (BT.601标准) */
void v4l2_yuyv_to_rgb32(int w, int h, int line_length,
                        unsigned char *yuyv, uint32_t *rgb)
{
    int i, j;
    unsigned char *yuv_ptr = yuyv;
    uint32_t *rgb_ptr = rgb;
    
    for (i = 0; i < h; i++) {
        for (j = 0; j < w; j += 2) {
            uint8_t y0 = yuv_ptr[0];
            uint8_t u  = yuv_ptr[1];
            uint8_t y1 = yuv_ptr[2];
            uint8_t v  = yuv_ptr[3];
            
            int u_val = u - 128;
            int v_val = v - 128;
            
            int r0 = y0 + ((1402 * v_val) >> 10);
            int g0 = y0 - ((344 * u_val + 714 * v_val) >> 10);
            int b0 = y0 + ((1772 * u_val) >> 10);
            
            int r1 = y1 + ((1402 * v_val) >> 10);
            int g1 = y1 - ((344 * u_val + 714 * v_val) >> 10);
            int b1 = y1 + ((1772 * u_val) >> 10);
            
            r0 = (r0 < 0) ? 0 : (r0 > 255) ? 255 : r0;
            g0 = (g0 < 0) ? 0 : (g0 > 255) ? 255 : g0;
            b0 = (b0 < 0) ? 0 : (b0 > 255) ? 255 : b0;
            r1 = (r1 < 0) ? 0 : (r1 > 255) ? 255 : r1;
            g1 = (g1 < 0) ? 0 : (g1 > 255) ? 255 : g1;
            b1 = (b1 < 0) ? 0 : (b1 > 255) ? 255 : b1;
            
            rgb_ptr[0] = (0xFF << 24) | (r0 << 16) | (g0 << 8) | b0;
            rgb_ptr[1] = (0xFF << 24) | (r1 << 16) | (g1 << 8) | b1;
            
            yuv_ptr += 4;
            rgb_ptr += 2;
        }
    }
}

/* V4L2初始化 */
int v4l2_init(v4l2_ctx_t *ctx)
{
    fd = open(ctx->device, O_RDWR);
    if (fd < 0) {
        perror("open video device");
        return -1;
    }
    
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return -1;
    }
    printf("Camera: %s (driver: %s)\n", cap.card, cap.driver);
    
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = ctx->fps > 0 ? ctx->fps : 30;
    ioctl(fd, VIDIOC_S_PARM, &parm);
    
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = ctx->width;
    fmt.fmt.pix.height = ctx->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;  /* OV5640 CSI输出YUYV */
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = ctx->width * 2;
    fmt.fmt.pix.sizeimage = ctx->width * ctx->height * 2;
    
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        close(fd);
        return -1;
    }
    
    img_width = fmt.fmt.pix.width;
    img_height = fmt.fmt.pix.height;
    printf("Format: %dx%d YUYV\n", img_width, img_height);
    
    rgb_buffer = malloc(img_width * img_height * 4);
    if (!rgb_buffer) {
        close(fd);
        return -1;
    }
    
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        free(rgb_buffer);
        close(fd);
        return -1;
    }
    
    buffers = calloc(req.count, sizeof(buffer_t));
    n_buffers = req.count;
    
    for (int i = 0; i < n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = req.type;
        buf.memory = req.memory;
        buf.index = i;
        
        ioctl(fd, VIDIOC_QUERYBUF, &buf);
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);
        
        ioctl(fd, VIDIOC_QBUF, &buf);
    }
    
    enum v4l2_buf_type type = req.type;
    ioctl(fd, VIDIOC_STREAMON, &type);
    
    printf("✓ OV5640 CSI Camera init: %dx%d @ %dfps\n",
           img_width, img_height, ctx->fps > 0 ? ctx->fps : 30);
    
    return 0;
}

/* 获取帧 */
int v4l2_get_frame(void **data, int *index, int *size)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return -1;
        perror("VIDIOC_DQBUF");
        return -1;
    }
    
    *index = buf.index;
    *size = buf.bytesused;
    
    v4l2_yuyv_to_rgb32(img_width, img_height, img_width * 2,
                       (unsigned char *)buffers[buf.index].start,
                       (uint32_t *)rgb_buffer);
    
    *data = rgb_buffer;
    return 0;
}

/* 归还帧 */
int v4l2_put_frame(int index)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    return ioctl(fd, VIDIOC_QBUF, &buf);
}

/* 释放资源 */
void v4l2_deinit(void)
{
    if (fd < 0) return;
    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    
    for (int i = 0; i < n_buffers; i++)
        munmap(buffers[i].start, buffers[i].length);
    
    free(buffers);
    free(rgb_buffer);
    close(fd);
    fd = -1;
    
    printf("Camera deinit complete\n");
}

/* 获取摄像头信息 */
int v4l2_get_info(v4l2_info_t *info)
{
    if (fd < 0) return -1;
    
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    
    ioctl(fd, VIDIOC_QUERYCAP, &cap);
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_G_FMT, &fmt);
    
    strncpy(info->driver, cap.driver, sizeof(info->driver));
    strncpy(info->card, cap.card, sizeof(info->card));
    info->width = fmt.fmt.pix.width;
    info->height = fmt.fmt.pix.height;
    info->pixelformat = fmt.fmt.pix.pixelformat;
    
    return 0;
}