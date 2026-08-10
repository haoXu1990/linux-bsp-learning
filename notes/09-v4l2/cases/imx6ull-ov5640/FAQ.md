# i.MX6ULL + OV5640 DVP 常见问题FAQ

> 项目：100ASK i.MX6ULL、Linux 4.9.88、OV5640 V3、8位DVP、`mx6s_capture.c`、PXP。

## 0. 这份FAQ怎样维护

FAQ只记录以下内容：

1. 反复容易混淆的架构概念。
2. 已通过源码、原理图、测量或上板实验确认的结论。
3. 会影响后续设备树、驱动、编译和调试决策的问题。
4. 可以复用到下一次练习的排查方法。

每条问题包含：

```text
编号
问题
短答案
为什么
确认依据或主文档
状态：已确认 / 待验证
```

临时日志、尚未证实的猜测不直接写成结论。以后解决重要问题时，可以直接说“把这条记入FAQ”。

历史会话太长时，不能保证一次找回所有问题。处理方法是：

```text
现有文档和实验结论作为事实源
  → 分批回填FAQ
  → 后续每解决一个关键问题立即追加
```

---

## 1. 基础概念

### FAQ-C01：OV5640是不是Sensor？

是。OV5640是CMOS图像Sensor，内部包含感光阵列、ADC、ISP、时序控制和DVP/MIPI输出模块。

“Sensor”说明它负责感光和产生图像；“DVP/MIPI”说明图像怎样离开Sensor。

状态：已确认。

### FAQ-C02：UVC、MIPI、DVP、CVBS和CMOS是同一类概念吗？

不是。

| 名称 | 所属层次 |
|---|---|
| CMOS | 感光器件技术 |
| OV5640 | 具体Sensor型号 |
| DVP | 并行数字图像接口 |
| MIPI CSI-2 | 高速串行数字图像接口 |
| CVBS | 模拟复合视频接口 |
| UVC | USB视频设备协议和软件规范 |

一个UVC摄像头内部也可能使用CMOS Sensor，只是模块把图像封装成USB/UVC。

状态：已确认。

### FAQ-C03：为什么是DVP摄像头，却连接i.MX6ULL的CSI？

这里的CSI是i.MX6ULL内部Camera Sensor Interface控制器名称，不等于MIPI CSI-2。

当前链路是：

```text
OV5640 DVP并行输出
  → i.MX6ULL CSI并行输入
```

状态：已确认。

### FAQ-C04：OV5640驱动是不是有I²C和DVP两条路？

更准确地说，整个摄像头系统有两条路径：

```text
控制平面：ov5640_v3.c → I²C → OV5640寄存器
数据平面：OV5640硬件 → DVP引脚 → CSI硬件
```

Linux Sensor驱动主要控制数据平面，图像字节本身不经过`ov5640_v3.c`。

状态：已确认。

### FAQ-C05：OV5640连续输出图像的代码在哪里？

Linux中没有逐像素、逐行、逐帧输出代码。

Linux可见的启动边界是：

```c
ov5640_s_stream()
  → ov5640_start()
  → ov5640_write_reg(0x3008, 0x02)
```

之后由OV5640内部时钟、时序状态机、ISP和DVP发送器持续工作。这些是芯片内部硬件，不是Linux中的`while`。

状态：已确认。

主文档：[V4L2、Media与CSI源码分析](03-V4L2_Media与CSI源码分析.md)

---

## 2. 设备树、IOMUX和硬件

### FAQ-D01：`ov5640:`和`ov5640@3c`分别是什么？

```dts
ov5640: ov5640@3c {
};
```

- 第一个`ov5640:`是label，供其他节点用`&ov5640`引用，可以改名。
- `ov5640@3c`是节点名和unit-address。
- `3c`应与`reg = <0x3c>`对应，表示I²C从地址。

状态：已确认。

### FAQ-D02：pinctrl和`some-gpios`是什么关系？

pinctrl决定PAD复用成什么功能以及电气属性：

```dts
MX6UL_PAD_CSI_DATA05__GPIO4_IO26 0x...
```

GPIO属性说明某个设备要使用哪个GPIO及有效电平：

```dts
some-gpios = <&gpio4 26 GPIO_ACTIVE_HIGH>;
```

通常先由pinctrl把PAD复用为GPIO，再由GPIO消费者请求和控制它。

状态：已确认。

