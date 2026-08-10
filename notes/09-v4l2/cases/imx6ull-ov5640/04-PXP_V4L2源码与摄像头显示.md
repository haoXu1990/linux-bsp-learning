# i.MX6ULL PXP V4L2 源码与摄像头显示课程

> 对象：Linux 4.9.88、`mxc_pxp_v4l2.c`、OV5640 DVP、LCD framebuffer  
> 目标：用 PXP 完成 YUYV → RGB、缩放和显示，替代 CPU逐像素转换。

---

## 0. 从已经解决的花屏问题开始

OV5640 V3 的 DVP配置原来为：

```c
{0x4740, 0x23, 0, 0}
```

实测改成：

```c
{0x4740, 0x21, 0, 0}
```

之后花屏恢复正常。这个问题属于 **OV5640 DVP同步/采样条件与 i.MX6ULL CSI接收条件不一致**。

现在要解决的是另一层问题：画面已经正确，但 CPU软件执行 YUV→RGB 太慢，导致卡顿。

```text
花屏：Sensor到CSI的接口契约问题，已解决
卡顿：图像后处理和显示效率问题，准备交给PXP
```

不要把这两个问题重新混在一起。

---

## 1. 先画出优化前后的架构

### 1.1 原来的软件路径

```text
OV5640
  │ DVP YUYV
  ▼
i.MX6ULL CSI
  │ DMA
  ▼
Camera V4L2 Buffer（YUYV）
  │ CPU逐像素计算YUV→RGB
  ▼
用户RGB临时Buffer
  │ CPU写入/拷贝
  ▼
/dev/fb0
  ▼
LCDIF → LCD
```

640×480 YUYV每帧约614400字节。软件转换不只是遍历内存，还要对每两个像素执行多次乘加、限幅和RGB写入，i.MX6ULL上的CPU很容易成为瓶颈。

### 1.2 使用PXP后的路径

```text
OV5640
  │ DVP YUYV
  ▼
i.MX6ULL CSI
  │ DMA
  ▼
Camera V4L2 Buffer（YUYV）
  │ APP把Buffer交给PXP
  ▼
PXP S0输入
  │ 硬件CSC + 缩放 + 旋转
  ▼
PXP输出Buffer（RGB565/XRGB8888）
  │ framebuffer扫描
  ▼
LCDIF → LCD
```

CPU的职责变成：

- 协调 Buffer；
- 调用 ioctl；
- 在 CSI 与 PXP队列之间交接 Buffer；
- 不再逐像素做颜色转换。

---

## 2. PXP到底是什么

PXP是 Pixel Pipeline，是i.MX6ULL内部的图像处理硬件。当前驱动可以用它完成：

- YUV与RGB颜色空间转换；
- 缩放；
- 90/180/270度旋转；
- 水平/垂直翻转；
- 裁剪和目标窗口；
- Overlay、Alpha、Color Key。

PXP不是Sensor、不是CSI，也不是LCDIF：

| 模块 | 输入 | 输出 | 作用 |
|---|---|---|---|
| OV5640 | 光线 | DVP YUYV | 产生图像 |
| CSI | DVP信号 | 内存YUYV | 抓取图像 |
| PXP | 内存图像 | 另一块内存图像 | 转换/缩放/旋转 |
| LCDIF | framebuffer内存 | LCD时序 | 扫描显示 |

这里所有模块都通过内存衔接。当前板上不存在：

```text
CSI硬件输出直接连进PXP硬件输入
```

必须由软件管理两端的 Buffer 生命周期。

---

## 3. 当前内核中PXP实际有三层

### 3.1 用户接口层：`mxc_pxp_v4l2.c`

路径：

```text
drivers/media/platform/mxc/output/mxc_pxp_v4l2.c
```

它负责：

