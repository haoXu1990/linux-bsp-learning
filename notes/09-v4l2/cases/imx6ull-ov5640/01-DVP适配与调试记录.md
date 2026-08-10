# i.MX6ULL OV5640 DVP 摄像头适配与思路总结

> 适用环境：100ASK i.MX6ULL、Linux 4.9.88、NXP/100ASK BSP、OV5640 DVP 并口摄像头  
> 内核源码：`Z:\work\100ask\imx6ull\100ask_imx6ull-sdk\Buildroot_2020.02.x\output\build\linux-origin_master`  
> 主要设备树：`arch/arm/boot/dts/100ask_imx6ull-14x14.dts`  
> 如果实际使用 mini 板，则把文中的设备树文件替换成 `100ask_imx6ull_mini.dts`。

---

## 1. 先理清思路：我到底在适配什么

### 1.1 不要把 CMOS、DVP、CSI 混在同一层

这几个名词处于不同层次：

| 名词 | 所在层次 | 在本项目中的作用 |
|---|---|---|
| CMOS | 感光技术 | OV5640 内部使用 CMOS 感光 |
| OV5640 | Sensor 型号 | 把光转换成图像数据 |
| DVP | 图像传输总线 | 使用 D0～D7、PCLK、VSYNC、HREF 传图像 |
| I²C/SCCB | 控制总线 | 配置 OV5640 寄存器 |
| CSI | i.MX6ULL 内部摄像头控制器 | 接收 OV5640 的 DVP 并行数据 |
| V4L2 | Linux 摄像头框架 | 向应用提供 `/dev/videoX` |

这里最容易混淆的是：

> i.MX6ULL 手册中的 CSI 是 `Camera Sensor Interface` 控制器，不是 MIPI CSI-2。

本项目的连接关系是：

```text
OV5640 的 DVP 输出
        ↓
i.MX6ULL 的并行 CSI 控制器
        ↓
DMA 写入内存
        ↓
Linux V4L2
        ↓
/dev/videoX
```

### 1.2 控制通道和数据通道是两条不同的路

#### 控制通道

```text
i.MX6ULL I2C1
    │
    ├── SCL
    └── SDA
         ↓
      OV5640
```

I²C/SCCB 用来设置：

- 分辨率；
- 输出格式；
- 帧率；
- 曝光；
- 白平衡；
- 翻转；
- OV5640 内部 PLL 和时序。

#### 数据通道

```text
OV5640
    ├── D0～D7
    ├── PCLK
    ├── VSYNC
    └── HREF
         ↓
i.MX6ULL CSI
```

数据通道用于连续传输图像。

因此：

- I²C 通了，只能证明“CPU 能配置摄像头”；
- `/dev/videoX` 出现了，只能证明“V4L2 设备注册了”；
- 真正抓到内容正确的图像，才能证明整个链路都正常。

### 1.3 Linux 中至少涉及两个驱动

```text
ov5640_v2.c
    │ 负责识别和配置 OV5640
    ↓
mx6s_capture.c
    │ 负责 i.MX6ULL CSI、DMA 和 V4L2 Capture
    ↓
/dev/videoX
```

本 BSP 中应该使用：

```text
drivers/media/platform/mxc/capture/ov5640_v2.c
drivers/media/platform/mxc/capture/mx6s_capture.c
```

不应该选择：

- `ov5640.c`：主要用于 i.MX6Q/DL 的旧框架；
- `ov5640_mipi.c`；
- `ov5640_mipi_v2.c`。

后两个是 MIPI 方向，不适合当前 DVP 模块。

### 1.4 一次完整适配应该按层推进

推荐始终按照下面的顺序调试：

```text
确认启动的 DTB
      ↓
检查硬件接线和管脚复用
      ↓
确认供电、MCLK、PWDN、RESET
      ↓
确认 I²C 和 OV5640 芯片 ID
      ↓
确认 Sensor 驱动 probe
      ↓
确认 CSI 驱动注册
      ↓
确认 /dev/videoX
      ↓
抓取 640×480 小分辨率图像
      ↓
检查颜色、同步、帧率和稳定性
```

不要跳层排查。例如 Sensor 都没有识别时，不要先研究 YUV 颜色错误。

---

## 2. 当前硬件连接

根据 100ASK 原理图，摄像头接口 J5 已经引出完整信号。

### 2.1 OV5640 与 J5/i.MX6ULL 的连接

| OV5640模块 | J5/i.MX6ULL信号 | 作用 |
|---|---|---|
| `3V3` | `VDD_3V3` | 模块供电 |
| `GND` | `GND` | 公共地 |
| `SCL` | `I2C1_SCL` | 寄存器控制时钟 |
| `SDA` | `I2C1_SDA` | 寄存器控制数据 |
| `D0～D7` | `CSI_DATA0～CSI_DATA7` | 8位图像数据 |
| `PCLK` | `CSI_PIXCLK` | 像素时钟 |
| `VS` | `CSI_VSYNC` | 帧同步 |
| `HREF` | `CSI_HSYNC` | 行有效/行同步 |
| `PD` | `74595_CSI_PWREN` | Power Down/电源控制 |
| `RST` | `74595_CSI_RST` | Sensor 复位 |
| MCLK | 模块板载 24 MHz 振荡器 | Sensor 参考时钟；模块18针接口没有引出 XCLK |

