# 从模块到链路理解 V4L2、Sensor 与 CSI

> 适用环境：100ASK i.MX6ULL、Linux 4.9.88、OV5640 DVP、`ov5640_v2.c + mx6s_capture.c`。
>
> 学习方法：先拆模块，明确输入、输出和职责边界；再找连接点；最后串注册、控制和图像数据链。

## 0. 学习目标与使用方法

学完本文，应能独立回答：

1. V4L2 是什么，它是不是图像搬运程序？
2. OV5640、CSI、V4L2、vb2 分别负责什么？
3. 为什么 OV5640 驱动没有 `vb2_ops`？
4. 为什么换 SoC 时 Capture 驱动会变化？
5. 设备树、总线匹配、async notifier 分别连接什么？
6. APP 的 ioctl 怎样走到 OV5640 I²C 寄存器？
7. 一帧图像怎样进入 APP buffer？
8. probe、配置、采集分别有什么先后关系？

建议分三遍阅读：

```text
第一遍：只读模块卡片，不追函数
第二遍：对照对象图和三条主链
第三遍：打开真实源码逐项验证
```

第一阶段不懂的寄存器先跳过，只问：

```text
谁调用谁？传入什么对象？改变Sensor、CSI还是buffer？
```

---

# 第一部分：先建立总地图

## 1. 一句话认识整个系统

```text
APP通过V4L2提出要求
    ↓
mx6s_capture接待APP、管理buffer和CSI
    ↓ 控制调用
ov5640_v2配置Sensor

OV5640产生图像
    ↓ DVP
i.MX6ULL CSI接收图像
    ↓ DMA
vb2 buffer保存图像
    ↓ DQBUF
APP取得图像
```

> V4L2 是组织者和接口规范；产生像素的是 Sensor，搬运像素的是 CSI/DMA。

## 2. 两种方向相反的流

### 2.1 控制流：APP 走向 Sensor

```text
APP → /dev/videoX → V4L2 Core → mx6s_capture.c
    → v4l2_subdev_call() → ov5640_v2.c
    → I²C → OV5640寄存器
```

负责上电、格式、帧率、mode 和理想情况下的 stream 开关。

### 2.2 图像流：Sensor 走向 APP

```text
光线 → OV5640 → D0～D7/PCLK/VSYNC/HREF
     → i.MX6ULL CSI → DMA → vb2 buffer
     → VIDIOC_DQBUF → APP
```

像素不经过 I²C、`v4l2_subdev_call()`，也不是 V4L2 Core 逐字节复制的。

### 2.3 四个不能画等号的判断

```text
I²C正常 ≠ DVP正常
/dev/videoX存在 ≠ Sensor已经绑定
Sensor found ≠ CSI能收到图像
CSI有中断 ≠ 像素格式解释正确
```

---

# 第二部分：逐个认识模块

## 3. 模块一：OV5640 Sensor 硬件

### 3.1 它是谁

OV5640 是 CMOS 图像传感器，把光转换成数字图像，并按寄存器配置输出像素。

### 3.2 输入与输出

```text
输入：供电、MCLK、PWDN、RESET、SCCB/I²C配置
输出：D0～D7、PCLK、VSYNC、HREF
```

### 3.3 它不知道什么

它不知道 Linux、V4L2、`/dev/videoX`、用户 buffer、CSI 驱动文件名，只按电气信号和寄存器工作。

### 3.4 检查点

```text
为什么I²C不是图像通道？
为什么没有MCLK可能读不到ID？
为什么Sensor不需要DMA地址？
```

## 4. 模块二：`ov5640_v2.c` Sensor 驱动

### 4.1 两个身份

I²C driver：

```c
static struct i2c_driver ov5640_i2c_driver = {
    .probe = ov5640_probe,
    .remove = ov5640_remove,
    .id_table = ov5640_id,
};
```

V4L2 subdev：

```c
v4l2_i2c_subdev_init(&ov5640_data.subdev,
                      client, &ov5640_subdev_ops);
v4l2_async_register_subdev(&ov5640_data.subdev);
```

