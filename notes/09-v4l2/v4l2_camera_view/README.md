# v4l2_camera_view

这是一个面向 IMX6ULL 平台 学习用的 V4L2 摄像头实时显示示例。目标不是做完整播放器，而是理解 V4L2 数据采集， FB 的操作只是为了快速展示，
实际业务只有快速倒车会用 FB 的方式，其它情况还是要封装视频帧，这个暂时不走这一步：
本文档有部分内容不属于 UVC 摄像头链路，是基于当前业务中 T113 平台 CVBS 摄像头参考总结；
```text
/dev/videoX 摄像头
  -> V4L2 mmap 采集
  -> 取出一帧 YUYV/RGB565 数据
  -> 必要时做颜色格式转换
  -> 写入 /dev/fbN framebuffer
  -> LCD 显示
```

## 项目结构

```text
v4l2_camera_view/
  Makefile              编译规则
  main.c                V4L2 采集、framebuffer 初始化、主循环显示
  imx_fb_alpha.h        i.MX framebuffer global alpha ioctl 的最小定义
  yuyv_rgb565.c         YUV 到 RGB565/RGB888 的颜色转换
  yuyv_rgb565.h         颜色转换接口
  test_yuyv_rgb565.c    颜色转换的本地 self-test
```

`main.c` 是主流程，负责：

1. 打开 LCD framebuffer。
2. 打开摄像头设备。
3. 设置摄像头采集格式。
4. 申请并映射 V4L2 buffer。
5. 启动采集。
6. 循环取帧、转换、显示、归还 buffer。
7. 退出时停止采集并释放资源。

`yuyv_rgb565.c` 只负责像素格式转换，和 V4L2、framebuffer 没有直接关系。


## V4L2 操作流程

V4L2 是 Linux 的视频设备框架。应用程序通过 `/dev/videoX` 和一系列 `ioctl` 来获取数据和配置摄像头。

本程序使用的是 streaming mmap 模式，大致流程如下：

```text
open("/dev/video1")
  -> VIDIOC_QUERYCAP
  -> VIDIOC_S_FMT
  -> VIDIOC_REQBUFS
  -> VIDIOC_QUERYBUF
  -> mmap
  -> VIDIOC_QBUF
  -> VIDIOC_STREAMON
  -> poll
  -> VIDIOC_DQBUF
  -> 处理这一帧
  -> VIDIOC_QBUF
  -> ...
  -> VIDIOC_STREAMOFF
  -> munmap
  -> close
```

### 1. 打开摄像头

```c
fd = open("/dev/video1", O_RDWR);
```

### 2. 查询设备能力

```c
ioctl(fd, VIDIOC_QUERYCAP, &cap);
```

重点检查：

```text
V4L2_CAP_VIDEO_CAPTURE   是否是视频采集设备
V4L2_CAP_STREAMING       是否支持 mmap/streaming 方式
```

### 3. 设置采集格式

```c
fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
fmt.fmt.pix.width = 640;
fmt.fmt.pix.height = 480;
fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
fmt.fmt.pix.field = V4L2_FIELD_ANY;
ioctl(fd, VIDIOC_S_FMT, &fmt);
```

`VIDIOC_S_FMT` 成功后，驱动可能会修改你传入的宽、高、像素格式。比如你请求 `1920x1080`，摄像头不支持，但是驱动会自动调整到最靠进设置的值例如： `1280x1024`。

我这里的 PixelFormat 设置为 `V4L2_PIX_FMT_YUYV` 是因为摄像头枚举出来支持 YUYV，且 YUYV 可以直接用 C 代码转换成 RGB 后显示，不需要 JPEG 解码库。

### 4. 申请 buffer

```c
req.count = 4;
req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
req.memory = V4L2_MEMORY_MMAP;
ioctl(fd, VIDIOC_REQBUFS, &req);
```

这一步向驱动申请视频缓冲区。驱动会分配若干块 buffer，用于存放摄像头采集到的帧数据。

### 5. 查询并映射 buffer

