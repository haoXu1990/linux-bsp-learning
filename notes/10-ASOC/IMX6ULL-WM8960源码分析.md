# i.MX6ULL + WM8960 ASoC 源码分析

> 文档性质：持续更新的源码学习笔记  
> 分析方法：沿用 V4L2 的分析方式，从设备树出发，跟踪驱动匹配、`probe()`、核心对象注册以及运行时回调。  
> 当前进度：Machine 驱动 `imx-wm8960.c` 的设备匹配与 `probe()` 主线。

## 0. 源码基准与阅读约定

本仓库能够找到 100ASK i.MX6ULL 的 Linux 4.9.8 设备树，但当前没有保存完整的 `sound/soc/fsl/imx-wm8960.c`。本文先依据同代 NXP/Freescale `imx-wm8960.c` 的结构分析；后续拿到实际 BSP 文件后，再逐行校对行号和版本差异。

主要参考位置：

- 板级 DTS：`notes/09-v4l2/IMX_6ULL_PRO_V4L2_OV5640/linux-4.9.8/arch/arm/boot/dts/100ask_imx6ull-14x14.dts`
- Machine 驱动：`sound/soc/fsl/imx-wm8960.c`
- CPU DAI 驱动：`sound/soc/fsl/fsl_sai.c`
- Codec 驱动：`sound/soc/codecs/wm8960.c`
- ASoC core：`sound/soc/soc-core.c`

阅读时始终区分：

```text
静态注册线：启动时发现设备、匹配驱动、注册声卡
动态运行线：aplay/arecord 打开 PCM 后配置并启动硬件
```

## 1. `imx-wm8960.c` 的定位

`imx-wm8960.c` 是 **Machine 驱动**：

- WM8960 I2C/寄存器驱动是 `sound/soc/codecs/wm8960.c`；
- SAI CPU DAI 驱动是 `sound/soc/fsl/fsl_sai.c`。

Machine 驱动的核心职责：

```text
读取 sound 设备树节点
  → 找到 SAI 和 WM8960
  → 描述两端的 DAI 连接关系
  → 添加板级 DAPM 端点和路由
  → 注册 snd_soc_card
```

与 V4L2 作帮助理解的类比：

| V4L2 | ASoC |
| --- | --- |
| Camera bridge 驱动 | Machine 驱动 |
| CSI 控制器 | CPU DAI/SAI |
| OV5640 sensor | WM8960 Codec |
| media link | DAI link |
| video pipeline | audio runtime |

## 2. 声卡涉及的设备与驱动

```text
                         sound
                           │
                    imx-wm8960.c
                    Machine Driver
                           │
              创建 snd_soc_card/dai_link
                           │
              ┌────────────┴─────────────┐
              │                          │
            &sai2                   wm8960@1a
              │                          │
          fsl_sai.c                  wm8960.c
          CPU DAI                 Codec + Codec DAI
              │                          │
              └────── I2S/SAI ──────────┘
```

| 对象 | 设备树节点 | Linux device | 匹配的驱动 |
| --- | --- | --- | --- |
| 声卡连接描述 | `sound` | `platform_device` | `imx-wm8960.c` |
| SAI2 控制器 | `&sai2` | `platform_device` | `fsl_sai.c` |
| WM8960 | `wm8960@1a` | `i2c_client` | `wm8960.c` |
| ASRC | `&asrc` | `platform_device` | ASRC 驱动 |

ASoC 组件不要求属于同一种 Linux 总线。Machine 可以是 platform 驱动，Codec 可以是 I2C 驱动，最终由 ASoC core 组装。

## 3. 从 DTS 的 `sound` 节点开始

```dts
sound {
    compatible = "fsl,imx6ul-evk-wm8960",
                 "fsl,imx-audio-wm8960";
    model = "wm8960-audio";
    cpu-dai = <&sai2>;
    audio-codec = <&codec>;
    asrc-controller = <&asrc>;
    codec-master;
    gpr = <&gpr 4 0x100000 0x100000>;
    hp-det = <3 0>;
    audio-routing = ...;
};
```

