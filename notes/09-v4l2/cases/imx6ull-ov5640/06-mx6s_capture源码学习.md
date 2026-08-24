# i.MX6ULL `mx6s_capture` 源码学习

> 内核版本：Linux 4.9.88  
> CSI 驱动：`drivers/media/platform/mxc/capture/mx6s_capture.c`  
> Sensor 驱动：`drivers/media/platform/mxc/capture/ov5640_v3.c`  
> 学习状态：Probe、对象创建、Media 拓扑和异步绑定部分已完成；APP 运行与采集部分待继续。

## 1. 文档目标

本文围绕 `mx6s_capture.c` 的真实代码学习 i.MX6ULL 摄像头采集流程。重点不是孤立记忆函数，而是回答下面几个问题：

1. `mx6s_capture` 在整个摄像头 Pipeline 中是什么角色？
2. `/dev/videoX` 是谁创建的？
3. CSI、Video Node 和 OV5640 怎样通过 Media、Subdev 和异步 notifier 建立关系？
4. APP 的 ioctl 怎样到达 `mx6s_capture`？
5. Buffer 怎样经过 VB2、CSI DMA 和中断返回 APP？

完整数据链路是：

~~~text
OV5640 Sensor
  → DVP：PCLK、VSYNC、HREF、D[7:0]
  → i.MX6ULL CSI
  → CSI DMA
  → 内存中的 VB2 Buffer
  → /dev/videoX
  → APP
~~~

完整控制链路是：

~~~text
APP
  → V4L2 Core
  → mx6s_capture
  → V4L2 Subdev Core
  → ov5640_v3
  → I2C 寄存器读写
~~~

## 2. `mx6s_capture` 的角色

`mx6s_capture` 是 i.MX6ULL CSI 的采集主机驱动，也是当前摄像头 Pipeline 的中枢。它主要负责：

1. 管理 CSI 寄存器、IRQ 和时钟等硬件资源；
2. 创建面向 APP 的 `video_device`；
3. 初始化 videobuf2 采集队列；
4. 创建 CSI 对应的 Subdev 和 Media Entity；
5. 等待并绑定 OV5640 Sensor Subdev；
6. 在采集时把 VB2 Buffer 交给 CSI DMA；
7. 在 CSI 中断中完成一帧并把 Buffer 还给 VB2；
8. 通过 Subdev 操作控制 OV5640 的格式与启停。

因此：

- `mx6s_capture` 不是 Sensor 驱动；
- OV5640 驱动也不负责把图像通过 I2C 搬进内存；
- I2C 是控制路径，DVP + CSI DMA 是数据路径。

## 3. Probe 总览

设备树 CSI 节点的 `compatible` 与 `mx6s_capture.c` 中的 `of_match_table` 匹配，并且节点状态为 `okay` 后，Platform Bus 调用：

~~~c
mx6s_csi_probe(struct platform_device *pdev)
~~~

Probe 主线可以概括成：

~~~text
设备树 CSI 节点匹配
  → 分配 csi_dev
  → 获取寄存器、中断、时钟资源
  → 初始化 media_device
  → 注册 v4l2_device
  → 初始化 VB2 队列
  → 初始化 CSI Subdev Entity
  → 初始化 Video Entity
  → 注册 /dev/videoX
  → 建立 CSI → Video Link
  → 注册 CSI 中断处理函数
  → 注册异步 notifier，等待 OV5640
  → 启用 Runtime PM
~~~

这里完成的是“资源和软件对象准备”，并没有开始采集。

## 4. 核心私有结构 `struct mx6s_csi_dev`

Probe 分配的 `csi_dev` 是整个驱动实例的总上下文，里面保存：

- CSI 寄存器虚拟地址；
- IRQ 编号和时钟对象；
- `media_device`；
- `v4l2_device`；
- CSI 对应的 `v4l2_subdev`；
- 面向 APP 的 `video_device`；
- videobuf2 队列；
- Buffer 链表；
- 异步 notifier；
- 已经绑定的 Sensor 指针 `csi_dev->sd`。

### 4.1 Buffer 链表的当前理解

- `capture`：APP 已经通过 `QBUF` 交给驱动，正在等待硬件使用的 Buffer；
- `active_bufs`：已经交给 CSI DMA，当前正在采集的 Buffer；
- `discard`：没有合适的用户 Buffer 等情况下使用的丢弃 Buffer。

