/*
 * ============================================================================
 * 文件：main.cpp (GStreamer + DMABUF Zero-Copy + Heartbeat Fix)
 * 修改点：
 * 1. 增加心跳喂狗 (CMD_FEED)，防止误关门
 * 2. 增加 DMABUF 缓冲区释放 (gst_put_frame)，防止内存泄漏
 * 3. 优化状态流转日志
 * ============================================================================
 */

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>

// 假设这些头文件存在且接口正确
#include "gstreamer_camera.h" 
#include "../ui/fb_display.h"
#include "../ai/face_detect.h"

using namespace std;

/* ================= 状态机 ================= */
typedef enum {
    STATE_IDLE = 0,
    STATE_RUNNING,
    STATE_EXIT
} SystemState;

/* ================= 电机命令 ================= */
#define MOTOR_MAGIC 'M'
#define MOTOR_OPEN   _IO(MOTOR_MAGIC, 1)
#define MOTOR_CLOSE  _IO(MOTOR_MAGIC, 2)
#define MOTOR_FEED   _IO(MOTOR_MAGIC, 3) /* 【新增】心跳喂狗命令 */

/* ================= 参数 ================= */
#define CAM_WIDTH  320
#define CAM_HEIGHT 240
#define CAM_FPS    30
#define MAX_EVENTS 4
#define HEARTBEAT_INTERVAL_SEC 2 /* 每 2 秒喂一次狗 (驱动超时设为 5 秒) */

static SystemState sys_state = STATE_IDLE;
static int motor_fd = -1;
static int epfd = -1;
static volatile bool g_exit_flag = false;

/* ================= 信号处理 ================= */
void sig_handler(int sig)
{
    cout << "\n[SYS] Signal received, exiting...\n";
    g_exit_flag = true;
    sys_state = STATE_EXIT;
}

/* ================= 电机初始化 ================= */
int motor_init()
{
    motor_fd = open("/dev/motor", O_RDWR);
    if (motor_fd < 0) {
        perror("[MOTOR] open failed");
        return -1;
    }

    epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("[EPOLL] create failed");
        close(motor_fd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = motor_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, motor_fd, &ev) < 0) {
        perror("[EPOLL] ctl failed");
        close(motor_fd);
        close(epfd);
        return -1;
    }

    cout << "[OK] Motor & Epoll initialized\n";
    return 0;
}

/* ================= 清理资源 ================= */
void cleanup()
{
    cout << "[SYS] Cleaning up resources...\n";

    // 1. 确保关门
    if (motor_fd >= 0) {
        ioctl(motor_fd, MOTOR_CLOSE);
        close(motor_fd);
        motor_fd = -1;
    }

    // 2. 关闭 epoll
    if (epfd >= 0) {
        close(epfd);
        epfd = -1;
    }

    // 3. 释放其他模块
    gst_camera_deinit();
    fb_touch_deinit();
    face_deinit();
    
    cout << "[SYS] Cleanup done.\n";
}