关键引用：

```text
cpu-dai         = <&sai2>  → 数字音频控制器
audio-codec     = <&codec> → WM8960
asrc-controller = <&asrc>  → 可选采样率转换器
```

`sound` 没有 `reg`，因为它不是带寄存器的物理硬件，而是逻辑上的板级声卡描述。设备树展开时，OF/platform core 仍会为它创建 `platform_device`。

## 4. `platform_driver` 如何匹配

```c
static const struct of_device_id imx_wm8960_dt_ids[] = {
    { .compatible = "fsl,imx-audio-wm8960", },
    { }
};

static struct platform_driver imx_wm8960_driver = {
    .driver = {
        .name = "imx-wm8960",
        .pm = &snd_soc_pm_ops,
        .of_match_table = imx_wm8960_dt_ids,
    },
    .probe = imx_wm8960_probe,
};

module_platform_driver(imx_wm8960_driver);
```

匹配过程：

```text
sound platform_device
  → 尝试 "fsl,imx6ul-evk-wm8960"
  → 尝试 "fsl,imx-audio-wm8960"，匹配成功
  → 调用 imx_wm8960_probe(pdev)
```

有设备树时，关键是 `.of_match_table` 与 `compatible`。进入 `probe()` 时：

```text
pdev              → sound 对应的 platform_device
pdev->dev.of_node → sound 设备树节点
```

## 5. `probe()` 第一阶段：找到 SAI 和 Codec

### 5.1 找到 `cpu-dai`

```c
cpu_np = of_parse_phandle(pdev->dev.of_node, "cpu-dai", 0);
cpu_pdev = of_find_device_by_node(cpu_np);
```

对应：

```dts
cpu-dai = <&sai2>;
```

两步转换：

```text
sound 的 cpu-dai 属性
  → sai2 的 struct device_node
  → SAI2 的 platform_device
```

### 5.2 找到 `audio-codec`

```c
codec_np = of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
codec_dev = of_find_i2c_device_by_node(codec_np);
```

对应的 Codec 节点：

```dts
codec: wm8960@1a {
    compatible = "wlf,wm8960";
    reg = <0x1a>;
    clocks = <&clks IMX6UL_CLK_SAI2>;
    clock-names = "mclk";
    wlf,shared-lrclk;
};
```

转换关系：

```text
sound 的 audio-codec 属性
  → wm8960 的 struct device_node
  → WM8960 的 struct i2c_client
```

关键对比：

```c
cpu_pdev  = of_find_device_by_node(cpu_np);       /* SAI：platform */
codec_dev = of_find_i2c_device_by_node(codec_np); /* WM8960：I2C */
```

旧版代码还检查 `codec_dev->dev.driver`，即不仅要求 I2C client 已创建，还要求 WM8960 I2C 驱动已经绑定。

## 6. `codec-master` 与 MCLK

### 6.1 读取主从模式

```c
if (of_property_read_bool(pdev->dev.of_node, "codec-master"))
    data->is_codec_master = true;
```

DTS 中存在：

```dts
codec-master;
```

表示 WM8960 是 I2S 的位时钟和帧时钟主设备：

```text
WM8960 提供 BCLK
WM8960 提供 LRCLK/Frame Sync
SAI2 接收 BCLK 和 LRCLK
```

旧版 ASoC 使用 `SND_SOC_DAIFMT_CBM_CFM` 表示：

```text
CBM = Codec Bit-clock Master
CFM = Codec Frame-clock Master
```

### 6.2 获取 WM8960 的 MCLK

```c
data->codec_clk = devm_clk_get(&codec_dev->dev, "mclk");
```

由于传入的是 `codec_dev->dev`，时钟框架会在 WM8960 节点中查找：

```dts
clocks = <&clks IMX6UL_CLK_SAI2>;
clock-names = "mclk";
```

SAI2 节点将该时钟配置为 12.288 MHz：