### 2.2 本项目的 MCLK 已经确认

重新核对 OV5640 模块原理图和转接板原理图后确认：

- 模块带有 24.000 MHz 有源振荡器；
- 振荡器输出直接连接 OV5640 的 XCLK；
- 模块18针接口没有引出 XCLK，对应位置标为 NC；
- 转接板虽然从开发板 J5 接入 CSI_MCLK，但摄像头模块侧没有把它送到 Sensor。

因此，本项目中 OV5640 的真实参考时钟来自模块自身，而不是 i.MX6ULL 的 CSI_MCLK PAD。

设备树仍保留 clocks、clock-names 和 mclk，是因为当前 4.9 BSP 的 OV5640 V2/V3 驱动会强制获取名为 csi_mclk 的时钟并读取 mclk；删除后会导致 probe 失败。这些属性在本项目中主要用于兼容旧驱动，并不表示 Sensor 的物理 XCLK 来自 SoC。

MX6UL_PAD_CSI_MCLK__CSI_MCLK 对当前模块属于冗余 PAD 配置。调试阶段可以先保留，系统稳定后再移除并释放该 PAD。

### 2.3 PWDN 和 RESET 不是 SoC 直连 GPIO

原理图中：

```text
74595_CSI_PWREN
74595_CSI_RST
```

说明它们由 74HC595 GPIO 扩展器控制，因此设备树使用：

```dts
pwn-gpios = <&gpio_spi 5 GPIO_ACTIVE_HIGH>;
rst-gpios = <&gpio_spi 4 GPIO_ACTIVE_LOW>;
```

这里的 5、4 是 74HC595 内部输出位号，不是 i.MX6ULL 原生 GPIO 编号。它们已经通过实际电压和 I²C 地址 0x3c 验证：bit5 控制 PWDN，bit4 控制 RESET。

PWDN 高有效，RESET 低有效，所以使用 GPIO_ACTIVE_HIGH 和 GPIO_ACTIVE_LOW 表达真实电气语义。当前旧驱动使用整数 GPIO 接口并手动输出电平，不会自动利用极性标志，但仍应按真实含义填写，方便阅读和后续升级。

这也是为什么驱动还没有正确加载时，直接运行 `i2cdetect` 不一定能看到 OV5640：Sensor 可能仍处于 PWDN 或 RESET 状态。

---

## 3. 第一步：确认实际启动的设备树

设备树文件名包含 `100ask` 还不够，需要确认 U-Boot 实际加载的是哪个 DTB。

### 3.1 Linux 中检查

```bash
cat /proc/device-tree/model
cat /proc/cmdline
```

注意：不同 DTB 可能写了相同的 `model`，因此这一结果只能作为参考。

### 3.2 U-Boot 中检查

进入 U-Boot 命令行：

```bash
printenv fdt_file
printenv fdtfile
printenv bootcmd
```

可能使用的文件包括：

```text
100ask_imx6ull-14x14.dtb
100ask_imx6ull_mini.dtb
100ask_myir_imx6ull_mini.dtb
```

本文后续以：

```text
100ask_imx6ull-14x14.dts
```

为例。如果实际启动 mini DTB，必须修改对应的 mini DTS。

### 3.3 建议记录

```text
实际 DTS：
实际 DTB：
DTB 位于启动介质的路径：
U-Boot 加载 DTB 的命令：
```

---

## 4. 第二步：解决 CSI 管脚复用冲突

在现有 `100ask_imx6ull-14x14.dts` 中，部分 CSI 管脚已经被 UART6 和 ECSPI1 占用。

### 4.1 UART6 冲突

UART6 当前使用：

```dts
MX6UL_PAD_CSI_MCLK__UART6_DCE_TX
MX6UL_PAD_CSI_PIXCLK__UART6_DCE_RX
```

但摄像头需要：

```dts
MX6UL_PAD_CSI_MCLK__CSI_MCLK
MX6UL_PAD_CSI_PIXCLK__CSI_PIXCLK
```

启用摄像头时禁用 UART6：

```dts
&uart6 {
    status = "disabled";
};
```

### 4.2 ECSPI1 冲突

ECSPI1 当前使用多个 CSI_DATA 管脚：

```dts
MX6UL_PAD_CSI_DATA04__ECSPI1_SCLK
MX6UL_PAD_CSI_DATA06__ECSPI1_MOSI
MX6UL_PAD_CSI_DATA07__ECSPI1_MISO
MX6UL_PAD_CSI_DATA05__GPIO4_IO26
MX6UL_PAD_CSI_DATA03__GPIO4_IO24
```

启用摄像头时禁用 ECSPI1：

```dts
&ecspi1 {
    status = "disabled";
};
```

原来的 `pinctrl_uart6` 和 `pinctrl_ecspi1` 定义可以暂时保留，只要对应外设是 `disabled`，就不会申请这些管脚。

---

## 5. 第三步：添加 CSI pinctrl

在 `&iomuxc` 内增加：