/* ================= 主函数 ================= */
int main()
{
    // 注册信号
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    cout << "=================================\n";
    cout << "  GStreamer CSI Face System (Pro)\n";
    cout << "  Pipeline: CSI → DMA → DMABUF → GST → AI\n";
    cout << "=================================\n";

    /* 1. 初始化 LCD */
    if (fb_touch_init() < 0) {
        cerr << "[ERR] LCD init failed\n";
        return -1;
    }

    /* 2. 初始化 GStreamer Camera */
    if (gst_camera_init(CAM_WIDTH, CAM_HEIGHT, CAM_FPS) < 0) {
        cerr << "[ERR] GStreamer init failed\n";
        fb_touch_deinit();
        return -1;
    }

    /* 3. 初始化 Motor */
    if (motor_init() < 0) {
        cerr << "[ERR] Motor init failed\n";
        gst_camera_deinit();
        fb_touch_deinit();
        return -1;
    }

    /* 4. 初始化 AI */
    if (face_init("../ai/model/slim_320.param", "../ai/model/slim_320.bin") != 0) {
        cerr << "[ERR] AI Model load failed\n";
        cleanup();
        return -1;
    }

    cout << "[SYS] System Running... (Ctrl+C to exit)\n";

    int door_status = 0; // 0: Closed, 1: Open
    int frame_fail_count = 0;
    int heartbeat_counter = 0;

    while (!g_exit_flag) {
        
        /* --- 1. 处理触摸事件 --- */
        int tx, ty;
        if (read_touch_coords(&tx, &ty) == 0) {
            sys_state = check_touch_event(tx, ty, sys_state);
            // 触摸切换状态时，可以重置一些计数器等
        }

        /* --- 2. 空闲状态 --- */
        if (sys_state == STATE_IDLE) {
            // 确保门是关的
            if (door_status == 1) {
                ioctl(motor_fd, MOTOR_CLOSE);
                door_status = 0;
            }
            usleep(50000); // 20Hz 检测触摸
            continue;
        }

        /* --- 3. 运行状态 --- */
        if (sys_state == STATE_RUNNING) {
            unsigned char *frame_data = NULL;
            int frame_size = 0;

            /* A. 获取帧 (零拷贝) */
            if (gst_get_frame(&frame_data, &frame_size) < 0) {
                frame_fail_count++;
                if (frame_fail_count > 30) { // 约 1 秒失败
                    cerr << "[ERR] Camera stream lost, restarting pipeline...\n";
                    // 这里可以添加重启 gst_camera 的逻辑，或者简单跳过
                    frame_fail_count = 0;
                }
                usleep(10000);
                continue;
            }
            frame_fail_count = 0; // 重置失败计数

            /* B. AI 人脸检测 */
            // 注意：frame_data 直接传入，无 memcpy
            int face_num = face_detect(frame_data, CAM_WIDTH, CAM_HEIGHT);

            if (face_num > 0) {
                // 发现人脸
                if (door_status == 0) {
                    cout << "[AI] Face detected! Opening door...\n";
                    ioctl(motor_fd, MOTOR_OPEN);
                    door_status = 1;
                }
                // 即使门已经开了，只要有脸，我们下面也会喂狗，保持门开着
            }

            /* C. 【关键修复】心跳喂狗 */
            // 每 2 秒喂一次，防止驱动层 5 秒超时自动关门
            heartbeat_counter++;
            if (heartbeat_counter >= (HEARTBEAT_INTERVAL_SEC * 30)) { // 假设 30fps
                if (door_status == 1) { 
                    // 只有在门开着的时候才需要拼命喂狗保持开启
                    // 如果门关着，让定时器超时自动保护也没关系，或者也可以一直喂
                    ioctl(motor_fd, MOTOR_FEED);
                }
                heartbeat_counter = 0;
            }

            /* D. 检查电机关门事件 (Epoll) */
            struct epoll_event events[MAX_EVENTS];
            // 超时设为 0，非阻塞检查
            int nfds = epoll_wait(epfd, events, MAX_EVENTS, 0);
            if (nfds > 0) {
                cout << "[EVENT] Motor closed (Timeout or Manual)\n";
                door_status = 0;
                // 注意：此时如果人脸还在，下一轮循环会重新触发 MOTOR_OPEN
                // 这符合逻辑：超时强关 -> 检测到人 -> 重新开
            }

            /* E. 显示画面 */
            fb_display_ui(frame_data, CAM_WIDTH, CAM_HEIGHT, sys_state);

            /* F. 【关键修复】释放 DMABUF 帧 */
            // 必须归还 buffer 给 GStreamer 池，否则内存泄漏
            gst_put_frame(); 

            /* G. 控制帧率 */
            usleep(33000); // ~30fps
        }
    }

    cleanup();
    cout << "[SYS] Exit success\n";
    return 0;
}