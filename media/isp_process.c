#include "isp_process.h"
#include <string.h>

void isp_process_frame(uint8_t *yuyv_in, uint32_t *rgb_out, 
                       int width, int height, isp_config_t *cfg)
{
    // 默认配置
    int wb_r = 256, wb_g = 256, wb_b = 256;
    if (cfg) {
        wb_r = cfg->wb_r_gain;
        wb_b = cfg->wb_b_gain;
    }

    for (int i = 0; i < height; i++) {
        uint8_t *ptr = yuyv_in + i * width * 2;
        for (int j = 0; j < width; j += 2) {
            int y0 = ptr[0];
            int u  = ptr[1] - 128;
            int y1 = ptr[2];
            int v  = ptr[3] - 128;
            
            // BT.601
            int r0 = (298 * y0 + 409 * v + 128) >> 8;
            int g0 = (298 * y0 - 100 * u - 208 * v + 128) >> 8;
            int b0 = (298 * y0 + 516 * u + 128) >> 8;
            
            int r1 = (298 * y1 + 409 * v + 128) >> 8;
            int g1 = (298 * y1 - 100 * u - 208 * v + 128) >> 8;
            int b1 = (298 * y1 + 516 * u + 128) >> 8;
            
            // Apply WB
            r0 = (r0 * wb_r) >> 8; b0 = (b0 * wb_b) >> 8;
            r1 = (r1 * wb_r) >> 8; b1 = (b1 * wb_b) >> 8;
            
            #define CLAMP(x) ((x)<0?0:(x)>255?255:(x))
            
            rgb_out[0] = (0xFF << 24) | (CLAMP(r0)<<16) | (CLAMP(g0)<<8) | CLAMP(b0);
            rgb_out[1] = (0xFF << 24) | (CLAMP(r1)<<16) | (CLAMP(g1)<<8) | CLAMP(b1);
            
            ptr += 4;
            rgb_out += 2;
        }
    }
}