```c
ioctl(fd, VIDIOC_QUERYBUF, &buf);
mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
```

`VIDIOC_QUERYBUF` 用来查询每个 buffer 的长度和 offset。

`mmap` 把驱动中的 buffer 映射到用户空间。这样应用程序拿到一帧时，不需要再额外复制一份数据，可以直接访问映射出来的地址。

### 6. buffer 入队

```c
ioctl(fd, VIDIOC_QBUF, &buf);
```

`QBUF` 表示把空 buffer 交给驱动。驱动拿到空 buffer 后，摄像头采集到的数据就可以填进去。

### 7. 启动采集

```c
type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_STREAMON, &type);
```

这一步之后，摄像头开始输出视频流，驱动开始填充队列里的 buffer。

### 8. 等待一帧

```c
poll(&pfd, 1, 1000);
```

`poll` 等待摄像头设备变成可读。可读通常表示至少有一个 buffer 已经采集完成，可以取出来处理。

### 9. 取出一帧

```c
ioctl(fd, VIDIOC_DQBUF, &buf);
```

`DQBUF` 从驱动队列中取出一个已经填充好的 buffer。

取出后：

```text
buf.index     表示是哪一个 mmap buffer
buf.bytesused 表示这一帧实际使用了多少字节
```

### 10. 处理并归还 buffer

处理完这一帧后，必须重新入队：

```c
ioctl(fd, VIDIOC_QBUF, &buf);
```

如果不归还 buffer，驱动可用 buffer 会越来越少，最后采集会停住。

## 像素格式

项目里面会接触几个容易混淆的格式。也是平时技术对接较为困惑的部分；

### YUYV

当前的USB摄像头枚举出了：

```text
YUYV 4:2:2
```

YUYV 是一种 YUV422 内存格式。两个像素共用一组 U/V 色度分量：

```text
Y0 U0 Y1 V0
```

含义：

```text
Y0  第 0 个像素亮度
U0  两个像素共用的蓝色色度
Y1  第 1 个像素亮度
V0  两个像素共用的红色色度
```

LCD framebuffer 一般不能直接显示 YUYV，所以要转成 RGB。

### RGB565

RGB565 是 16bpp 显示格式，一个像素占 16bit：

```text
R 5bit
G 6bit
B 5bit
```

如果 LCD framebuffer 是 16bpp，就需要程序会把 YUYV 转成 RGB565，然后直接写入 framebuffer。

### 32bpp framebuffer

当前系统使用的是 iMX6ULL 现在 framebuffer 是32bpp：

```text
32bpp
```

32bpp 常见格式有：

```text
XRGB8888
ARGB8888
BGRA8888
```

不同平台的 R/G/B 通道位置可能不同，所以程序不能只假设某一种顺序。当前代码会读取：

```c
fb_var_screeninfo.red.offset
fb_var_screeninfo.green.offset
fb_var_screeninfo.blue.offset
```

然后按实际 offset 打包 32 位像素。

### BT.656 和 RGB565 的区别

BT.656 不是 RGB565 这种显示像素格式。

```text
BT.656 = 视频输入侧的数字传输接口/时序标准
RGB565 = 显示侧/内存侧的像素格式
```

常用的链路是：

```text
CVBS/AHD 摄像头
  -> video decoder
  -> BT.656
  -> SoC VIN
  -> V4L2 输出 YUYV
  -> 应用转 RGB565/RGB888
  -> framebuffer
  -> LCD
```


## framebuffer 显示流程

framebuffer 是 Linux 里比较传统的显示接口，通常是：

```text
/dev/fb0
/dev/fb1
```

应用程序打开 framebuffer：

```c
fb_fd = open("/dev/fbN", O_RDWR);
```

查询屏幕参数：

```c
ioctl(fb_fd, FBIOGET_VSCREENINFO, &var);
ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
```

关键字段：