```dts
pinctrl_csi1: csi1grp {
    fsl,pins = <
        MX6UL_PAD_CSI_MCLK__CSI_MCLK         0x1b088
        MX6UL_PAD_CSI_PIXCLK__CSI_PIXCLK     0x1b088
        MX6UL_PAD_CSI_VSYNC__CSI_VSYNC       0x1b088
        MX6UL_PAD_CSI_HSYNC__CSI_HSYNC       0x1b088

        MX6UL_PAD_CSI_DATA00__CSI_DATA02     0x1b088
        MX6UL_PAD_CSI_DATA01__CSI_DATA03     0x1b088
        MX6UL_PAD_CSI_DATA02__CSI_DATA04     0x1b088
        MX6UL_PAD_CSI_DATA03__CSI_DATA05     0x1b088
        MX6UL_PAD_CSI_DATA04__CSI_DATA06     0x1b088
        MX6UL_PAD_CSI_DATA05__CSI_DATA07     0x1b088
        MX6UL_PAD_CSI_DATA06__CSI_DATA08     0x1b088
        MX6UL_PAD_CSI_DATA07__CSI_DATA09     0x1b088
    >;
};
```

### 5.1 为什么 DATA00 映射到 DATA02

i.MX6UL/ULL 的 CSI 内部数据通路为 10 位，官方 8 位摄像头示例使用内部 `CSI_DATA02～CSI_DATA09`。

因此：

```text
物理 PAD CSI_DATA00 → CSI控制器内部 DATA02
物理 PAD CSI_DATA01 → CSI控制器内部 DATA03
……
物理 PAD CSI_DATA07 → CSI控制器内部 DATA09
```

这是本 BSP 官方 `imx6ull-14x14-evk.dts` 的已有写法，不要根据名字直觉改成 `DATA00 → DATA00`。

---

## 6. 第四步：添加 OV5640 设备树节点

原理图标明摄像头使用 `I2C1`，所以节点应添加在 `&i2c1` 下。

如果 DTS 已有 `&i2c1`，把 `ov5640@3c` 合并进去，不要重复创建两个 `&i2c1` 节点。

```dts
&i2c1 {
    clock-frequency = <100000>;
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_i2c1>;
    status = "okay";

    ov5640: ov5640@3c {
        compatible = "ovti,ov5640";
        reg = <0x3c>;

        pinctrl-names = "default";
        pinctrl-0 = <&pinctrl_csi1>;

        clocks = <&clks IMX6UL_CLK_CSI>;
        clock-names = "csi_mclk";

        pwn-gpios = <&gpio_spi 5 GPIO_ACTIVE_HIGH>;
        rst-gpios = <&gpio_spi 4 GPIO_ACTIVE_LOW>;

        csi_id = <0>;
        mclk = <24000000>;
        mclk_source = <0>;
        status = "okay";

        port {
            ov5640_ep: endpoint {
                remote-endpoint = <&csi1_ep>;
            };
        };
    };
};
```

### 6.1 为什么属性叫 `pwn-gpios`

当前 4.9 BSP 的 `ov5640_v2.c` 明确读取：

```c
of_get_named_gpio(dev->of_node, "pwn-gpios", 0);
of_get_named_gpio(dev->of_node, "rst-gpios", 0);
```

因此当前内核要写：

```dts
pwn-gpios
rst-gpios
```

不要直接套用新内核教程中的：

```dts
powerdown-gpios
reset-gpios
```

设备树属性名必须和当前驱动实际读取的名字一致。

### 6.2 先理解：设备树在这条链路中负责什么

设备树负责描述板级事实，而不是保存图像格式算法。对本项目来说，它需要回答：

1. Sensor 挂在哪个 I²C 控制器、地址是多少；
2. 哪些 PAD 要复用成 DVP 信号；
3. PWDN、RESET 由谁控制；
4. 当前使用 DVP 还是 MIPI；
5. Sensor 的输出端连接到哪个 CSI 接收端；
6. CSI 控制器是否启用。

YUYV/UYVY、分辨率寄存器表和开关流时序属于驱动逻辑，不由当前设备树选择。

### 6.3 为什么 OV5640 放在 &i2c1 下面

原理图连接为：

~~~text
OV5640 SCL → I2C1_SCL
OV5640 SDA → I2C1_SDA
~~~

因此 OV5640 必须作为 &i2c1 的子节点。设备树中的 i2c1 在 Linux 用户空间通常注册为 /dev/i2c-0，所以使用 i2cdetect -y 0 扫到 0x3c 是正常的；设备树控制器编号和用户空间适配器编号不是同一个编号体系。

### 6.4 compatible 和 reg 分别解决什么问题

~~~dts
compatible = "ovti,ov5640";
reg = <0x3c>;
~~~

- compatible 用于匹配 OV5640 I²C 驱动。当前 V2/V3 驱动的 I²C ID 都是 ov5640，因此保持 ovti,ov5640，不要改成 ovti,ov5640_v3。
- reg 是 OV5640 的7位 I²C 从地址，只决定控制命令发给总线上的哪个器件。
- 0x300a、0x300b 是进入 OV5640 以后访问的芯片 ID 寄存器，与 reg = <0x3c> 不是同一层地址。

可以记成：

~~~text
0x3c        = OV5640 在 I²C 总线上的门牌号
0x300a/300b = OV5640 内部的芯片 ID 寄存器
~~~

