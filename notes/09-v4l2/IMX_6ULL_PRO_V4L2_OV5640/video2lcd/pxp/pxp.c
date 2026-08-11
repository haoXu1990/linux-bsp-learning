#include <pxp.h>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PXP_BUFFER_COUNT 2

static int g_iPxpFd = -1;
static unsigned int g_iPxpBufferCount;
static unsigned int g_iNextBuffer;
static int g_bStreaming;

static int SetInputFormat(int iWidth, int iHeight, int iPixelFormat)
{
    // 设置 APP 送给 PXP 的源数据格式，也就是 pxp 驱动中  s0_param；
    //  V4L2_BUF_TYPE_VIDEO_OUTPUT 对应的是 APP 把图像输出到 PXP 设备
    struct v4l2_format tFmt;

    memset(&tFmt, 0, sizeof(tFmt));
    tFmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    tFmt.fmt.pix.width = iWidth;
    tFmt.fmt.pix.height = iHeight;
    tFmt.fmt.pix.pixelformat = iPixelFormat;
    tFmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(g_iPxpFd, VIDIOC_S_FMT, &tFmt) < 0) {
        perror("PXP: VIDIOC_S_FMT(VIDEO_OUTPUT)");
        return -1;
    }
    return 0;
}

static int SetDisplayWindow(int iSrcWidth, int iSrcHeight,
                            int iLeft, int iTop,
                            int iDstWidth, int iDstHeight)
{
    struct v4l2_format tOverlay;
    struct v4l2_crop tCrop;

    memset(&tOverlay, 0, sizeof(tOverlay));
    tOverlay.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
    tOverlay.fmt.win.w.width = iSrcWidth;
    tOverlay.fmt.win.w.height = iSrcHeight;
    tOverlay.fmt.win.field = V4L2_FIELD_NONE;
    tOverlay.fmt.win.global_alpha = 0xff;
    if (ioctl(g_iPxpFd, VIDIOC_S_FMT, &tOverlay) < 0) {
        perror("PXP: VIDIOC_S_FMT(VIDEO_OUTPUT_OVERLAY)");
        return -1;
    }

    memset(&tCrop, 0, sizeof(tCrop));
    tCrop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
    tCrop.c.left = iLeft;
    tCrop.c.top = iTop;
    tCrop.c.width = iDstWidth;
    tCrop.c.height = iDstHeight;
    if (ioctl(g_iPxpFd, VIDIOC_S_CROP, &tCrop) < 0) {
        perror("PXP: VIDIOC_S_CROP");
        return -1;
    }
    return 0;
}

int PxpInit(const char *pcDevName,
            int iSrcWidth, int iSrcHeight, int iSrcFormat,
            int iDstLeft, int iDstTop,
            int iDstWidth, int iDstHeight)
{
    struct v4l2_capability tCap;
    struct v4l2_requestbuffers tReq;
    unsigned int iOutput = 0;

    g_iPxpFd = open(pcDevName, O_RDWR);
    if (g_iPxpFd < 0) {
        perror("PXP: open");
        return -1;
    }

    memset(&tCap, 0, sizeof(tCap));
    if (ioctl(g_iPxpFd, VIDIOC_QUERYCAP, &tCap) < 0) {
        perror("PXP: VIDIOC_QUERYCAP");
        goto err;
    }
    if (!(tCap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        fprintf(stderr, "PXP: device is not VIDEO_OUTPUT%c", 10);
        goto err;
    }

    // 设置 PXP 输出
    //  这个感觉有个问题，按照常理来看应该在这里配置输出的 数据格式，但是驱动直接封装了，有点奇怪
    if (ioctl(g_iPxpFd, VIDIOC_S_OUTPUT, &iOutput) < 0) {
        perror("PXP: VIDIOC_S_OUTPUT");
        goto err;
    }

    // 配置输入
    if (SetInputFormat(iSrcWidth, iSrcHeight, iSrcFormat) < 0)
        goto err;
    if (SetDisplayWindow(iSrcWidth, iSrcHeight,
                         iDstLeft, iDstTop,
                         iDstWidth, iDstHeight) < 0)
        goto err;

    memset(&tReq, 0, sizeof(tReq));
    tReq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    tReq.memory = V4L2_MEMORY_USERPTR;
    tReq.count = PXP_BUFFER_COUNT;
    if (ioctl(g_iPxpFd, VIDIOC_REQBUFS, &tReq) < 0) {
        perror("PXP: VIDIOC_REQBUFS");
        goto err;
    }
    if (!tReq.count) {
        fprintf(stderr, "PXP: VIDIOC_REQBUFS returned zero buffers%c", 10);
        goto err;
    }

    g_iPxpBufferCount = tReq.count;
    g_iNextBuffer = 0;
    g_bStreaming = 0;
    printf("PXP: mxc VIDEO_OUTPUT path, %dx%d -> (%d,%d) %dx%d%c",
           iSrcWidth, iSrcHeight, iDstLeft, iDstTop,
           iDstWidth, iDstHeight, 10);
    return 0;
// 错误后一定要清楚资源
err:
    PxpExit();
    return -1;
}

int PxpDisplayFrame(const T_VideoBuf *ptVideoBuf)
{
    struct v4l2_buffer tBuf;
    enum v4l2_buf_type eType;

    if (g_iPxpFd < 0 || !ptVideoBuf)
        return -1;

    memset(&tBuf, 0, sizeof(tBuf));
    tBuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    tBuf.memory = V4L2_MEMORY_USERPTR;
    tBuf.index = g_iNextBuffer;
    tBuf.m.userptr =
        (unsigned long)ptVideoBuf->tPixelDatas.aucPixelDatas;
    tBuf.length = ptVideoBuf->tPixelDatas.iTotalBytes;
    tBuf.bytesused = ptVideoBuf->tPixelDatas.iTotalBytes;
    if (ioctl(g_iPxpFd, VIDIOC_QBUF, &tBuf) < 0) {
        perror("PXP: QBUF");
        return -1;
    }

    if (!g_bStreaming) {
        eType = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (ioctl(g_iPxpFd, VIDIOC_STREAMON, &eType) < 0) {
            perror("PXP: STREAMON");
            return -1;
        }
        g_bStreaming = 1;
    }

    memset(&tBuf, 0, sizeof(tBuf));
    tBuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    tBuf.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(g_iPxpFd, VIDIOC_DQBUF, &tBuf) < 0) {
        perror("PXP: DQBUF");
        return -1;
    }
    g_iNextBuffer = (tBuf.index + 1) % g_iPxpBufferCount;
    return 0;
}

void PxpExit(void)
{
    enum v4l2_buf_type eType;

    if (g_iPxpFd < 0)
        return;
    if (g_bStreaming) {
        eType = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(g_iPxpFd, VIDIOC_STREAMOFF, &eType);
    }
    close(g_iPxpFd);
    g_iPxpFd = -1;
    g_iPxpBufferCount = 0;
    g_bStreaming = 0;
}
