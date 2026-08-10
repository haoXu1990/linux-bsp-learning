#include <config.h>
#include <pic_operation.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <poll.h>
#include <errno.h>
#include <linux/videodev2.h>

#define PXP_DEVICE    "/dev/video0"
#define PXP_NB_BUF    2

static int g_iPxpFd = -1;
static int g_iWidth, g_iHeight;

struct pxp_buf {
    unsigned char *pucAddr;
    int iLength;
    int iIndex;
};
static struct pxp_buf g_atOutBufs[PXP_NB_BUF];
static struct pxp_buf g_atCapBufs[PXP_NB_BUF];
static int g_iOutIdx, g_iCapIdx;

static int PxpQBuf(int iFd, int iType, int iIndex, int iBytesUsed)
{
    struct v4l2_buffer tBuf;
    memset(&tBuf, 0, sizeof(tBuf));
    tBuf.type   = iType;
    tBuf.memory = V4L2_MEMORY_MMAP;
    tBuf.index  = iIndex;
    if (iType == V4L2_BUF_TYPE_VIDEO_OUTPUT)
        tBuf.bytesused = iBytesUsed;
    return ioctl(iFd, VIDIOC_QBUF, &tBuf);
}

static int PxpDQBuf(int iFd, int iType)
{
    struct v4l2_buffer tBuf;
    memset(&tBuf, 0, sizeof(tBuf));
    tBuf.type   = iType;
    tBuf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(iFd, VIDIOC_DQBUF, &tBuf) < 0)
        return -1;
    return tBuf.index;
}

int PxpInit(int iWidth, int iHeight)
{
    struct v4l2_format tFmt;
    struct v4l2_requestbuffers tReq;
    struct v4l2_buffer tBuf;
    int i, ret;

    g_iWidth  = iWidth;
    g_iHeight = iHeight;

    g_iPxpFd = open(PXP_DEVICE, O_RDWR);
    if (g_iPxpFd < 0) {
        printf("PXP: cannot open %s\n", PXP_DEVICE);
        return -1;
    }

    /* Set OUTPUT format: YUYV */
    memset(&tFmt, 0, sizeof(tFmt));
    tFmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    tFmt.fmt.pix.width       = iWidth;
    tFmt.fmt.pix.height      = iHeight;
    tFmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    tFmt.fmt.pix.field       = V4L2_FIELD_NONE;
    ret = ioctl(g_iPxpFd, VIDIOC_S_FMT, &tFmt);
    if (ret < 0) {
        printf("PXP: S_FMT OUTPUT YUYV failed\n");
        goto err;
    }
    printf("PXP: OUTPUT fmt set to YUYV %dx%d\n", iWidth, iHeight);

    /* REQBUFS + mmap for OUTPUT (must be before CAPTURE S_FMT for M2M) */
    memset(&tReq, 0, sizeof(tReq));
    tReq.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    tReq.memory = V4L2_MEMORY_MMAP;
    tReq.count  = PXP_NB_BUF;
    ret = ioctl(g_iPxpFd, VIDIOC_REQBUFS, &tReq);
    if (ret < 0) {
        printf("PXP: OUTPUT REQBUFS failed\n");
        goto err;
    }
    for (i = 0; i < tReq.count; i++) {
        memset(&tBuf, 0, sizeof(tBuf));
        tBuf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        tBuf.memory = V4L2_MEMORY_MMAP;
        tBuf.index  = i;
        ioctl(g_iPxpFd, VIDIOC_QUERYBUF, &tBuf);
        g_atOutBufs[i].pucAddr = mmap(NULL, tBuf.length,
                      PROT_READ | PROT_WRITE, MAP_SHARED,
                      g_iPxpFd, tBuf.m.offset);
        g_atOutBufs[i].iLength = tBuf.length;
        g_atOutBufs[i].iIndex  = i;
    }

    /* Set CAPTURE format: RGB565 (after OUTPUT REQBUFS for proper negotiation) */
    memset(&tFmt, 0, sizeof(tFmt));
    tFmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    tFmt.fmt.pix.width       = iWidth;
    tFmt.fmt.pix.height      = iHeight;
    tFmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    tFmt.fmt.pix.field       = V4L2_FIELD_NONE;
    ret = ioctl(g_iPxpFd, VIDIOC_S_FMT, &tFmt);
    if (ret < 0) {
        printf("PXP: CAPTURE S_FMT failed, trying G_FMT\n");
        memset(&tFmt, 0, sizeof(tFmt));
        tFmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ret = ioctl(g_iPxpFd, VIDIOC_G_FMT, &tFmt);
        if (ret < 0) {
            printf("PXP: CAPTURE G_FMT also failed\n");
            goto err;
        }
    }
    printf("PXP: CAPTURE fmt pixelformat=0x%x size=%dx%d\n",
           tFmt.fmt.pix.pixelformat,
           tFmt.fmt.pix.width, tFmt.fmt.pix.height);

    /* REQBUFS + mmap for CAPTURE */
    memset(&tReq, 0, sizeof(tReq));
    tReq.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    tReq.memory = V4L2_MEMORY_MMAP;
    tReq.count  = PXP_NB_BUF;
    ret = ioctl(g_iPxpFd, VIDIOC_REQBUFS, &tReq);
    if (ret < 0) {
        printf("PXP: CAPTURE REQBUFS failed\n");
        goto err;
    }
    for (i = 0; i < tReq.count; i++) {
        memset(&tBuf, 0, sizeof(tBuf));
        tBuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        tBuf.memory = V4L2_MEMORY_MMAP;
        tBuf.index  = i;
        ioctl(g_iPxpFd, VIDIOC_QUERYBUF, &tBuf);
        g_atCapBufs[i].pucAddr = mmap(NULL, tBuf.length,
                      PROT_READ | PROT_WRITE, MAP_SHARED,
                      g_iPxpFd, tBuf.m.offset);
        g_atCapBufs[i].iLength = tBuf.length;
        g_atCapBufs[i].iIndex  = i;
    }

    /* Pre-queue all CAPTURE buffers */
    for (i = 0; i < PXP_NB_BUF; i++) {
        PxpQBuf(g_iPxpFd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0);
    }

    /* Start streaming: OUTPUT first, then CAPTURE */
    {
        int iType = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(g_iPxpFd, VIDIOC_STREAMON, &iType);
        iType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_iPxpFd, VIDIOC_STREAMON, &iType);
    }

    g_iOutIdx = 0;
    g_iCapIdx = 0;

    printf("PXP: init OK, %dx%d YUYV->RGB565\n", iWidth, iHeight);
    return 0;

