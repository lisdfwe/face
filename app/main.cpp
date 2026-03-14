#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>

// 硬件模块
#include "../media/v4l2_camera.h"
#include "../ui/fb_display.h"

// AI模块
#include "../ai/face_detect.h"

using namespace std;

/* motor ioctl 命令（必须和驱动一致） */
#define MOTOR_MAGIC 'M'
#define MOTOR_OPEN   _IO(MOTOR_MAGIC, 1)
#define MOTOR_CLOSE  _IO(MOTOR_MAGIC, 2)

/* 摄像头分辨率 */
#define CAM_WIDTH  320
#define CAM_HEIGHT 240

#define MAX_EVENTS 4

/* 系统状态 */
static SystemState sys_state = STATE_IDLE;

/* motor设备 */
static int motor_fd = -1;

/* epoll */
static int epfd = -1;

/**
 * Ctrl+C信号处理
 */
void sig_handler(int sig)
{
    cout << "\nReceive SIGINT, exiting..." << endl;
    sys_state = STATE_EXIT;
}

/**
 * 初始化motor设备
 *
 * 面试考点：
 * 用户空间访问字符设备
 * epoll监听驱动poll
 */
int motor_init()
{
    motor_fd = open("/dev/motor", O_RDWR);
    if (motor_fd < 0)
    {
        perror("open motor");
        return -1;
    }

    /* 创建epoll */
    epfd = epoll_create(1);
    if (epfd < 0)
    {
        perror("epoll_create");
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = motor_fd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, motor_fd, &ev);

    cout << "Motor device init success" << endl;

    return 0;
}

/**
 * 检查驱动事件
 *
 * 面试考点：
 * epoll + poll + waitqueue
 */
void motor_check_event()
{
    struct epoll_event events[MAX_EVENTS];

    int nfds = epoll_wait(epfd, events, MAX_EVENTS, 0);

    if (nfds <= 0)
        return;

    for (int i = 0; i < nfds; i++)
    {
        if (events[i].data.fd == motor_fd)
        {
            cout << "EVENT: motor timer triggered (auto close)" << endl;
        }
    }
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sig_handler);

    cout << "===== LCD Face Detect System =====" << endl;

    /* 初始化LCD+触摸 */
    if (fb_touch_init() < 0)
    {
        cout << "LCD init failed" << endl;
        return -1;
    }

    /* 初始化摄像头 */
    v4l2_ctx_t cam_ctx;
    memset(&cam_ctx, 0, sizeof(cam_ctx));

    cam_ctx.width = CAM_WIDTH;
    cam_ctx.height = CAM_HEIGHT;
    cam_ctx.device = "/dev/video2";

    if (v4l2_init(&cam_ctx) < 0)
    {
        cout << "Camera init failed" << endl;
        return -1;
    }

    cout << "Camera init success" << endl;

    /* 初始化motor */
    if (motor_init() < 0)
    {
        cout << "Motor init failed" << endl;
        return -1;
    }

    /* RGB buffer */
    size_t rgb_buf_size = CAM_WIDTH * CAM_HEIGHT * 4;

    unsigned char *rgb_buffer =
        (unsigned char *)malloc(rgb_buf_size);

    if (!rgb_buffer)
    {
        cout << "malloc buffer failed" << endl;
        return -1;
    }

    /* 初始化AI */
    cout << "Loading AI model..." << endl;

    if (face_init("../ai/model/slim_320.param",
                  "../ai/model/slim_320.bin") != 0)
    {
        cout << "AI init failed" << endl;
        return -1;
    }

    cout << "AI model loaded" << endl;

    cout << "System running..." << endl;

    int frame_id = 0;

    int door_open = 0;   // 防止重复开门

    while (sys_state != STATE_EXIT)
    {
        /* ---------------------- */
        /* 触摸事件 */
        /* ---------------------- */

        int touch_x, touch_y;

        if (read_touch_coords(&touch_x, &touch_y) == 0)
        {
            sys_state =
                check_touch_event(touch_x, touch_y, sys_state);
        }

        /* ---------------------- */
        /* IDLE状态 */
        /* ---------------------- */

        if (sys_state == STATE_IDLE)
        {
            memset(rgb_buffer, 0, rgb_buf_size);

            fb_display_ui(rgb_buffer,
                          CAM_WIDTH,
                          CAM_HEIGHT,
                          STATE_IDLE);

            usleep(50000);

            continue;
        }

        /* ---------------------- */
        /* 运行状态 */
        /* ---------------------- */

        if (sys_state == STATE_RUNNING)
        {
            void *frame_data;
            int frame_index;
            int frame_size;

            if (v4l2_get_frame(&frame_data,
                               &frame_index,
                               &frame_size) < 0)
            {
                usleep(10000);
                continue;
            }

            /* MJPEG -> RGB */
            mjpeg_to_rgb32(
                (unsigned char *)frame_data,
                frame_size,
                rgb_buffer,
                CAM_WIDTH);

            /* 人脸检测 */
            int face_num =
                face_detect(rgb_buffer,
                            CAM_WIDTH,
                            CAM_HEIGHT);

            /* ---------------------- */
            /* 人脸检测触发开门 */
            /* ---------------------- */

            if (face_num > 0)
            {
                cout << "Face detected: "
                     << face_num << endl;

                if (!door_open)
                {
                    cout << "IOCTL MOTOR_OPEN" << endl;

                    /*
                     面试考点：
                     用户态 -> 内核态
                     ioctl控制驱动
                    */
                    ioctl(motor_fd, MOTOR_OPEN);

                    door_open = 1;
                }
            }

            /* ---------------------- */
            /* epoll监听驱动事件 */
            /* ---------------------- */

            motor_check_event();

            /* 显示UI */
            fb_display_ui(rgb_buffer,
                          CAM_WIDTH,
                          CAM_HEIGHT,
                          STATE_RUNNING);

            /* 释放帧 */
            v4l2_put_frame(frame_index);

            frame_id++;

            usleep(33000);
        }
    }

    cout << "System exiting..." << endl;

    /* 资源释放 */

    if (motor_fd > 0)
        close(motor_fd);

    if (epfd > 0)
        close(epfd);

    free(rgb_buffer);

    face_deinit();

    v4l2_deinit();

    fb_touch_deinit();

    cout << "Exit success" << endl;

    return 0;
}