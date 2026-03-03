

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

// 硬件相关头文件
#include "../media/v4l2_camera.h"
#include "../ui/fb_display.h"
// AI检测头文件
#include "../ai/face_detect.h"

// 摄像头/LCD尺寸配置（和模型严格一致）
#define CAM_WIDTH   320
#define CAM_HEIGHT  240

// 全局运行标志（控制主循环退出）
static int running = 1;

// 信号处理函数：捕获Ctrl+C，优雅退出
void sig_handler(int sig)
{
    printf("\n[INFO] Receive SIGINT (Ctrl+C), exiting...\n");
    running = 0;
}

int main(int argc, char *argv[])
{
    // 1. 注册退出信号
    signal(SIGINT, sig_handler);
    printf("===== i.MX6 Face Detection System (Final Version) =====\n");

    /* ----------------------------------------
     * 2. 初始化摄像头（MJPEG 320x240）
     * ---------------------------------------- */
    v4l2_ctx_t ctx;
    memset(&ctx, 0, sizeof(v4l2_ctx_t)); // 清空结构体，避免脏数据
    ctx.width  = CAM_WIDTH;
    ctx.height = CAM_HEIGHT;
    ctx.device = "/dev/video1";           // 摄像头设备节点（根据实际调整）

    if (v4l2_init(&ctx) < 0) {
        printf("[ERROR] Camera init failed! Device: %s\n", ctx.device);
        return -1;
    }
    printf("[INFO] Camera init success: %dx%d MJPEG\n", CAM_WIDTH, CAM_HEIGHT);

    /* ----------------------------------------
     * 3. 初始化LCD显示屏
     * ---------------------------------------- */
    if (fb_init() < 0) {
        printf("[ERROR] LCD init failed!\n");
        v4l2_deinit(); // 失败时释放摄像头资源
        return -1;
    }
    printf("[INFO] LCD init success\n");

    /* ----------------------------------------
     * 4. 分配RGB32缓冲区（用于LCD显示，RGBA格式）
     * ---------------------------------------- */
    size_t rgb_buf_size = CAM_WIDTH * CAM_HEIGHT * 4; // 320*240*4=307200字节
    unsigned char *rgb_buffer = (unsigned char*)malloc(rgb_buf_size);
    if (!rgb_buffer) {
        printf("[ERROR] Malloc RGB buffer failed! Size: %zu bytes\n", rgb_buf_size);
        fb_deinit();
        v4l2_deinit();
        return -1;
    }
    printf("[INFO] RGB buffer alloc success: %zu bytes\n", rgb_buf_size);

    /* ----------------------------------------
     * 5. 初始化AI人脸检测模型（核心：调用face_init，统一管理模型）
     * ---------------------------------------- */
    if (face_init("../ai/model/slim_320.param", "../ai/model/slim_320.bin") != 0) {
        printf("[ERROR] AI model init failed! Check param/bin path\n");
        free(rgb_buffer);
        fb_deinit();
        v4l2_deinit();
        return -1;
    }
    printf("[INFO] AI model init success (slim_320)\n");

    /* ----------------------------------------
     * 6. 主循环：实时采集→解码→检测→显示
     * 核心优化：每帧都检测（先保证准，再优化性能）
     * ---------------------------------------- */
    printf("[INFO] System running... (Press Ctrl+C to exit)\n");
    int frame_id = 0;
    int detect_count = 0; // 统计检测到的人脸总数

    while (running) {
        void *frame_data;   // 摄像头原始MJPEG数据
        int frame_index;    // 帧缓冲区索引
        int frame_size;     // 帧数据长度

        // 6.1 获取摄像头一帧数据（失败则跳过，避免卡死）
        if (v4l2_get_frame(&frame_data, &frame_index, &frame_size) < 0) {
            printf("[WARN] Get camera frame failed, skip (frame: %d)\n", frame_id);
            usleep(10000); // 休眠10ms，降低CPU占用
            continue;
        }

        // 6.2 MJPEG解码为RGB32（RGBA），适配LCD显示
        mjpeg_to_rgb32((unsigned char*)frame_data, frame_size, rgb_buffer, CAM_WIDTH);

        // 6.3 实时人脸检测（每帧都检测，解决"框滞后/错位"）
        // 关键：检测结果直接画在rgb_buffer上，立刻显示
        int face_num = face_detect(rgb_buffer, CAM_WIDTH, CAM_HEIGHT);
        if (face_num > 0) {
            detect_count += face_num;
            // 每30帧打印一次统计（避免日志刷屏）
            if (frame_id % 30 == 0) {
                printf("[STAT] Frame: %d, Total faces detected: %d\n", frame_id, detect_count);
            }
        }

        // 6.4 实时显示到LCD（检测后的画面直接输出，无延迟）
        fb_display_fullscreen(rgb_buffer, CAM_WIDTH, CAM_HEIGHT);

        // 6.5 释放摄像头帧缓冲区（必须！否则缓冲区会耗尽）
        v4l2_put_frame(frame_index);

        frame_id++;
    }

    /* ----------------------------------------
     * 7. 资源释放（优雅退出）
     * ---------------------------------------- */
    printf("\n[INFO] Exiting, release all resources...\n");
    free(rgb_buffer);      // 释放RGB缓冲区
    fb_deinit();           // 关闭LCD
    v4l2_deinit();         // 关闭摄像头
    // face_deinit();         // 释放模型资源（如果实现了face_deinit）

    printf("[INFO] System exit success! Total frames: %d, Total faces: %d\n", frame_id, detect_count);
    return 0;
}