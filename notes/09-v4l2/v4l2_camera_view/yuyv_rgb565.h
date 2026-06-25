#ifndef YUYV_RGB565_H
#define YUYV_RGB565_H

#include <stddef.h>
#include <stdint.h>

uint16_t yuv_to_rgb565_pixel(int y, int u, int v);
uint32_t yuv_to_rgb888_pixel(int y, int u, int v);
void yuyv_to_rgb565(const unsigned char *yuyv, uint16_t *rgb565, size_t pixel_count);

#endif
