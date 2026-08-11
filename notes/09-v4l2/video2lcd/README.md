# video2lcd：OV5640 + CSI + PXP + LCD

## 1. 项目目标

本项目在 100ASK i.MX6ULL Linux 4.9.88 上实现：

~~~text
OV5640 Sensor
    │ DVP：YUYV 640x480
    ▼
i.MX6ULL CSI / mx6s_capture.c
    │ /dev/video1，V4L2 VIDEO_CAPTURE
    ▼
video2lcd
    │ USERPTR
    ▼
i.MX6ULL PXP / mxc_pxp_v4l2.c
    │ /dev/video0，V4L2 VIDEO_OUTPUT
    │ YUV→RGB、缩放、输出到Framebuffer
    ▼s
LCD /dev/fb0，1024x600
~~~



PXP 路径的目的是把耗时的颜色转换和缩放交给硬件，降低 CPU 占用并改善卡顿。

---

## 2. 从老师原版到当前版本

### 2.1 老师原版video2lcd做了什么

老师原版只支持：

~~~sh
./video2lcd /dev/video1
~~~

原始项目中没有：

~~~text
pxp/pxp.c
include/pxp.h
PxpInit()
PxpDisplayFrame()
~~~

原版 <code>Makefile</code> 也只编译：

~~~make
obj-y += main.o
obj-y += convert/
obj-y += display/
obj-y += render/
obj-y += video/
~~~

原版每一帧都经过下面的CPU路径：

~~~text
GetFrame()
  → 从/dev/video1得到摄像头YUYV
  → GetVideoConvertForFormats()
  → ptVideoConvert->Convert()
  → CPU软件YUYV转RGB
  → 必要时PicZoom()
  → CPU软件缩放
  → PicMerge()
  → 合并到Framebuffer内存
  → FlushPixelDatasToDev()
  → 刷新到/dev/fb0
  → PutFrame()
  → 归还摄像头Buffer
~~~

原版核心代码可以概括为：

~~~c
iError = tVideoDevice.ptOPr->GetFrame(&tVideoDevice, &tVideoBuf);
ptVideoBufCur = &tVideoBuf;

if (iPixelFormatOfVideo != iPixelFormatOfDisp) {
    iError = ptVideoConvert->Convert(&tVideoBuf, &tConvertBuf);
    ptVideoBufCur = &tConvertBuf;
}

if (ptVideoBufCur->tPixelDatas.iWidth > iLcdWidth ||
    ptVideoBufCur->tPixelDatas.iHeight > iLcdHeight) {
    PicZoom(&ptVideoBufCur->tPixelDatas, &tZoomBuf.tPixelDatas);
    ptVideoBufCur = &tZoomBuf;
}

PicMerge(iTopLeftX, iTopLeftY,
         &ptVideoBufCur->tPixelDatas,
         &tFrameBuf.tPixelDatas);
FlushPixelDatasToDev(&tFrameBuf.tPixelDatas);
tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
~~~

### 2.2 原版为什么会卡顿

640x480 YUYV一帧包含：

~~~text
640 × 480 = 307200个像素
YUYV 16bpp = 614400字节
~~~

CPU路径需要对每个像素执行：

- 读取Y、U、V分量；
- YUV到RGB公式计算；
- 限幅；
- 写RGB像素；
- 如果需要缩放，再执行一次大量像素访问；
- 最后复制或合并到Framebuffer。

循环每得到一帧都重复这些操作。i.MX6ULL CPU性能有限，因此主要耗时点是：

~~~c
ptVideoConvert->Convert(&tVideoBuf, &tConvertBuf);
~~~

以及软件缩放：

~~~c
PicZoom(...);
~~~

这就是引入PXP的原因。

### 2.4 当前正确方向：mxc VIDEO_OUTPUT直接显示

当前PXP分支为：

~~~text
CSI GetFrame()
  → 得到摄像头YUYV Buffer
  → PxpDisplayFrame()
  → 把同一个Buffer作为USERPTR提交给/dev/video0
  → mxc_pxp_v4l2驱动调用PXP硬件
  → PXP完成YUV→RGB和缩放
  → 驱动输出到Framebuffer
  → PXP DQBUF完成
  → CSI PutFrame()归还摄像头Buffer
~~~

当前主循环核心代码：

~~~c
if (bUsePxp) {
    iError = PxpDisplayFrame(&tVideoBuf);
    if (iError) {
        PxpExit();
        tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
        break;
    }

    iError = tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
    if (iError)
        break;

    continue;
}
~~~

这里的 <code>continue</code> 很重要。PXP分支成功后不再执行：

~~~text
ptVideoConvert->Convert()
PicZoom()
PicMerge()
FlushPixelDatasToDev()
~~~

因为这些工作已经由PXP和 <code>mxc_pxp_v4l2.c</code> 完成。如果PXP处理后又执行一次CPU转换和Framebuffer合并，就失去了硬件加速的意义。

### 2.5 修改了哪些文件

