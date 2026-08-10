#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <config.h>
#include <disp_manager.h>
#include <video_manager.h>
#include <convert_manager.h>
#include <render.h>
#include <pxp.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>


/* video2lcd </dev/video0,1,...> */
int main(int argc, char **argv)
{	
	int iError;
    T_VideoDevice tVideoDevice;
    PT_VideoConvert ptVideoConvert;
    int bUsePxp = 0;
    int iPixelFormatOfVideo;
    int iPixelFormatOfDisp;

    PT_VideoBuf ptVideoBufCur;
    T_VideoBuf tVideoBuf;
    T_VideoBuf tConvertBuf;
    T_VideoBuf tPxpOutBuf;   /* PXP RGB565→RGB32 转换输出 */
    T_VideoBuf tZoomBuf;
    T_VideoBuf tFrameBuf;
    
    int iLcdWidth;
    int iLcdHeigt;
    int iLcdBpp;

    int iTopLeftX;
    int iTopLeftY;

    float k;
    
    if (argc != 2)
    {
        printf("Usage:\n");
        printf("%s </dev/video0,1,...>\n", argv[0]);
        return -1;
    }
    
    

    /* 一系列的初始化 */
	/* 注册显示设备 */
	DisplayInit();
	/* 可能可支持多个显示设备: 选择和初始化指定的显示设备 */
	SelectAndInitDefaultDispDev("fb");
    GetDispResolution(&iLcdWidth, &iLcdHeigt, &iLcdBpp);
    GetVideoBufForDisplay(&tFrameBuf);
    iPixelFormatOfDisp = tFrameBuf.iPixelFormat;

    VideoInit();

    iError = VideoDeviceInit(argv[1], &tVideoDevice);
    if (iError)
    {
        DBG_PRINTF("VideoDeviceInit for %s error!\n", argv[1]);
        return -1;
    }
    iPixelFormatOfVideo = tVideoDevice.ptOPr->GetFormat(&tVideoDevice);


    /* 启动摄像头设备 */
    iError = tVideoDevice.ptOPr->StartDevice(&tVideoDevice);
    if (iError)
    {
        DBG_PRINTF("StartDevice for %s error!\n", argv[1]);
        return -1;
    }

    memset(&tVideoBuf, 0, sizeof(tVideoBuf));
    memset(&tConvertBuf, 0, sizeof(tConvertBuf));
    memset(&tPxpOutBuf, 0, sizeof(tPxpOutBuf));
    tConvertBuf.iPixelFormat     = iPixelFormatOfDisp;
    tConvertBuf.tPixelDatas.iBpp = iLcdBpp;
    
    
    memset(&tZoomBuf, 0, sizeof(tZoomBuf));

    /* 初始化 PXP 硬件 YUYV→RGB565 转换 */
    if (iPixelFormatOfVideo != iPixelFormatOfDisp)
    {
        iError = PxpInit(tVideoDevice.iWidth, tVideoDevice.iHeight);
        if (iError)
        {
            printf("PXP init failed, fallback to CPU convert\n");
            /* 回退到 CPU 软解 */
            VideoConvertInit();
            ptVideoConvert = GetVideoConvertForFormats(iPixelFormatOfVideo, iPixelFormatOfDisp);
            if (NULL == ptVideoConvert)
            {
                printf("can not support format convert\n");
                return -1;
            }
        }
        else
        {
            /* PXP 输出 RGB565，需要再转 RGB32 以匹配 LCD */
            tConvertBuf.iPixelFormat     = V4L2_PIX_FMT_RGB565;
            tConvertBuf.tPixelDatas.iBpp = 16;
            tConvertBuf.tPixelDatas.iWidth     = tVideoDevice.iWidth;
            tConvertBuf.tPixelDatas.iHeight    = tVideoDevice.iHeight;
            tConvertBuf.tPixelDatas.iLineBytes = tVideoDevice.iWidth * 2;
            tConvertBuf.tPixelDatas.iTotalBytes = tConvertBuf.tPixelDatas.iLineBytes * tVideoDevice.iHeight;

            /* 获取 RGB565→LCD格式 转换器 (PicMerge 要求相同bpp) */
            VideoConvertInit();
            ptVideoConvert = GetVideoConvertForFormats(V4L2_PIX_FMT_RGB565, iPixelFormatOfDisp);
            if (NULL == ptVideoConvert) {
                printf("PXP: no RGB565→DISPLAY converter, fallback\n");
                ptVideoConvert = GetVideoConvertForFormats(iPixelFormatOfVideo, iPixelFormatOfDisp);
                if (NULL == ptVideoConvert) {
                    printf("can not support format convert\n");
                    return -1;
                }
                /* CPU 回退：重新设置 tConvertBuf */
                tConvertBuf.iPixelFormat     = iPixelFormatOfDisp;
                tConvertBuf.tPixelDatas.iBpp = iLcdBpp;
            } else {
                bUsePxp = 1;  /* PXP 管线就绪 */
            }
        }
    }
    else
    {
        ptVideoConvert = NULL;
    }

    /* PXP 输出缓冲仅首次 malloc，后续复用 */
    tPxpOutBuf.tPixelDatas.aucPixelDatas = NULL;

    while (1)
    {
        unsigned char *pucPxpDst = NULL;

        /* 读入摄像头数据 */
        iError = tVideoDevice.ptOPr->GetFrame(&tVideoDevice, &tVideoBuf);
        if (iError)
        {
            DBG_PRINTF("GetFrame for %s error!\n", argv[1]);
            return -1;
        }
        ptVideoBufCur = &tVideoBuf;

        if (iPixelFormatOfVideo != iPixelFormatOfDisp)
        {
            if (bUsePxp)
            {
                /* PXP: YUYV → PXP(RGB565) → rgb2rgb(RGB32) → PicMerge */
                iError = PxpConvert(
                    tVideoBuf.tPixelDatas.aucPixelDatas,
                    &pucPxpDst);
                if (iError)
                {
                    printf("PXP convert error!\n");
                    return -1;
                }
                /* Step 1: PXP output as RGB565 */
                tConvertBuf.iPixelFormat           = V4L2_PIX_FMT_RGB565; /* reset */
                tConvertBuf.tPixelDatas.iBpp       = 16;
                tConvertBuf.tPixelDatas.iLineBytes = tVideoDevice.iWidth * 2;
                tConvertBuf.tPixelDatas.aucPixelDatas = pucPxpDst;
                /* Step 2: Convert RGB565 → LCD format to tPxpOutBuf */
                tPxpOutBuf.iPixelFormat     = iPixelFormatOfDisp;
                tPxpOutBuf.tPixelDatas.iBpp = iLcdBpp;
                tPxpOutBuf.tPixelDatas.iWidth  = tVideoDevice.iWidth;
                tPxpOutBuf.tPixelDatas.iHeight = tVideoDevice.iHeight;
                iError = ptVideoConvert->Convert(&tConvertBuf, &tPxpOutBuf);
                if (iError)
                {
                    printf("PXP: RGB565→LCD convert error!\n");
                    return -1;
                }
                ptVideoBufCur = &tPxpOutBuf;
            }
            else
            {
                /* CPU 软解 YUYV→LCD格式 */
                iError = ptVideoConvert->Convert(&tVideoBuf, &tConvertBuf);
                if (iError)
                {
                    DBG_PRINTF("Convert error!\n");
                    return -1;
                }
                ptVideoBufCur = &tConvertBuf;
            }
        }
        

        /* 如果图像分辨率大于LCD, 缩放 */
        if (ptVideoBufCur->tPixelDatas.iWidth > iLcdWidth || ptVideoBufCur->tPixelDatas.iHeight > iLcdHeigt)
        {
            /* 确定缩放后的分辨率 */
            /* 把图片按比例缩放到VideoMem上, 居中显示
             * 1. 先算出缩放后的大小
             */
            k = (float)ptVideoBufCur->tPixelDatas.iHeight / ptVideoBufCur->tPixelDatas.iWidth;
            tZoomBuf.tPixelDatas.iWidth  = iLcdWidth;
            tZoomBuf.tPixelDatas.iHeight = iLcdWidth * k;
            if ( tZoomBuf.tPixelDatas.iHeight > iLcdHeigt)
            {
                tZoomBuf.tPixelDatas.iWidth  = iLcdHeigt / k;
                tZoomBuf.tPixelDatas.iHeight = iLcdHeigt;
            }
            tZoomBuf.tPixelDatas.iBpp        = iLcdBpp;
            tZoomBuf.tPixelDatas.iLineBytes  = tZoomBuf.tPixelDatas.iWidth * tZoomBuf.tPixelDatas.iBpp / 8;
            tZoomBuf.tPixelDatas.iTotalBytes = tZoomBuf.tPixelDatas.iLineBytes * tZoomBuf.tPixelDatas.iHeight;

            if (!tZoomBuf.tPixelDatas.aucPixelDatas)
            {
                tZoomBuf.tPixelDatas.aucPixelDatas = malloc(tZoomBuf.tPixelDatas.iTotalBytes);
            }
            
            PicZoom(&ptVideoBufCur->tPixelDatas, &tZoomBuf.tPixelDatas);
            ptVideoBufCur = &tZoomBuf;
        }

        /* 合并进framebuffer */
        /* 接着算出居中显示时左上角坐标 */
        iTopLeftX = (iLcdWidth - ptVideoBufCur->tPixelDatas.iWidth) / 2;
        iTopLeftY = (iLcdHeigt - ptVideoBufCur->tPixelDatas.iHeight) / 2;

        PicMerge(iTopLeftX, iTopLeftY, &ptVideoBufCur->tPixelDatas, &tFrameBuf.tPixelDatas);

        FlushPixelDatasToDev(&tFrameBuf.tPixelDatas);

        iError = tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
        if (iError)
        {
            DBG_PRINTF("PutFrame for %s error!\n", argv[1]);
            return -1;
        }

        /* 归还 PXP CAPTURE buffer */
        if (bUsePxp && pucPxpDst)
            PxpPutCapBuf();
    }

    PxpExit();
    return 0;
}