这些链表的具体移动过程，要在 `STREAMON`、CSI DMA 和中断章节继续跟踪。

## 5. 获取硬件资源

下面四个动作必须区分：

~~~text
platform_get_resource()
  获取设备树转换得到的寄存器资源描述

devm_ioremap_resource()
  将寄存器物理地址映射为内核可访问的虚拟地址

platform_get_irq()
  只取得 IRQ 编号

devm_request_irq(..., mx6s_csi_irq_handler, ...)
  把 IRQ 与 CSI 中断处理函数关联起来
~~~

所以，`platform_get_irq()` 不等于已经注册中断。同理，`devm_clk_get()` 是取得时钟对象，也不代表 CSI 已经开始运行。

## 6. 三个容易混淆的核心对象

| 对象 | 主要作用 | 是否直接产生 `/dev/videoX` |
|---|---|---|
| `media_device` | 管理 Entity、Pad、Link 形成的媒体拓扑 | 否 |
| `v4l2_device` | 聚合一个硬件实例的 V4L2 对象，并管理 Subdev 列表 | 否 |
| `video_device` | 表示 APP 可以打开并执行 ioctl 的视频节点 | 注册后形成节点 |

### 6.1 `v4l2_device` 和 `media_device` 为什么不冲突

两者观察同一套摄像头系统的不同方面：

~~~text
v4l2_device
  关注：有哪些 V4L2 子设备，驱动怎样找到并调用 Subdev

media_device
  关注：有哪些媒体模块，它们的数据端口怎样连接
~~~

因此 `v4l2_device` 中存在 Subdev 列表，`media_device` 中又存在 Subdev 对应的 Entity，并不是重复管理。

## 7. `video_device` 与 `/dev/videoX`

驱动会给 `video_device` 设置：

- `v4l2_dev`：它属于哪个 `v4l2_device`；
- `fops`：`open`、`poll`、`mmap`、`unlocked_ioctl` 等系统调用入口；
- `ioctl_ops`：各种 V4L2 ioctl 的具体驱动回调；
- `queue`：CSI 对应的 videobuf2 队列；
- `lock`：ioctl 使用的互斥锁；
- 私有数据：通过 `video_set_drvdata()` 关联回 `csi_dev`。

随后调用：

~~~c
video_register_device(vdev, VFL_TYPE_GRABBER, -1);
~~~

准确表述是：

> `mx6s_capture` 创建、配置并提交 `video_device`；V4L2 Core 完成设备号、字符设备、sysfs 等公共注册；devtmpfs/udev 最终呈现 `/dev/videoX`。

所以：

- 不能只说“`/dev/videoX` 完全由 V4L2 Core 创建”；
- 也不能说它完全与 V4L2 Core 无关；
- `mx6s_capture` 是节点的提供者，V4L2 Core 是公共注册框架。

### 7.1 没有 OV5640 时节点会不会存在

按当前驱动的 Probe 顺序，`video_register_device()` 发生在 Sensor 异步绑定之前，因此可能出现：

~~~text
/dev/video1 已存在
但 csi_dev->sd == NULL
~~~

这表示 CSI Video Node 已注册，但 Sensor 尚未绑定。此时节点存在不等于能够正常枚举格式或采集。

## 8. `video_ioctl2` 与 `vdev->ioctl_ops`

它们不是两套互相冲突的格式处理函数。

`video_ioctl2` 是 V4L2 Core 提供的通用 ioctl 分发入口；`vdev->ioctl_ops` 是 `mx6s_capture` 提供的具体硬件实现表。

以 `VIDIOC_S_FMT` 为例：

~~~text
APP ioctl(fd, VIDIOC_S_FMT, &fmt)
  → video_device.fops->unlocked_ioctl
  → video_ioctl2()
  → V4L2 Core 检查命令和参数
  → 根据 Buffer Type 选择对应 ioctl_ops 回调
  → vdev->ioctl_ops->vidioc_s_fmt_vid_cap
  → mx6s_capture 的格式设置函数
~~~

一个负责公共分发，一个负责具体实现。

## 9. Entity、Pad 和 Link