| 文件 | 原版 | 当前修改 | 为什么修改 |
|---|---|---|---|
| <code>Makefile</code> | 不编译PXP目录 | 增加 <code>obj-y += pxp/</code> | 把PXP应用封装加入最终程序 |
| <code>pxp/Makefile</code> | 不存在 | 编译 <code>pxp.o</code> | 接入现有递归构建系统 |
| <code>include/pxp.h</code> | 不存在 | 声明初始化、显示、退出API | 隔离PXP实现与主流程 |
| <code>pxp/pxp.c</code> | 不存在 | 实现mxc VIDEO_OUTPUT、USERPTR和ioctl流程 | 调用当前内核PXP驱动 |
| <code>main.c</code> | 只有CPU路径 | 增加PXP/CPU选择、PXP分支和回退 | 保留对比能力并使用硬件加速 |
| <code>video/v4l2.c</code> | 请求LCD尺寸作为采集尺寸 | 暂时固定640x480并打印返回格式 | 避免OV5640使用无效的1024x600模式 |

### 2.6 main.c具体怎样修改

#### 修改一：命令行增加PXP节点与CPU对比

原版：

~~~sh
./video2lcd /dev/video1
~~~

当前：

~~~sh
./video2lcd /dev/video1 /dev/video0
./video2lcd /dev/video1 --cpu
~~~

原因：

- 明确传入摄像头节点和PXP节点；
- 可以强制CPU路径作画质、帧率和CPU占用对比；
- PXP失败时仍可以验证摄像头和LCD。

#### 修改二：初始化PXP

摄像头和LCD初始化完成后，程序已经知道：

~~~text
摄像头宽高
摄像头像素格式
LCD宽高
目标显示区域
~~~

此时调用：

~~~c
PxpInit(pcPxpDevice,
        tVideoDevice.iWidth,
        tVideoDevice.iHeight,
        iPixelFormatOfVideo,
        iDstLeft,
        iDstTop,
        iDstWidth,
        iDstHeight);
~~~

原因：PXP必须同时知道输入YUYV的布局和Framebuffer上的输出区域。

#### 修改三：保留CPU回退

~~~c
if (PxpInit(...) != 0) {
    printf("PXP: init failed, fallback to CPU\n");
    bUsePxp = 0;
}
~~~

原因：如果PXP设备树、DMA Channel或ioctl配置有问题，仍然可以通过CPU路径显示，用于区分“摄像头问题”和“PXP问题”。

#### 修改四：PXP完成前不能归还CSI Buffer

当前PXP使用CSI Buffer作为USERPTR。正确顺序：

~~~text
CSI DQBUF
  → PXP QBUF
  → PXP DQBUF
  → CSI QBUF
~~~

不能写成：

~~~text
CSI DQBUF
  → PXP QBUF
  → 立即CSI QBUF
~~~

否则CSI可能覆盖PXP仍在读取的内存。

#### 修改五：增加PxpExit顺序保护

如果PXP提交后发生错误，先关闭PXP，再归还CSI Buffer：

~~~c
PxpExit();
tVideoDevice.ptOPr->PutFrame(&tVideoDevice, &tVideoBuf);
~~~

原因：确保PXP不再持有该USERPTR。

### 2.7 video/v4l2.c具体怎样修改

#### 原版

~~~c
GetDispResolution(&iLcdWidth, &iLcdHeigt, &iLcdBpp);
tV4l2Fmt.fmt.pix.width  = iLcdWidth;
tV4l2Fmt.fmt.pix.height = iLcdHeigt;
~~~

在当前LCD上等价于请求：

~~~text
1024x600
~~~

#### 当前

~~~c
tV4l2Fmt.fmt.pix.width  = 640;
tV4l2Fmt.fmt.pix.height = 480;
~~~

同时增加：

~~~c
printf("Camera format returned: %dx%d, bytesperline=%d, sizeimage=%d\n",
       tV4l2Fmt.fmt.pix.width,
       tV4l2Fmt.fmt.pix.height,
       tV4l2Fmt.fmt.pix.bytesperline,
       tV4l2Fmt.fmt.pix.sizeimage);
~~~

原因：

- 当前OV5640 V3已经验证640x480 YUYV正确；
- 1024x600不是当前Sensor驱动的有效模式；
- 必须使用驱动返回的宽高、步长和帧大小检查真实内存布局。

这还是阶段性方案。正式通用方案应执行：

~~~text
VIDIOC_ENUM_FRAMESIZES
  → 选择摄像头真正支持的分辨率
  → VIDIOC_S_FMT
  → 使用驱动返回值
~~~

### 2.8 CalcDisplayRect为什么修改

早期函数只在源图大于LCD时缩小：

~~~text
640x480输入
  → scale保持1.0
  → 目标仍是640x480
  → 在1024x600中间显示
~~~

当前对放大和缩小使用相同的等比例计算：

~~~c
fWidthScale = (float)iLcdWidth / iSrcWidth;
fHeightScale = (float)iLcdHeight / iSrcHeight;
fScale = (fWidthScale < fHeightScale) ?
         fWidthScale : fHeightScale;
~~~

640x480最终得到：