err:
    close(g_iPxpFd);
    g_iPxpFd = -1;
    return -1;
}

int PxpConvert(unsigned char *pucSrcBuf, unsigned char **ppucDstBuf)
{
    int iCapIndex, ret;

    if (g_iPxpFd < 0 || !pucSrcBuf || !ppucDstBuf)
        return -1;

    memcpy(g_atOutBufs[g_iOutIdx].pucAddr, pucSrcBuf,
           g_atOutBufs[g_iOutIdx].iLength);

    ret = PxpQBuf(g_iPxpFd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
                  g_atOutBufs[g_iOutIdx].iIndex,
                  g_atOutBufs[g_iOutIdx].iLength);
    if (ret < 0) {
        perror("PXP: QBUF OUTPUT");
        return -1;
    }

    /* Wait for PXP to process: poll CAPTURE only */
    {
        struct pollfd tPoll;
        tPoll.fd = g_iPxpFd;
        tPoll.events = POLLIN;
        ret = poll(&tPoll, 1, 2000);
        if (ret <= 0) {
            printf("PXP: poll timeout or error: %d errno=%d\n",
                   ret, errno);
            return -1;
        }
    }

    iCapIndex = PxpDQBuf(g_iPxpFd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    if (iCapIndex < 0) {
        perror("PXP: DQBUF CAPTURE");
        return -1;
    }

    /* DQBUF OUTPUT to recycle source buffer (M2M pipeline requirement) */
    {
        int iOutDone = PxpDQBuf(g_iPxpFd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
        if (iOutDone < 0) {
            perror("PXP: DQBUF OUTPUT");
            return -1;
        }
    }

    *ppucDstBuf = g_atCapBufs[iCapIndex].pucAddr;
    g_iCapIdx  = iCapIndex;
    g_iOutIdx  = (g_iOutIdx + 1) % PXP_NB_BUF;

    return 0;
}

void PxpPutCapBuf(void)
{
    if (g_iPxpFd >= 0) {
        PxpQBuf(g_iPxpFd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
                g_atCapBufs[g_iCapIdx].iIndex, 0);
    }
}

void PxpExit(void)
{
    int i;

    if (g_iPxpFd < 0)
        return;

    {
        int iType = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(g_iPxpFd, VIDIOC_STREAMOFF, &iType);
        iType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_iPxpFd, VIDIOC_STREAMOFF, &iType);
    }

    for (i = 0; i < PXP_NB_BUF; i++) {
        if (g_atOutBufs[i].pucAddr) {
            munmap(g_atOutBufs[i].pucAddr, g_atOutBufs[i].iLength);
            g_atOutBufs[i].pucAddr = NULL;
        }
        if (g_atCapBufs[i].pucAddr) {
            munmap(g_atCapBufs[i].pucAddr, g_atCapBufs[i].iLength);
            g_atCapBufs[i].pucAddr = NULL;
        }
    }

    close(g_iPxpFd);
    g_iPxpFd = -1;
}
