#ifndef _ISP_PROCESS_H
#define _ISP_PROCESS_H

#include <stdint.h>

/* ISP 配置参数 */
typedef struct {
    int wb_r_gain;  /* 白平衡红色增益 (0-512, 256=1.0x) */
    int wb_g_gain;  /* 白平衡绿色增益 */
    int wb_b_gain;  /* 白平衡蓝色增益 */
    int brightness; /* 亮度偏移 (-128 ~ 128) */
    int contrast;   /* 对比度 (0-256, 128=1.0x) */
} isp_config_t;

/**
 * @brief ISP 图像处理 (YUYV -> RGB32 + 后处理)
 * @param yuyv_in 输入 YUYV 数据指针
 * @param rgb_out 输出 RGB32 数据指针 (ARGB8888)
 * @param width 图像宽度
 * @param height 图像高度
 * @param cfg ISP 配置参数 (NULL 使用默认值)
 */
void isp_process_frame(uint8_t *yuyv_in, uint32_t *rgb_out, 
                       int width, int height, isp_config_t *cfg);

#endif /* _ISP_PROCESS_H */