- 创建名为 `PxP` 的 `/dev/videoX`；
- 接收 V4L2 VIDEO_OUTPUT ioctl；
- 管理输入图像队列；
- 保存输入格式、裁剪、旋转和输出参数；
- 向 DMAEngine申请一个 PXP Channel；
- 把任务描述符交给底层驱动；
- PXP完成后把输入Buffer标记为DONE；
- 把PXP输出Buffer切换给 framebuffer显示。

### 3.2 通用提交层：DMAEngine

关键接口：

```c
dma_request_channel()
device_prep_slave_sg()
tx_submit()
dma_async_issue_pending()
```

DMAEngine在这里不是普通内存复制。NXP把一次PXP图像处理任务包装成DMAEngine事务，描述符中同时包含：

- 输入物理地址；
- 输出物理地址；
- 输入/输出像素格式；
- 输入/输出尺寸；
- 源矩形和目标矩形；
- 旋转、翻转、颜色空间等参数。

### 3.3 硬件驱动层：`pxp_dma_v3.c`

i.MX6ULL的硬件节点 compatible 为：

```dts
compatible = "fsl,imx6ull-pxp-dma", "fsl,imx7d-pxp-dma";
```

它匹配：

```text
drivers/dma/pxp/pxp_dma_v3.c
```

底层驱动真正负责：

- 映射 `0x021cc000` PXP寄存器；
- 获取PXP时钟和IRQ；
- 注册DMAEngine设备和虚拟Channel；
- 把描述符参数翻译成PXP寄存器；
- 启动PXP；
- 响应硬件完成中断；
- 触发上层回调。

完整软件栈：

```text
APP的V4L2 ioctl
  ↓
mxc_pxp_v4l2.c
  ↓ DMAEngine API
pxp_dma_v3.c
  ↓ 寄存器
i.MX6ULL PXP硬件
```

最重要的结论：

> `mxc_pxp_v4l2.c`不是直接操作PXP寄存器的底层驱动，它是PXP DMAEngine的一个V4L2客户端。

---

## 4. 它不是现代V4L2 M2M驱动

现代Memory-to-Memory驱动通常同时拥有：

```text
V4L2_BUF_TYPE_VIDEO_OUTPUT   输入队列
V4L2_BUF_TYPE_VIDEO_CAPTURE  输出队列
```

用户向OUTPUT队列送YUV，再从CAPTURE队列取RGB。

当前 `mxc_pxp_v4l2.c` 不是这种结构。它只有一个供用户排队的：

```c
V4L2_BUF_TYPE_VIDEO_OUTPUT
```

PXP转换后的RGB输出Buffer由驱动内部使用 `dma_alloc_coherent()` 分配，保存在：

```c
pxp->outbuf
```

应用不能通过第二个V4L2 CAPTURE队列把它DQ出来。显示模式下，驱动直接让 framebuffer扫描这个输出Buffer。

因此当前接口更准确的名称是：

> 旧式V4L2 Video Output + framebuffer显示驱动。

这也解释了为什么 `VIDIOC_QUERYCAP` 返回：

```c
V4L2_CAP_STREAMING |
V4L2_CAP_VIDEO_OUTPUT |
V4L2_CAP_VIDEO_OUTPUT_OVERLAY
```

这里的VIDEO_OUTPUT容易让新手误会。它表示：

```text
用户把视频帧输出给这个设备
```

而不是“应用从它读取输出图像”。

---

## 5. 两个设备树节点必须分清

正确架构需要两个不同节点。

### 5.1 PXP硬件节点

SoC `.dtsi` 中：

```dts
pxp: pxp@021cc000 {
    compatible = "fsl,imx6ull-pxp-dma", "fsl,imx7d-pxp-dma";
    reg = <0x021cc000 0x4000>;
    interrupts = <...>;
    clocks = <...>;
    status = "disabled";
};
```

板级DTS只应启用它：

```dts
&pxp {
    status = "okay";
};
```

这个节点由 `pxp_dma_v3.c` 使用，因为只有底层驱动需要寄存器、IRQ和时钟。

### 5.2 PXP V4L2客户端节点

根节点下另外创建：