```text
Linux设备模型身份：I²C驱动
V4L2 Pipeline身份：Sensor subdev
```

### 4.2 负责什么

- 时钟、GPIO、regulator、复位；
- 芯片 ID 和 I²C 寄存器；
- 下载 mode 表；
- 实现 `v4l2_subdev_ops`；
- 注册 async subdev。

### 4.3 不负责什么

- 不创建主要 `/dev/videoX`；
- 不接收 mmap buffer；
- 不执行 CSI DMA；
- 不完成 frame IRQ；
- 不需要 `vb2_ops` 和 Capture 的 file operations。

### 4.4 对外回调

```text
core.s_power        → ov5640_s_power
video.g_parm        → ov5640_g_parm
video.s_parm        → ov5640_s_parm
pad.enum_mbus_code  → ov5640_enum_code
pad.set_fmt         → ov5640_set_fmt
```

这些通常由 Capture 驱动通过 `v4l2_subdev_call()` 调用。

### 4.5 检查点

```text
为什么它既是i2c_driver又是v4l2_subdev？
为什么它没有vb2_ops？
ov5640_write_reg()最终改变哪块硬件？
```

## 5. 模块三：i.MX6ULL CSI 硬件

### 5.1 它是谁

CSI 是 SoC 内部 Camera Sensor Interface。当前项目接收 DVP；这里的 CSI 不等于 MIPI CSI-2。

### 5.2 负责什么

```text
按PCLK采样D0～D7
按VSYNC识别帧
按HREF识别有效行
把像素交给DMA
一帧完成产生IRQ
```

### 5.3 需要什么配置

宽高、像素格式、bytesperline、同步极性、采样沿、DMA 地址、中断和时钟。

### 5.4 它不知道什么

CSI 硬件通常不知道 Sensor 型号。只要接口和时序符合配置，就可以接收。

### 5.5 检查点

```text
为什么换Sensor不一定换CSI驱动？
为什么换SoC通常换Capture驱动？
为什么CSI需要DMA地址而Sensor不需要？
```

## 6. 模块四：`mx6s_capture.c` Capture 驱动

### 6.1 它是谁

i.MX6ULL CSI 的 platform driver，也是 APP 与 Pipeline 的主要入口。

### 6.2 负责什么

- 获取 CSI 寄存器、IRQ、时钟；
- 注册 `v4l2_device` 和 `video_device`；
- 形成 `/dev/videoX`；
- 提供 file operations、ioctl ops、vb2 queue；
- 配置 CSI/DMA，中断完成 buffer；
- 用 async notifier 找 Sensor；
- 用 `v4l2_subdev_call()` 调 Sensor。

### 6.3 不负责什么

不应理解 OV5640 私有寄存器，不产生像素，也不决定 PCB 接了哪颗 Sensor。

### 6.4 三组回调

```text
v4l2_file_operations → open、poll、mmap、unlocked_ioctl
v4l2_ioctl_ops        → S_FMT、REQBUFS、QBUF、STREAMON
vb2_ops               → queue_setup、buf_queue、start_streaming
```

### 6.5 检查点

```text
谁创建/dev/videoX？谁拥有DMA寄存器？
APP的STREAMON先进入哪个驱动？
```

## 7. 模块五：V4L2 Core

### 7.1 它是谁

内核 media 子系统的公共框架，定义对象、注册函数、标准 ioctl 分发和用户 ABI。

### 7.2 负责什么

- 定义 `video_device`、`v4l2_device`、`v4l2_subdev`；
- 注册视频节点；
- 复制、校验并分发 ioctl 参数；
- 提供 subdev call 和 async notifier；
- 让不同硬件共享统一 ABI。

### 7.3 不负责什么

不理解 OV5640 寄存器，不知道 CSI 寄存器地址，不配置 DMA，也不读取 DVP 像素。

### 7.4 `video_ioctl2` 的位置

```text
APP ioctl → VFS → video_device.fops.unlocked_ioctl
→ video_ioctl2 → 查v4l2_ioctl_ops
→ mx6s_capture具体回调
```

### 7.5 检查点

```text
V4L2 Core为什么能服务USB和SoC摄像头？
统一ABI与具体硬件在哪里分界？
```

