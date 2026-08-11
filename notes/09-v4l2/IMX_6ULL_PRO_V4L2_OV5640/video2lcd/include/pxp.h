#ifndef _VIDEO2LCD_PXP_H
#define _VIDEO2LCD_PXP_H

#include <video_manager.h>

int PxpInit(const char *pcDevName,
            int iSrcWidth, int iSrcHeight, int iSrcFormat,
            int iDstLeft, int iDstTop,
            int iDstWidth, int iDstHeight);
int PxpDisplayFrame(const T_VideoBuf *ptVideoBuf);
void PxpExit(void);

#endif
