#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

// 硬件头文件
#include "../media/v4l2_camera.h"
#include "../ui/fb_display.h"
// AI检测头文件
#include "../ai/face_detect.h"

// 摄像头分辨率（和模型匹配）
#define CAM_WIDTH    320
#define CAM_HEIGHT   240

// 全局系统状态
static SystemState sys_state = STATE_IDLE;

/**
 * @brief 信号处理函数（捕获Ctrl+C）
 * @param sig 信号值
 */
void sig_handler(int sig)
{
    printf("\nReceive SIGINT (Ctrl+C), exiting...\n");
    sys_state = STATE_EXIT;
}

int main(int argc, char *argv[])
{
    // 1. 注册退出信号
    signal(SIGINT, sig_handler);
    printf("===== 7寸LCD人脸检测系统 =====\n");

    // 2. 初始化LCD和触摸
    if (fb_touch_init() < 0) {
        printf("ERROR: LCD/Touch init failed!\n");
        return -1;
    }

    // 3. 初始化摄像头
    v4l2_ctx_t cam_ctx;
    memset(&cam_ctx, 0, sizeof(cam_ctx));
    cam_ctx.width = CAM_WIDTH;
    cam_ctx.height = CAM_HEIGHT;
    cam_ctx.device = "/dev/video2"; // 根据实际摄像头设备调整

    if (v4l2_init(&cam_ctx) < 0) {
        printf("ERROR: Camera init failed!\n");
        fb_touch_deinit();
        return -1;
    }
    printf("Camera init success: %dx%d MJPEG\n", CAM_WIDTH, CAM_HEIGHT);

    // 4. 分配RGB缓冲区（320x240x4）
    size_t rgb_buf_size = CAM_WIDTH * CAM_HEIGHT * 4;
    unsigned char *rgb_buffer = (unsigned char *)malloc(rgb_buf_size);
    if (!rgb_buffer) {
        printf("ERROR: Malloc RGB buffer failed!\n");
        v4l2_deinit();
        fb_touch_deinit();
        return -1;
    }

    // 5. 初始化人脸检测模型
    printf("NCNN: Loading model...\n");
    if (face_init("../ai/model/slim_320.param", "../ai/model/slim_320.bin") != 0) {
        printf("ERROR: AI model init failed!\n");
        free(rgb_buffer);
        v4l2_deinit();
        fb_touch_deinit();
        return -1;
    }
    printf("NCNN: Model loaded successfully\n");

    // 6. 主循环
    printf("System running... (Touch Start/Exit or Ctrl+C to exit)\n");
    int frame_id = 0;
    int detect_count = 0;

    while (sys_state != STATE_EXIT) {
        // 6.1 检测触摸事件（非阻塞，无事件则跳过）
        int touch_x, touch_y;
        if (read_touch_coords(&touch_x, &touch_y) == 0) {
            sys_state = check_touch_event(touch_x, touch_y, sys_state);
        }

        // 6.2 空闲状态：绘制完整UI，增加睡眠时长减少刷新频率
        if (sys_state == STATE_IDLE) {
            // 填充黑色背景（避免缓冲区脏数据）
            memset(rgb_buffer, 0, rgb_buf_size);
            // 绘制完整UI并显示
            fb_display_ui(rgb_buffer, CAM_WIDTH, CAM_HEIGHT, STATE_IDLE);
            // 增加睡眠时长到50ms，降低CPU占用+减少闪烁
            usleep(50000);
            continue;
        }

        // 6.3 运行状态：采集→解码→检测→显示
        if (sys_state == STATE_RUNNING) {
            void *frame_data;   // 摄像头MJPEG数据
            int frame_index;    // 帧缓冲区索引
            int frame_size;     // 帧数据长度

            // 获取摄像头帧数据
            if (v4l2_get_frame(&frame_data, &frame_index, &frame_size) < 0) {
                printf("WARN: Get camera frame failed (frame: %d)\n", frame_id);
                usleep(10000);
                continue;
            }

            // MJPEG解码为RGB32
            mjpeg_to_rgb32((unsigned char *)frame_data, frame_size, rgb_buffer, CAM_WIDTH);

            // 人脸检测（画框到rgb_buffer）
            int face_num = face_detect(rgb_buffer, CAM_WIDTH, CAM_HEIGHT);
            if (face_num > 0) {
                detect_count += face_num;
                if (frame_id % 30 == 0) {
                    printf("STAT: Frame %d, Total faces: %d\n", frame_id, detect_count);
                }
            }

            // 显示带UI的画面
            fb_display_ui(rgb_buffer, CAM_WIDTH, CAM_HEIGHT, STATE_RUNNING);

            // 释放摄像头帧缓冲区
            v4l2_put_frame(frame_index);

            frame_id++;
            // 运行状态适当睡眠，控制帧率
            usleep(33000); // ~30fps
        }
    }

    // 7. 释放资源
    printf("\nExiting, release resources...\n");
    free(rgb_buffer);         // 释放RGB缓冲区
    face_deinit();            // 释放AI模型
    v4l2_deinit();            // 释放摄像头
    fb_touch_deinit();        // 释放LCD/触摸

    printf("System exit success! Total frames: %d, Total faces: %d\n", frame_id, detect_count);
    return 0;
}