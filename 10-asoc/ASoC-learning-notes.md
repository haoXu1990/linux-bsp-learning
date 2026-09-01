# ASoC 架构学习总结

> 本文结合韦东山音频专题中的旧内核代码，解释 Machine、Platform、Codec 的分工，并映射到设备树时代的 i.MX6ULL + WM8960。
>
> 正确名称是 **ASoC**（ALSA System on Chip），不是 AOSC。课程 API 虽然较老，但分层思想、组件绑定方法和 PCM 运行流程仍值得学习。

## 1. 一张 SoC 声卡由什么组成

以 i.MX6ULL + WM8960 为例：

```text
aplay
  ↓
ALSA PCM 设备
  ↓
ASoC 组装的声卡与 PCM runtime
  ├─ CPU DAI：i.MX6ULL SAI，收发 I2S 数据和时钟
  ├─ PCM Platform：DMA、ring buffer、数据搬运
  ├─ Codec DAI：WM8960 的数字音频接口
  └─ Codec 模拟通路：DAC、Mixer、PGA、耳机/扬声器输出
```

录音方向相反：

```text
麦克风 → WM8960 PGA/ADC → I2S → SAI → DMA → 内存 → arecord
```

要区分两条物理通路：

- **控制通路**：SoC 通过 I2C 配置 WM8960 的音量、时钟、电源和路由；
- **数据通路**：SoC 通过 I2S/SAI 连续传输 PCM 采样。

所以，WM8960 注册成 I2C 设备，不代表音频数据也通过 I2C 传输。

## 2. ALSA 与 ASoC

ALSA 是 Linux 通用声音框架，向用户空间提供 PCM、Control 以及 `/dev/snd/` 设备节点。`aplay`、`arecord`、`amixer` 都使用 ALSA 接口。

ASoC 是 ALSA 针对 SoC 声卡的分层框架。它把容易复用的硬件能力与板级连接分开：

| 对象 | 职责 | i.MX6ULL 示例 |
| --- | --- | --- |
| Machine/Card | 描述板上各器件如何连接，组装声卡 | i.MX6ULL + WM8960 machine driver |
| CPU DAI | SoC 数字音频口、格式与时钟 | SAI |
| PCM Platform | PCM buffer 与 DMA 搬运 | DMAengine PCM / SDMA |
| Codec Component | Codec 寄存器、模拟通路、控件与电源 | WM8960 |
| Codec DAI | Codec 的数字音频接口能力 | WM8960 HiFi DAI |

旧教程常概括成 Machine、Platform、Codec 三部分。现代内核更多使用 Component 模型，但职责没有消失。

## 3. Machine：描述“这块板怎么接”

Machine 不负责搬运 PCM，也不实现 DAC。它描述板级关系：

- 用哪个 CPU DAI；
- 用哪个 Codec 和 Codec DAI；
- 使用 I2S、Left Justified 还是 DSP_A/B；
- 谁提供 BCLK/LRCLK；
- MCLK 来源和频率；
- 耳机、麦克风、扬声器接到 Codec 哪些引脚；
- 播放/录音时需要哪些板级初始化。

两个关键数据结构：

```c
struct snd_soc_card;      /* 整张声卡 */
struct snd_soc_dai_link;  /* 一条数字音频链路 */
```

一条 DAI link 将以下对象关联起来：

```text
CPU DAI ←──── I2S/SAI ────→ Codec DAI
   │                           │
PCM/DMA Platform          Codec Component
```

旧内核常用字符串名称查找组件，现代内核更多使用设备树 phandle、firmware node 和 component match。

## 4. Platform：两个含义不要混在一起

### 4.1 Linux 设备模型中的 platform bus

```text
platform_device ← name/compatible 匹配 → platform_driver
```

片上 SAI、DMA 控制器以及设备树中的 sound 节点，通常会成为 platform device，再匹配各自的 platform driver。

### 4.2 旧 ASoC 中的 PCM Platform

它是音频框架里的功能角色，主要负责：

- 分配和管理 DMA buffer；
- 实现或协助实现 `snd_pcm_ops`；
- 申请、配置、启动、停止 DMA；
- 返回 DMA 当前指针；
- 每完成一个 period 调用 `snd_pcm_period_elapsed()`。

> `platform_device/platform_driver` 是 Linux 的设备匹配机制；ASoC Platform 是 PCM/DMA 功能角色。两者相关，但不是同一个概念。

