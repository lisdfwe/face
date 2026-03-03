#ifndef FACE_DETECTOR_H
#define FACE_DETECTOR_H
#include <ncnn/net.h>
int face_init(const char* param, const char* bin);
int face_detect(unsigned char* rgba, int lcd_w, int lcd_h);
void face_deinit(void);

#endif