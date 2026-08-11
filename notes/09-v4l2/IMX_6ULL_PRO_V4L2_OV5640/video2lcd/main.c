#include <config.h>
#include <convert_manager.h>
#include <disp_manager.h>
#include <pxp.h>
#include <render.h>
#include <video_manager.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void CalcDisplayRect(int iSrcWidth, int iSrcHeight,
                            int iLcdWidth, int iLcdHeight,
                            int *piLeft, int *piTop,
                            int *piWidth, int *piHeight)
{
    float fWidthScale;
    float fHeightScale;
    float fScale;

    fWidthScale = (float)iLcdWidth / iSrcWidth;
    fHeightScale = (float)iLcdHeight / iSrcHeight;
    fScale = (fWidthScale < fHeightScale) ?
             fWidthScale : fHeightScale;

    *piWidth = ((int)(iSrcWidth * fScale)) & ~7;
    *piHeight = ((int)(iSrcHeight * fScale)) & ~7;
    *piLeft = ((iLcdWidth - *piWidth) / 2) & ~7;
    *piTop = ((iLcdHeight - *piHeight) / 2) & ~7;
}

static void PrintUsage(const char *pcProgram)
{
    printf("Usage: %s <camera-device> [pxp-device|--cpu]%c",
           pcProgram, 10);
    printf("PXP: %s /dev/video1 /dev/video0%c", pcProgram, 10);
    printf("CPU: %s /dev/video1 --cpu%c", pcProgram, 10);
}