### 9.1 基础概念

- Entity：一个具有媒体功能的模块，例如 Sensor、CSI 或 Video Node；
- Pad：Entity 的数据端口；
- Link：两个 Pad 之间有方向的数据连接。

可以把它们理解成：

~~~text
Entity = 芯片框图中的功能方块
Pad    = 方块边缘的输入/输出引脚
Link   = 方块之间的数据连线
~~~

### 9.2 CSI 为什么需要两个 Pad

CSI 位于 Pipeline 中间：既要接收 OV5640 的数据，又要把采集数据送到 Video Node。

~~~text
[OV5640 Source]
        ↓
[CSI Sink | CSI Entity | CSI Source]
                              ↓
                       [Video Sink]
~~~

所以 CSI 需要：

- 一个 `MEDIA_PAD_FL_SINK` 输入 Pad；
- 一个 `MEDIA_PAD_FL_SOURCE` 输出 Pad。

Video Node 是这条 Media Pipeline 的终点，只需要一个 Sink Pad。

### 9.3 `media_entity_pads_init()` 做什么

它把 Pad 登记为某个 Entity 的端口，设置 Pad 的归属和编号，但不会把两个设备连接起来。

真正创建两个 Pad 之间连接的是：

~~~c
media_create_pad_link(...)
~~~

Link 的方向必须是：

~~~text
上游 Source Pad → 下游 Sink Pad
~~~

`MEDIA_LNK_FL_ENABLED` 表示 Link 当前启用；`MEDIA_LNK_FL_IMMUTABLE` 表示用户空间不能任意修改这条固定硬件连接。

### 9.4 CSI Entity 是不是整个 `mx6s_capture`

不是。

`csi_dev->csi_sd.entity` 是 `mx6s_capture` 创建和管理的一个媒体对象，用来表示 CSI 在 Subdev/Media 框架中的身份。

而整个 `mx6s_capture` 还管理：

- Platform Device；
- CSI 硬件资源；
- Video Node；
- VB2 队列；
- DMA Buffer；
- IRQ；
- Sensor Subdev 指针。

### 9.5 `entity.function = MEDIA_ENT_F_IO_V4L`

这行代码是在给 CSI Entity 设置功能分类，方便 Media Core 和用户空间识别这个 Entity 的用途。

它不是：

- 启动 CSI 的开关；
- 数据格式配置；
- Pad 方向配置；
- Link 创建操作。

### 9.6 为什么 Video Entity 的代码看起来更少

驱动主要显式设置：

~~~c
csi_dev->vdev_pad.flags = MEDIA_PAD_FL_SINK;
media_entity_pads_init(&vdev->entity,
                       MX6S_CSI_VDEV_PADS_NUM,
                       &csi_dev->vdev_pad);
~~~

`video_device` 内部本身已经包含 `entity`。`video_register_device()` 还会补充 Video Node 的公共属性并注册它。

CSI Subdev 是驱动显式构造的内部模块，因此它的 Entity、Pad、Subdev 操作等代码看起来更多。

## 10. 两条 Media Link

### 10.1 CSI 到 Video Node

~~~text
CSI Source Pad → Video Entity Sink Pad
~~~

CSI 和 Video Node 都由 `mx6s_capture` 创建。Probe 运行时两边对象已经存在，因此驱动可以直接创建这条 Link。

### 10.2 OV5640 到 CSI

~~~text
OV5640 Source Pad → CSI Sink Pad
~~~

OV5640 是独立的 I2C 驱动，它可能尚未 Probe 完成，所以 `mx6s_capture` 不能在普通 Probe 中假定 Sensor Entity 已经存在。这条 Link 要等异步绑定完成后创建。

## 11. 设备树 endpoint 与 Media Link

设备树中的 `port`、`endpoint` 和 `remote-endpoint` 描述板级硬件连接。例如：

~~~dts
port {
    ov5640_ep: endpoint {
        remote-endpoint = <&csi1_ep>;
    };
};
~~~

其中：

- `port` 和 `endpoint` 是 OF Graph 约定的节点结构，不是随便起名的普通 label；
- `ov5640_ep` 是 DTS label，供其他节点通过 `&ov5640_ep` 引用；
- `remote-endpoint` 表示这一个端点与哪个远端端点相连；
- `csi1_ep` 是 CSI 一侧 endpoint 的 label，名字本身可以修改，但引用必须一致。