~~~text
目标尺寸：800x600
目标位置：(112,0)
~~~

宽高和坐标向下按8像素对齐，便于满足PXP矩形和硬件处理的对齐要求。

### 2.9 pxp/pxp.c具体怎样修改

#### 初始化阶段

~~~text
open(PxP节点)
  → QUERYCAP确认VIDEO_OUTPUT
  → S_OUTPUT选择Framebuffer输出
  → S_FMT配置YUYV输入
  → S_FMT(VIDEO_OUTPUT_OVERLAY)配置源区域srect
  → S_CROP配置目标区域drect
  → REQBUFS(USERPTR)
~~~

#### 每帧阶段

~~~text
QBUF(USERPTR)
  → 首帧STREAMON
  → DQBUF等待PXP完成
~~~

#### 为什么采用USERPTR

摄像头Buffer已经由CSI驱动通过MMAP提供。使用USERPTR可以把该Buffer地址直接提交给PXP接口，避免APP先复制到另一块PXP输入缓冲区。

#### 为什么增加每个ioctl的独立perror

原来只看到：

~~~text
PXP init failed
~~~

无法判断是打开节点、设置格式、设置区域还是申请Buffer失败。

现在可以准确打印：

~~~text
PXP: VIDIOC_S_FMT(VIDEO_OUTPUT): Device or resource busy
~~~

这使后续能够从具体ioctl追到 <code>acquire_dma_channel()</code>。

### 2.10 修改前后数据流对照

| 阶段 | 老师原版CPU路径 | 当前PXP路径 |
|---|---|---|
| 摄像头采集 | CSI MMAP | CSI MMAP |
| YUYV转RGB | APP软件循环 | PXP硬件 |
| 缩放 | <code>PicZoom()</code> | PXP硬件 |
| 合并到Framebuffer | <code>PicMerge()</code> | <code>mxc_pxp_v4l2.c</code> |
| 刷新LCD | APP调用Flush | PXP驱动处理 |
| CSI Buffer归还 | APP处理完成后QBUF | PXP DQBUF完成后QBUF |
| 故障回退 | 无 | 可以退回原CPU路径 |

---

## 3. 设备节点

当前开发板：

~~~text
/dev/video0: PxP
/dev/video1: mx6s-csi
/dev/fb0:    LCD framebuffer
~~~

不要永久假设 video 编号固定，应这样确认：

~~~sh
for v in /sys/class/video4linux/video*; do
    echo -n "$v: "
    cat "$v/name"
done
~~~

---

## 4. 编译与运行

### 3.1 编译

~~~sh
cd notes/09-v4l2/video2lcd
make clean
make
~~~

交叉编译器前缀在 Makefile 中配置为 <code>arm-linux-</code>。

### 3.2 PXP 硬件路径

~~~sh
./video2lcd /dev/video1 /dev/video0
~~~

正常日志类似：

~~~text
/dev/video1 supports streaming i/o
Camera format returned: 640x480, bytesperline=1280, sizeimage=614400
PXP: mxc VIDEO_OUTPUT path, 640x480 -> (112,0) 800x600
video2lcd: mxc PXP hardware display path
~~~

### 3.3 CPU 对比路径

~~~sh
./video2lcd /dev/video1 --cpu
~~~

如果 PXP 初始化失败，程序会自动退回 CPU 路径。

---

## 5. 当前 PXP 接口模型

当前内核使用：

~~~text
drivers/media/platform/mxc/output/mxc_pxp_v4l2.c
~~~

它不是普通 V4L2 M2M 驱动。当前模型是：

~~~text
APP
  └─ VIDEO_OUTPUT：提交摄像头YUYV帧
       └─ mxc_pxp_v4l2
            ├─ 通过DMAEngine申请PXP Channel
            ├─ YUV→RGB
            ├─ 缩放
            └─ 输出到Framebuffer
~~~

老师参考资料中的 <code>imx-pxp.c</code> 是另一种 M2M 模型：

~~~text
VIDEO_OUTPUT：输入YUYV
VIDEO_CAPTURE：输出RGB
~~~

它可以用来理解 PXP，但不能把其 APP ioctl 流程直接套在当前 <code>mxc_pxp_v4l2.c</code> 上。

---

## 6. video2lcd 中增加的 PXP 流程

主要代码：

~~~text
pxp/pxp.c
include/pxp.h
main.c
~~~

### 5.1 初始化

~~~text
open(/dev/video0)
  → VIDIOC_QUERYCAP
  → 检查 V4L2_CAP_VIDEO_OUTPUT
  → VIDIOC_S_OUTPUT
  → VIDIOC_S_FMT(VIDEO_OUTPUT)
  → VIDIOC_S_FMT(VIDEO_OUTPUT_OVERLAY)
  → VIDIOC_S_CROP
  → VIDIOC_REQBUFS(USERPTR)
~~~

- <code>VIDEO_OUTPUT</code>：PXP 输入图像的宽、高和 YUYV 格式。
- <code>VIDEO_OUTPUT_OVERLAY</code>：源图参与处理的区域，对应驱动的 <code>srect</code>。
- <code>VIDIOC_S_CROP</code>：Framebuffer 上的目标区域，对应驱动的 <code>drect</code>。