int main(int argc, char **argv)
{
    const char *pcPxpDevice = "/dev/video0";
    T_VideoDevice tVideoDevice;
    T_VideoBuf tVideoBuf;
    T_VideoBuf tConvertBuf;
    T_VideoBuf tZoomBuf;
    T_VideoBuf tFrameBuf;
    PT_VideoBuf ptVideoBufCur;
    PT_VideoConvert ptVideoConvert = NULL;
    int iPixelFormatOfVideo;
    int iPixelFormatOfDisp;
    int iLcdWidth, iLcdHeight, iLcdBpp;
    int iDstLeft, iDstTop, iDstWidth, iDstHeight;
    int iTopLeftX, iTopLeftY;
    int bUsePxp = 1;
    int iError;
    float k;

    if (argc < 2 || argc > 3) {
        PrintUsage(argv[0]);
        return -1;
    }
    if (argc == 3) {
        if (!strcmp(argv[2], "--cpu"))
            bUsePxp = 0;
        else
            pcPxpDevice = argv[2];
    }

    memset(&tVideoDevice, 0, sizeof(tVideoDevice));
    memset(&tVideoBuf, 0, sizeof(tVideoBuf));
    memset(&tConvertBuf, 0, sizeof(tConvertBuf));
    memset(&tZoomBuf, 0, sizeof(tZoomBuf));
    memset(&tFrameBuf, 0, sizeof(tFrameBuf));

    DisplayInit();
    SelectAndInitDefaultDispDev("fb");
    GetDispResolution(&iLcdWidth, &iLcdHeight, &iLcdBpp);
    GetVideoBufForDisplay(&tFrameBuf);
    iPixelFormatOfDisp = tFrameBuf.iPixelFormat;

    VideoInit();
    iError = VideoDeviceInit(argv[1], &tVideoDevice);
    if (iError) {
        DBG_PRINTF("VideoDeviceInit for %s error!%c", argv[1], 10);
        return -1;
    }
    iPixelFormatOfVideo = tVideoDevice.ptOPr->GetFormat(&tVideoDevice);

    CalcDisplayRect(tVideoDevice.iWidth, tVideoDevice.iHeight,
                    iLcdWidth, iLcdHeight,
                    &iDstLeft, &iDstTop, &iDstWidth, &iDstHeight);

    if (bUsePxp &&
        PxpInit(pcPxpDevice,
                tVideoDevice.iWidth, tVideoDevice.iHeight,
                iPixelFormatOfVideo,
                iDstLeft, iDstTop, iDstWidth, iDstHeight)) {
        printf("PXP: init failed, fallback to CPU%c", 10);
        bUsePxp = 0;
    }

    if (!bUsePxp) {
        VideoConvertInit();
        ptVideoConvert = GetVideoConvertForFormats(iPixelFormatOfVideo,
                                                   iPixelFormatOfDisp);
        if (!ptVideoConvert) {
            DBG_PRINTF("can not support this format convert%c", 10);
            return -1;
        }
        printf("video2lcd: CPU YUV-to-RGB path%c", 10);
    } else {
        printf("video2lcd: mxc PXP hardware display path%c", 10);
    }

    iError = tVideoDevice.ptOPr->StartDevice(&tVideoDevice);
    if (iError) {
        DBG_PRINTF("StartDevice for %s error!%c", argv[1], 10);
        PxpExit();
        return -1;
    }

    tConvertBuf.iPixelFormat = iPixelFormatOfDisp;
    tConvertBuf.tPixelDatas.iBpp = iLcdBpp;

    while (1) {
        iError = tVideoDevice.ptOPr->GetFrame(&tVideoDevice, &tVideoBuf);
        if (iError) {
            DBG_PRINTF("GetFrame for %s error!%c", argv[1], 10);
            break;
        }

        if (bUsePxp) {
            iError = PxpDisplayFrame(&tVideoBuf);
            if (iError) {
                DBG_PRINTF("PXP display frame error%c", 10);
                // xian close  pxp
                PxpExit();
                tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
                break;
            }
            iError = tVideoDevice.ptOPr->PutFrame(&tVideoDevice,
                                                  &tVideoBuf);
            if (iError) {
                DBG_PRINTF("PutFrame for %s error!%c", argv[1], 10);
                break;
            }
            continue;
        }

        ptVideoBufCur = &tVideoBuf;
        if (iPixelFormatOfVideo != iPixelFormatOfDisp) {
            iError = ptVideoConvert->Convert(&tVideoBuf, &tConvertBuf);
            if (iError) {
                DBG_PRINTF("CPU convert error%c", 10);
                tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
                break;
            }
            ptVideoBufCur = &tConvertBuf;
        }

        if (ptVideoBufCur->tPixelDatas.iWidth > iLcdWidth ||
            ptVideoBufCur->tPixelDatas.iHeight > iLcdHeight) {
            k = (float)ptVideoBufCur->tPixelDatas.iHeight /
                ptVideoBufCur->tPixelDatas.iWidth;
            tZoomBuf.tPixelDatas.iWidth = iLcdWidth;
            tZoomBuf.tPixelDatas.iHeight = iLcdWidth * k;
            if (tZoomBuf.tPixelDatas.iHeight > iLcdHeight) {
                tZoomBuf.tPixelDatas.iWidth = iLcdHeight / k;
                tZoomBuf.tPixelDatas.iHeight = iLcdHeight;
            }
            tZoomBuf.tPixelDatas.iBpp = iLcdBpp;
            tZoomBuf.tPixelDatas.iLineBytes =
                tZoomBuf.tPixelDatas.iWidth * iLcdBpp / 8;
            tZoomBuf.tPixelDatas.iTotalBytes =
                tZoomBuf.tPixelDatas.iLineBytes *
                tZoomBuf.tPixelDatas.iHeight;
            if (!tZoomBuf.tPixelDatas.aucPixelDatas)
                tZoomBuf.tPixelDatas.aucPixelDatas =
                    malloc(tZoomBuf.tPixelDatas.iTotalBytes);
            PicZoom(&ptVideoBufCur->tPixelDatas,
                    &tZoomBuf.tPixelDatas);
            ptVideoBufCur = &tZoomBuf;
        }

        iTopLeftX =
            (iLcdWidth - ptVideoBufCur->tPixelDatas.iWidth) / 2;
        iTopLeftY =
            (iLcdHeight - ptVideoBufCur->tPixelDatas.iHeight) / 2;
        PicMerge(iTopLeftX, iTopLeftY,
                 &ptVideoBufCur->tPixelDatas,
                 &tFrameBuf.tPixelDatas);
        FlushPixelDatasToDev(&tFrameBuf.tPixelDatas);

        iError = tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
        if (iError) {
            DBG_PRINTF("PutFrame for %s error!%c", argv[1], 10);
            break;
        }
    }

    PxpExit();
    tVideoDevice.ptOPr->StopDevice(&tVideoDevice);
    tVideoDevice.ptOPr->ExitDevice(&tVideoDevice);
    free(tZoomBuf.tPixelDatas.aucPixelDatas);
    return -1;
}