### FAQ-D03：`iomuxc`和`iomuxc_snvs`怎样选择？

普通SoC PAD使用`iomuxc`。属于SNVS低功耗域的专用PAD使用`iomuxc_snvs`。

判断依据是芯片手册中的PAD归属和对应的IOMUX寄存器模块，不是根据用途猜测。CSI_DATA、CSI_PIXCLK等普通CSI PAD属于`iomuxc`。

状态：已确认。

### FAQ-D04：`csi1_ep`和`ov5640_ep`会传输图像吗？

不会。它们是设备树graph中的label和endpoint节点，用于描述软件拓扑及V4L2 async绑定。

真正图像走PCB上的PCLK、VSYNC、HREF和D0～D7。endpoint不替代原理图、pinctrl、Sensor寄存器和CSI寄存器配置。

状态：已确认。

### FAQ-D05：`reg = <0x3c>`和读取寄存器`0x300a`有什么关系？

没有地址层次上的关联：

- `0x3c`是OV5640在I²C总线上的从设备地址。
- `0x300a`是OV5640芯片内部的Chip ID寄存器地址。

一次读取大致是：

```text
先通过I²C寻址从设备0x3c
  → 再告诉它读取内部寄存器0x300a
```

状态：已确认。

### FAQ-D06：设备树写`&i2c1`，为什么用户空间看到`/dev/i2c-0`？

设备树控制器编号、SoC手册实例编号和Linux动态分配的adapter编号不是同一套命名。

应通过：

```sh
i2cdetect -l
```

根据控制器物理地址确认映射，不要只看数字猜测。

状态：已确认。

### FAQ-D07：模块带24MHz晶振，设备树里的MCLK是否一定要输出？

不一定。若模块自己的24MHz振荡器直接给OV5640提供XVCLK，SoC不需要再从CSI_MCLK引脚输出时钟。

但旧驱动可能仍要求`mclk`、`clocks`属性用于内部逻辑或校验。应分别确认：

1. 原理图中OV5640 XVCLK实际来源。
2. 驱动是否操作SoC CSI MCLK。
3. 示波器测得的时钟是否存在。

状态：硬件时钟来源已确认；是否能删除全部旧驱动时钟属性需结合驱动逐项验证。

---

## 3. 驱动绑定、编译和部署

### FAQ-K01：没有手动`insmod`，为什么OV5640驱动会自动加载？

设备树创建I²C设备后，内核根据`compatible`和模块生成的alias匹配驱动。udev或模块自动加载机制可以据此加载`.ko`。

模块加载不等于probe成功，仍需查看I²C绑定和dmesg。

状态：已确认。

### FAQ-K02：怎样确认当前生效的是V2还是V3？

组合检查：

```sh
lsmod | grep -i ov5640
ls -l /sys/bus/i2c/devices/0-003c/driver
ls -l /sys/bus/i2c/devices/0-003c/driver/module
```

已经确认V3时，链接应指向`ov5640_v3`和`ov5640_camera_v3`。

状态：已确认。

### FAQ-K03：修改内核config后到底编译和部署什么？

| 修改内容 | 至少需要的产物 |
|---|---|
| DTS | 对应`.dtb` |
| 驱动配置为`m`或模块源码 | `.ko`及`modules.dep`等模块目录 |
| 配置为`y`、核心媒体框架或内核源码 | `zImage` |
| Buildroot目标文件变化 | 根文件系统镜像 |

`.config`是编译输入，不直接烧到板子；配置结果进入zImage或决定生成哪些模块。

状态：已确认。

### FAQ-K04：`disagrees about version of symbol`意味着什么？

运行中的zImage与根文件系统里的`.ko`不是同一次匹配构建，符号版本CRC不一致。

解决方向是同一份源码和`.config`重新生成并成套部署zImage、DTB和模块，而不是继续强行加载旧模块。

状态：已确认。

### FAQ-K05：怎样证明CSI数据由`mx6s_capture.c`处理？

视频节点信息已经给出：

```text
driver   : mx6s-csi
card     : i.MX6S_CSI
bus_info : platform:21c4000.csi
```

并且`/sys/class/video4linux/video1/name`为`mx6s-csi`。源码中`video_register_device()`、IRQ和vb2回调都位于`mx6s_capture.c`。

状态：已确认。

---

## 4. DVP、V4L2和Buffer