设备树 endpoint 的作用是：

1. 描述静态硬件连线；
2. 帮助 CSI 驱动找到远端 Sensor 的 OF 节点；
3. 作为异步 notifier 的匹配依据。

它不等于运行时的 `struct media_link`。Media Link 必须等双方 Entity 和 Pad 都创建完成后，由驱动调用 Media API 建立。

## 12. 异步 notifier 怎样连接 CSI 与 OV5640

### 12.1 CSI 一侧

`mx6sx_register_subdevs()` 通过 OF Graph 找到 CSI endpoint 的远端父节点，也就是 OV5640 的设备树节点，然后设置 OF 匹配条件并注册 notifier：

~~~text
mx6s_capture
  → 解析 CSI port/endpoint
  → of_graph_get_remote_port_parent()
  → 得到 OV5640 OF node
  → 设置 V4L2_ASYNC_MATCH_OF
  → v4l2_async_notifier_register()
~~~

### 12.2 OV5640 一侧

OV5640 的 I2C Probe 成功后：

~~~text
ov5640_probe()
  → 检查芯片 ID
  → 初始化 struct v4l2_subdev
  → 初始化 Sensor Entity/Pad
  → v4l2_async_register_subdev()
~~~

### 12.3 真正的绑定点

V4L2 Async Core 比较双方 OF 节点。匹配成功后，调用 `mx6s_capture` 的 notifier `bound` 回调。

真正保存绑定关系的代码在 `mx6s_capture.c`：

~~~c
csi_dev->sd = subdev;
~~~

含义是：

> 把已经匹配成功的 OV5640 `v4l2_subdev` 指针保存进 CSI 驱动，以后 CSI 可以通过 `csi_dev->sd` 调用 Sensor 的 Subdev 操作。

它不是在 `ov5640_v3.c` 中执行，也不是设备树直接给这个 C 指针赋值。

### 12.4 `complete` 回调

当 notifier 中要求的 Subdev 全部匹配完成后，`subdev_notifier_complete()` 执行：

1. 创建 OV5640 Source → CSI Sink 的 Media Link；
2. 注册 Subdev 节点；
3. 注册或完善 Media Device 拓扑。

此时 Media Pipeline 才完整表现为：

~~~text
OV5640 Entity → CSI Entity → Video Entity
~~~

## 13. Probe 收尾

Probe 后半段的重要动作是：

~~~text
video_set_drvdata()
  → video_register_device()
  → 创建 CSI → Video Link
  → devm_request_irq(mx6s_csi_irq_handler)
  → mx6sx_register_subdevs()
  → pm_runtime_enable()
~~~

异步绑定也完成后，系统应具有：

~~~text
/dev/videoX 已注册
CSI 寄存器、时钟和 IRQ 资源已准备
VB2 队列已初始化
CSI Entity 和 Video Entity 已注册
CSI → Video Link 已建立
csi_dev->sd 指向 OV5640 Subdev
OV5640 → CSI Link 已建立
Runtime PM 已启用
~~~

但是，这依然不等于摄像头已经开始采集。真正开始采集要等 APP 设置格式、准备 Buffer 并调用 `VIDIOC_STREAMON`。

## 14. 本阶段问答记录

### Q1：`/dev/videoX` 不是由 V4L2 Core 创建的吗？

答：`mx6s_capture` 创建并配置 `video_device`，然后调用 `video_register_device()`；V4L2 Core 完成公共注册，系统最终呈现 `/dev/videoX`。这是驱动与框架协作的结果。

### Q2：没有 OV5640，只要 CSI 节点启用，`/dev/videoX` 会不会存在？

答：按当前驱动的 Probe 顺序，Video Node 在 Sensor 异步绑定前注册，因此可能存在。节点存在不代表 Sensor 已绑定，也不代表能够正常采集。

### Q3：OV5640 图像是否通过 I2C 搬到内存？

答：不是。I2C 只配置 Sensor 寄存器。图像经 DVP 到 CSI，再由 CSI DMA 写入内存中的 VB2 Buffer。

### Q4：`mx6s_capture` 在控制路径和数据路径中分别做什么？