### 6.5 为什么 Sensor 节点引用 pinctrl_csi1

~~~dts
pinctrl-names = "default";
pinctrl-0 = <&pinctrl_csi1>;
~~~

pinctrl_csi1 定义 PCLK、HSYNC、VSYNC、D0～D7 等 PAD 应复用成 CSI 功能。当前 OV5640 驱动在 probe 时调用 devm_pinctrl_get_select_default()，因此把这个 pinctrl 组挂在 OV5640 节点上，Sensor probe 时就会应用它。

这也与 NXP imx6ull-9x9-evk.dts 的组织方式一致。UART6 和 ECSPI1 中即使还保留旧 pinctrl 定义，只要对应节点为 disabled，就不会在运行时申请 CSI PAD。

### 6.6 为什么时钟属性看起来重复

~~~dts
clocks = <&clks IMX6UL_CLK_CSI>;
clock-names = "csi_mclk";
mclk = <24000000>;
mclk_source = <0>;
~~~

它们处在不同层次：

- clocks 引用 i.MX6ULL 时钟控制器中的 CSI 时钟对象；
- clock-names 必须与驱动中的 devm_clk_get(dev, "csi_mclk") 一致；
- mclk 是旧驱动期望设置的频率；
- mclk_source 是旧驱动要求存在的历史属性，当前代码读取后没有继续使用。

本模块实际由板载24 MHz振荡器驱动 XCLK，但旧驱动仍强制读取这些属性，所以为了通过 probe 暂时保留。

### 6.7 csi_id 与 DVP/MIPI 模式如何确定

~~~dts
csi_id = <0>;
~~~

i.MX6ULL 当前使用 CSI0，所以填写0。旧驱动要求该属性存在，当前代码读取后基本没有继续使用。

本项目是并行 DVP，因此设备树中故意不写下面两个布尔属性：

~~~dts
mipi_csi;
fsl,mipi-mode;
~~~

- OV5640 V3 驱动发现 mipi_csi 才会进入 MIPI 配置分支；没有该属性就是 DVP。
- mx6s_capture 发现 fsl,mipi-mode 才会按 MIPI 输入处理；没有该属性就是并行 CSI 输入。

也不要添加 fsl,two-8bit-sensor-mode。它表示特殊的双8位 Sensor 模式，不是“使用一个8位 OV5640”就必须开启的选项。

### 6.8 endpoint 为什么必须双向连接

I²C 子节点只能说明控制通道，不能说明图像数据输出到哪里。两个 endpoint 用于建立数据通道：

~~~text
OV5640 输出端 ov5640_ep ←→ i.MX6ULL CSI 输入端 csi1_ep
~~~

mx6s_capture 会遍历 &csi 的 port/endpoint，通过 remote-endpoint 找到远端 OV5640 节点，再把它注册为 V4L2 异步 subdev。

因此，缺少 endpoint 时可能出现这种情况：I²C 能读到芯片 ID，但 Sensor 无法与 CSI capture 组成完整链路，也就不能正常形成对应的 /dev/videoX。

### 6.9 为什么当前不添加总线宽度和采样沿属性

标准视频 endpoint 经常出现：

~~~dts
bus-width = <8>;
data-shift = <2>;
hsync-active = <1>;
vsync-active = <1>;
pclk-sample = <1>;
~~~

这些属性在现代驱动中用于描述并行总线宽度、数据线位置、同步有效电平和 PCLK 采样沿。但是当前 4.9 BSP 的 mx6s_capture.c：

- 只通过 endpoint 查找远端 Sensor；
- 没有调用 endpoint 总线参数解析函数；
- 没有读取上述属性；
- PCLK 采样沿、HSYNC 和 VSYNC 极性是在 CSI 驱动中固定设置的。

所以目前不要为了“配置完整”而盲目添加这些属性。设备树里写了某个属性，并不代表驱动一定使用；必须回到源码确认是否存在对应的 of_property_read、of_get_property 或 endpoint 解析代码。

### 6.10 配置项与使用者对照表

| 设备树内容 | 主要使用者 | 解决的问题 |
|---|---|---|
| &i2c1、reg = <0x3c> | I²C核心 | 在正确总线上创建正确地址的器件 |
| compatible | I²C驱动匹配 | 选择 OV5640 驱动 |
| pinctrl_csi1 | pinctrl子系统、OV5640 probe | 把物理 PAD 复用成 CSI 信号 |
| pwn-gpios、rst-gpios | OV5640驱动 | 退出掉电并执行复位 |
| clocks、clock-names、mclk | OV5640旧驱动 | 满足驱动时钟获取和初始化流程 |
| 不存在的 mipi_csi | OV5640 V3驱动 | 选择 DVP 分支 |
| 不存在的 fsl,mipi-mode | mx6s_capture | 选择并行 CSI 输入 |
| remote-endpoint | V4L2异步框架、mx6s_capture | 把 Sensor subdev 连接到 CSI capture |
| &csi 的 status = "okay" | 平台总线 | 启用 i.MX6ULL CSI 控制器 |

### 6.11 新手必须先会拆解一行 DTS

以这一行为例：