## 8. 模块六：videobuf2

### 8.1 它是谁

videobuf2，简称 vb2，是 V4L2 常用的 buffer 队列框架。

### 8.2 负责什么

buffer 分配/导入、内存模型、状态机、队列、STREAMON/OFF 通用检查、等待和唤醒，并回调驱动 `vb2_ops`。

```text
DEQUEUED → PREPARED → QUEUED → ACTIVE → DONE → DEQUEUED
```

### 8.3 不负责什么

不决定 Sensor mode、VSYNC 极性，不自动写 CSI DMA 地址，也不产生中断。

### 8.4 检查点

```text
QBUF表示APP交出什么所有权？
为什么STREAMON前需要buffer？谁调用vb2_buffer_done()？
```

## 9. 模块七：设备树

### 9.1 描述三类信息

```text
设备身份：compatible
本地资源：地址、IRQ、时钟、GPIO、pinctrl
拓扑关系：port、endpoint、remote-endpoint
```

```dts
ov5640_ep: endpoint { remote-endpoint = <&csi1_ep>; };
csi1_ep: endpoint { remote-endpoint = <&ov5640_ep>; };
```

### 9.2 它不负责什么

不会主动调用 probe、搬运图像或替驱动写寄存器，只是硬件描述数据。

### 9.3 检查点

```text
compatible和remote-endpoint有什么区别？
为什么有Sensor节点还需要CSI节点？
```

## 10. 模块八：V4L2 async notifier

### 10.1 它解决什么

OV5640 是 I²C driver，CSI 是 platform driver，probe 顺序没有保证。CSI 借助 notifier 等待 endpoint 指向的 Sensor subdev。

### 10.2 它不解决什么

不负责供电、DVP、格式兼容、DMA buffer 或 stream 顺序，只负责对象识别和绑定。

### 10.3 当前驱动的关键结果

```c
csi_dev->sd = subdev;
```

```text
csi_dev->sd → OV5640的v4l2_subdev
```

这根指针是 Capture 调用 Sensor 回调的桥梁。

### 10.4 检查点

```text
为什么Sensor和CSI不要求固定probe顺序？
endpoint与notifier怎样配合？
csi_dev->sd什么时候才有效？
```

---

# 第三部分：把模块连接起来

## 11. 四层连接缺一不可

| 层次 | 连接什么 | 主要证据 |
| --- | --- | --- |
| 硬件连接 | OV5640 输出接到哪个接收器 | 原理图、PCB、DVP 引脚 |
| 拓扑描述 | Sensor endpoint 指向哪个 CSI | DTS `remote-endpoint` |
| 驱动匹配 | 每块硬件由哪个驱动管理 | `compatible`、总线 match |
| V4L2 对象绑定 | Capture 怎样拿到 Sensor 对象 | async notifier、`csi_dev->sd` |

必须逐层回答：

```text
原理图：OV5640接到i.MX6ULL CSI
DTS：ov5640_ep连接csi1_ep
总线：ov5640匹配ov5640_v2，CSI匹配mx6s_capture
V4L2：async bound令csi_dev->sd指向ov5640 subdev
```

不能只因为两个 `.c` 文件放在同一个目录，就认为它们天然配对。

## 12. 内核对象总图

```text
用户空间
┌──────────────────────────────┐
│ APP：open/ioctl/mmap/poll    │
└──────────────┬───────────────┘
               │ /dev/videoX
内核           ▼
┌──────────────────────────────┐
│ struct video_device          │
│ fops + ioctl_ops + vb2_queue │
└──────────────┬───────────────┘
               │ video_drvdata
               ▼
┌──────────────────────────────┐
│ struct mx6s_csi_dev          │
│ CSI寄存器、IRQ、DMA、buffer  │
│ struct v4l2_subdev *sd ──────┼────┐
└──────────────────────────────┘    │
                                    │ csi_dev->sd
                                    ▼
                         ┌───────────────────────┐
                         │ ov5640_data.subdev    │
                         │ v4l2_subdev_ops       │
                         └──────────┬────────────┘
                                    │ I²C
                                    ▼
                                OV5640硬件
```