```dts
/ {
    pxp_v4l2 {
        compatible = "fsl,imx6ul-pxp-v4l2",
                     "fsl,imx6sx-pxp-v4l2",
                     "fsl,imx6sl-pxp-v4l2";
        status = "okay";
    };
};
```

这个节点没有 `reg` 和 `interrupts`，因为它只是让 `mxc_pxp_v4l2.c` 创建上层V4L2设备。

### 5.3 当前DTS中必须修正的问题

当前 `100ask_imx6ull-14x14.dts` 末尾存在：

```dts
&pxp {
    compatible = "fsl,imx6ul-pxp-v4l2";
    status = "okay";
};
```

这里不应该覆盖 `compatible`。覆盖之后，真正拥有PXP寄存器的节点不再匹配 `pxp_dma_v3.c`，反而可能被上层 `mxc_pxp_v4l2.c` 当成第二个客户端节点。

应改为：

```dts
&pxp {
    status = "okay";
};
```

根节点下原来的 `pxp_v4l2` 保留。

---

## 6. 内核配置怎样对应三层架构

当前配置已经包含：

```text
CONFIG_VIDEO_MXC_PXP_V4L2=y
CONFIG_MXC_PXP_V3=y
CONFIG_MXC_PXP_CLIENT_DEVICE=y
```

含义：

| 配置 | 产物 | 作用 |
|---|---|---|
| `VIDEO_MXC_PXP_V4L2` | `mxc_pxp_v4l2.o` | `/dev/videoX` PXP输出接口 |
| `MXC_PXP_V3` | `pxp_dma_v3.o` | i.MX6ULL PXP硬件与DMAEngine |
| `MXC_PXP_CLIENT_DEVICE` | `pxp_device.o` | 额外的 `/dev/pxp_device` 私有接口 |

本课程使用的是 `/dev/videoX`，因此核心是前两项。`/dev/pxp_device` 是另一套私有API，不要和PXP V4L2节点混用。

`CONFIG_MXC_PXP_V2=y` 即使同时打开，也不会匹配 i.MX6ULL 的 `fsl,imx6ull-pxp-dma`；当前SoC实际由V3驱动匹配。

---

## 7. `struct pxps`：先认识整台机器

`mxc_pxp_v4l2.h` 中的核心私有结构：

```c
struct pxps {
    struct video_device *vdev;
    struct videobuf_queue s0_vbq;
    struct pxp_buffer *active;
    struct list_head outq;
    struct pxp_channel *pxp_channel[1];
    struct pxp_config_data pxp_conf;
    struct dma_mem outbuf;
    struct pxp_data_format *s0_fmt;
    struct fb_info *fbi;
    struct v4l2_framebuffer fb;
    ...
};
```

逐个理解：

| 成员 | 代表什么 |
|---|---|
| `vdev` | 用户打开的PxP `/dev/videoX` |
| `s0_vbq` | 用户提交YUV输入帧的队列 |
| `active/outq` | 当前处理和等待处理的输入Buffer |
| `pxp_channel` | 从底层DMAEngine申请到的PXP Channel |
| `pxp_conf` | 一次PXP任务的全部参数 |
| `outbuf` | 驱动内部RGB输出Buffer |
| `s0_fmt` | 当前输入格式，例如YUYV |
| `fbi/fb` | 目标 framebuffer 信息 |

PXP术语中的 `S0` 可以先理解为主要源图层，也就是本项目里的摄像头YUYV帧。

---

## 8. probe：`/dev/video0`怎样出现

设备树客户端节点匹配后进入：

```c
pxp_probe(struct platform_device *pdev)
```

调用过程：

```text
分配struct pxps
  ↓
v4l2_device_register
  ↓
video_device_alloc
  ↓
复制pxp_template
  ↓
设置pxp_fops和pxp_ioctl_ops
  ↓
video_register_device
  ↓
生成名为PxP的/dev/videoX
```

`pxp_template` 设置：