### 5.2 每帧处理

~~~text
CSI DQBUF
  → 得到一帧YUYV及其USERPTR
  → PXP QBUF(USERPTR)
  → 第一次提交时STREAMON
  → PXP完成YUV→RGB和缩放
  → PXP DQBUF
  → CSI QBUF归还摄像头Buffer
~~~

必须等 PXP <code>DQBUF</code> 后，才能把同一个 CSI Buffer 归还给摄像头，否则两个硬件可能同时访问该内存。

### 5.3 细化错误日志

PXP 初始化已经为每一步保留独立错误日志：

~~~text
PXP: VIDIOC_QUERYCAP
PXP: VIDIOC_S_OUTPUT
PXP: VIDIOC_S_FMT(VIDEO_OUTPUT)
PXP: VIDIOC_S_FMT(VIDEO_OUTPUT_OVERLAY)
PXP: VIDIOC_S_CROP
PXP: VIDIOC_REQBUFS
~~~

只打印统一的 <code>PXP init failed</code>，无法判断故障发生在哪一层。

---

## 7. 摄像头分辨率与显示区域

### 6.1 为什么不能直接请求1024x600

旧代码直接使用 LCD 分辨率配置摄像头：

~~~c
tV4l2Fmt.fmt.pix.width  = iLcdWidth;
tV4l2Fmt.fmt.pix.height = iLcdHeigt;
~~~

LCD 是1024x600，但当前 OV5640 V3 没有对应的有效 Sensor 模式。CSI 驱动可能表面返回1024x600，但 Sensor 实际输出布局没有正确切换，最终出现：

- 黑白或颜色异常；
- 横条纹；
- 一屏多个重复小画面；
- 帧内存真实布局与程序理解不一致。

当前使用经过验证的尺寸：

~~~c
tV4l2Fmt.fmt.pix.width  = 640;
tV4l2Fmt.fmt.pix.height = 480;
~~~

并打印 <code>VIDIOC_S_FMT</code> 的返回值：

~~~c
printf("Camera format returned: %dx%d, bytesperline=%d, sizeimage=%d\n",
       tV4l2Fmt.fmt.pix.width,
       tV4l2Fmt.fmt.pix.height,
       tV4l2Fmt.fmt.pix.bytesperline,
       tV4l2Fmt.fmt.pix.sizeimage);
~~~

后续应使用 <code>VIDIOC_ENUM_FRAMESIZES</code> 枚举 Sensor 支持的尺寸，再选择适合 LCD 的有效模式。

### 6.2 为什么最初只显示640x480

旧的 <code>CalcDisplayRect()</code> 只在源图大于 LCD 时缩小，源图较小时缩放系数保持1.0：

~~~text
源图：640x480
LCD： 1024x600
目标：640x480
位置：(192,60)
~~~

现在同时计算横向和纵向比例：

~~~c
fWidthScale = (float)iLcdWidth / iSrcWidth;
fHeightScale = (float)iLcdHeight / iSrcHeight;
fScale = (fWidthScale < fHeightScale) ?
         fWidthScale : fHeightScale;
~~~

结果：

~~~text
640x480 → 800x600
目标位置：(112,0)
~~~

这样保持完整的4:3画面，不发生拉伸。LCD 左右保留黑边属于正常结果。

---

## 8. FAQ / 调试记录

### FAQ-01：采集测试正常，video2lcd却黑白、横条并出现16个小画面，为什么？

本次根因不是 OV5640、DVP 或 CSI 损坏，而是两个 APP 请求的尺寸不同：

~~~text
ov5640_camera_test：640x480 YUYV，正常
video2lcd：请求1024x600 YUYV，异常
~~~

当最小采集程序正常时，应优先检查 APP 的：

- <code>VIDIOC_S_FMT</code> 请求值；
- 驱动实际返回的宽高；
- <code>bytesperline</code>；
- <code>sizeimage</code>；
- 每帧 <code>bytesused</code>；
- PXP 输入宽高。

多个小画面通常指向宽高或步长错误。YUYV/UYVY 顺序错误更常表现为颜色异常，不会稳定地产生4×4重复画面。

### FAQ-02：PXP只把640x480显示在LCD中间？

因为旧的 <code>CalcDisplayRect()</code> 禁止放大，只允许缩小。修正后目标区域为800x600。

### FAQ-03：为什么保持比例后不是1024x600全屏？

640x480是4:3，1024x600不是4:3。完整且不变形时最大尺寸是800x600，因此左右留边正确。

如果强制铺满，只能选择横向拉伸，或者保持比例后裁掉部分画面。

### FAQ-04：VIDIOC_S_FMT(VIDEO_OUTPUT)返回Device or resource busy？

调用链：

~~~text
video2lcd
  → VIDIOC_S_FMT(VIDEO_OUTPUT)
  → mxc_pxp_v4l2.c:pxp_s_fmt_video_output()
  → acquire_dma_channel()
  → dma_request_channel()
  → 没有找到PXP DMA Channel
  → -EBUSY
