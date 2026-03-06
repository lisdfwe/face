#include "face_detect.h"
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdint>

// ------------------- 全局配置 -------------------
static ncnn::Net net;
#define MODEL_INPUT_W 320
#define MODEL_INPUT_H 240

// ------------------- 画框函数（清晰版） -------------------
static void draw_rect(unsigned char* rgba, int w, int h, int x, int y, int bw, int bh, uint32_t color)
{
    if (!rgba || x < 0 || y < 0 || x + bw >= w || y + bh >= h) return;

    // 解析颜色（RGBA）
    uint8_t r = (color >> 24) & 0xFF;
    uint8_t g = (color >> 16) & 0xFF;
    uint8_t b = (color >> 8)  & 0xFF;
    uint8_t a = 255; // 强制不透明

    // 画边框（加粗3像素，易识别）
    for (int i = x; i < x + bw; i++) {
        // 上边框
        int idx = (y * w + i) * 4;
        rgba[idx] = r; rgba[idx+1] = g; rgba[idx+2] = b; rgba[idx+3] = a;
        if (y+1 < h) { idx = ((y+1)*w+i)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }
        if (y+2 < h) { idx = ((y+2)*w+i)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }

        // 下边框
        idx = ((y + bh) * w + i) * 4;
        rgba[idx] = r; rgba[idx+1] = g; rgba[idx+2] = b; rgba[idx+3] = a;
        if (y+bh-1 >= 0) { idx = ((y+bh-1)*w+i)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }
        if (y+bh-2 >= 0) { idx = ((y+bh-2)*w+i)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }
    }
    for (int i = y; i < y + bh; i++) {
        // 左边框
        int idx = (i * w + x) * 4;
        rgba[idx] = r; rgba[idx+1] = g; rgba[idx+2] = b; rgba[idx+3] = a;
        if (x+1 < w) { idx = (i*w+x+1)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }
        if (x+2 < w) { idx = (i*w+x+2)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }

        // 右边框
        idx = (i * w + x + bw) * 4;
        rgba[idx] = r; rgba[idx+1] = g; rgba[idx+2] = b; rgba[idx+3] = a;
        if (x+bw-1 >= 0) { idx = (i*w+x+bw-1)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }
        if (x+bw-2 >= 0) { idx = (i*w+x+bw-2)*4; rgba[idx]=r; rgba[idx+1]=g; rgba[idx+2]=b; }
    }
}

// ------------------- 模型初始化 -------------------
int face_init(const char* param, const char* bin)
{
    printf("NCNN: Loading model...\n");
    net.opt.use_vulkan_compute = false;
    net.opt.use_fp16_packed = false;
    net.opt.use_fp16_storage = false;
    net.opt.use_int8_inference = false;
    net.opt.num_threads = 1;
    net.opt.lightmode = true;

    if (net.load_param(param) != 0 || net.load_model(bin) != 0) {
        printf("NCNN: Load model failed!\n");
        return -1;
    }

    printf("NCNN: Model loaded successfully\n");
    return 0;
}