```c
.name     = "PxP"
.vfl_dir  = VFL_DIR_TX
.fops     = &pxp_fops
.ioctl_ops = &pxp_ioctl_ops
```

虽然注册时使用了 `VFL_TYPE_GRABBER`，能力和方向仍明确是VIDEO_OUTPUT。这是旧驱动的历史写法，判断节点用途应以 `VIDIOC_QUERYCAP` 为准。

板上可以这样识别：

```sh
for v in /sys/class/video4linux/video*; do
    echo -n "$v: "
    cat "$v/name"
done
```

预期类似：

```text
/sys/class/video4linux/video0: PxP
/sys/class/video4linux/video1: mx6s-csi
```

不要永久假设编号一定是0和1，应用最好根据 `name` 或 `VIDIOC_QUERYCAP` 查找。

---

## 9. open：为什么PXP会查找framebuffer

应用打开PxP视频节点后进入 `pxp_open()`：

```text
限制为单进程打开
  ↓
pxp_set_fbinfo
  ↓
查找fix.id以"mxs"开头的framebuffer
  ↓
读取LCD宽、高、bpp、物理地址
  ↓
初始化旧式videobuf连续DMA队列
```

`pxp_set_fbinfo()` 根据LCD framebuffer确定PXP输出：

```c
fb->fmt.width  = fbi->var.xres;
fb->fmt.height = fbi->var.yres;

if (bits_per_pixel == 16)
    fb->fmt.pixelformat = V4L2_PIX_FMT_RGB565;
else
    fb->fmt.pixelformat = V4L2_PIX_FMT_RGB24;
```

这里有一个旧驱动命名陷阱：32bpp framebuffer虽然写成 `V4L2_PIX_FMT_RGB24`，后续实际按4字节分配，并映射为 `PXP_PIX_FMT_XRGB32`。所以不能只看字符串“RGB24”就认为输出Buffer每像素3字节。

---

## 10. 输入格式：YUYV怎样告诉PXP

应用设置：

```c
fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
fmt.fmt.pix.width = 640;
fmt.fmt.pix.height = 480;
fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
ioctl(pxp_fd, VIDIOC_S_FMT, &fmt);
```

调用链：

```text
VIDIOC_S_FMT
  ↓
pxp_s_fmt_video_output
  ↓
acquire_dma_channel
  ↓
pxp_try_fmt_video_output
  ↓
v4l2_fmt_to_pxp_fmt
  ↓
保存到pxp_conf.s0_param
```

关键转换：

```c
V4L2_PIX_FMT_YUYV → PXP_PIX_FMT_YUYV
V4L2_PIX_FMT_UYVY → PXP_PIX_FMT_UYVY
```

输入格式必须与CSI Buffer中的真实字节顺序完全一致。如果Camera实际是YUYV，却告诉PXP是UYVY，硬件会忠实地按错误顺序转换，结果仍然会偏色。

`VIDIOC_S_FMT` 还会通过 `acquire_dma_channel()` 调用：

```c
dma_request_channel(mask, chan_filter, NULL)
```

`chan_filter()` 只接受 `imx_dma_is_pxp(chan)` 的Channel，所以拿到的是底层 `pxp_dma_v3.c` 注册的PXP DMAEngine Channel。

---

## 11. 输出尺寸与显示窗口怎样设置

当前驱动把两个V4L2概念用于PXP缩放。

### 11.1 VIDEO_OUTPUT_OVERLAY格式：源矩形

```c
overlay.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
overlay.fmt.win.w.left = 0;
overlay.fmt.win.w.top = 0;
overlay.fmt.win.w.width = 640;
overlay.fmt.win.w.height = 480;
ioctl(fd, VIDIOC_S_FMT, &overlay);
```

保存到：

```c
pxp_conf.proc_data.srect
```

可以理解为“从输入图像的哪一块取数据”。

### 11.2 S_CROP：目标矩形

```c
crop.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
crop.c.left = 0;
crop.c.top = 0;
crop.c.width = 1024;
crop.c.height = 600;
ioctl(fd, VIDIOC_S_CROP, &crop);
```