~~~

本次根因是设备树错误覆盖了底层 PXP 硬件节点：

~~~dts
&pxp {
    compatible = "fsl,imx6ul-pxp-v4l2";
    status = "okay";
};
~~~

正确结构需要保留两个不同层次的节点：

~~~dts
/* 上层V4L2客户端，生成PxP /dev/videoX */
pxp_v4l2 {
    compatible = "fsl,imx6ul-pxp-v4l2",
                 "fsl,imx6sx-pxp-v4l2",
                 "fsl,imx6sl-pxp-v4l2";
    status = "okay";
};

/* 只启用dtsi中原有的PXP硬件节点 */
&pxp {
    status = "okay";
};
~~~

<code>imx6ull.dtsi</code> 中硬件节点原本使用：

~~~dts
compatible = "fsl,imx6ull-pxp-dma",
             "fsl,imx7d-pxp-dma";
~~~

调试时只读检查设备树，并由开发者手工修改；不要把硬件 compatible 覆盖成 V4L2 compatible。

#### FAQ-04.1：为什么一看到EBUSY，不能马上断定“有其他进程占用了/dev/video0”？

<code>EBUSY</code> 只是内核返回给用户态的错误码，必须找到驱动中具体哪一行返回它。

本次日志为：

~~~text
PXP: VIDIOC_S_FMT(VIDEO_OUTPUT): Device or resource busy
~~~

这已经说明：

1. <code>open("/dev/video0")</code> 成功，否则日志会是 <code>PXP: open</code>。
2. <code>VIDIOC_QUERYCAP</code> 成功。
3. <code>VIDIOC_S_OUTPUT</code> 成功。
4. 失败点准确落在 <code>VIDIOC_S_FMT(VIDEO_OUTPUT)</code>。

板上还执行了：

