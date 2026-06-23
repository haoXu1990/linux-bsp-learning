# V4L2 架构文档集设计说明

## 1. 目标

基于百问网 V4L2 课程资料、用户提供的网络参考文章，以及 Linux 4.9 V4L2 内核框架，整理一套可以独立阅读的 V4L2 架构文档。

文档需要同时满足两个目标：

1. 帮助初学者建立完整、准确的 V4L2 心智模型。
2. 帮助驱动开发者从 APP 调用一路追踪到 V4L2 Core、videobuf2 和具体硬件驱动，并能够着手编写简单视频设备驱动和复杂 MIPI 摄像头驱动。

## 2. 读者与版本基线

- 主要读者：已经学习过 Linux 字符设备、platform、I2C、USB 等子系统，希望系统理解 V4L2 的嵌入式 Linux 开发者。
- 讲解方式：先用直观模型说明职责和关系，再深入核心结构体及调用链。
- 主要版本：Linux 4.9。
- 新内核说明：只在 Linux 6.x 的接口变化会妨碍后续阅读时增加简短版本提示，不将文档写成跨版本 API 对照手册。

## 3. 内容边界

### 3.1 包含内容

- V4L2 的整体定位以及各框架之间的分工。
- `/dev/videoX`、`/dev/v4l-subdevX`、`/dev/mediaX` 三类接口。
- APP 查询能力、协商格式、控制参数和 streaming 采集流程。
- `v4l2_device`、`video_device`、`v4l2_file_operations`、`v4l2_ioctl_ops`。
- V4L2 ioctl 分发路径。
- videobuf2 的对象模型、三组 ops、buffer 状态和数据流。
- V4L2 controls 框架。
- 简单、整体式 V4L2 capture 驱动的编写步骤和可编译骨架。
- Media Controller 的 entity、pad、link、pipeline。
- `v4l2_subdev` 的职责、ops、注册和调用。
- MIPI Sensor、CSI Receiver、ISP、Scaler、Capture/DMA 的协作方式。
- 复杂摄像头 pipeline 的注册、异步绑定、格式传播、启动和停止。
- UVC 与 SoC MIPI 摄像头的架构对比。
- 常用工具、调试顺序和典型故障。

### 3.2 不包含内容

- 完整介绍 MIPI CSI-2 和 D-PHY 电气协议。
- 详细讲解具体厂商 ISP 算法、3A 算法和私有接口。
- 编写完整的特定 SoC CSI/ISP 驱动。
- 对 Linux 6.x 所有 API 变化进行逐项迁移说明。
- 展开视频编码、解码以及 DRM/KMS 显示子系统。

## 4. 核心叙事模型

文档不把 V4L2 描述成一个单独模块，而是解释为一组互相协作的框架：

```text
用户空间
  APP / v4l2-ctl / media-ctl
             |
             v
设备接口
  /dev/videoX    /dev/v4l-subdevX    /dev/mediaX
        |               |                 |
        v               v                 v
V4L2 Core          V4L2 Subdev       Media Controller
设备和 ioctl       子模块操作接口      拓扑和链路管理
        |
        v
videobuf2
buffer 生命周期、内存模型、队列状态和同步
        |
        v
具体驱动
UVC / 虚拟设备 / Sensor / CSI / ISP / Scaler / Capture
        |
        v
硬件、总线与 DMA
```

所有专题都回答以下问题：

1. 该模块负责什么，不负责什么？
2. 谁创建它，谁调用它？
3. 核心结构体之间如何关联？
4. 控制命令或视频帧怎样经过它？
5. 驱动作者必须实现哪些部分，V4L2 Core 已经提供哪些部分？

## 5. 文档结构

文档放在 `notes/09-v4l2/architecture/`，入口放在 `notes/09-v4l2/README.md`。

### 5.1 `00-V4L2学习路线与全景图.md`

- V4L2 解决的问题。
- 先区分控制面、数据面和拓扑面。
- V4L2 Core、vb2、controls、subdev、Media Controller 的关系。
- 简单整体式设备和复杂 pipeline 设备的区别。
- 推荐阅读顺序和术语索引。

### 5.2 `01-APP接口与采集流程.md`

- 三类设备节点。
- capability、format、input、stream parameter、control。
- read、MMAP、USERPTR、DMABUF 的定位，重点讲 MMAP。
- `REQBUFS → QUERYBUF → mmap → QBUF → STREAMON → DQBUF`。
- buffer 所有权在 APP、vb2 和硬件之间的迁移。
- 单平面与多平面 API。
- 结合现有 `v4l2_camera_view` 代码说明 APP 主线。

