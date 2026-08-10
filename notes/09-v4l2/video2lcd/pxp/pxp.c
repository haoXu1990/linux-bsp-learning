#include <pxp.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define PXP_BUFFER_COUNT 2

struct pxp_buffer {
    unsigned char *pucAddr;
    unsigned int iLength;
};

static int g_iPxpFd = -1;
static int g_iOutputStreaming, g_iCaptureStreaming;
static int g_iOutputBufCount, g_iCaptureBufCount;
static int g_iNextOutputIndex;
static int g_iHeldCaptureIndex = -1;
static int g_iDstWidth, g_iDstHeight, g_iDstFormat, g_iDstBpp;
static struct pxp_buffer g_atOutputBufs[PXP_BUFFER_COUNT];
static struct pxp_buffer g_atCaptureBufs[PXP_BUFFER_COUNT];

static int QueueBuffer(enum v4l2_buf_type eType, int iIndex,
                       unsigned int iBytesUsed)
{
    struct v4l2_buffer tBuf;
    memset(&tBuf, 0, sizeof(tBuf));
    tBuf.type = eType;
    tBuf.memory = V4L2_MEMORY_MMAP;
    tBuf.index = iIndex;
    if (eType == V4L2_BUF_TYPE_VIDEO_OUTPUT)
        tBuf.bytesused = iBytesUsed;
    return ioctl(g_iPxpFd, VIDIOC_QBUF, &tBuf);
}

static int DequeueBuffer(enum v4l2_buf_type eType)
{
    struct v4l2_buffer tBuf;
    memset(&tBuf, 0, sizeof(tBuf));
    tBuf.type = eType;
    tBuf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_iPxpFd, VIDIOC_DQBUF, &tBuf) < 0)
        return -1;
    return tBuf.index;
}

static int SetFormat(enum v4l2_buf_type eType, int iWidth, int iHeight,
                     int iPixelFormat)
{
    struct v4l2_format tFmt;
    memset(&tFmt, 0, sizeof(tFmt));
    tFmt.type = eType;
    tFmt.fmt.pix.width = iWidth;
    tFmt.fmt.pix.height = iHeight;
    tFmt.fmt.pix.pixelformat = iPixelFormat;
    tFmt.fmt.pix.field = V4L2_FIELD_NONE;
    return ioctl(g_iPxpFd, VIDIOC_S_FMT, &tFmt);
}

static int RequestAndMap(enum v4l2_buf_type eType,
                         struct pxp_buffer *ptBuffers, int *piCount)
{
    struct v4l2_requestbuffers tReq;
    struct v4l2_buffer tBuf;
    int i;

    memset(&tReq, 0, sizeof(tReq));
    tReq.type = eType;
    tReq.memory = V4L2_MEMORY_MMAP;
    tReq.count = PXP_BUFFER_COUNT;
    if (ioctl(g_iPxpFd, VIDIOC_REQBUFS, &tReq) < 0)
        return -1;
    if (!tReq.count || tReq.count > PXP_BUFFER_COUNT)
        return -1;

    *piCount = tReq.count;
    for (i = 0; i < *piCount; i++) {
        memset(&tBuf, 0, sizeof(tBuf));
        tBuf.type = eType;
        tBuf.memory = V4L2_MEMORY_MMAP;
        tBuf.index = i;
        if (ioctl(g_iPxpFd, VIDIOC_QUERYBUF, &tBuf) < 0)
            return -1;
        ptBuffers[i].iLength = tBuf.length;
        ptBuffers[i].pucAddr = mmap(NULL, tBuf.length,
                                    PROT_READ | PROT_WRITE, MAP_SHARED,
                                    g_iPxpFd, tBuf.m.offset);
        if (ptBuffers[i].pucAddr == MAP_FAILED) {
            ptBuffers[i].pucAddr = NULL;
            return -1;
        }
    }
    return 0;
}

int PxpInit(const char *pcDevName,
            int iSrcWidth, int iSrcHeight, int iSrcFormat,
            int iDstWidth, int iDstHeight, int iDstFormat)
{
    struct v4l2_capability tCap;
    enum v4l2_buf_type eType;
    int i;
    memset(g_atOutputBufs, 0, sizeof(g_atOutputBufs));
    memset(g_atCaptureBufs, 0, sizeof(g_atCaptureBufs));
    g_iHeldCaptureIndex = -1;
    g_iNextOutputIndex = 0;
    g_iPxpFd = open(pcDevName, O_RDWR);
    if (g_iPxpFd < 0) {
        perror("PXP: open");
        return -1;
    }

    memset(&tCap, 0, sizeof(tCap));
    if (ioctl(g_iPxpFd, VIDIOC_QUERYCAP, &tCap) < 0) {
        perror("PXP: QUERYCAP");
        goto err;
    }
    printf("PXP: device=%s driver=%s card=%s%c",
           pcDevName, tCap.driver, tCap.card, 10);

    if (SetFormat(V4L2_BUF_TYPE_VIDEO_OUTPUT,
                  iSrcWidth, iSrcHeight, iSrcFormat) < 0) {
        perror("PXP: set OUTPUT format");
        goto err;
    }
    if (RequestAndMap(V4L2_BUF_TYPE_VIDEO_OUTPUT,
                      g_atOutputBufs, &g_iOutputBufCount) < 0) {
        perror("PXP: allocate OUTPUT buffers");
        goto err;
    }
    if (SetFormat(V4L2_BUF_TYPE_VIDEO_CAPTURE,
                  iDstWidth, iDstHeight, iDstFormat) < 0) {
        perror("PXP: set CAPTURE format");
        goto err;
    }
    if (RequestAndMap(V4L2_BUF_TYPE_VIDEO_CAPTURE,
                      g_atCaptureBufs, &g_iCaptureBufCount) < 0) {
        perror("PXP: allocate CAPTURE buffers");
        goto err;
    }

    for (i = 0; i < g_iCaptureBufCount; i++) {
        if (QueueBuffer(V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0) < 0)
            goto err;
    }
    eType = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(g_iPxpFd, VIDIOC_STREAMON, &eType) < 0)
        goto err;
    g_iOutputStreaming = 1;
    eType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_iPxpFd, VIDIOC_STREAMON, &eType) < 0)
        goto err;
    g_iCaptureStreaming = 1;

    g_iDstWidth = iDstWidth;
    g_iDstHeight = iDstHeight;
    g_iDstFormat = iDstFormat;
    g_iDstBpp = (iDstFormat == V4L2_PIX_FMT_RGB565) ? 16 : 32;
    printf("PXP: hardware CSC/scale %dx%d -> %dx%d%c",
           iSrcWidth, iSrcHeight, iDstWidth, iDstHeight, 10);
    return 0;