~~~sh
ps | grep -E 'pxp|video2lcd|mxc-csi' | grep -v grep
ls -l /proc/*/fd/* 2>/dev/null | grep -E '/dev/video0|pxp'
~~~

均未发现其他进程持有PXP节点。因此“另一个APP占用/dev/video0”被排除，但仍需继续追踪驱动为何返回 <code>-EBUSY</code>。

#### FAQ-04.2：从APP的ioctl怎样找到内核中的处理函数？

APP代码位置：

~~~text
notes/09-v4l2/video2lcd/pxp/pxp.c
SetInputFormat()
~~~

关键调用：

~~~c
tFmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
ioctl(g_iPxpFd, VIDIOC_S_FMT, &tFmt);
~~~

V4L2核心根据 ioctl 类型和 <code>v4l2_ioctl_ops</code> 分发表找到：

~~~text
drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:1212
~~~

~~~c
.vidioc_s_fmt_vid_out = pxp_s_fmt_video_output,
~~~

因此调用链为：

~~~text
APP: ioctl(VIDIOC_S_FMT)
  → V4L2核心 video_ioctl2
  → vidioc_s_fmt_vid_out
  → mxc_pxp_v4l2.c:pxp_s_fmt_video_output()
~~~

#### FAQ-04.3：驱动中哪一行真正返回了EBUSY？

<code>mxc_pxp_v4l2.c:564</code>：

~~~c
static int pxp_s_fmt_video_output(struct file *file, void *fh,
                                 struct v4l2_format *f)
{
    ...
    ret = acquire_dma_channel(pxp);
    if (ret < 0)
        return ret;

    ret = pxp_try_fmt_video_output(file, fh, f);
    ...
}
~~~

注意顺序：驱动先申请PXP DMA Channel，然后才检查并保存输入格式。

继续进入 <code>mxc_pxp_v4l2.c:254</code>：

~~~c
dma_cap_zero(mask);
dma_cap_set(DMA_SLAVE, mask);
dma_cap_set(DMA_PRIVATE, mask);

chan = dma_request_channel(mask, chan_filter, NULL);
if (!chan)
    return -EBUSY;
~~~

<code>chan_filter()</code> 位于 <code>mxc_pxp_v4l2.c:246</code>：

~~~c
static bool chan_filter(struct dma_chan *chan, void *arg)
{
    if (imx_dma_is_pxp(chan))
        return true;
    else
        return false;
}
~~~

所以这里的 <code>-EBUSY</code> 的准确含义是：

~~~text
DMAEngine中没有找到一个同时满足：
  DMA_SLAVE
  DMA_PRIVATE
  imx_dma_is_pxp(chan) == true
的可用Channel
~~~

它有两类可能：

1. PXP DMA provider 已注册，但所有Channel都被其他客户端占用。
2. PXP DMA provider根本没有probe，因此系统中一个PXP Channel也没有。

本项目最终属于第二种。

必须注意：到这一步还不能直接断言“设备树错误”。<code>-EBUSY</code> 是故障现象，不是设备树结论。

从现象到根因的证据关系如下：

| 证据 | 能证明什么 | 暂时不能证明什么 |
|---|---|---|
| <code>VIDIOC_S_FMT</code>返回EBUSY | 没有申请到符合条件的PXP Channel | 不知道是占用、provider未注册还是匹配错误 |
| 没有进程打开PXP节点 | 不是普通用户态APP占用 <code>/dev/video0</code> | 仍不能排除内核客户端或provider缺失 |
| <code>/dev/video0</code>存在 | 上层 <code>mxc_pxp_v4l2.c</code> 已probe | 不代表底层 <code>pxp_dma_v3.c</code> 已probe |
| <code>imx-pxp-v3</code>下没有硬件绑定 | 底层provider没有绑定到PXP硬件节点 | 还需查明为什么没有绑定 |
| 实际DTS覆盖了硬件compatible | 硬件节点无法匹配 <code>pxp_dma_v3.c</code> | 此时已经得到直接的静态配置证据 |
| 只修正DTB后同一ioctl成功 | 修正硬件节点后Channel恢复 | 构成修复前后的因果验证 |

因此严谨结论应写成：

~~~text
dma_request_channel()返回NULL
  → 先得到“没有可用PXP Channel”
  → 再检查Channel provider是否注册
  → 发现imx-pxp-v3没有绑定硬件节点
  → 对比硬件节点compatible与驱动of_match_table
  → 发现compatible被板级DTS覆盖
  → 修正DTB后Channel申请成功
  → 最终确认本次根因是设备树覆盖
~~~

#### FAQ-04.4：为什么已经有/dev/video0，底层PXP仍可能没有probe？

因为PXP由两个不同驱动层组成，它们分别匹配两个不同的设备树节点。

##### 上层：PXP V4L2客户端

设备树：

~~~dts
pxp_v4l2 {
    compatible = "fsl,imx6ul-pxp-v4l2",
                 "fsl,imx6sx-pxp-v4l2",
                 "fsl,imx6sl-pxp-v4l2";
    status = "okay";
};
~~~

驱动：

~~~text
drivers/media/platform/mxc/output/mxc_pxp_v4l2.c
pxp_probe()：约1247行
~~~

该 probe 执行：

~~~text
v4l2_device_register()
  → video_device_alloc()
  → video_register_device()
  → 生成名称为PxP的/dev/videoX
~~~

所以看到：

~~~text
/dev/video0
/sys/class/video4linux/video0/name = PxP
dmesg: pxp-v4l2 pxp_v4l2: initialized
~~~

只能证明上层V4L2外壳已经注册。

##### 底层：PXP DMAEngine provider

设备树原始节点位于：

~~~text
arch/arm/boot/dts/imx6ull.dtsi:1027
~~~

~~~dts
pxp: pxp@021cc000 {
    compatible = "fsl,imx6ull-pxp-dma",
                 "fsl,imx7d-pxp-dma";
    reg = <0x021cc000 0x4000>;
    interrupts = <...>;
    clocks = <...>;
    status = "disabled";
};
~~~

板级DTS只负责启用：

~~~dts
&pxp {
    status = "okay";
};
~~~

底层驱动：

~~~text
drivers/dma/pxp/pxp_dma_v3.c
~~~

OF匹配表位于约7517行：

~~~c
{ .compatible = "fsl,imx7d-pxp-dma", ... },
{ .compatible = "fsl,imx6ull-pxp-dma", ... },
~~~

平台驱动名称位于约8086行：

~~~c
.name = "imx-pxp-v3",
~~~

底层 <code>pxp_probe()</code> 位于约7883行，负责：

~~~text
ioremap PXP寄存器
  → 获取时钟
  → 申请PXP IRQ
  → 创建pxp_dispatch内核线程
  → 初始化PXP虚拟Channel
  → dma_async_device_register()
~~~

只有它成功，<code>dma_request_channel()</code> 才可能找到PXP Channel。

#### FAQ-04.5：本次设备树错误为什么只影响底层，不影响/dev/video0？

板级DTS中曾经存在另一段：

~~~dts
&pxp {
    compatible = "fsl,imx6ul-pxp-v4l2";
    status = "okay";
};
~~~

设备树的 <code>&pxp</code> 是对 <code>imx6ull.dtsi</code> 中同一个硬件节点的补充或覆盖，不是在创建一个新的PXP硬件。

这段代码把原来的：

~~~text
fsl,imx6ull-pxp-dma
~~~

覆盖成：

~~~text
fsl,imx6ul-pxp-v4l2
~~~

结果：

~~~text
根节点pxp_v4l2
  → 仍能匹配mxc_pxp_v4l2.c
  → /dev/video0仍然存在

021cc000 PXP硬件节点
  → 不再匹配pxp_dma_v3.c
  → 不注册DMAEngine和PXP Channel
  → S_FMT申请Channel失败
  → -EBUSY
~~~

这就是“接口节点存在，但硬件后端不存在”的完整原因。

#### FAQ-04.6：当时的分析过程如何一步步收敛？

~~~text
第一步：细化APP错误日志
  原来：PXP init failed
  修改：每个ioctl单独perror
  结果：锁定VIDIOC_S_FMT(VIDEO_OUTPUT)

第二步：排除普通占用
  检查ps和/proc/*/fd
  结果：没有其他进程占用/dev/video0

