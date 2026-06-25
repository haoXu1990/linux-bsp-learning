#ifndef IMX_FB_ALPHA_H
#define IMX_FB_ALPHA_H

#include <sys/ioctl.h>

/*
 * i.MX framebuffer global alpha 控制。
 *
 * 很多 i.MX BSP 会在 linux/mxcfb.h 中定义相同结构和 ioctl。
 * 当前学习工程只需要 global alpha，因此在这里保留最小定义，
 * 避免用户态环境缺少 mxcfb.h 时无法编译。
 */
struct mxcfb_gbl_alpha {
    int enable;
    int alpha;
};

#ifndef MXCFB_SET_GBL_ALPHA
#define MXCFB_SET_GBL_ALPHA _IOW('F', 0x21, struct mxcfb_gbl_alpha)
#endif

#endif