这张图只记两根关键指针：

```text
video_device → mx6s_csi_dev
mx6s_csi_dev.sd → ov5640 subdev
```

APP 找 Capture，Capture 再找 Sensor。

## 13. 启动注册链：probe 没有固定先后顺序

### 13.1 OV5640 一侧

```text
设备树ov5640@3c
→ I²C Core创建i2c_client
→ 匹配i2c_device_id "ov5640"
→ ov5640_probe
→ 获取GPIO、MCLK、电源
→ 复位并读芯片ID
→ 初始化默认mode
→ v4l2_i2c_subdev_init
→ v4l2_async_register_subdev
```

### 13.2 CSI 一侧

```text
设备树csi@021c4000
→ platform创建platform_device
→ compatible匹配mx6s_csi_driver
→ mx6s_csi_probe
→ 获取寄存器、IRQ、时钟
→ v4l2_device_register
→ video_register_device
→ 得到/dev/videoX
→ 解析endpoint
→ v4l2_async_notifier_register
```

### 13.3 两种合法顺序

CSI 先来：

```text
CSI注册notifier并等待
→ Sensor以后注册subdev
→ async匹配
→ bound()
```

Sensor 先来：

```text
Sensor先注册subdev
→ CSI以后注册notifier
→ async发现已有匹配对象
→ bound()
```

两种情况最后都执行：

```c
csi_dev->sd = subdev;
```

### 13.4 真正需要保证的顺序

probe 顺序不固定，但 APP 真正使用 Pipeline 前，必须满足：

```text
Sensor probe成功
CSI probe成功
async bound完成
csi_dev->sd有效
```

当前老 BSP 可能在 bound 前就出现 `/dev/videoX`，所以视频节点存在不能证明 Sensor 已绑定。

## 14. 为什么换平台时 Capture 驱动会变

Sensor 只负责输出某种接口和格式；Capture 驱动管理的是 SoC 内部接收器。

```text
同一颗OV5640
├─ 接i.MX6ULL CSI → mx6s_capture
├─ 接STM32 DCMI   → STM32 DCMI驱动
└─ 接Rockchip CIF → Rockchip CIF驱动
```

确认当前 Capture 驱动的方法：

```text
原理图找接收控制器
→ DTS沿remote-endpoint找到远端节点
→ 查远端compatible
→ 搜驱动of_match_table
→ 查Kconfig/Makefile生成哪个模块
```

### 14.1 本项目如何实证 CSI 由 `mx6s_capture.c` 管理

不能因为文件名像就直接下结论。应沿着下面的证据链逐层确认：

```text
CSI硬件地址
→ DTS节点及compatible
→ 驱动of_match_table
→ platform driver名称及模块
→ /dev/videoX报告的driver和bus_info
→ 实际STREAMON调用栈
```

#### 证据一：CSI 硬件节点

`imx6ull.dtsi` 中定义：

```dts
csi: csi@021c4000 {
    compatible = "fsl,imx6ul-csi", "fsl,imx6s-csi";
    reg = <0x021c4000 0x4000>;
    status = "disabled";
};
```

板级 DTS 通过 `&csi { status = "okay"; };` 启用它，并用 endpoint 连接 OV5640。因此，本项目的 CSI 控制器是地址为 `0x021c4000` 的 platform device。

#### 证据二：驱动的设备树匹配表

`mx6s_capture.c` 包含：

```c
static const struct of_device_id mx6s_csi_dt_ids[] = {
    { .compatible = "fsl,imx6s-csi", ... },
    { .compatible = "fsl,imx6sl-csi", ... },
    { }
};

static struct platform_driver mx6s_csi_driver = {
    .driver = {
        .name = "mx6s-csi",
        .of_match_table = mx6s_csi_dt_ids,
    },
    .probe = mx6s_csi_probe,
    .remove = mx6s_csi_remove,
};
```

i.MX6ULL 节点同时提供了 `"fsl,imx6s-csi"` 兼容字符串，因此能够匹配这份驱动。芯片虽然叫 i.MX6ULL，驱动仍叫 `mx6s_capture.c`，是因为多个 i.MX6 系列 SoC 复用了这套 CSI IP 和驱动。