~~~dts
ov5640: ov5640@3c {
~~~

它不是一个完整名字，而是三个语法成分：

~~~text
ov5640:       ov5640@3c        {
   │              │            │
   │              │            └─ 节点内容开始，DTS固定语法
   │              └─ 节点名（node name）
   └─ 标签（label）
~~~

以后看到任何 DTS，都先判断它属于 label、节点名、属性名、属性值还是 phandle 引用，不要把它们当成同一种名字。

#### 6.11.1 ov5640: 是 label，可以改名

~~~dts
ov5640:
~~~

冒号前面的是 label，作用是让 DTS 其他位置可以使用 &ov5640 引用这个节点。

它可以改成：

~~~dts
camera_sensor: ov5640@3c {
~~~

前提是所有 &ov5640 引用也同步改成 &camera_sensor。当前设备树实际上没有地方引用 &ov5640，因此这个 label 即使删除，当前功能通常也不会变化。

label 主要由设备树编译器 dtc 在编译阶段处理。驱动不会根据字符串 ov5640: 匹配设备。编译成 DTB 后，引用关系会转成 phandle 数字。

#### 6.11.2 ov5640@3c 是节点名，不是 label

~~~text
ov5640@3c
   │    │
   │    └─ unit-address
   └─ 节点基础名称
~~~

- ov5640 是描述性节点名称。它最好反映器件类型，但当前 I²C 驱动不靠它匹配。
- @3c 是 unit-address，应当与 reg = <0x3c> 一致。
- 真正创建 I²C 设备时，内核读取 compatible 和 reg，而不是依靠 ov5640@3c 这个字符串。

对应源码：

- drivers/i2c/i2c-core.c:1634 调用 of_modalias_node()，根据 compatible 生成设备类型；
- drivers/i2c/i2c-core.c:1640 读取固定属性 reg；
- drivers/of/base.c:1073-1078 的 of_modalias_node() 读取 compatible。

因此把节点写成 camera@3c，在语法和匹配层面也可以工作；但本项目沿用官方示例 ov5640@3c，看到节点名就能识别器件。

#### 6.11.3 port 是节点名，而且本驱动要求这个固定名字

~~~dts
port {
~~~

这里没有冒号，所以 port 不是 label，而是节点名。

在当前驱动中不能随便改成 camera_port。证据位于：

~~~text
drivers/media/platform/mxc/capture/mx6s_capture.c:1766-1768
~~~

核心代码：

~~~c
for_each_available_child_of_node(parent, node) {
    if (of_node_cmp(node->name, "port"))
        continue;
}
~~~

驱动逐个检查 &csi 的子节点，只有节点名等于 port 才继续处理。改成别的名字后，驱动会直接 continue，无法继续寻找远端 Sensor。

为什么要配置 port：它表示设备的媒体数据端口，把 I²C 控制关系和 DVP 图像数据关系分开描述。

#### 6.11.4 ov5640_ep: 是 endpoint 的 label，可以改名

~~~dts
ov5640_ep: endpoint {
~~~

拆开后是：

~~~text
ov5640_ep:       endpoint
     │              │
     │              └─ 节点名
     └─ label
~~~

ov5640_ep 是可以修改的 label。它被 CSI 一侧使用：

~~~dts
remote-endpoint = <&ov5640_ep>;
~~~

如果改成 sensor_out_ep，就必须把对面的引用一起改成：

~~~dts
remote-endpoint = <&sensor_out_ep>;
~~~

#### 6.11.5 endpoint 是标准节点名，不建议改

endpoint 表示 port 中的一个具体连接点。当前 mx6s_capture.c:1771 使用 of_get_next_child(node, NULL) 取得 port 的第一个子节点，并没有直接比较字符串 endpoint。

但是 endpoint 是 Linux OF graph/media binding 的标准结构名称，其他通用函数和未来驱动都按这种结构理解设备树。因此这里应视为规范固定写法，不要因为当前代码没有比较名字就随意修改。

#### 6.11.6 remote-endpoint 是固定属性名

~~~dts
remote-endpoint = <&csi1_ep>;
~~~

remote-endpoint 不是 label，而是属性名。不能改成 remote、peer 或其他名字。

内核固定读取这个字符串，源码位于：

~~~text
drivers/of/base.c:2462-2469
~~~

核心代码：

~~~c
np = of_parse_phandle(node, "remote-endpoint", 0);
~~~

如果属性名写错，of_parse_phandle() 返回空，mx6s_capture 就找不到远端 OV5640。

#### 6.11.7 <&csi1_ep> 是 phandle 引用

~~~text
<  &csi1_ep  >
│      │      │
│      │      └─ 一个设备树 cell 列表结束
│      └─ 引用 label csi1_ep
└─ 一个设备树 cell 列表开始
~~~

csi1_ep 是 CSI 侧 endpoint 的 label，可以改名。它并不表示“CSI1控制器”，也不决定 csi_id。

例如可以把两边改成更直观的名字：

~~~dts
sensor_out_ep: endpoint {
    remote-endpoint = <&csi_in_ep>;
};

csi_in_ep: endpoint {
    remote-endpoint = <&sensor_out_ep>;
};
~~~

只要引用保持对应，功能不变。

#### 6.11.8 为什么两侧都要配置

Sensor 侧：

~~~dts
port {
    ov5640_ep: endpoint {
        remote-endpoint = <&csi1_ep>;
    };
};
~~~

表示“OV5640 的图像输出端连接到 CSI 输入端”。

CSI 侧：

~~~dts
port {
    csi1_ep: endpoint {
        remote-endpoint = <&ov5640_ep>;
    };
};
~~~

表示“CSI 输入端的远端设备是 OV5640”。

mx6s_capture.c:1767 找到名为 port 的节点，1771取得其中第一个 endpoint，1774调用 of_graph_get_remote_port_parent()。该通用函数在 drivers/of/base.c:2469 读取 remote-endpoint，沿引用找到 OV5640 的设备节点。随后 mx6s_capture 把该节点登记为 V4L2 异步匹配对象。

这两段配置不会传输图像，也不会设置 D0～D7。它们只是在软件中声明：

~~~text
这个 OV5640 subdev 和这个 mx6s-csi capture 属于同一条视频链路
~~~

#### 6.11.9 这一组写法的分类总结

| 写法 | 类型 | 能否改名 | 谁使用 |
|---|---|---|---|
| ov5640: | label | 可以；引用必须同步修改 | dtc和DTS中的 &ov5640 |
| ov5640@3c | 节点名 | 基础名可调整；@3c应与reg一致 | 用于设备树层级和可读性 |
| compatible | 固定属性名 | 不可随便改 | I²C/OF匹配，i2c-core.c:1634 |
| reg | 固定属性名 | 不可改；值由硬件地址决定 | i2c-core.c:1640 |
| port | 节点名 | 当前驱动不可改 | mx6s_capture.c:1767 |
| ov5640_ep: | label | 可以；引用必须同步修改 | 对侧 remote-endpoint |
| endpoint | 标准节点名 | 不建议改 | OF graph标准结构 |
| remote-endpoint | 固定属性名 | 不可改 | drivers/of/base.c:2469 |
| csi1_ep | label | 可以；它不是CSI编号 | dtc解析 &csi1_ep |
| <&csi1_ep> | phandle引用 | 跟随label修改 | OF graph建立远端关系 |



---

## 7. 第五步：启用 i.MX6ULL CSI 控制器

在 DTS 中增加：

```dts
&csi {
    status = "okay";

    port {
        csi1_ep: endpoint {
            remote-endpoint = <&ov5640_ep>;
        };
    };
};
```

设备树通过两个 endpoint 表达硬件连接：

```text
ov5640_ep ←────────────→ csi1_ep
```

两边必须互相引用：

```dts
ov5640_ep {
    remote-endpoint = <&csi1_ep>;
};

csi1_ep {
    remote-endpoint = <&ov5640_ep>;
};
```

可以把它理解为：

> 告诉 Linux，OV5640 的输出端连接到了 i.MX6ULL CSI 的输入端。

---

## 8. 建议的设备树修改汇总

最终至少包含五类修改：

```text
1. 禁用 &uart6
2. 禁用 &ecspi1
3. 添加 pinctrl_csi1
4. 在 &i2c1 添加 ov5640@3c
5. 启用 &csi 并连接 endpoint
```

修改前建议保存当前版本：

```bash
git status
git diff -- arch/arm/boot/dts/100ask_imx6ull-14x14.dts
```

修改后先检查重复标签：

```bash
grep -n "pinctrl_csi1\\|ov5640_ep\\|csi1_ep\\|ov5640@3c" \
    arch/arm/boot/dts/100ask_imx6ull-14x14.dts
```

每个标签通常只能定义一次。

---

## 9. 第六步：配置内核驱动

### 9.1 本项目需要的配置

```text
CONFIG_VIDEO_MXC_CAPTURE=m
CONFIG_VIDEO_MXC_CSI_CAMERA=m
CONFIG_MXC_CAMERA_OV5640_V2=m
```

### 9.2 建议关闭错误或多余的 OV5640 驱动

```text
# CONFIG_MXC_CAMERA_OV5640 is not set
# CONFIG_MXC_CAMERA_OV5640_MIPI is not set
# CONFIG_MXC_CAMERA_OV5640_MIPI_V2 is not set
```

原因：

- 当前平台是 i.MX6ULL；
- 当前摄像头是 DVP；
- `ov5640_v2.c` 才是这套 BSP 为 i.MX6UL/ULL 准备的版本；
- 同时加载多个名称相同的 OV5640 I²C 驱动可能产生冲突。

### 9.3 对应模块

预计生成：

```text
ov5640_camera_v2.ko
mx6s_capture.ko
```

可以在编译输出中查找：

```bash
find . -name 'ov5640_camera_v2.ko' -o -name 'mx6s_capture.ko'
```

---

## 10. 第七步：编译并部署

### 10.1 先使用当前 SDK 已验证的编译环境

不要在没有设置交叉编译器的普通终端中直接编译。

需要确认：

```bash
echo "$ARCH"
echo "$CROSS_COMPILE"
```

通常应类似：

```text
ARCH=arm
CROSS_COMPILE=arm-buildroot-linux-gnueabihf-
```

具体前缀以当前 100ASK SDK 为准。

### 10.2 单独编译 DTB

进入内核源码目录后：

```bash
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    100ask_imx6ull-14x14.dtb
```

如果实际使用 mini：

```bash
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    100ask_imx6ull_mini.dtb
```

### 10.3 编译模块

```bash
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" modules
```

也可以使用 Buildroot 原有的 Linux 重编译流程，避免环境和安装目录不一致。

### 10.4 部署时必须确认三件事

1. 新 DTB 覆盖的是 U-Boot 实际加载的那个文件；
2. 新模块被放进目标板对应的 `/lib/modules/4.9.88/`；
3. 模块依赖更新完成。

如果目标板带 `depmod`：

```bash
depmod -a
```

---

## 11. 第八步：分层验证

### 11.1 确认新设备树已经生效

检查 CSI 节点状态：

```bash
find /proc/device-tree -iname '*csi*'
```

也可以检查启动日志中是否有 pinctrl 冲突：

```bash
dmesg | grep -i -E "pinctrl|uart6|ecspi1|csi"
```

如果出现类似：

```text
pin ... already requested
```

说明 UART6、ECSPI1 或其他外设仍占用了 CSI 管脚。

### 11.2 加载正确模块

```bash
modprobe ov5640_camera_v2
modprobe mx6s_capture
```

如果 Buildroot 没有 `modprobe`，找到模块路径后使用 `insmod`。

不要加载：

```text
ov5640_camera_int.ko
ov5640_camera_mipi_int.ko
ov5640_camera_mipi_v2.ko
```

### 11.3 查看 Sensor 是否识别

```bash
dmesg | grep -i ov5640
```

驱动会读取：

```text
0x300A → 0x56
0x300B → 0x40
```

成功日志通常包含：

```text
camera ov5640, is found
```

失败日志可能是：

```text
camera ov5640 is not found
```

如果失败，优先检查：

```text
3.3V → MCLK → PWDN → RESET → I²C1 → 地址0x3c
```

### 11.4 检查 I²C

先列出总线：

```bash
i2cdetect -l
```

原理图上的 `I2C1` 在 Linux 中不一定显示为 `/dev/i2c-1`，必须以 `i2cdetect -l` 为准。

扫描对应总线：

```bash
i2cdetect -y <总线编号>
```

可能看到：

```text
3c
```

驱动已绑定时可能显示：

```text
UU
```

`UU` 不是错误，它表示该地址已经被内核驱动占用。

### 11.5 检查视频设备

```bash
ls -l /dev/video*
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
```

不要默认一定是 `/dev/video0`，以 `v4l2-ctl --list-devices` 的输出为准。

### 11.6 第一次抓图先使用小分辨率

先查看驱动支持的格式，再选择实际存在的格式。假设支持 `YUYV 640×480`：

```bash
v4l2-ctl -d /dev/video0 \
    --set-fmt-video=width=640,height=480,pixelformat=YUYV \
    --stream-mmap=3 \
    --stream-count=10 \
    --stream-to=/tmp/ov5640-640x480.yuv
```

把文件复制到电脑后可以使用：

```bash
ffplay -f rawvideo \
    -pixel_format yuyv422 \
    -video_size 640x480 \
    ov5640-640x480.yuv
```

只有在 VGA 稳定以后，再尝试 720P 或其他分辨率。

---

## 12. 故障排查表

| 现象 | 说明 | 优先检查 |
|---|---|---|
| DTS 编译报重复 label | 同一标签定义了两次 | `ov5640_ep`、`csi1_ep`、`pinctrl_csi1` |
| pinctrl 报管脚已占用 | 外设复用冲突 | UART6、ECSPI1 是否 disabled |
| I²C 扫不到 `0x3c` | Sensor 未正常工作 | 供电、MCLK、PWDN、RESET、SCL/SDA |
| 显示 `UU` | 驱动已经绑定 | 属于正常现象，继续看 dmesg |
| `camera ov5640 is not found` | 芯片 ID 读取失败 | 时钟、电源控制、I²C地址 |
| OV5640 found，但没有 `/dev/videoX` | CSI 主机或 endpoint 未连接 | `&csi`、endpoint、mx6s_capture |
| 有 `/dev/videoX`，抓帧超时 | 没收到正确同步或 PCLK | PCLK、VSYNC、HREF、pinmux |
| 能抓帧但图像撕裂/滚动 | 同步极性或时序不匹配 | VSYNC、HREF、PCLK采样沿 |
| 图像颜色明显错误 | 像素格式或字节顺序不匹配 | YUYV/UYVY、应用参数 |
| 图像有规律条纹 | 数据线错位或丢失 | D0～D7顺序、接触不良 |
| 模块加载冲突 | 同时启用了多个 OV5640 驱动 | 只保留 `OV5640_V2` |

### 已确认：`0x4740` 同步极性导致花屏（2026-08-10）

OV5640 V3 的 DVP 配置由 `{0x4740, 0x23, 0, 0}` 改为 `{0x4740, 0x21, 0, 0}` 后，花屏恢复正常。

这个实验证明：问题位于 OV5640 DVP 输出同步极性与 i.MX6ULL CSI 采样条件不匹配，不是 YUYV 软件转 RGB，也不是 CSI pinctrl 的 `0x1b088`。

---

## 13. 如何判断问题属于哪一层

### 第一层：设备树是否真的生效

问题：

```text
我改的是不是实际启动的 DTS/DTB？
```

证据：

- U-Boot 的 DTB 文件名；
- 新 DTB 时间和哈希；
- 启动后 `/proc/device-tree` 内容；
- pinctrl 和设备节点日志。

### 第二层：Sensor 是否具备工作条件

问题：

```text
电源、参考时钟、PWDN、RESET 是否正确？
```

证据：

- 万用表测3.3V；
- 示波器测约24MHz MCLK；
- PWDN、RESET 上电时序；
- 74HC595 输出电平。

### 第三层：控制通道是否打通

问题：

```text
i.MX6ULL 能否通过 I²C 读到 OV5640？
```

证据：

- `i2cdetect`；
- `dmesg`；
- 芯片 ID 为 `0x5640`。

### 第四层：数据通道是否打通

问题：

```text
CSI 是否收到了 D0～D7、PCLK、VSYNC、HREF？
```

证据：

- CSI 驱动注册；
- `/dev/videoX`；
- 抓帧不超时；
- 示波器或逻辑分析仪观察 PCLK/同步信号。

### 第五层：图像解释是否正确

问题：

```text
分辨率、像素格式、字节顺序是否一致？
```

证据：

- `v4l2-ctl --list-formats-ext`；
- 实际每帧大小；
- YUYV/UYVY 参数；
- 图像是否偏色、错行或撕裂。

---

## 14. 本次适配的最小成功标准

不要把“摄像头能显示到 LCD”作为第一次适配的最小目标。建议分成三个里程碑。

### 里程碑一：Sensor 被识别

```text
dmesg 出现 camera ov5640, is found
```

### 里程碑二：V4L2 设备创建

```text
/dev/videoX 存在
v4l2-ctl 能列出格式
```

### 里程碑三：稳定抓取原始图像

```text
连续抓取10帧不超时
PC上用ffplay查看内容正确
```

达到第三个里程碑后，再做：

- LCD 显示；
- 格式转换；
- OpenCV；
- 自动曝光等控制；
- 更高分辨率；
- 性能优化。

---

## 15. 每次练习后的总结模板

以后适配其他摄像头，也可以重复填写这份模板。

```markdown
## 摄像头基本信息

- Sensor 型号：
- 感光类型：
- 数据接口：
- 控制接口：
- I²C 地址：
- 输出格式：
- 参考时钟：
- 供电电压：

## SoC 接收端

- SoC 型号：
- 接收控制器：
- 数据位宽：
- 使用的 pinctrl：
- PWDN GPIO：
- RESET GPIO：

## Linux 软件链路

- Sensor 驱动：
- 控制器驱动：
- 设备树文件：
- 内核配置：
- 模块名称：
- 视频节点：

## 验证结果

- 实际加载的 DTB：
- MCLK 测量结果：
- I²C 扫描结果：
- 芯片 ID：
- dmesg 关键日志：
- 支持的格式：
- 抓帧结果：

## 遇到的问题

- 表面现象：
- 问题属于哪一层：
- 收集到的证据：
- 根本原因：
- 修改内容：
- 修改后的验证结果：

## 本次最重要的认识

- 
- 
- 
```

---

## 16. 最终检查清单

### 修改前

- [ ] 确认实际启动的 100ASK DTB 文件名；
- [ ] 确认摄像头插接方向；
- [ ] 确认模块使用3.3V；
- [ ] 确认 MCLK 来源；
- [ ] 保存原始 DTS 修改记录。

### 设备树

- [ ] UART6 已禁用；
- [ ] ECSPI1 已禁用；
- [ ] 已添加 `pinctrl_csi1`；
- [ ] OV5640 位于 `&i2c1`；
- [ ] I²C 地址为 `0x3c`；
- [ ] PWDN 为 74HC595 bit5，RESET 为 bit4；
- [ ] 已确认 Sensor 使用模块板载24MHz振荡器；
- [ ] 为兼容旧驱动保留 csi_mclk 和 mclk = <24000000>；
- [ ] 未添加 mipi_csi 和 fsl,mipi-mode；
- [ ] `ov5640_ep` 和 `csi1_ep` 双向连接；
- [ ] `&csi` 状态为 `okay`。

### 内核

- [ ] `CONFIG_VIDEO_MXC_CSI_CAMERA` 已启用；
- [ ] `CONFIG_MXC_CAMERA_OV5640_V2` 已启用；
- [ ] 普通 OV5640 和 MIPI OV5640 驱动已关闭；
- [ ] DTB 和模块均重新编译。

### 部署与验证

- [ ] 新 DTB 已部署到 U-Boot 实际加载的位置；
- [ ] 新模块已部署；
- [ ] dmesg 无 pinctrl 冲突；
- [ ] OV5640 芯片 ID 识别成功；
- [ ] `/dev/videoX` 创建成功；
- [ ] VGA 抓帧成功；
- [ ] 图像颜色和同步正常。

---

## 17. 一句话复盘

这次适配的本质是：

> 使用 I²C1 配置 OV5640，通过 MCLK、PWDN 和 RESET 让 Sensor 正常启动，再把 OV5640 输出的 8 位 DVP 图像送入 i.MX6ULL 的并行 CSI 控制器，最终由 `ov5640_v2`、`mx6s_capture` 和 V4L2 向应用提供 `/dev/videoX`。