```dts
assigned-clock-rates = <0>, <12288000>;
```

因此实际时钟关系可以是：

```text
i.MX6ULL SAI2 → 给 WM8960 提供 MCLK
WM8960         → 根据 MCLK 产生 BCLK 和 LRCLK
```

`codec-master` 主要描述 BCLK/LRCLK 的主从关系，不代表 MCLK 也必须由 Codec 晶振产生。

## 7. `probe()` 最核心步骤：构造 DAI link

驱动中预定义：

```c
static struct snd_soc_dai_link imx_wm8960_dai[] = {
    {
        .name = "HiFi",
        .stream_name = "HiFi",
        .codec_dai_name = "wm8960-hifi",
        .ops = &imx_hifi_ops,
    },
    ...
};
```

这个静态结构最初还不知道具体 SAI 和 Codec 实例。`probe()` 动态补充：

```c
data->card.dai_link = imx_wm8960_dai;

imx_wm8960_dai[0].codec_of_node = codec_np;
imx_wm8960_dai[0].cpu_dai_name = dev_name(&cpu_pdev->dev);
imx_wm8960_dai[0].platform_of_node = cpu_np;
```

组装后的第一条链路：

```text
dai_link[0]：HiFi
│
├─ CPU DAI
│    cpu_dai_name = SAI2 设备名
│
├─ PCM Platform
│    platform_of_node = &sai2
│
├─ Codec Component
│    codec_of_node = &codec
│
├─ Codec DAI
│    codec_dai_name = "wm8960-hifi"
│
└─ Machine 运行时操作
     ops = imx_hifi_ops
```

最终连接：

```text
fsl_sai.c 注册的 CPU DAI
           │
           │ I2S
           │
wm8960.c 注册的 "wm8960-hifi" Codec DAI
```

### 为什么 `platform_of_node` 也指向 SAI2

这里的 Platform 指 ASoC 的 PCM/DMA 功能角色，不是 `imx-wm8960.c` 这个 platform driver。

在这套 NXP 驱动中，SAI 设备还关联 DMAengine PCM，因此：

```text
CPU DAI 节点      → sai2
PCM Platform 节点 → sai2
```

它们可以指向同一个设备节点，但在 ASoC 中代表不同职责。

## 8. 构造 `snd_soc_card`

```c
data->card.dev = &pdev->dev;
```

表示这张声卡归属于 `sound` platform device。

解析声卡名：

```c
snd_soc_of_parse_card_name(&data->card, "model");
```

对应：

```dts
model = "wm8960-audio";
```

添加板级 DAPM 端点：

```c
static const struct snd_soc_dapm_widget imx_wm8960_dapm_widgets[] = {
    SND_SOC_DAPM_HP("Headset Jack", NULL),
    SND_SOC_DAPM_SPK("Ext Spk", NULL),
    SND_SOC_DAPM_MIC("Hp MIC", NULL),
    SND_SOC_DAPM_MIC("Main MIC", NULL),
};
```

解析板级音频路由：

```c
snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
```

DTS 中的路由将 Machine 的板级端点与 WM8960 驱动内部的 DAPM 引脚连接。例如：

```text
Playback → WM8960 DAC → 输出 PGA → HP_L/HP_R → Headphone Jack
Mic Jack → LINPUT2/LINPUT3 → 输入 PGA → WM8960 ADC → Capture
```

## 9. 注册声卡

```c
platform_set_drvdata(pdev, &data->card);
snd_soc_card_set_drvdata(&data->card, data);

ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
```

三者含义：

```text
platform_set_drvdata
  → platform_device 可以找到 snd_soc_card

snd_soc_card_set_drvdata
  → snd_soc_card 可以找到 imx_wm8960_data 私有数据

devm_snd_soc_register_card
  → 将声卡交给 ASoC core 绑定和实例化
```

ASoC core 接下来查找：

- `fsl_sai.c` 是否已注册对应 CPU DAI；
- `wm8960.c` 是否已注册对应 Codec component；
- Codec DAI 中是否存在 `wm8960-hifi`；
- PCM/DMA component 是否存在。