#### 证据三：`VIDIOC_QUERYCAP` 的实测结果

```text
driver   : mx6s-csi
card     : i.MX6S_CSI
bus_info : platform:21c4000.csi
```

这里的 `mx6s-csi` 是 platform driver 名称，`21c4000.csi` 对应 DTS 中地址为 `0x021c4000` 的 CSI platform device。因此 `/dev/video1` 是由 `mx6s_capture.c` 注册和管理的采集节点。

#### 证据四：实际 STREAMON 调用栈

```text
vb2_plane_cookie
→ mx6s_start_streaming [mx6s_capture]
→ vb2_start_streaming
→ mx6s_vidioc_streamon [mx6s_capture]
```

方括号中的 `[mx6s_capture]` 表示函数来自 `mx6s_capture.ko`。这直接证明 `VIDIOC_STREAMON`、vb2 buffer 和 CSI DMA 启动路径正在经过该模块。

#### 板端复核命令

```bash
readlink -f /sys/class/video4linux/video1/device
readlink -f /sys/bus/platform/devices/21c4000.csi/driver
readlink -f /sys/bus/platform/devices/21c4000.csi/driver/module
```

预期能分别追到 `21c4000.csi`、`mx6s-csi` 和 `module/mx6s_capture`。

#### 必须保留的边界判断

```text
确认mx6s_capture.c负责CSI接收
≠
确认当前图像异常一定由mx6s_capture.c造成
```

`ov5640_v3.c` 负责通过 I²C 配置 Sensor，并控制 Sensor 开始或停止输出；`mx6s_capture.c` 负责 i.MX6ULL CSI 寄存器、DMA、vb2 queue 和 `/dev/video1`。图像异常仍可能来自 Sensor 输出配置、DVP 时序、CSI 采样配置或 DMA 打包，必须继续用证据逐层排除。

```text
OV5640 Sensor
  └─DVP：D0~D7、PCLK、VSYNC、HREF
      └─i.MX6ULL CSI硬件（0x021c4000）
          └─mx6s_capture.c：CSI寄存器、DMA、vb2、/dev/video1
              └─V4L2应用程序
```
确认 Sensor 版本的方法：

```text
先看DVP还是MIPI
→ 看SoC/BSP的框架版本
→ 看Capture使用async subdev还是老int-device
→ 看DTS生成的I²C modalias
→ 对照sensor驱动id_table
```

当前项目实测结论：DVP + i.MX6ULL + async subdev；Sensor 当前绑定 `ov5640_camera_v3.ko`，CSI Host 当前绑定 `mx6s_capture.ko`。应以 `lsmod`、sysfs 的 `driver/module` 链接和实际调用栈为准，不再仅凭文件名或最初方案判断版本。

---

# 第四部分：串控制调用链

## 15. `open()`：从视频节点到 Sensor 时钟

APP：

```c
fd = open("/dev/video0", O_RDWR);
```

调用链：

```text
APP open
→ VFS
→ video_device.fops.open
→ mx6s_csi_open
→ 初始化vb2_queue
→ pm_runtime_get_sync(CSI)
→ csi_dev->sd
→ v4l2_subdev_call(sd, core, s_power, 1)
→ ov5640_subdev_core_ops.s_power
→ ov5640_s_power
→ clk_enable(sensor_clk)
```

这里首次把三个模块串起来：

```text
V4L2 video_device → Capture → Sensor subdev
```

## 16. `VIDIOC_ENUM_FMT`：询问 Sensor 能输出什么

```text
APP VIDIOC_ENUM_FMT
→ video_ioctl2
→ mx6s_vidioc_enum_fmt_vid_cap
→ csi_dev->sd
→ v4l2_subdev_call(sd, pad, enum_mbus_code)
→ ov5640_enum_code
→ MEDIA_BUS_FMT_YUYV8_2X8
→ mx6s_capture映射为V4L2_PIX_FMT_YUYV
→ 返回APP
```

要区分：