```text
var.xres              可见宽度
var.yres              可见高度
var.xres_virtual      虚拟宽度
var.yres_virtual      虚拟高度
var.bits_per_pixel    每像素位数
var.red.offset        red 分量在一个像素里的 bit 偏移
var.green.offset      green 分量在一个像素里的 bit 偏移
var.blue.offset       blue 分量在一个像素里的 bit 偏移
fix.line_length       一行显存占多少字节
fix.smem_len          framebuffer 显存总大小
```

映射显存：

```c
fb_mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
```

之后只要往 `fb_mem` 写像素，LCD 就会显示对应内容。

## 从摄像头帧到 LCD 的数据路径

本程序主循环的数据流是：

```text
poll 等待摄像头
  -> VIDIOC_DQBUF 取出一帧
  -> 判断摄像头帧格式
  -> 判断 framebuffer bpp
  -> 转换或打包像素
  -> 写入 framebuffer
  -> VIDIOC_QBUF 归还 buffer
```

16bpp LCD：

```text
YUYV
  -> yuyv_to_rgb565()
  -> draw_rgb565_to_fb()
  -> /dev/fbN
```

32bpp LCD：

```text
YUYV
  -> yuv_to_rgb888_pixel()
  -> pack_fb32_pixel()
  -> /dev/fbN
```

如果摄像头本身已经输出 RGB565：

```text
RGB565
  -> 16bpp fb: 直接写入
  -> 32bpp fb: RGB565 转 RGB888，再按 fb offset 打包
```

## fb0、fb1 和 LVGL

如果系统里有：

```text
/dev/fb0
/dev/fb1
```

它可能表示两个 framebuffer layer，也可能是平台显示驱动暴露出的两个显示面。常见情况是：

```text
fb0 -> 主 UI 层
fb1 -> overlay/video 层
```

但这不是绝对的，要看平台驱动。

可以查看：

```sh
cat /sys/class/graphics/fb0/name
cat /sys/class/graphics/fb1/name
cat /sys/class/graphics/fb0/modes
cat /sys/class/graphics/fb1/modes
cat /sys/class/graphics/fb0/bits_per_pixel
cat /sys/class/graphics/fb1/bits_per_pixel
cat /sys/class/graphics/fb0/virtual_size
cat /sys/class/graphics/fb1/virtual_size
```

如果 LVGL 正在写 `/dev/fb0`，而本程序也写 `/dev/fb0`，就会出现两个程序抢同一个显存的问题：

```text
LVGL -> /dev/fb0
camera demo -> /dev/fb0
```

谁最后写，屏幕上就显示谁的内容。

快速倒车常见有两种做法：

```text
方案 1：单 framebuffer
LVGL 暂停刷新
摄像头程序独占 /dev/fb0
倒车退出后 LVGL 全屏重绘
```

```text
方案 2：多 layer / overlay
LVGL 写 fb0
摄像头写 fb1
显示控制器把 fb1 盖在 fb0 上
倒车退出后隐藏 fb1
```

如果想让摄像头画面不要直接覆盖 LVGL 主界面，可以通过第二个参数指定 `/dev/fb1`，再观察它是否是 overlay 层。

从当前的 `mxsfb.c` 看，fb1 overlay 的 `global_alpha` 在驱动初始化时固定为 255，用户态不一定能通过 ioctl 改它。真正打开/关闭 fb1 overlay 的路径是 `overlayfb_blank()`：


## 常用排查命令

查看摄像头支持格式：

```sh
v4l2-ctl -d /dev/video1 --list-formats-ext
```

查看 framebuffer：

```sh
cat /sys/class/graphics/fb0/modes
cat /sys/class/graphics/fb0/bits_per_pixel
cat /sys/class/graphics/fb0/virtual_size
cat /sys/class/graphics/fb0/name
```

如果有 fb1：

```sh
cat /sys/class/graphics/fb1/modes
cat /sys/class/graphics/fb1/bits_per_pixel
cat /sys/class/graphics/fb1/virtual_size
cat /sys/class/graphics/fb1/name
```

用 `fbset` 查看更详细参数：

```sh
fbset -fb /dev/fb0
fbset -fb /dev/fb1
```