err:
    PxpExit();
    return -1;
}

int PxpConvert(const T_VideoBuf *ptSrcBuf, T_VideoBuf *ptDstBuf)
{
    struct pollfd tPoll;
    struct pxp_buffer *ptOutput;
    unsigned int iBytesUsed;
    int iOutputDone, iCaptureDone, iRet;

    if (g_iPxpFd < 0 || !ptSrcBuf || !ptDstBuf ||
        g_iHeldCaptureIndex >= 0)
        return -1;

    ptOutput = &g_atOutputBufs[g_iNextOutputIndex];
    iBytesUsed = ptSrcBuf->tPixelDatas.iTotalBytes;
    if (iBytesUsed > ptOutput->iLength) {
        fprintf(stderr, "PXP: source frame is larger than OUTPUT buffer%c", 10);
        return -1;
    }

    memcpy(ptOutput->pucAddr,
           ptSrcBuf->tPixelDatas.aucPixelDatas, iBytesUsed);
    if (QueueBuffer(V4L2_BUF_TYPE_VIDEO_OUTPUT,
                    g_iNextOutputIndex, iBytesUsed) < 0) {
        perror("PXP: QBUF OUTPUT");
        return -1;
    }

    memset(&tPoll, 0, sizeof(tPoll));
    tPoll.fd = g_iPxpFd;
    tPoll.events = POLLIN;
    iRet = poll(&tPoll, 1, 2000);
    if (iRet <= 0) {
        fprintf(stderr, "PXP: poll failed or timed out%c", 10);
        return -1;
    }

    iCaptureDone = DequeueBuffer(V4L2_BUF_TYPE_VIDEO_CAPTURE);
    if (iCaptureDone < 0) {
        perror("PXP: DQBUF CAPTURE");
        return -1;
    }
    iOutputDone = DequeueBuffer(V4L2_BUF_TYPE_VIDEO_OUTPUT);
    if (iOutputDone < 0) {
        perror("PXP: DQBUF OUTPUT");
        QueueBuffer(V4L2_BUF_TYPE_VIDEO_CAPTURE, iCaptureDone, 0);
        return -1;
    }

    g_iHeldCaptureIndex = iCaptureDone;
    g_iNextOutputIndex = (iOutputDone + 1) % g_iOutputBufCount;
    memset(ptDstBuf, 0, sizeof(*ptDstBuf));
    ptDstBuf->iPixelFormat = g_iDstFormat;
    ptDstBuf->tPixelDatas.iWidth = g_iDstWidth;
    ptDstBuf->tPixelDatas.iHeight = g_iDstHeight;
    ptDstBuf->tPixelDatas.iBpp = g_iDstBpp;
    ptDstBuf->tPixelDatas.iLineBytes = g_iDstWidth * g_iDstBpp / 8;
    ptDstBuf->tPixelDatas.iTotalBytes =
        ptDstBuf->tPixelDatas.iLineBytes * g_iDstHeight;
    ptDstBuf->tPixelDatas.aucPixelDatas =
        g_atCaptureBufs[iCaptureDone].pucAddr;
    return 0;
}

int PxpPutBuffer(void)
{
    int iIndex;
    if (g_iPxpFd < 0 || g_iHeldCaptureIndex < 0)
        return -1;
    iIndex = g_iHeldCaptureIndex;
    g_iHeldCaptureIndex = -1;
    return QueueBuffer(V4L2_BUF_TYPE_VIDEO_CAPTURE, iIndex, 0);
}

void PxpExit(void)
{
    enum v4l2_buf_type eType;
    int i;

    if (g_iPxpFd < 0)
        return;
    if (g_iOutputStreaming) {
        eType = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(g_iPxpFd, VIDIOC_STREAMOFF, &eType);
        g_iOutputStreaming = 0;
    }
    if (g_iCaptureStreaming) {
        eType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_iPxpFd, VIDIOC_STREAMOFF, &eType);
        g_iCaptureStreaming = 0;
    }
    for (i = 0; i < g_iOutputBufCount; i++) {
        if (g_atOutputBufs[i].pucAddr)
            munmap(g_atOutputBufs[i].pucAddr,
                   g_atOutputBufs[i].iLength);
    }
    for (i = 0; i < g_iCaptureBufCount; i++) {
        if (g_atCaptureBufs[i].pucAddr)
            munmap(g_atCaptureBufs[i].pucAddr,
                   g_atCaptureBufs[i].iLength);
    }
    close(g_iPxpFd);
    g_iPxpFd = -1;
    g_iOutputBufCount = 0;
    g_iCaptureBufCount = 0;
    g_iHeldCaptureIndex = -1;
}