```text
mbus code：Sensor与CSI线路上的格式
FourCC：CSI DMA写入内存后的布局
```

## 17. `VIDIOC_S_FMT`：内存格式与总线格式协商

```text
APP VIDIOC_S_FMT
→ video_ioctl2
→ mx6s_vidioc_s_fmt_vid_cap
→ mx6s_vidioc_try_fmt_vid_cap
→ v4l2_fill_mbus_format
→ v4l2_subdev_call(sd, pad, set_fmt)
→ ov5640_set_fmt
→ 返回修正后的mbus format
→ mx6s保存pix、mbus_code、sizeimage
→ mx6s_configure_csi
```

当前老驱动的特殊点：`ov5640_set_fmt()` 主要校验 mbus code，没有根据 width/height 下载 mode 表。

## 18. `VIDIOC_S_PARM`：真正走到 Sensor 寄存器

这是当前 BSP 最完整的“V4L2 到 Sensor”链：

```text
APP VIDIOC_S_PARM
→ video_ioctl2
→ mx6s_vidioc_s_parm
→ csi_dev->sd
→ v4l2_subdev_call(sd, video, s_parm, a)
→ ov5640_subdev_video_ops.s_parm
→ ov5640_s_parm
→ 解析timeperframe和capturemode
→ ov5640_change_mode
→ ov5640_change_mode_direct
  或ov5640_change_mode_exposure_calc
→ ov5640_download_firmware
→ ov5640_write_reg
→ i2c_master_send
→ OV5640硬件寄存器
```

把它压缩成五级：

```text
用户ABI
→ V4L2公共分发
→ Capture驱动
→ Sensor subdev回调
→ I²C硬件操作
```

## 19. 控制链速查表

| APP操作 | Capture入口 | subdev调用 | OV5640回调 | 结果 |
| --- | --- | --- | --- | --- |
| `open` | `mx6s_csi_open` | `core.s_power` | `ov5640_s_power` | 打开MCLK |
| `ENUM_FMT` | `mx6s_vidioc_enum_fmt_vid_cap` | `pad.enum_mbus_code` | `ov5640_enum_code` | 返回YUYV能力 |
| `S_FMT` | `mx6s_vidioc_s_fmt_vid_cap` | `pad.set_fmt` | `ov5640_set_fmt` | 协商mbus格式 |
| `S_PARM` | `mx6s_vidioc_s_parm` | `video.s_parm` | `ov5640_s_parm` | I²C切换mode |
| `STREAMON` | `mx6s_vidioc_streamon` | `video.s_stream` | 当前未实现 | BSP特殊点 |
| `close` | `mx6s_csi_close` | `core.s_power` | `ov5640_s_power` | 关闭MCLK |

---

# 第五部分：串 buffer 与图像数据链

## 20. 为什么先准备 buffer

APP 的典型顺序：

```text
VIDIOC_REQBUFS
→ VIDIOC_QUERYBUF
→ mmap
→ VIDIOC_QBUF × N
→ VIDIOC_STREAMON
```

含义：

```text
REQBUFS：建立vb2 buffer集合
mmap：把内核buffer映射到APP
QBUF：APP暂时交出buffer所有权
STREAMON：允许硬件开始写已排队buffer
```

没有可用 buffer 就启动 DMA，硬件没有安全的目标地址。

## 21. `VIDIOC_STREAMON` 的理想顺序

合理原则是先准备下游，最后启动上游数据源：

```text
1. Sensor供电和时钟就绪
2. 配置Sensor格式、分辨率、帧率
3. APP排队足够buffer
4. 配置CSI格式和DMA地址
5. 启动CSI、DMA和中断
6. 最后Sensor s_stream(1)
```

调用模型：

```text
APP STREAMON
→ video_ioctl2
→ mx6s_vidioc_streamon
→ vb2_streamon
→ mx6s_start_streaming
→ 取两块queued buffer
→ vb2_dma_contig_plane_dma_addr
→ 写CSI两个帧地址
→ mx6s_csi_enable
→ v4l2_subdev_call(sd, video, s_stream, 1)
→ Sensor开始输出
```