第三步：从ioctl表追代码
  vidioc_s_fmt_vid_out
    → pxp_s_fmt_video_output()
    → acquire_dma_channel()
    → dma_request_channel()
    → -EBUSY

第四步：确认/dev/video0只代表上层
  dmesg只有pxp-v4l2 initialized
  上层probe并不注册DMA Channel

第五步：寻找DMA Channel由谁提供
  chan_filter要求imx_dma_is_pxp()
  找到drivers/dma/pxp/pxp_dma_v3.c

第六步：核对底层驱动OF匹配
  只接受fsl,imx6ull-pxp-dma等compatible

第七步：核对实际板级DTS
  发现&pxp的compatible被覆盖成V4L2客户端类型

第八步：恢复硬件compatible并重新生成DTB
  底层provider注册
  dma_request_channel成功
  VIDIOC_S_FMT成功
~~~

#### FAQ-04.7：修复后怎样证明上下两层都工作？

检查V4L2客户端：

~~~sh
cat /sys/class/video4linux/video0/name
~~~

预期：

~~~text
PxP
~~~

检查底层platform driver：

~~~sh
ls -l /sys/bus/platform/drivers/imx-pxp-v3/
~~~

预期出现类似：

~~~text
21cc000.pxp
~~~

检查运行时设备树中的compatible：

~~~sh
for f in $(find /proc/device-tree -name compatible | grep pxp); do
    echo "$f"
    tr '\0' '\n' < "$f"
done
~~~

应同时看到两类compatible：

~~~text
fsl,imx6ul-pxp-v4l2
fsl,imx6ull-pxp-dma
fsl,imx7d-pxp-dma
~~~

最终APP应越过 <code>VIDIOC_S_FMT(VIDEO_OUTPUT)</code>，打印：

~~~text
PXP: mxc VIDEO_OUTPUT path, ...
video2lcd: mxc PXP hardware display path
~~~

#### FAQ-04.8：Channel成功申请后，一帧图像怎样真正进入PXP？

完整逐帧路径如下：

~~~text
1. mx6s_capture完成一帧
2. video2lcd从/dev/video1执行DQBUF
3. APP得到CSI Buffer的USERPTR和bytesused
4. PxpDisplayFrame()向/dev/video0执行QBUF
5. mxc_pxp_v4l2.c:pxp_buf_prepare()
6. videobuf_iolock()映射USERPTR
7. device_prep_slave_sg()创建DMA事务描述符
8. 描述符保存：
     S0输入物理地址
     PXP RGB输出地址
     源区域srect
     目标区域drect
     YUV/RGB格式和宽高
9. mxc_pxp_v4l2.c:pxp_buf_queue()
10. tx_submit()提交描述符
11. dma_async_issue_pending()启动DMAEngine
12. pxp_dma_v3.c:pxp_issue_pending()
13. 唤醒pxp_dispatch内核线程
14. pxpdma_dostart_work()
15. pxp_config()配置PXP寄存器
16. pxp_start()/pxp_start2()启动PXP硬件
17. PXP读取YUYV，完成颜色转换和缩放
18. PXP写入RGB输出Buffer/Framebuffer
19. PXP硬件产生完成中断
20. pxp_dma_v3.c:pxp_irq()
21. 调用DMA完成callback
22. mxc_pxp_v4l2.c:video_dma_done()
23. Buffer状态改为VIDEOBUF_DONE并唤醒等待者
24. video2lcd的PXP DQBUF返回
25. APP再把原CSI Buffer QBUF归还给摄像头
~~~

关键源码位置：

| 阶段 | 文件与位置 |
|---|---|
| APP设置PXP输入格式 | <code>video2lcd/pxp/pxp.c：SetInputFormat()</code> |
| V4L2 ioctl映射 | <code>mxc_pxp_v4l2.c:1212</code> |
| 设置输入格式、申请Channel | <code>mxc_pxp_v4l2.c:564</code> |
| PXP Channel过滤与申请 | <code>mxc_pxp_v4l2.c:246、254</code> |
| USERPTR准备与DMA描述符 | <code>mxc_pxp_v4l2.c:约779</code> |
| 提交并启动DMAEngine | <code>mxc_pxp_v4l2.c:879</code> |
| V4L2层DMA完成回调 | <code>mxc_pxp_v4l2.c:210</code> |
| 底层准备DMA描述符 | <code>pxp_dma_v3.c:4150</code> |
| 底层issue pending | <code>pxp_dma_v3.c:4225</code> |
| 配置并启动PXP | <code>pxp_dma_v3.c:约3895</code> |
| PXP硬件中断 | <code>pxp_dma_v3.c:4011</code> |
| 注册PXP DMA Channels | <code>pxp_dma_v3.c:约7415</code> |
| 底层PXP probe | <code>pxp_dma_v3.c:7883</code> |

最重要的架构结论：

~~~text
mxc_pxp_v4l2.c不是直接操作PXP寄存器的最终硬件驱动。