保存到：

```c
pxp_conf.proc_data.drect
```

可以理解为“转换后放到LCD的什么位置、多大尺寸”。因此640×480可以由PXP硬件缩放到1024×600。

当前代码把位置和尺寸按8像素对齐，这是 `PXP_MIN_PIX` 的限制。

---

## 12. Buffer准备：数据从哪里来、到哪里去

### 12.1 输入地址

PXP输入Buffer由用户通过VIDEO_OUTPUT队列提交。在 `pxp_buf_prepare()` 中：

```c
pxp_conf->s0_param.paddr = videobuf_to_dma_contig(vb);
```

这就是PXP读取YUYV数据的物理地址。

### 12.2 输出地址

驱动在 `pxp_s_output()` 中使用 `dma_alloc_coherent()` 分配：

```c
pxp->outbuf
```

在 `pxp_buf_prepare()` 中：

```c
pxp_conf->out_param.paddr = pxp->outbuf.paddr;
```

这就是PXP写入RGB结果的物理地址。

### 12.3 描述符的三项内容

当前代码创建3个scatterlist/描述符位置：

```text
sg[0]：S0输入图像
sg[1]：PXP输出图像
sg[2]：可选Overlay图层
```

这里的scatterlist主要承担任务描述作用，不应简单理解为普通分散内存复制。

数据路径最终是：

```text
s0_param.paddr（YUYV）
  ↓ PXP硬件读取
颜色转换/缩放/旋转
  ↓ PXP硬件写入
out_param.paddr（RGB）
```

---

## 13. QBUF：任务怎样真正交给硬件

应用调用：

```c
ioctl(pxp_fd, VIDIOC_QBUF, &buf);
```

经过旧式videobuf框架进入 `pxp_buf_queue()`：

```text
输入Buffer加入outq
  ↓
标记ACTIVE或QUEUED
  ↓
txd->tx_submit(txd)
  ↓
dma_async_issue_pending()
  ↓
pxp_dma_v3.c配置寄存器并启动PXP
```

关键代码：

```c
cookie = txd->tx_submit(txd);
dma_async_issue_pending(&pchan->dma_chan);
```

在底层对应：

```text
pxp_tx_submit()
pxp_issue_pending()
pxp_config()
pxp_start()
```

这就是从V4L2队列跨到实际PXP寄存器的完整桥梁。

---

## 14. PXP完成：为什么DQBUF代表硬件处理结束

PXP硬件完成后：

```text
PXP IRQ
  ↓
pxp_dma_v3.c中的pxp_irq
  ↓
DMAEngine完成事务
  ↓
调用mxc_pxp_v4l2注册的video_dma_done
  ↓
输入Buffer状态改为VIDEOBUF_DONE
  ↓
wake_up(&vb->done)
  ↓
用户VIDIOC_DQBUF返回
```

`video_dma_done()`并不是返回RGB Buffer。它表示：

> 这块YUYV输入Buffer已经被PXP读取完成，现在应用可以安全复用或还给CSI。

RGB结果已经被PXP写进驱动内部 `outbuf`。

---

## 15. STREAMON：PXP输出怎样交给LCD

`pxp_streamon()`：

```c
videobuf_streamon(&pxp->s0_vbq);

if (pxp->output == 0)
    pxp_show_buf(pxp, pxp->outbuf.paddr);
```

`pxp_show_buf()` 的关键动作：

```c
fbi->fix.smem_start = paddr;
fb_pan_display(fbi, &fbi->var);
```

也就是说，Display Output模式下，驱动让 framebuffer/LCDIF去扫描PXP的RGB输出Buffer。

`VIDIOC_S_OUTPUT`支持：

```text
0：PxP Display Output，显示到LCD
1：PxP Virtual Output，不切换LCD扫描地址
```

