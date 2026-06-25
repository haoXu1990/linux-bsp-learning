#include "yuyv_rgb565.h"

static unsigned char clamp_u8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (unsigned char)value;
}

uint16_t yuv_to_rgb565_pixel(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    unsigned char r;
    unsigned char g;
    unsigned char b;

    if (c < 0)
        c = 0;

    r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = clamp_u8((298 * c + 516 * d + 128) >> 8);

    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

uint32_t yuv_to_rgb888_pixel(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    unsigned char r;
    unsigned char g;
    unsigned char b;

    if (c < 0)
        c = 0;

    r = clamp_u8((298 * c + 409 * e + 128) >> 8);
    g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = clamp_u8((298 * c + 516 * d + 128) >> 8);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void yuyv_to_rgb565(const unsigned char *yuyv, uint16_t *rgb565, size_t pixel_count)
{
    size_t i;

    for (i = 0; i + 1 < pixel_count; i += 2) {
        unsigned char y0 = yuyv[i * 2 + 0];
        unsigned char u  = yuyv[i * 2 + 1];
        unsigned char y1 = yuyv[i * 2 + 2];
        unsigned char v  = yuyv[i * 2 + 3];

        rgb565[i] = yuv_to_rgb565_pixel(y0, u, v);
        rgb565[i + 1] = yuv_to_rgb565_pixel(y1, u, v);
    }

    if (i < pixel_count) {
        unsigned char y = yuyv[i * 2 + 0];
        unsigned char u = yuyv[i * 2 + 1];
        unsigned char v = yuyv[i * 2 + 1];

        rgb565[i] = yuv_to_rgb565_pixel(y, u, v);
    }
}