现代内核把旧 `snd_soc_platform_driver` 的职责逐渐并入 component 和 DMAengine PCM，所以源码中不一定还能找到一个独立、明显叫 Platform 的 ASoC 驱动。

## 5. CPU DAI：SoC 一侧数字音频接口

DAI 是 Digital Audio Interface。CPU DAI 驱动描述 SAI/I2S 控制器：

- 支持的采样率、位宽、声道数；
- 总线格式与时钟方向；
- sysclk、分频和 TDM slot；
- 如何启动和停止收发器。

常见结构和回调：

```c
struct snd_soc_dai_driver;
struct snd_soc_dai_ops;

.startup
.shutdown
.hw_params
.set_fmt
.set_sysclk
.set_clkdiv
.trigger
```

CPU DAI 负责 I2S/SAI 和数字时钟，不负责扬声器模拟放大。

## 6. Codec：不只是数模转换

WM8960 一般包含：

- DAC：PCM 数字采样转模拟信号；
- ADC：模拟麦克风信号转 PCM；
- PGA：模拟增益；
- 模拟 Mixer：选择、叠加和路由信号；
- 耳机/扬声器输出级；
- I2S 数字接口；
- PLL、分频与音频时钟；
- 偏置、电源和防爆音控制。

所以“Codec 就是数模转换”只说对了核心的一部分。更准确地说：

> Codec 是数字 PCM 世界和麦克风、耳机、扬声器等模拟世界之间的接口，并通常集成增益、混音、路由和电源管理。

WM8960 的 I2C 驱动通常做这些事：

1. 匹配设备树中的 I2C 子节点；
2. 创建 regmap，访问 Codec 寄存器；
3. 注册 ASoC component；
4. 注册 Codec DAI；
5. 提供 controls、DAPM widgets/routes 和电源管理。

它本来就是物理 I2C 外设，因此不需要自己再创建一个 platform device。

## 7. DSP 与 Codec，以及两种“混音”

| 对比 | Codec（WM8960） | 音频 DSP/处理器 |
| --- | --- | --- |
| 核心职责 | ADC/DAC、模拟接口、基础路由 | 大规模数字信号处理 |
| 工作域 | 数字 + 模拟 | 主要是数字域 |
| 常见能力 | PGA、静音、模拟 Mixer、简单数字处理 | 矩阵混音、EQ、FIR/IIR、延时、分频、AEC、ANC |
| 灵活度 | 固定功能为主 | 可编程或高度参数化 |
| 是否一定有 ADC/DAC | 通常有 | 不一定；有些复合芯片会集成 |

两者都可能“混音”，但层次不同：

- Codec Mixer 多用于模拟或简单数字通路选择；
- DSP Mixer 通常是多输入、多输出的数字矩阵，可给每条路径独立设置增益，并叠加 EQ、延时等算法。

车载方案中看不到独立 Codec，常见原因是：带 DSP 的芯片已经集成 ADC/DAC 和模拟前端，或系统全程使用数字链路直到数字功放。此时不是没有 Codec 能力，而是它被集成到了复合芯片内部。

## 8. 旧教程中的 `soc-audio` 方式

课程中的 Machine 驱动会显式创建：

```c
platform_device_alloc("soc-audio", ...);
platform_set_drvdata(pdev, &card);
platform_device_add(pdev);
```

ASoC core 中存在名为 `soc-audio` 的 platform driver：

```text
Machine 创建 platform_device("soc-audio")
                  │ 名称匹配
                  ▼
ASoC core 的 platform_driver("soc-audio")
                  │ probe
                  ▼
读取 snd_soc_card，绑定 CPU DAI、Platform、Codec
```

因此，你从 `sound/soc/soc-core.c` 找到 `driver.name = "soc-audio"`，再反向搜索谁创建同名 platform device，是完全正确的源码追踪方式。

视频中“先找 platform device，再根据名字找 platform driver”只是阅读入口，不是强制顺序。源码可能把 device 放在板文件/Machine 驱动，把 driver 放在框架核心；从任何一端找到 name，都可以反向追踪。

旧版组卡过程可以概括为：

```text
注册 CPU DAI
注册 PCM Platform
注册 Codec + Codec DAI
注册 Machine 的 snd_soc_card/dai_link
              ↓
ASoC core 按 dai_link 中的名称寻找组件
              ↓
创建 snd_soc_pcm_runtime
              ↓
创建 ALSA card、PCM、control 和 DAPM 路由
              ↓
/dev/snd/* 对用户空间可见
```

老代码中的 `snd_soc_instantiate_card()`、`soc_bind_dai_link()` 体现了一个不变思想：各组件可分别注册，必要组件齐全后，ASoC core 再实例化整张声卡。