当前驱动只有一个内部输出Buffer。PXP写入和LCD扫描可能同时发生，所以虽然CPU负载下降，仍要关注撕裂问题；如果后面出现水平撕裂，方向应是输出双Buffer/页翻转或同步，而不是重新使用软件YUV转换。

---

## 16. 摄像头与PXP怎样在应用中桥接

CSI和PXP是两个独立V4L2节点：

```text
/dev/video1：VIDEO_CAPTURE，生产YUYV帧
/dev/video0：VIDEO_OUTPUT，消费YUYV帧
```

应用的核心循环是：

```text
CSI DQBUF
  ↓
把同一块YUYV Buffer提交给PXP QBUF
  ↓
等待PXP DQBUF，确认硬件读取完毕
  ↓
把该Buffer重新QBUF给CSI
```

Buffer所有权必须严格遵守：

```text
CSI正在DMA写：PXP和CPU不能使用
CSI DQBUF后：应用拥有
提交PXP后：PXP拥有，不能立即还给CSI
PXP DQBUF后：应用重新拥有，才能还给CSI
```

如果刚把Camera Buffer交给PXP就立刻QBUF回CSI，可能发生：

```text
CSI正在覆盖下一帧
PXP还在读取上一帧
```

结果会出现撕裂、半帧新半帧旧或随机花屏。

---

## 17. 两种桥接方案

### 17.1 简单方案：复制到PXP MMAP Buffer

```text
CSI MMAP Buffer
  ↓ memcpy
PXP MMAP Buffer
  ↓ PXP硬件
RGB outbuf
```

优点是容易理解、驱动兼容性高；缺点是每帧仍有一次614400字节左右的内存复制。

### 17.2 推荐验证方案：CSI MMAP + PXP USERPTR

Camera Buffer已经mmap到应用地址空间，将该地址作为PXP USERPTR：

```text
同一块物理YUYV内存
  ├── CSI先DMA写入
  └── PXP随后DMA读取
```

应用不执行逐像素转换，也不执行整帧 `memcpy()`。但必须一直扣住该Camera Buffer，直到PXP DQBUF后才重新交给CSI。

本地已有桥接示例：

- [mxc-csi-pxp-demo.c](Z:/work/100ask/linux-bsp-learning/hardware/ov5640/mxc-csi-pxp-code/tools/mxc-csi-pxp-demo.c)

其 `run_bridge_loop()` 正是这个所有权模型：

```text
Camera MMAP DQBUF
  ↓
PXP USERPTR QBUF
  ↓
PXP DQBUF
  ↓
Camera重新QBUF
```

这不是CSI与PXP的直接硬件Pipeline，而是由应用调度的近零拷贝内存Pipeline。

---

## 18. 用户态初始化顺序

推荐按照下面的固定顺序写程序：

```text
1. 找到mx6s-csi视频节点和PxP视频节点
2. 打开Camera与PXP
3. Camera VIDIOC_S_FMT：640×480 YUYV
4. PXP VIDIOC_S_OUTPUT：0，显示到LCD
5. PXP VIDIOC_S_FMT(VIDEO_OUTPUT)：640×480 YUYV
6. PXP VIDIOC_S_FMT(VIDEO_OUTPUT_OVERLAY)：源矩形
7. PXP VIDIOC_S_CROP：LCD目标矩形，例如1024×600
8. Camera申请MMAP Buffer
9. PXP建立USERPTR队列
10. 所有Camera Buffer先QBUF
11. Camera STREAMON
12. 循环Camera DQBUF → PXP QBUF → PXP DQBUF → Camera QBUF
13. 退出时先PXP STREAMOFF，再Camera STREAMOFF
```

输入像素格式要从Camera `VIDIOC_S_FMT` 的返回值取得，不应只相信请求值。

---

## 19. 本地示例怎么使用

现有示例默认：

```text
Camera：/dev/video1
PXP：/dev/video0
输入：640×480 YUYV
PXP输出：LCD
```

运行形式：