### FAQ-V01：DVP到底在哪里配置和工作？

四层共同完成：

```text
原理图       决定物理连线
pinctrl      把PAD复用为CSI
ov5640_v3    配置DVP发送端
mx6s_capture 配置CSI接收端、DMA和中断
```

DVP真正工作在OV5640发送器、PCB信号线和i.MX6ULL CSI接收器之间。

状态：已确认。

主文档：[DVP在哪里工作与mx6s_capture流程](03-V4L2_Media与CSI源码分析.md#19-dvp在哪里工作从引脚到mx6s_capturec完整贯通)

### FAQ-V02：`mx6s_capture.c`的核心职责是什么？

它创建`/dev/videoX`，绑定Sensor Subdev，实现V4L2 ioctl和vb2队列，配置CSI/RxFIFO/DMA，写FB1/FB2地址，在帧完成中断中轮换Buffer并交给APP。

它不感光、不生成YUV、不负责YUV转RGB。

状态：已确认。

### FAQ-V03：`REQBUFS`做什么？

它建立可轮换的Buffer池，确定Buffer数量、每块大小和内存模型。它不启动Sensor和CSI，也不采集图像。

当前CSI至少需要两块已排队Buffer用于FB1/FB2乒乓采集；4块是常见折中，不是按帧率计算出的固定值。

状态：已确认。

主文档：[APP接口与采集流程](../../architecture/01-APP接口与采集流程.md)

### FAQ-V04：连续采集的循环在哪里？

有三层：

```text
OV5640内部硬件状态机持续产生帧
CSI DMA在FB1/FB2之间持续接收
APP执行poll/DQBUF/处理/QBUF循环
```

`mx6s_csi_irq_handler()`只在一帧DMA完成后处理Buffer，不逐像素采集。

状态：已确认。

### FAQ-V05：`YUYV-16`中的16是什么意思？

表示一个YUYV 4:2:2内存像素平均占16bit，也就是2字节/像素。它不是16位Sensor，也不是16根数据线。

当前DVP只有8根数据线，所以一个PCLK传1字节，两个PCLK平均组成一个像素的2字节数据。

状态：已确认。

---

## 5. 已确认故障结论

### FAQ-T01：花屏、分层和重复画面的最终原因是什么？

当前实测根因是OV5640 V3 DVP配置：

```c
{0x4740, 0x23, 0, 0}
```

改为：

```c
{0x4740, 0x21, 0, 0}
```

后显示恢复正常。

这说明发送端DVP同步条件与CSI接收采样不匹配。不是应用YUV转RGB，也不是pinctrl的`0x1b088`导致。

状态：已上板确认，2026-08-10。

### FAQ-T02：为什么当前源码注释仍写`0x4740=0x23`？

`mx6s_capture.c`的`csi_init_interface()`附近存在旧注释，未随OV5640 DVP实测配置更新。代码注释不是硬件事实，应以当前寄存器值、CSI配置和测量结果为准。

状态：已确认需要后续修正注释。

---

## 6. PXP

### FAQ-P01：PXP在链路中负责什么？

PXP是i.MX6ULL图像处理硬件，可完成YUV到RGB、缩放、旋转和叠加，减少CPU逐像素转换开销。

```text
Camera YUYV Buffer
  → PXP硬件
  → RGB Buffer
  → Framebuffer/LCD
```

状态：已确认。

### FAQ-P02：当前`mxc_pxp_v4l2.c`是普通V4L2 M2M驱动吗？

不是。当前旧BSP实现是：

```text
V4L2 VIDEO_OUTPUT输入队列
  → DMAEngine
  → pxp_dma_v3
  → PXP内部RGB输出Buffer
  → framebuffer显示
```

Camera和PXP之间没有自动硬件连接，需要APP把Camera DQBUF得到的Buffer提交给PXP。

状态：已确认。

主文档：[PXP V4L2源码与摄像头显示](04-PXP_V4L2源码与摄像头显示.md)

---

## 7. 待继续补充的问题

以下问题出现新实验结果时再补充，不提前下结论：

- 当前V3所有分辨率下Sensor真实输出的media-bus格式是否一致。
- `mclk`、`mclk_source`在模块自带24MHz晶振条件下哪些可以安全删除。
- PXP输出双Buffer和LCD同步的最终实现方式。
- 当前DTS中DVP极性是否应该改为标准endpoint属性并让驱动解析。
