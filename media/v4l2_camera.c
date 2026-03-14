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
#include <jpeglib.h>

#define BUFFER_COUNT 6

typedef struct {
    void *start;
    size_t length;
} buffer_t;

static int fd = -1;
static buffer_t *buffers = NULL;
static int n_buffers = 0;

static unsigned char *rgb_buffer = NULL;
static int img_width;
static int img_height;

/* ================= MJPEG → RGB32 ================= */

void mjpeg_to_rgb32(unsigned char *jpeg_data, int jpeg_size,
                           unsigned char *rgb_buffer, int img_width)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    int row_stride = cinfo.output_width * 3; // RGB888

    unsigned char *line_buffer = malloc(row_stride);
    if (!line_buffer) {
        printf("malloc failed\n");
        return;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *rowptr[1];
        rowptr[0] = line_buffer;

        jpeg_read_scanlines(&cinfo, rowptr, 1);
        int y = cinfo.output_scanline - 1;

        for (int x = 0; x < cinfo.output_width; x++) {
            unsigned char r = line_buffer[x * 3];
            unsigned char g = line_buffer[x * 3 + 1];
            unsigned char b = line_buffer[x * 3 + 2];

            ((uint32_t *)rgb_buffer)[y * img_width + x] =
                (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }

    free(line_buffer);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
}

/* ================= 初始化 ================= */

int v4l2_init(v4l2_ctx_t *ctx)
{
    fd = open(ctx->device, O_RDWR);
    if (fd < 0) {
        perror("open video");
        return -1;
    }
    struct v4l2_streamparm parm;
memset(&parm, 0, sizeof(parm));
parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

parm.parm.capture.timeperframe.numerator = 1;
parm.parm.capture.timeperframe.denominator = 30;

if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
    perror("VIDIOC_S_PARM");
}

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = ctx->width;
    fmt.fmt.pix.height = ctx->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    img_width = fmt.fmt.pix.width;
    img_height = fmt.fmt.pix.height;

    /* 申请RGB缓冲区（只申请一次） */
    rgb_buffer = malloc(img_width * img_height * 4);
    if (!rgb_buffer)
        return -1;

    /* 请求buffer */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));

    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    buffers = calloc(req.count, sizeof(buffer_t));
    n_buffers = req.count;

    for (int i = 0; i < n_buffers; i++) {

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));

        buf.type = req.type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);

        if (buffers[i].start == MAP_FAILED)
            return -1;

        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = req.type;
    ioctl(fd, VIDIOC_STREAMON, &type);

    printf("Camera init success: %dx%d MJPEG\n",
           img_width, img_height);

    return 0;
}


int v4l2_get_frame(void **data, int *index,int *size)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return -1;
        perror("VIDIOC_DQBUF");
        return -1;
    }

    *index = buf.index;
    *data  = buffers[buf.index].start;
    *size  = buf.bytesused;   // 关键：真实 MJPEG 数据大小

    return 0;
}

/* ================= 归还帧 ================= */

int v4l2_put_frame(int index)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;

    return ioctl(fd, VIDIOC_QBUF, &buf);
}

/* ================= 释放 ================= */

void v4l2_deinit(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < n_buffers; i++)
        munmap(buffers[i].start, buffers[i].length);

    free(buffers);
    free(rgb_buffer);

    close(fd);
}