```sh
./mxc-csi-pxp-demo \
    --capture /dev/video1 \
    --pxp /dev/video0 \
    --width 640 \
    --height 480 \
    --display-width 640 \
    --display-height 480 \
    --pixfmt YUYV \
    --pxp-output 0
```

源码还支持 `--fb N`。这一参数依赖本地准备的PXP V4L2扩展补丁：

- [扩展版 mxc_pxp_v4l2.c](Z:/work/100ask/linux-bsp-learning/hardware/ov5640/mxc-csi-pxp-code/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c)
- [扩展版 pxp_dma.h](Z:/work/100ask/linux-bsp-learning/hardware/ov5640/mxc-csi-pxp-code/include/uapi/linux/pxp_dma.h)

原始内核 `mxc_pxp_v4l2.c` 没有 `PXP_IOC_V4L2_S_OUTPUT_FB`，会自动查找第一个 `fix.id` 以 `mxs` 开头的 framebuffer。因此在未合入扩展补丁前，不要给示例传 `--fb`，否则会返回不支持的ioctl。

---

## 20. 上板检查顺序

### 20.1 检查节点

```sh
for v in /sys/class/video4linux/video*; do
    echo -n "$v: "
    cat "$v/name"
done
```

### 20.2 检查底层PXP是否probe

```sh
dmesg | grep -i pxp
ls /sys/bus/platform/drivers | grep -i pxp
```

必须同时具备：

```text
PXP DMA硬件驱动可用
PXP V4L2节点可用
```

只有 `PxP /dev/videoX` 而申请DMA Channel失败，通常说明底层 `pxp_dma_v3.c` 没有probe，首先检查 `&pxp compatible` 是否被错误覆盖。

### 20.3 检查能力

没有 `v4l2-ctl` 时，可以在测试程序中调用 `VIDIOC_QUERYCAP`。PXP节点应包含：

```text
V4L2_CAP_VIDEO_OUTPUT
V4L2_CAP_STREAMING
```

CSI节点应包含：

```text
V4L2_CAP_VIDEO_CAPTURE
V4L2_CAP_STREAMING
```

---

## 21. 常见错误怎样分层

| 现象 | 优先检查 |
|---|---|
| 找不到PxP视频节点 | `pxp_v4l2`客户端节点、V4L2配置 |
| `VIDIOC_S_FMT`返回EBUSY | 没拿到PXP DMA Channel、底层驱动未probe |
| PXP有节点但无硬件动作 | `&pxp` compatible/status、V3 DMA驱动 |
| 输出严重偏色 | Camera真实YUYV/UYVY与PXP输入格式不一致 |
| PXP DQBUF不返回 | QBUF/STREAMON顺序、IRQ、DMA描述符 |
| Camera后续卡死 | Camera Buffer在PXP完成前被错误重排队 |
| 颜色正确但尺寸不对 | `srect`、`drect`、framebuffer分辨率 |
| CPU仍然很高 | 是否还在执行软件YUV转换或每帧memcpy |
| 水平撕裂 | PXP单输出Buffer与LCD扫描并发 |
| 打开PXP失败EBUSY | 当前驱动只允许一个用户打开 |

---

## 22. 源码导航表

### 22.1 PXP V4L2客户端

- [输入格式表](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:60)
- [V4L2格式转PXP格式](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:104)
- [PXP完成回调](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:210)
- [申请PXP DMA Channel](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:254)
- [读取framebuffer信息](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:299)
- [切换framebuffer扫描地址](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:342)
- [设置PXP输入格式](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:564)
- [准备输入/输出物理地址和描述符](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:773)
- [提交任务并启动DMAEngine](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:879)
- [V4L2 ioctl表](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:1194)
- [PXP video_device模板](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:1229)
- [PXP V4L2 probe](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/media/platform/mxc/output/mxc_pxp_v4l2.c:1247)

### 22.2 PXP硬件/DMAEngine驱动