为什么 Sensor 最后启动：如果 Sensor 已经发送而接收端和 DMA 未就绪，可能丢帧、FIFO overflow 或同步异常。

## 22. 当前 BSP 的 `s_stream` 特例

`mx6s_capture.c` 会调用：

```c
v4l2_subdev_call(sd, video, s_stream, 1);
```

但 `ov5640_v2.c` 没有实现 `.s_stream`。因此当前实际行为更接近：

```text
ov5640_probe/init_device或S_PARM
→ Sensor已被配置并可能持续输出

APP STREAMON
→ 主要启动CSI和DMA
→ 调用Sensor s_stream，但没有对应实现
```

这不是 V4L2 理论要求，而是当前 NXP 老 BSP 的实现缺口。学习时必须区分：

```text
通用合理模型
与
当前代码实际行为
```

## 23. 一帧图像怎样进入 APP

这条链不再是函数一路调用 Sensor，而是硬件数据流：

```text
光线照到CMOS像素
→ OV5640内部生成YUYV
→ 按PCLK从D0～D7输出
→ VSYNC标记帧边界
→ HREF标记有效行
→ i.MX6ULL CSI采样
→ DMA写active buffer
→ frame done IRQ
→ mx6s_csi_irq_handler
→ mx6s_csi_frame_done
→ 设置payload、sequence、timestamp
→ vb2_buffer_done(DONE)
→ 唤醒poll或阻塞DQBUF
→ APP VIDIOC_DQBUF取得buffer
```

关键所有权变化：

```text
APP持有buffer
→ QBUF交给vb2/驱动
→ DMA写入
→ vb2_buffer_done
→ DQBUF归还APP
→ APP处理后再次QBUF
```

## 24. STREAMOFF 与 close

理想停止顺序通常与启动相反：

```text
Sensor停止输出
→ 停CSI/DMA/IRQ
→ 归还所有active buffer
→ 关闭时钟和电源
```

当前 BSP：

```text
APP STREAMOFF
→ mx6s_vidioc_streamoff
→ vb2_streamoff
→ mx6s_stop_streaming
→ 停CSI并归还buffer
→ 尝试Sensor s_stream(0)，但当前未实现

APP close
→ mx6s_csi_close
→ v4l2_subdev_call(sd, core, s_power, 0)
→ ov5640_s_power
→ 关闭Sensor MCLK
```

---

# 第六部分：完整时序与学习路线

## 25. 一张完整时序表

```text
【内核启动】
创建设备
├─ i2c_client：OV5640
└─ platform_device：CSI

驱动probe，顺序不固定
├─ ov5640_probe → 注册subdev
└─ mx6s_csi_probe → 注册video_device和notifier

async bound
└─ csi_dev->sd = ov5640_subdev

【APP打开】
open /dev/videoX
├─ 初始化vb2
└─ Sensor s_power(1)

【配置】
S_FMT  → Sensor set_fmt，协商mbus格式
S_PARM → Sensor s_parm，I²C写mode寄存器

【准备内存】
REQBUFS → mmap → QBUF × N

【开始】
STREAMON
├─ vb2启动
├─ CSI取得DMA地址
├─ 启动CSI/DMA
└─ 理论上Sensor s_stream(1)

【持续采集】
Sensor DVP → CSI → DMA → IRQ
→ vb2_buffer_done → DQBUF/QBUF循环

【停止】
STREAMOFF → 停CSI/DMA并归还buffer
close → Sensor s_power(0)
```

## 26. 三条主线最终合并

### 注册线

```text
DTS → 总线设备 → 两个probe → async bound → csi_dev->sd
```

### 控制线

```text
APP ioctl → V4L2 Core → Capture → subdev ops → I²C → Sensor
```

### 数据线

```text
Sensor → DVP → CSI → DMA → vb2 → APP
```

三条线只有放在一起，才是完整摄像头系统。

## 27. 按老师思路安排六次复习

### 第一次：只看硬件模块

目标：画出控制通道和数据通道。

只读：本文第 1～5 节。

输出：手画下面两条线，不看文档也能复述。

