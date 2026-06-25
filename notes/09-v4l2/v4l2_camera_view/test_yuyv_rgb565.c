#include <stdint.h>
#include <stdio.h>

#include "yuyv_rgb565.h"

static int expect_equal_u16(const char *name, uint16_t actual, uint16_t expected)
{
    if (actual != expected) {
        printf("%s failed: actual=0x%04x expected=0x%04x\n", name, actual, expected);
        return -1;
    }

    return 0;
}

int main(void)
{
    unsigned char black_yuyv[] = {16, 128, 16, 128};
    unsigned char white_yuyv[] = {235, 128, 235, 128};
    uint16_t out[2] = {0};

    yuyv_to_rgb565(black_yuyv, out, 2);
    if (expect_equal_u16("black pixel 0", out[0], 0x0000) != 0)
        return -1;
    if (expect_equal_u16("black pixel 1", out[1], 0x0000) != 0)
        return -1;

    yuyv_to_rgb565(white_yuyv, out, 2);
    if (expect_equal_u16("white pixel 0", out[0], 0xffff) != 0)
        return -1;
    if (expect_equal_u16("white pixel 1", out[1], 0xffff) != 0)
        return -1;

    if (yuv_to_rgb888_pixel(16, 128, 128) != 0x000000)
        return -1;

    if (yuv_to_rgb888_pixel(235, 128, 128) != 0xffffff)
        return -1;

    printf("yuyv_rgb565 selftest passed\n");
    return 0;
}
