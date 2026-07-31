# i.MX6ULL OV5640：内核配置与编译部署补充

## 1. `.config` 是否需要部署

`.config` 是内核编译的输入，不是通常需要复制到开发板运行的固件。

配置值决定代码进入哪个编译产物：

```text
CONFIG_xxx=y
    → 驱动编进 zImage

CONFIG_xxx=m
    → 驱动编译成 .ko 模块

# CONFIG_xxx is not set
    → 不编译该功能
```

本次配置：

```text
CONFIG_VIDEO_MXC_CAPTURE=m
CONFIG_VIDEO_MXC_CSI_CAMERA=m
CONFIG_MXC_CAMERA_OV5640_V2=m
```

都是 `=m`，因此主要生成 `.ko` 模块。

## 2. 为什么仍建议重编 zImage

理论上，仅把选项从 `n` 改为 `m`，且内核导出符号、版本和依赖完全一致时，可以只更新模块。

实际适配时，为避免以下问题：

- 新模块和旧内核配置不一致；
- `vermagic` 不一致；
- `CONFIG_MODVERSIONS` 导致符号 CRC 不一致；
- 依赖功能被编进内核或从内核移除；
- 后续忘记哪些产物属于同一轮编译；

建议修改内核配置后，同一轮重编并部署：

```text
zImage + DTB + modules
```

## 3. 推荐编译命令

先由 defconfig 生成当前 `.config`：

```bash
make ARCH=arm 100ask_imx6ull_defconfig
```

确认配置：

```bash
grep -E 'VIDEO_MXC_CAPTURE|VIDEO_MXC_CSI_CAMERA|MXC_CAMERA_OV5640' .config
```

然后编译：

```bash
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    zImage modules 100ask_imx6ull-14x14.dtb
```

也可以使用 100ASK Buildroot 已有的 Linux 重编译流程。

## 4. 三类产物分别对应什么修改

| 修改内容 | 主要产物 | 是否需要更新 |
|---|---|---|
| 修改 DTS | DTB | 必须 |
| `CONFIG_xxx=m` | `.ko` | 必须 |
| `CONFIG_xxx=y` | `zImage` | 必须 |
| 修改内核核心代码 | `zImage` | 必须 |
| 修改模块驱动代码 | `.ko` | 必须 |

本次为了保持一致，部署以下全部产物：

```text
arch/arm/boot/zImage
arch/arm/boot/dts/100ask_imx6ull-14x14.dtb
对应的摄像头和CSI .ko模块
```

## 5. 模块安装

推荐使用：

```bash
make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    modules_install INSTALL_MOD_PATH=<目标根文件系统目录>
```

然后在目标板执行：

```bash
depmod -a
```

如果手动复制 `.ko`，必须同时注意：

- 复制到正确的 `/lib/modules/$(uname -r)/`；
- 保留模块目录结构；
- 更新模块依赖；
- 删除已被关闭但仍残留在根文件系统中的旧模块。

## 6. 部署检查清单

- [ ] `zImage` 来自本轮编译；
- [ ] DTB 来自本轮编译；
- [ ] `.ko` 来自本轮编译；
- [ ] U-Boot 实际加载了新的 `zImage`；
- [ ] U-Boot 实际加载了新的 `100ask_imx6ull-14x14.dtb`；
- [ ] 模块位于 `/lib/modules/$(uname -r)/`；
- [ ] 已执行 `depmod -a`；
- [ ] `modinfo` 的 vermagic 与 `uname -r` 匹配。