组件齐全后，框架创建：

```text
snd_soc_pcm_runtime
  → snd_card
  → snd_pcm
  → ALSA controls
  → DAPM graph
  → /dev/snd/*
```

## 10. ASRC 分支

DTS 包含：

```dts
asrc-controller = <&asrc>;
```

因此驱动除普通 `HiFi` link 外，还会启用：

```text
HiFi-ASRC-FE：Front End，面向用户 PCM
HiFi-ASRC-BE：Back End，连接真实 SAI 与 WM8960
```

```text
应用 PCM
  → ASRC Front End
  → 采样率/位宽转换
  → ASRC Back End
  → SAI2
  → WM8960
```

`be_hw_params_fixup()` 将后端参数固定到 ASRC 节点配置的输出采样率和格式。这属于 DPCM/ASRC 扩展，第一次理解 Machine 驱动时，可以先抓住直接 `HiFi` 链路，再回头分析这两条 link。

## 11. `probe()` 与播放回调的分界

`imx_wm8960_probe()` 只负责组装和注册声卡，不负责真正播放。

DAI link 指向：

```c
static struct snd_soc_ops imx_hifi_ops = {
    .startup   = imx_hifi_startup,
    .hw_params = imx_hifi_hw_params,
    .hw_free   = imx_hifi_hw_free,
    .shutdown  = imx_hifi_shutdown,
};
```

时间关系：

```text
系统启动
  → imx_wm8960_probe()
  → 注册声卡

aplay/arecord 打开 PCM
  → imx_hifi_startup()
  → imx_hifi_hw_params()
  → 配置 CPU DAI、Codec DAI 和时钟
  → SAI/DMA 开始传输

关闭 PCM
  → imx_hifi_hw_free()
  → imx_hifi_shutdown()
```

## 12. 当前静态注册主线

```text
sound DTS 节点
  → 创建 sound platform_device
  → compatible 匹配 imx_wm8960_driver
  → imx_wm8960_probe
  → cpu-dai 找到 SAI2 platform_device
  → audio-codec 找到 WM8960 i2c_client
  → 读取 codec-master、MCLK、GPR、ASRC 和路由
  → 填充 snd_soc_dai_link
  → 填充 snd_soc_card
  → devm_snd_soc_register_card
  → ASoC core 绑定 SAI、PCM/DMA、WM8960
  → 创建 ALSA 声卡与 PCM 设备
```

## 13. 下一步源码分析路线

1. `imx_hifi_startup()`：打开 PCM 时为什么启用 MCLK，以及采样率约束；
2. `imx_hifi_hw_params()`：主从模式、`set_fmt()`、PLL、BCLK 的计算；
3. `fsl_sai.c`：CPU DAI 如何注册，SAI 如何与 DMAengine PCM 关联；
4. `wm8960.c`：I2C probe 如何注册 Codec component 和 `wm8960-hifi`；
5. `soc-core.c`：`devm_snd_soc_register_card()` 后怎样绑定 component；
6. 播放调用链：从 `aplay` 到 DMA period 中断。

## 14. 后续问题记录

后续每个问题按以下格式追加：

```text
问题：
结论：
对应源码：
调用关系：
容易混淆点：
```

### Q1：SAI 是 i.MX 专用的吗？

结论：SAI 是 Synchronous Audio Interface。NXP 将其音频串行控制器命名为 SAI；其他厂商具有相同角色的控制器，但可能叫 I2S/PCM、DAUDIO、McASP 或 SSI。在 ASoC 中它们通常都充当 CPU DAI。

### Q2：为什么 WM8960 不注册为 platform device？

结论：WM8960 是实际的 I2C 外设，因此由 I2C core 创建 `i2c_client` 并与 `i2c_driver` 匹配。ASoC Machine 通过设备树 phandle 找到该 I2C 设备，并在 ASoC 层绑定其 Codec component 和 Codec DAI。

