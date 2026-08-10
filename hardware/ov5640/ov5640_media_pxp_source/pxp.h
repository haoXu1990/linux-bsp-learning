#ifndef _PXP_H
#define _PXP_H

int  PxpInit(int iWidth, int iHeight);
int  PxpConvert(unsigned char *pucSrcBuf, unsigned char **ppucDstBuf);
void PxpPutCapBuf(void);
void PxpExit(void);

#endif