它是：
  V4L2用户接口
  + DMAEngine客户端

pxp_dma_v3.c才是：
  PXP寄存器
  + 时钟
  + IRQ
  + DMAEngine provider
~~~

### FAQ-05：dmesg显示pxp-v4l2 initialized，为什么仍申请不到PXP？

该日志只证明上层 <code>mxc_pxp_v4l2.c</code> 注册成功，不证明底层 <code>pxp_dma_v3.c</code> 已匹配。

~~~text
mxc_pxp_v4l2.c：用户接口层
pxp_dma_v3.c：寄存器、IRQ、DMAEngine层
~~~

检查底层绑定：

~~~sh
ls -l /sys/bus/platform/drivers/imx-pxp-v3/
~~~

预期能看到类似 <code>21cc000.pxp</code>。

### FAQ-06：lsmod中没有PXP，是不是驱动没加载？

不一定。配置为 <code>=y</code> 时驱动编进 zImage，不会出现在 <code>lsmod</code>；只有 <code>=m</code> 的模块才会显示。

当前相关配置：

~~~text
CONFIG_VIDEO_MXC_PXP_V4L2=y
CONFIG_MXC_PXP_V3=y
~~~

### FAQ-07：为什么OV5640 V3之前必须手动insmod？

<code>CONFIG_MXC_CAMERA_OV5640_V3=m</code> 表示驱动是模块。自动加载链路为：

~~~text
设备树compatible
  → 设备modalias
  → modules.alias
  → /lib/modules/$(uname -r)中的ko
  → udev/mdev调用modprobe
~~~

当时内核 build 目录有 <code>ov5640_camera_v3.ko</code>，但 <code>output/target/lib/modules/4.9.88</code> 中没有该模块，所以根文件系统无法自动加载，只能手工复制并执行 <code>insmod</code>。

构建后应检查：

~~~sh
find output/target/lib/modules -name 'ov5640_camera_v3.ko'
grep -i ov5640_v3 output/target/lib/modules/4.9.88/modules.alias
grep -i ov5640_camera_v3 output/target/lib/modules/4.9.88/modules.dep
~~~

### FAQ-08：为什么不能直接使用老师参考的PXP M2M流程？

当前 <code>/dev/video0</code> 使用 <code>mxc_pxp_v4l2.c</code>，提供 <code>VIDEO_OUTPUT</code> 并直接输出到 framebuffer。

老师参考中的 <code>imx-pxp.c</code> 是 <code>VIDEO_OUTPUT + VIDEO_CAPTURE</code> M2M模型。接口模型不同，不能只根据设备名都叫PXP就复用相同 ioctl 流程。

应以 <code>VIDIOC_QUERYCAP</code> 和驱动源码中的 <code>v4l2_ioctl_ops</code> 为准。

### FAQ-09：PXP失败后为什么仍能显示画面？

程序保留了CPU回退：

~~~text
PXP初始化失败
  → PxpExit()
  → 软件YUYV转RGB
  → 写Framebuffer
~~~

看到下面日志只说明CPU路径工作，不代表PXP已工作：

~~~text
PXP: init failed, fallback to CPU
video2lcd: CPU YUV-to-RGB path
~~~

### FAQ-10：怎样证明当前确实走PXP硬件路径？

应同时满足：

~~~text
/dev/video0名称为PxP
PXP初始化全部ioctl成功
程序打印mxc PXP hardware display path
没有fallback to CPU
~~~

示例：

~~~text
PXP: mxc VIDEO_OUTPUT path, 640x480 -> (112,0) 800x600
video2lcd: mxc PXP hardware display path
~~~

---

## 9. 推荐排查顺序

1. 用 <code>ov5640_camera_test</code> 导出640x480 YUYV原始帧。
2. 确认原始帧、宽高和 <code>bytesused</code> 正确。
3. 确认 <code>/dev/video1</code> 是 <code>mx6s-csi</code>。
4. 确认 OV5640 I2C 设备已经绑定V3驱动。
5. 确认 <code>/dev/video0</code> 是 <code>PxP</code>。
6. 确认底层 <code>imx-pxp-v3</code> 已绑定硬件节点。
7. 查看 PXP 具体失败的 ioctl。
8. 对比摄像头返回尺寸与PXP输入尺寸。
9. 最后检查目标显示区域和Framebuffer参数。

核心原则：

~~~text
先证明输入帧正确
  → 再证明PXP收到的格式和尺寸正确
  → 最后检查RGB输出和LCD显示区域
~~~

---

## 10. 当前限制与后续改进

- 摄像头采集尺寸暂时固定为640x480。
- 后续使用 <code>VIDIOC_ENUM_FRAMESIZES</code> 自动选择有效分辨率。
- 当前保持比例完整显示，不强制拉伸全屏。
- 可以增加帧率和CPU占用统计，对比CPU与PXP路径。
- 可以增加 <code>--width</code>、<code>--height</code>、<code>--stretch</code> 和 <code>--crop</code> 参数。
- PXP通过USERPTR提交CSI缓冲区，后续应继续关注缓存一致性和DMA映射要求。