// ------------------- 人脸检测（得分输出+0.5阈值画绿框版） -------------------
int face_detect(unsigned char* rgba, int lcd_w, int lcd_h)
{
    if (!rgba) {
        printf("Error: RGBA buffer is NULL\n");
        return -1;
    }

    static int frame_count = 0;
    if (frame_count % 30 == 0) {
        printf("DEBUG: face_detect running, frame %d\n", frame_count);
    }
    frame_count++;

    // ------------------- 1. 图像预处理 -------------------
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(
        rgba, ncnn::Mat::PIXEL_RGBA2RGB,
        lcd_w, lcd_h,
        MODEL_INPUT_W, MODEL_INPUT_H
    );
    if (in.empty()) {
        printf("Error: Create input mat failed\n");
        // 兜底画红框（无有效图像时）
        draw_rect(rgba, lcd_w, lcd_h, lcd_w/2-60, lcd_h/2-80, 120, 160, 0xFF0000FF);
        printf("WARNING: No valid input image, draw red test rect (score: N/A)\n");
        return 0;
    }

    // 模型归一化（和训练一致）
    const float mean_vals[3] = {127.5f, 127.5f, 127.5f};
    const float norm_vals[3] = {1.0f/127.5f, 1.0f/127.5f, 1.0f/127.5f};
    in.substract_mean_normalize(mean_vals, norm_vals);
    printf("DEBUG: Input mat: %dx%dx%d\n", in.w, in.h, in.c);

    // ------------------- 2. 推理 -------------------
    ncnn::Extractor ex = net.create_extractor();
    ex.set_light_mode(true);
    ex.input("input", in);

    ncnn::Mat boxes, scores;
    ex.extract("boxes", boxes);
    ex.extract("scores", scores);

    // ------------------- 3. 核心逻辑：得分判断+画框控制 -------------------
    int face_count = 0;
    int best_x1 = 0, best_y1 = 0, best_w = 0, best_h = 0;
    float best_score = 0.0f;
    const float SCORE_THRESHOLD = 0.5f; // 0.5阈值，低于则不画绿框

    // 遍历检测结果，筛选有效框
    if (!boxes.empty() && !scores.empty()) {
        int max_candidates = std::min(500, scores.h);
        for (int i = 0; i < max_candidates; i++) {
            // 读取人脸得分（第0列）
            float current_score = scores.row(i)[0];
            // 输出每一个候选框的得分（调试用）
            // printf("DEBUG: Candidate %d - score=%.4f\n", i, current_score);

            // 仅处理得分>0.5的框
            if (current_score > SCORE_THRESHOLD) {
                const float* bbox = boxes.row(i);
                float x1_norm = bbox[0];
                float y1_norm = bbox[1];
                float x2_norm = bbox[2];
                float y2_norm = bbox[3];

                // 过滤无效坐标
                if (x1_norm < 0 || y1_norm < 0 || x2_norm > 1 || y2_norm > 1) continue;
                if (x2_norm <= x1_norm || y2_norm <= y1_norm) continue;

                // 坐标映射到LCD
                int x1 = (int)(x1_norm * lcd_w);
                int y1 = (int)(y1_norm * lcd_h);
                int x2 = (int)(x2_norm * lcd_w);
                int y2 = (int)(y2_norm * lcd_h);
                int w = x2 - x1;
                int h = y2 - y1;

                // 筛选合理尺寸
                if (w >= 30 && w <= 250 && h >= 40 && h <= 220) {
                    if (current_score > best_score) {
                        best_score = current_score;
                        best_x1 = x1;
                        best_y1 = y1;
                        best_w = w;
                        best_h = h;
                        face_count = 1;
                    }
                }
            }
        }
    }

    // ------------------- 4. 按阈值画框+得分输出 -------------------
    if (face_count > 0 && best_score > SCORE_THRESHOLD) {
        // 裁剪到屏幕内（留5像素边距）
        best_x1 = std::max(5, std::min(best_x1, lcd_w - best_w - 5));
        best_y1 = std::max(5, std::min(best_y1, lcd_h - best_h - 5));
        
        // 画绿框（仅得分>0.5时绘制）
        draw_rect(rgba, lcd_w, lcd_h, best_x1, best_y1, best_w, best_h, 0x00FF0080);
        // 清晰输出得分（保留4位小数，更精准）
        printf("✅ DETECTED FACE - Score=%.4f, Position=(x1:%d,y1:%d,w:%d,h:%d)\n", 
               best_score, best_x1, best_y1, best_w, best_h);
    } else {
        // 得分≤0.5，不画绿框，可选画红框（测试用，也可注释掉不画）
        // draw_rect(rgba, lcd_w, lcd_h, lcd_w/2-60, lcd_h/2-80, 120, 160, 0xFF000080);
        // 输出得分（≤0.5时提示）
        printf(" NO VALID FACE - Max score=%.4f (below threshold %.1f, no green rect)\n", 
               best_score, SCORE_THRESHOLD);
    }

    // 统计输出（每30帧汇总）
    if (frame_count % 30 == 0) {
        printf("[STAT] Frame: %d, Valid faces (score>%.1f): %d\n", 
               frame_count, SCORE_THRESHOLD, face_count);
    }

    return face_count;
}
void face_deinit(void)
{
    net.clear(); // 释放ncnn模型资源
    printf("AI model deinit success\n");
}