### 5.3 `02-V4L2-Core设备模型与ioctl调用链.md`

- `v4l2_device` 是实例级容器和 subdev 管理者，不是 `/dev/videoX`。
- `video_device` 表示用户可见的视频节点。
- `v4l2_fh` 表示一次打开产生的文件句柄上下文。
- 字符设备公共 `v4l2_fops` 到驱动 `v4l2_file_operations` 的调用关系。
- `video_ioctl2()`、`v4l2_ioctl_ops` 和标准 ioctl 检查。
- open、poll、mmap、ioctl 的调用链。
- 注册、注销和对象生命周期。

### 5.4 `03-videobuf2缓冲区管理.md`

- vb2 在 V4L2 中的准确定位。
- `vb2_queue`、`vb2_buffer`、`vb2_v4l2_buffer` 和驱动私有 buffer。
- `vb2_ops`、`vb2_mem_ops`、`vb2_buf_ops` 的职责边界。
- MMAP 内存分配和映射过程。
- DEQUEUED、PREPARING、QUEUED、ACTIVE、DONE、ERROR 状态。
- `REQBUFS`、`QBUF`、`STREAMON`、中断/DMA 完成、`DQBUF` 调用链。
- streamoff 时必须归还所有 ACTIVE buffer。
- vmalloc、dma-contig、dma-sg 的选型。

### 5.5 `04-Controls参数控制框架.md`

- format 和 control 的区别。
- `v4l2_ctrl_handler`、`v4l2_ctrl` 和 control ops。
- 标准 control、菜单、复合 control 和 control cluster。
- APP control ID 如何到达具体硬件设置。
- UVC entity/selector 映射作为实例。
- Sensor 曝光、增益、翻转等 controls 的典型实现。

### 5.6 `05-简单V4L2设备驱动编写.md`

- 适用范围：虚拟摄像头、USB 整体设备、无需 media graph 的简单 capture 设备。
- 私有设备对象设计。
- 初始化 `v4l2_device`、mutex/spinlock、vb2 queue、video device。
- 实现 fops、ioctl ops、vb2 ops。
- 格式协商。
- buffer 入驱动队列、数据生产、`vb2_buffer_done()`。
- start/stop streaming。
- controls。
- probe/remove 与错误回滚。
- 提供 Linux 4.9 风格的可编译骨架，硬件数据源使用清晰的占位接口隔离。

### 5.7 `06-Media-Controller与V4L2-Subdev.md`

- 为什么复杂设备不能只用一个 `video_device`。
- subdev 用于“操作模块”，Media Controller 用于“描述连接关系”。
- `v4l2_subdev` 及 core/video/pad ops。
- `media_device`、`media_entity`、`media_pad`、`media_link`、pipeline。
- subdev 与 entity 的包含关系。
- sink/source pad。
- link setup、link validation 和 pipeline start。
- `/dev/v4l-subdevX` 与 `/dev/mediaX` 的用户空间接口。
- async notifier 和固件图端点的作用。

### 5.8 `07-MIPI摄像头Pipeline与驱动编写.md`

- 典型链路：Sensor → D-PHY/CSI-2 → CSI Receiver → ISP → Scaler → Capture/DMA。
- 数据链路和 I2C 控制链路是两条不同路径。
- Sensor subdev 驱动伪代码：电源、时钟、GPIO、regulator、mbus format、frame interval、controls、`s_stream`。
- CSI/ISP/Scaler subdev 的 pad format 和 stream 控制职责。
- Capture 驱动的 video node、vb2 queue 和 DMA。
- 上层聚合驱动怎样注册 media/v4l2 device、绑定 subdev 和创建 links。
- 设备树 graph endpoint 与异步匹配。
- format 从 video node 到各 subdev pad 的协商和传播。
- pipeline 启动、停止和错误回滚顺序。

### 5.9 `08-完整数据流与典型调用链.md`

- APP 查询和设置格式的控制流。
- controls 设置流。
- buffer 分配流。
- QBUF 和 STREAMON 流。
- UVC 的 URB 完成到 `vb2_buffer_done()`。
- SoC capture 的 DMA 中断到 `vb2_buffer_done()`。
- DQBUF 唤醒 APP。
- MIPI pipeline 的注册和 streamon 时序。
- 用 Mermaid 时序图和调用链树集中展示。

### 5.10 `09-调试工具与常见问题.md`

- `v4l2-ctl`、`media-ctl`、`v4l2-compliance`、`yavta`。
- 查询节点、格式、controls 和 topology。
- 先验证拓扑，再验证 subdev format，最后验证 video node streaming。
- 常见问题：格式不一致、没有 buffer、STREAMON 失败、poll 超时、花屏、丢帧、bytesused 异常、streamoff 卡死。
- 通过 trace、dynamic debug、debugfs、内核日志和中断计数定位问题。