## 9. i.MX6ULL + WM8960 为什么看不到旧式 `soc-audio`

设备树时代通常不再由 Machine 手工创建 `soc-audio`。DTS 已经描述设备及关系，概念结构如下：

```dts
i2c1 {
    codec: wm8960@1a {
        compatible = "wlf,wm8960";
        reg = <0x1a>;
    };
};

sai2: sai@... {
    compatible = "fsl,imx6ul-sai";
    dmas = <...>;
    status = "okay";
};

sound {
    compatible = "fsl,imx-audio-wm8960"; /* 以实际 BSP 为准 */
    audio-cpu = <&sai2>;
    audio-codec = <&codec>;
};
```

启动后大致发生：

```text
WM8960 节点
  → I2C core 创建 i2c_client
  → wm8960 i2c_driver probe
  → 注册 Codec component + Codec DAI

SAI 节点
  → OF/platform core 创建 platform_device
  → fsl_sai platform_driver probe
  → 注册 CPU DAI，并关联 DMAengine PCM

sound 节点
  → OF/platform core 创建 platform_device
  → Machine platform_driver 按 compatible 匹配
  → 解析 audio-cpu/audio-codec
  → 注册 snd_soc_card
  → ASoC core 绑定组件并创建声卡
```

所以你的观察是正常的：

- WM8960 走 I2C device/driver；
- SAI 和 sound 节点一般走 platform device/driver；
- 最终通过 `snd_soc_card`、DAI link 和设备树引用组装；
- 没看到手工创建 `soc-audio`，是内核演进的结果，不代表没有 Machine 或 PCM Platform 功能。

## 10. 旧 API 与现代 API 对照

| 老教程 | 现代内核常见方式 | 不变的概念 |
| --- | --- | --- |
| `soc-audio` platform device | sound DT 节点匹配 Machine driver | Machine 负责组卡 |
| 字符串绑定 DAI/Codec | OF phandle、fwnode、component match | DAI link 连接两端 |
| `snd_soc_register_codec()` | `devm_snd_soc_register_component()` | Codec 注册能力、DAI、控件和路由 |
| `snd_soc_register_platform()` | component + DMAengine PCM | PCM/DMA 搬运数据 |
| `struct snd_soc_codec` | `struct snd_soc_component` | 可复用硬件组件 |
| 板文件创建 device | 设备树实例化 device | device 与 driver 仍需匹配 |

不要机械复制旧 API，要保留架构理解：

1. 各硬件驱动分别注册能力；
2. Machine 描述板级连接；
3. ASoC core 绑定组件并创建 runtime；
4. ALSA 向用户空间提供 PCM 和 control；
5. 运行时由 PCM、DAI、DMA、Codec 回调共同完成音频流。

## 11. `aplay` 播放调用链

```text
aplay
 → PCM open
 → CPU DAI / Codec DAI / DMA startup
 → snd_pcm_hw_params
 → Machine、CPU DAI、Codec DAI、DMA 配置
 → prepare
 → 用户数据写入 ring buffer
 → trigger(START)
 → 启动 DAI 和 DMA
 → DMA 每完成一个 period
 → snd_pcm_period_elapsed()
 → ALSA 唤醒应用继续填充数据
```

具体回调顺序会随内核版本变化，不要死记绝对先后；重点是每层的职责。

### Buffer 与 Period

```text
DMA ring buffer
┌────────┬────────┬────────┬────────┐
│period 0│period 1│period 2│period 3│
└────────┴────────┴────────┴────────┘
```

- Buffer 是整个环形缓冲区；
- Period 是一次 DMA 周期通知对应的数据块；
- period 小：延迟低，中断与调度压力大；
- period 大：更稳，但延迟更高；
- 播放供数不及时会 underrun；录音取数不及时会 overrun；两者都属于 XRUN。

## 12. Control 与 DAPM

### 12.1 Kcontrol

`amixer`/`alsamixer` 中的音量、开关、枚举选择来自 ALSA control。用户修改控件后，Codec 驱动一般通过 regmap/I2C 修改 WM8960 寄存器。

### 12.2 DAPM

DAPM 是 Dynamic Audio Power Management，用 widget 和 route 描述通路：

```text
Playback → DAC → Output Mixer → Headphone PGA → HP Jack
Mic Jack → Input PGA → ADC → Capture
```

DAPM 根据当前有效路径只开启必要模块，同时承担音频路由、动态电源、功耗和部分防爆音时序管理。