答：控制路径中，它处理 APP ioctl，并通过 Subdev 操作控制 OV5640；数据路径中，它配置 CSI、管理 DMA Buffer、处理中断，并把完成的 Buffer 交回 VB2。

### Q5：`platform_get_irq()` 是否已经安装中断函数？

答：没有。它只获取 IRQ 编号；`devm_request_irq()` 才将 IRQ 与 `mx6s_csi_irq_handler()` 关联。

### Q6：`capture` 与 `active_bufs` 有什么区别？

答：`capture` 保存 APP 已经 `QBUF`、等待硬件使用的 Buffer；`active_bufs` 保存当前已经交给 CSI DMA 的 Buffer。

### Q7：`v4l2_device_register()` 会不会生成 `/dev/videoX`？

答：不会。它注册的是 V4L2 聚合对象。注册 Video Node 的关键调用是 `video_register_device()`。

### Q8：`media_device` 主要管理 Buffer 还是媒体拓扑？

答：管理媒体拓扑。Buffer 主要由 videobuf2 和采集驱动管理。

### Q9：CSI Subdev Entity 是不是 `mx6s_capture` 本身？

答：不是。它是该驱动创建的一个媒体对象，用来表示 CSI 功能模块。整个驱动还管理 Video Node、VB2、DMA、IRQ 等内容。

### Q10：`video_ioctl2` 与 `vdev->ioctl_ops` 为什么都有格式相关内容？

答：`video_ioctl2` 是公共分发器，`ioctl_ops` 是驱动具体实现。ioctl 先进入 `video_ioctl2`，再被分发到 `mx6s_capture` 的对应回调。

### Q11：为什么 CSI 有两个 Pad，Video Node 只有一个 Pad？

答：CSI 位于 Pipeline 中间，既接收 Sensor 数据又向 Video Node 输出，所以有 Sink 和 Source；Video Node 是终点，只需要 Sink。

### Q12：设备树已经有 endpoint，为什么驱动还要创建 Link？

答：endpoint 是静态硬件描述和异步匹配依据；Media Link 是内核运行时拓扑对象。设备树告诉驱动“应该连接谁”，Media API 建立“当前内核对象怎样连接”。

### Q13：`csi_dev->sd = subdev` 在哪里执行？

答：在 `mx6s_capture.c` 的 notifier `bound` 回调中执行。V4L2 Async Core 匹配 CSI 与 OV5640 的 OF 节点后调用这个回调。

### Q14：Probe 完成后是否已经开始输出图像？

答：没有。Probe 只完成资源、对象和拓扑准备。连续采集要等 APP 执行 `VIDIOC_STREAMON`。

## 15. 当前学习进度

### 已经完成

- Platform Driver 匹配与 Probe；
- CSI 硬件资源获取；
- `media_device`、`v4l2_device`、`video_device` 的区别；
- `/dev/videoX` 注册；
- CSI/Video Entity、Pad 和 Link；
- endpoint 与 Media Link 的区别；
- OV5640 Subdev 异步绑定；
- Probe 收尾及相关问答。

### 尚未完成

- APP `open()` 如何进入驱动；
- `QUERYCAP、ENUM_FMT、S_FMT` 的调用链；
- `REQBUFS、QUERYBUF、mmap、QBUF` 与 VB2；
- `STREAMON` 怎样启动 CSI 和 OV5640；
- CSI DMA 怎样取得和切换 Buffer；
- `mx6s_csi_irq_handler()` 怎样完成一帧；
- `vb2_buffer_done()` 怎样唤醒 `poll()` 和 `DQBUF`；
- `STREAMOFF` 与资源回收。

## 16. 下一步学习顺序

下一阶段继续沿完整 APP 操作流程学习 `mx6s_capture`：

~~~text
open(/dev/videoX)
  → QUERYCAP / ENUM_FMT / S_FMT
  → REQBUFS / QUERYBUF / mmap / QBUF
  → STREAMON
  → CSI DMA
  → CSI IRQ
  → vb2_buffer_done()
  → poll() / DQBUF
  → APP 使用 Buffer
  → QBUF 归还
  → STREAMOFF
~~~

这条运行链路讲完后，才把 `mx6s_capture` 标记为“全部完成”。