## 6. 三条贯穿全文的流程

### 6.1 控制路径

```text
APP ioctl
  → VFS
  → V4L2 公共字符设备层
  → video_device.fops / video_ioctl2
  → v4l2_ioctl_ops 或 controls framework
  → 主驱动或 subdev
  → I2C、USB control transfer 或硬件寄存器
```

### 6.2 数据路径

```text
Sensor/USB 摄像头
  → 接口控制器或 USB URB
  → 驱动取出 ACTIVE buffer
  → DMA 或 CPU 填充 plane
  → 设置 payload、sequence、timestamp
  → vb2_buffer_done()
  → vb2 done queue
  → poll 唤醒
  → VIDIOC_DQBUF
  → APP 处理
  → VIDIOC_QBUF
```

### 6.3 拓扑路径

```text
设备树 endpoint / USB 描述符 / 板级关系
  → 驱动创建 entity 和 pads
  → async notifier 绑定 subdev
  → 创建 media links
  → media-ctl 查看或配置 links
  → pipeline validation
  → 启动各模块
```

## 7. 示例代码策略

### 7.1 简单设备

提供一个 Linux 4.9 风格、结构完整的 capture 驱动骨架。它必须展示：

- 私有对象及锁。
- video/v4l2/vb2/control 对象间的所有权关系。
- `queue_setup`、`buf_prepare`、`buf_queue`、`start_streaming`、`stop_streaming`。
- 驱动自己的 active buffer 链表。
- 完成一帧时设置 payload、sequence、timestamp 并调用 `vb2_buffer_done()`。
- stop/error 路径归还全部 buffer。
- ioctl ops 和格式状态保存在每个设备实例中。

代码骨架不伪装成真实硬件驱动。硬件启动、停止和取帧明确表示为需要平台实现的边界。

### 7.2 MIPI 设备

使用接近真实内核驱动的伪代码，分别展示：

- Sensor subdev。
- CSI/ISP/Scaler subdev。
- Capture video node。
- 上层 media 聚合驱动。

伪代码强调对象关系、注册顺序、调用方向和错误回滚，不绑定某一家 SoC 的私有寄存器。

## 8. 图示策略

使用 Mermaid 和文本图，避免文档依赖不可编辑的外部截图。至少包含：

- V4L2 总体分层图。
- 核心结构体关系图。
- vb2 buffer 所有权和状态图。
- ioctl 调用时序。
- UVC 数据流。
- MIPI media graph。
- MIPI pipeline 注册和 streamon 时序。

现有老师资料中的图片只作为理解和校验来源，不直接复制进新文档集。

## 9. 准确性规则

- 明确区分框架提供的公共逻辑与驱动必须实现的硬件逻辑。
- 明确区分 `queued_list`、驱动私有 active/irq queue 和 `done_list`，不将它们统称为一个“空闲链表”。
- 明确说明 `QBUF` 是所有权交接，而不是保证硬件已立即使用该 buffer。
- 明确说明 `mmap` 减少的是 APP 与内核之间的显式复制，不代表整条硬件链路绝对零拷贝。
- 明确说明 Media Controller 管拓扑，subdev ops 管模块操作，vb2 管内存与队列。
- 明确说明 `v4l2_device`、`video_device`、`media_device` 是不同层次的对象。
- Linux 4.9 的函数名和结构体成员以对应内核源码为准；引用现代内核概念时增加版本提示。

## 10. 验收标准

完成后，读者应能够：

1. 解释 APP、V4L2 Core、vb2、具体驱动各自的职责。
2. 从任意一个常用 V4L2 ioctl 追踪到驱动回调。
3. 解释一个 buffer 从 APP 到硬件再回到 APP 的完整生命周期。
4. 区分 UVC 整体式设备与 SoC MIPI pipeline 的驱动模型。
5. 解释 subdev 和 Media Controller 为什么同时存在。
6. 按文档骨架写出简单 capture 驱动。
7. 说出 Sensor subdev、CSI/ISP subdev、Capture video node 和聚合驱动分别实现什么。
8. 使用 v4l2-utils 对常见枚举、格式、拓扑和 streaming 问题进行分层排查。

文档还必须满足：

- 所有内部链接有效。
- Mermaid 代码块语法完整。
- 示例中的结构体和回调名称符合 Linux 4.9 风格。
- 不留有 `TODO`、`TBD` 或未解释的关键概念。