```text
CPU I²C → Sensor寄存器
Sensor DVP → CSI → DMA → DDR
```

### 第二次：只看驱动模块

目标：分清 `ov5640_v2.c` 与 `mx6s_capture.c`。

源码入口：

```text
ov5640_i2c_driver、ov5640_probe、ov5640_subdev_ops
mx6s_csi_driver、mx6s_csi_probe、mx6s_csi_fops
```

输出：为每个驱动写“负责/不负责”各三项。

### 第三次：只看注册绑定

目标：解释 `csi_dev->sd` 从哪里来。

源码入口：

```text
v4l2_async_register_subdev
mx6sx_register_subdevs
subdev_notifier_bound
```

输出：分别画 Sensor 先 probe 和 CSI 先 probe 两种时序。

### 第四次：只追一个控制命令

目标：把 `VIDIOC_S_PARM` 一直追到 `i2c_master_send()`。

禁止同时追其他 ioctl，先把这一条完全走通。

输出：不看文档写出至少八级函数链。

### 第五次：只追一个 buffer

目标：理解一个 buffer 的所有权循环。

```text
APP → QBUF → queued → active → DMA
→ IRQ → done → DQBUF → APP
```

输出：说明为什么 DMA 不能写已经 DQBUF 给 APP 的 buffer。

### 第六次：完整串联

目标：从开机讲到抓到第一帧。

要求讲清：

```text
设备怎样出现
驱动怎样匹配
Sensor和CSI怎样绑定
格式怎样配置
buffer怎样准备
硬件怎样开始
一帧怎样完成
```

## 28. 阅读源码时的停靠点

不要一次从文件头读到文件尾。每次只停靠一个对象：

| 想理解什么 | 先看哪里 |
| --- | --- |
| Sensor怎样进入内核 | `ov5640_i2c_driver`、`ov5640_probe` |
| Sensor提供哪些V4L2能力 | `ov5640_subdev_ops` |
| CSI怎样进入内核 | `mx6s_csi_driver`、`mx6s_csi_probe` |
| `/dev/videoX`怎样出现 | `video_register_device` |
| 两个驱动怎样认识 | `mx6sx_register_subdevs`、`bound` |
| ioctl怎样分发 | `mx6s_csi_fops`、`mx6s_csi_ioctl_ops` |
| mode怎样写入Sensor | `mx6s_vidioc_s_parm` 到 `ov5640_write_reg` |
| DMA怎样拿buffer | `mx6s_start_streaming` |
| 一帧怎样完成 | IRQ handler、`mx6s_csi_frame_done` |

## 29. 最终自测题

1. V4L2、Sensor 驱动、CSI 驱动、vb2 中谁真正搬运像素？
2. `video_device` 与 `v4l2_subdev` 分别代表谁？
3. `csi_dev->sd` 在什么时候赋值，为什么重要？
4. probe 为什么不要求固定顺序，采集启动为什么有合理顺序？
5. `VIDIOC_S_FMT` 和当前 BSP 的 `VIDIOC_S_PARM` 分别做什么？
6. 为什么 `ov5640_v2.c` 没有 `vb2_ops`？
7. 为什么图像数据不经过 I²C？
8. 为什么 `/dev/videoX` 出现不能证明完整 Pipeline 正常？
9. 换平台后怎样找到新的 Capture 驱动？
10. 当前 BSP 的 `.s_stream` 有什么缺口？

## 30. 最后只记住这幅图

```text
                       控制流
APP ──V4L2 ioctl──→ Capture ──subdev call──→ Sensor驱动
                         │                       │
                         │                       └──I²C──→ OV5640
                         │
                         └──vb2/DMA管理

                       图像流
OV5640 ──DVP──→ i.MX6ULL CSI ──DMA──→ vb2 buffer ──DQBUF──→ APP

                       绑定关系
DTS endpoint ──→ async notifier ──→ csi_dev->sd = ov5640 subdev
```

如果能不看资料解释这幅图，并能把 `VIDIOC_S_PARM` 和“一帧完成”两条链分别讲通，就真正理解了当前 V4L2、Sensor 与 CSI 的关系。