- [PXP寄存器配置入口](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/dma/pxp/pxp_dma_v3.c:3510)
- [提交事务](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/dma/pxp/pxp_dma_v3.c:3958)
- [PXP中断](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/dma/pxp/pxp_dma_v3.c:4011)
- [准备PXP事务描述符](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/dma/pxp/pxp_dma_v3.c:4150)
- [启动等待任务](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/dma/pxp/pxp_dma_v3.c:4225)
- [i.MX6ULL compatible匹配](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/dma/pxp/pxp_dma_v3.c:7517)
- [底层PXP probe](Z:/work/100ask/imx6ull/100ask_imx6ull-sdk/Buildroot_2020.02.x/output/build/linux-origin_master/drivers/dma/pxp/pxp_dma_v3.c:7883)

---

## 23. 阅读源码的推荐顺序

### 第一轮：认识接口

1. `pxp_template`
2. `pxp_fops`
3. `pxp_ioctl_ops`
4. `pxp_probe()`
5. `pxp_open()`

目标：证明它是VIDEO_OUTPUT节点，不是Camera Capture，也不是标准M2M。

### 第二轮：格式和输出

1. `pxp_s0_formats[]`
2. `v4l2_fmt_to_pxp_fmt()`
3. `pxp_s_fmt_video_output()`
4. `pxp_s_fmt_output_overlay()`
5. `pxp_s_crop()`
6. `pxp_s_output()`

目标：知道输入格式、源矩形、目标矩形和RGB输出格式分别在哪里确定。

### 第三轮：Buffer和硬件

1. `pxp_buf_setup()`
2. `pxp_buf_prepare()`
3. `pxp_buf_queue()`
4. `video_dma_done()`
5. `pxp_dma_v3.c:pxp_prep_slave_sg()`
6. `pxp_dma_v3.c:pxp_issue_pending()`
7. `pxp_dma_v3.c:pxp_irq()`

目标：能够指出每个时刻输入Buffer的所有者。

### 第四轮：Camera到PXP

1. Camera `DQBUF`
2. PXP USERPTR `QBUF`
3. PXP `DQBUF`
4. Camera重新 `QBUF`

目标：实现不做YUV软件转换、尽量不复制整帧的显示循环。

---

## 24. 一页复习卡

```text
mxc_pxp_v4l2.c是什么？
  旧式V4L2 VIDEO_OUTPUT客户端驱动。

谁真正操作PXP寄存器？
  pxp_dma_v3.c。

二者怎样连接？
  DMAEngine Channel和事务描述符。

YUYV从哪里来？
  mx6s-csi的VIDEO_CAPTURE Buffer。

YUYV怎样进入PXP？
  APP向PXP VIDEO_OUTPUT队列QBUF。

RGB到哪里去？
  mxc_pxp_v4l2内部outbuf，再由framebuffer/LCDIF扫描。

是不是标准V4L2 M2M？
  不是，用户只管理输入队列，输出Buffer由驱动内部管理。

能不能完全没有APP？
  当前驱动架构不能；CSI和PXP没有直接连接，需要APP桥接Buffer。

怎样减少CPU？
  PXP做CSC/缩放；Camera MMAP Buffer作为PXP USERPTR，避免逐像素转换和整帧复制。

最重要的规则？
  PXP DQBUF之前，不能把同一Camera Buffer重新交给CSI。
```

---

## 25. 下一步实施顺序

1. 先修正DTS中 `&pxp` 的compatible覆盖，只保留 `status = "okay"`；
2. 编译并部署DTB和内核，确认 `pxp_dma_v3` 与 `PxP /dev/videoX` 同时存在；
3. 先用单张已知正确的YUYV文件验证PXP→LCD；
4. 再接Camera MMAP → PXP USERPTR动态链路；
5. 记录CPU占用、FPS和是否撕裂；
6. 如果仍卡顿，再测量Camera等待、PXP处理和LCD刷新各阶段耗时，不凭感觉改代码。

先单独验证PXP的原因是：它可以把Camera问题与PXP问题分开。已知YUYV文件能正确显示，才能证明PXP格式、缩放和framebuffer链路正确。
