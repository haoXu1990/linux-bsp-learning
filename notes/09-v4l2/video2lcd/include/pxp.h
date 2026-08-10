#ifndef _VIDEO2LCD_PXP_H
#define _VIDEO2LCD_PXP_H

#include <video_manager.h>

#ifndef V4L2_PIX_FMT_XBGR32
#define V4L2_PIX_FMT_XBGR32 v4l2_fourcc('X', 'B', '2', '4')
#endif

int PxpInit(const char *pcDevName,
            int iSrcWidth, int iSrcHeight, int iSrcFormat,
            int iDstWidth, int iDstHeight, int iDstFormat);
int PxpConvert(const T_VideoBuf *ptSrcBuf, T_VideoBuf *ptDstBuf);
int PxpPutBuffer(void);
void PxpExit(void);

#endif
