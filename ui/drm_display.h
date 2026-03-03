#ifndef __DRM_DISPLAY_H__
#define __DRM_DISPLAY_H__

#include <stdint.h>

// 修正：在 .h 文件中添加 line_length 参数
int drm_init(int w, int h, uint32_t **vaddr, int *line_length);

#endif