## 13. 课程代码阅读顺序

课程资料目录：`Z:\work\100ask\014_【音频专题】韦东山音频专题(51节，已完结)`。

### 13.1 注册/组卡主线

1. Machine：`17th_myalsa/machine/s3c2440_uda1341.c`
2. CPU DAI/IIS：`17th_myalsa/platform/s3c2440_iis.c`
3. PCM/DMA：`17th_myalsa/platform/s3c2440_dma.c`
4. Codec：`17th_myalsa/codec/wm8976.c`
5. 框架分析：`驱动/doc/asoc分析.txt`

阅读时回答：每个模块注册了什么？DAI link 如何引用其他组件？何时组件齐全？ALSA card、PCM 和 control 在哪里创建？

### 13.2 运行主线

从 `20th_app/capture_playback.c` 开始追：

```text
snd_pcm_open
 → snd_pcm_hw_params
 → snd_pcm_prepare
 → snd_pcm_writei / snd_pcm_readi
 → trigger
 → 驱动 open/hw_params/prepare/trigger/pointer
 → DMA 回调
 → snd_pcm_period_elapsed
```

先画对象关系和调用链，再研究寄存器，不要一开始陷进寄存器位。

## 14. 在 i.MX6ULL BSP 中的固定查法

### Codec

```text
DTS 找 wm8960@1a
 → 看 compatible
 → 找 wm8960 的 of_match_table/i2c_device_id
 → 看 i2c_driver.probe
 → 找 devm_snd_soc_register_component
 → 看 component、DAI、controls、DAPM
```

### CPU DAI

```text
DTS 找 SAI 节点
 → 看 compatible
 → 找 fsl_sai platform_driver
 → 看 probe
 → 找 CPU DAI 和 DMAengine PCM 注册
```

### Machine

```text
DTS 找 sound 节点
 → 看 compatible
 → 找 Machine platform_driver
 → 看 probe 如何解析 CPU/Codec phandle
 → 找 devm_snd_soc_register_card
 → 看 dai_link、routing、hw_params
```

运行系统后可检查：

```sh
cat /proc/asound/cards
cat /proc/asound/pcm
aplay -l
arecord -l
amixer -c 0 controls
amixer -c 0 contents
```

## 15. 常见问题的分层定位

### 没有声卡

- sound、SAI、Codec 节点是否启用；
- compatible 是否匹配；
- I2C 地址、供电、复位是否正确；
- Machine probe 是否因组件未就绪而 defer；
- DAI link 引用的 CPU/Codec DAI 是否注册。

### 有声卡但不能播放

- 采样率、位宽、声道数是否都支持；
- I2S 格式和主从模式是否一致；
- MCLK、BCLK、LRCLK 是否存在且频率正确；
- DMA channel 是否申请成功；
- `hw_params`、`set_fmt`、`set_sysclk` 是否报错。

### 播放成功但无声

- DAC、Mixer、输出 PGA 是否上电；
- DAPM route 是否连通；
- mixer 是否静音；
- Machine routing 中的引脚名是否正确；
- 外部功放 enable/mute GPIO 和模拟供电是否正常。

### 爆音、杂音或速度不对

- 时钟树和采样率计算；
- 44.1 kHz/48 kHz 时钟族；
- 数据位宽、帧宽、极性、左右声道；
- DMA period/buffer 与 XRUN；
- 模拟增益以及 DAPM 上下电时序。

## 16. 最终要记住的两条主线

### 静态注册线

```text
总线发现设备
 → 各驱动 probe
 → 注册 CPU DAI / Codec Component / PCM-DMA
 → Machine 注册 snd_soc_card 和 dai_link
 → ASoC core 绑定组件
 → 创建 ALSA card、PCM、controls
```

### 动态播放线

```text
aplay
 → open
 → hw_params
 → Machine/CPU DAI/Codec DAI/DMA 配置
 → prepare
 → trigger START
 → DMA 周期搬运
 → period_elapsed
 → 应用持续供数
```

如果能把源码函数放回这两条线上，就不会再被 `platform_device`、I2C Codec、Machine 和 DMA 的注册顺序绕晕。

## 17. 一句话结论

> ASoC 的本质，是让 CPU 数字音频接口、PCM/DMA、Codec 分别注册自己的能力，再由 Machine 描述它们如何组成一张声卡；旧版使用 `soc-audio` 和字符串名称匹配，现代 i.MX6ULL 多用设备树、Component 与 phandle，但核心分工没有改变。
