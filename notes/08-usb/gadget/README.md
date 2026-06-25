# USB Gadget 学习笔记

本文总结资料目录：

`Z:\work\100ask\doc_and_source_for_drivers\STM32MP157\doc_pic\12_USB`

主要覆盖 `11`、`12`、`13`、`14`、`15` 开头的 USB Gadget 资料：

- `11_Gadget驱动程序框架.md`
- `12_Gadget应用实例之zero.md`
- `13_Gadget应用实例之serial.md`
- `14_configfs的使用与内部机制.md`
- `15_Gadget应用实例之adb.md`
- 对应的 `.tif` 框架图和调用关系图

## 1. Gadget 是什么

USB 协议是 Host 主动、Device 被动的主从结构。普通 USB 设备连接到 PC 后，PC 作为 Host 会发起枚举、读取描述符、分配地址、设置配置，然后再通过 endpoint 进行数据传输。

Linux USB Gadget 框架的作用是：让一块 Linux 开发板模拟成 USB Device。比如开发板可以模拟成：

- USB 串口
- USB 网卡
- USB 存储设备
- ADB 设备
- 自定义 USB 设备

理解 Gadget 时抓住两个核心问题：

1. 这个 USB 设备如何向 Host 描述自己？
2. 这个 USB 设备如何通过 endpoint 传输数据？

第一个问题对应各种描述符：

- Device Descriptor：设备描述符
- Configuration Descriptor：配置描述符
- Interface Descriptor：接口描述符
- Endpoint Descriptor：端点描述符
- String Descriptor：字符串描述符

第二个问题对应 endpoint 和 `usb_request`：

- endpoint 是 USB 数据传输的对象。
- `usb_request` 是 Gadget 驱动提交给 UDC 的传输请求。
- Host 发起传输后，UDC 完成收发并触发回调函数。

## 2. Gadget 框架分层

从下到上可以分成几层：

```text
USB Device Controller 硬件
        |
UDC 驱动，如 dwc2、chipidea
        |
usb_gadget / usb_udc
        |
usb_gadget_driver
        |
composite.c / usb_composite_driver
        |
usb_configuration
        |
usb_function，如 f_acm、f_loopback、f_sourcesink、f_fs
```

各层职责如下。

### 2.1 UDC 驱动

UDC 是 USB Device Controller，也就是 USB 设备控制器。

它负责最底层的 endpoint 操作、中断处理、SETUP 包接收、数据收发完成通知等。

资料里提到两个平台：

- IMX6ULL：`drivers/usb/chipidea/`
- STM32MP157：`drivers/usb/dwc2/`

STM32MP157 使用的是 dwc2 控制器，关键入口包括：

```text
drivers/usb/dwc2/platform.c
    dwc2_driver_probe
        dwc2_gadget_init

drivers/usb/dwc2/gadget.c
    dwc2_hsotg_irq
```

设备端发生 USB 事件后，会进入 UDC 中断函数。对于 STM32MP157，核心是 `dwc2_hsotg_irq`。

### 2.2 usb_gadget 和 usb_udc

`usb_gadget` 表示一个具体的 USB Device Controller 能力，比如它有哪些 endpoint、支持哪些速度、有哪些操作函数。

`usb_udc` 则把 UDC 和上层 gadget driver 关联起来。它里面通常包含：

- `struct usb_gadget *gadget`
- `struct usb_gadget_driver *driver`

可以把 `usb_udc` 理解成“底层 UDC 和上层 Gadget 驱动之间的绑定点”。

### 2.3 usb_gadget_driver

`usb_gadget_driver` 是上层 Gadget 驱动注册到 UDC 的接口。

Host 枚举设备时，会发送一些标准 USB 请求，比如：

- `GET_DESCRIPTOR`
- `SET_CONFIGURATION`
- `SET_INTERFACE`
- `GET_CONFIGURATION`

这些请求有很多通用处理逻辑，不应该每个 Gadget 驱动都重复实现，所以 Linux 内核提供了 composite 框架来统一处理。

### 2.4 composite 框架

`drivers/usb/gadget/composite.c` 是理解 Gadget 的重点。

composite 框架负责把一个 USB 设备拆成：

- 一个设备描述符
- 一个或多个配置
- 每个配置下有一个或多个 function
- 每个 function 提供接口和 endpoint

核心结构体是 `usb_composite_dev`。它把描述符、配置、function、当前状态等信息串起来。

legacy gadget 驱动常见入口类似：

```c
usb_composite_probe(&zero_driver);
```

后续典型调用链：

```text
usb_composite_probe
    composite_bind
        xxx_bind
            usb_add_config
                xxx_config_bind
                    usb_add_function
                        function->bind
```

这里的 `bind` 基本可以理解为初始化：创建描述符、申请 endpoint、注册 function。

### 2.5 usb_function

`usb_function` 表示一个具体 USB 功能，也就是一个或多个接口加上对应的数据传输逻辑。

例如：

- `f_loopback.c`：回环测试
- `f_sourcesink.c`：source/sink 测试
- `f_acm.c`：CDC ACM 串口
- `f_fs.c`：functionfs

function driver 通常负责：

- 提供接口描述符
- 提供 endpoint 描述符
- 在 `bind` 时申请 endpoint
- 在 `set_alt` 时 enable endpoint
- 分配并提交 `usb_request`
- 在 complete 回调中处理数据并再次提交请求

## 3. 描述符构造过程

Host 识别 USB 设备，靠的是描述符。

以 `g_zero` 为例，它是一个 legacy composite gadget。入口在 `zero.c`，调用：

```c
usb_composite_probe(&zero_driver);
```

描述符大致来源如下：

- 设备描述符：由 `zero.c` 这类 composite driver 提供
- 配置描述符：由 composite driver 创建配置时提供
- 接口描述符：由 function driver 提供
- endpoint 描述符：由 function driver 提供

因此可以这样记：

```text
设备级信息：应用这个 Gadget 的人决定
功能级信息：具体 function driver 决定
底层 endpoint 能不能满足：UDC 驱动决定
```

构造描述符的关键路径：

```text
usb_composite_probe
    composite_bind
        zero_bind
            usb_add_config
                loopback_bind_config
                    usb_add_function
                        loopback_bind

                sourcesink_bind_config
                    usb_add_function
                        sourcesink_bind
```

最终形成一个 `usb_composite_dev`，里面挂着配置链表和 function 链表。

## 4. Host 获取描述符的过程

Gadget 驱动加载后，只是把描述符准备好了。真正让 Host 看到这些描述符，是在 USB 枚举过程中。

典型枚举流程：

1. Host 读取 8 字节设备描述符，先知道 endpoint 0 的最大包长。
2. Host 给设备分配地址。
3. Host 使用新地址重新读取完整设备描述符，长度通常是 18 字节。
4. Host 读取配置描述符，并连带读取接口描述符、endpoint 描述符。
5. Host 读取字符串描述符。
6. Host 设置配置，设备进入可工作状态。

这些请求都是通过 endpoint 0 的控制传输完成的。

对于 STM32MP157，控制传输的底层入口在 dwc2：

```text
dwc2_hsotg_irq
    dwc2_hsotg_epint
        dwc2_hsotg_enqueue_setup
            dwc2_hsotg_complete_setup
```

当 UDC 收到 SETUP 事务后，会解析 `bRequest`，再决定谁处理：

- UDC 驱动处理底层请求，如 `SET_ADDRESS`
- composite 框架处理通用描述符和配置请求，如 `GET_DESCRIPTOR`
- function 或 configuration 处理非标准或功能相关请求

可以按下面三层理解控制请求：

```text
UDC 驱动
    处理硬件相关、地址设置等底层请求

composite.c
    处理描述符、配置、接口等通用 USB 请求

usb_function
    处理具体功能自己的 class/vendor 请求
```

## 5. 数据传输过程

USB 数据传输永远是 Host 主动发起。Gadget 不能主动把数据“推”给 Host，它只能提前准备好 IN endpoint 的数据，等待 Host 来读。

### 5.1 Gadget 接收 Host 数据

Gadget 想接收 Host 写来的数据时：

```text
分配 buffer
构造 usb_request
设置 complete 回调
提交到 OUT endpoint 队列
Host 发起 OUT 传输
UDC 收到数据
触发中断
调用 complete 回调
驱动处理数据
必要时再次提交 usb_request
```

### 5.2 Gadget 向 Host 提供数据

Gadget 想让 Host 读到数据时：

```text
分配 buffer
把要发送的数据填入 buffer
构造 usb_request
设置 complete 回调
提交到 IN endpoint 队列
Host 发起 IN 传输
UDC 把数据发给 Host
触发中断
调用 complete 回调
必要时再次提交 usb_request
```

### 5.3 STM32MP157 数据完成回调

STM32MP157 上，普通 endpoint 数据传输完成后，大致路径是：

```text
dwc2_hsotg_irq
    dwc2_hsotg_epint
        dwc2_hsotg_handle_outdone / dwc2_hsotg_complete_in
            dwc2_hsotg_complete_request
                usb_gadget_giveback_request
                    req->complete(ep, req)
```

也就是说，function driver 设置的 `usb_request.complete` 最终是从 UDC 中断路径回调上来的。

## 6. g_zero 示例

`g_zero` 是学习 Gadget 数据传输最合适的例子，因为它不牵涉复杂协议，只验证 endpoint 读写。

加载方式：

```sh
modprobe g_zero
```

Host 上可以用：

```sh
lsusb -v -d 0525:a4a0
```

查看描述符。

`g_zero` 有两个配置：

### 6.1 loopback

配置值：

```text
bConfigurationValue = 2
```

功能：

```text
Host 写给 Gadget 什么数据，之后再读就能读回同样的数据。
```

核心路径在 `f_loopback.c`：

```text
loopback_set_alt
    enable_loopback
        enable_endpoint(in_ep)
        enable_endpoint(out_ep)
        alloc_requests
            usb_ep_queue(out_ep, out_req)
```

当 OUT 请求收到 Host 数据后，`loopback_complete` 会把这个 request 转到 IN 方向，等待 Host 读回去。

### 6.2 sourcesink

配置值：

```text
bConfigurationValue = 3
```

功能分两部分：

- source：Gadget 构造数据给 Host 读
- sink：Gadget 接收 Host 写入的数据并校验

它和 loopback 的区别是：

- loopback 的读依赖之前的写
- sourcesink 的读写相对独立

资料里的 libusb 程序思路是：

```text
找到设备
选择配置
找到 interface
找到 IN/OUT endpoint
通过 endpoint 读写数据
```

## 7. g_serial 示例

`g_serial` 把开发板模拟成 USB 串口。

板端加载：

```sh
modprobe g_serial
```

板端会出现：

```text
/dev/ttyGS0
```

PC Linux 端通常会出现：

```text
/dev/ttyACM0
```

Windows 端会在设备管理器里看到 USB 串口。

### 7.1 软件框架

`g_serial` 的核心是把 USB endpoint 和 Linux TTY 框架连接起来。

大致关系：

```text
板端 APP
    open/read/write /dev/ttyGS0
        |
u_serial.c
        |
f_acm.c
        |
composite.c
        |
UDC 驱动
        |
USB
        |
PC /dev/ttyACM0
```

`u_serial.c` 提供两种能力：

- 注册 `tty_driver`，让应用访问 `/dev/ttyGS0`
- 注册 console，让 `console=ttyGS0` 时内核 printk 通过 USB 串口输出

### 7.2 APP 访问路径

板端 APP 打开 `/dev/ttyGS0` 时：

```text
gs_open
    gs_start_io
        给 OUT endpoint 分配 read request
        给 IN endpoint 分配 write request
        gs_start_rx
            usb_ep_queue(out, req)
```

APP 写 `/dev/ttyGS0` 时：

```text
gs_write
    gs_start_tx
        usb_ep_queue(in, req)
```

注意：

- 对 Host 来说 OUT 是 Host 写出。
- 对 Gadget 板子来说 OUT endpoint 是输入方向。
- 对 Host 来说 IN 是 Host 读入。
- 对 Gadget 板子来说 IN endpoint 是输出方向。

这个方向很容易绕，建议永远从 Host 视角记 IN/OUT。

## 8. configfs

legacy gadget 如 `g_zero`、`g_serial` 的特点是：设备描述符、配置、function 组合基本写死在驱动里。

configfs 提供了一种更灵活的方式：用户通过文件系统动态创建 Gadget。

### 8.1 基本使用流程

所有命令在开发板上执行。

挂载 configfs：

```sh
modprobe libcomposite
mount -t configfs none /sys/kernel/config
```

创建 gadget：

```sh
cd /sys/kernel/config/usb_gadget
mkdir test_serial
cd test_serial
```

设置设备描述符：

```sh
echo "0x1234" > idVendor
echo "0x5678" > idProduct
```

创建配置：

```sh
mkdir configs/c.1
```

创建 function：

```sh
mkdir functions/acm.test1
```

把 function 加入配置：

```sh
ln -s functions/acm.test1 configs/c.1/
```

绑定 UDC，使能 Gadget：

```sh
echo 49000000.usb-otg > UDC
```

STM32MP157 上资料里看到的 UDC 名称是：

```text
49000000.usb-otg
```

实际开发时可以查看：

```sh
ls /sys/class/udc/
```

### 8.2 清理流程

清理时顺序很重要，要先解绑 UDC：

```sh
echo "" > UDC
rm configs/c.1/acm.test1
rmdir configs/c.1
rmdir functions/acm.test1
cd ..
rmdir test_serial
```

如果 STM32MP157 出厂系统已经启用了 ADB，需要先清理已有 gadget，再创建新的串口 gadget。

### 8.3 configfs 和 sysfs 的区别

两者都是内存中的虚拟文件系统，但方向不同。

sysfs：

```text
内核创建对象
    用户通过 sysfs 查看或修改属性
```

configfs：

```text
用户 mkdir/write/ln/rmdir
    内核据此创建、配置、关联、销毁对象
```

所以 configfs 更像“用户态驱动内核创建配置对象”的接口。

### 8.4 configfs 内部对象

configfs 中几个重要概念：

- `configfs_attribute`：对应普通属性文件，读写会调用 `show/store`
- `configfs_bin_attribute`：对应二进制属性文件，读写会调用 `read/write`
- `config_item`：基本配置对象，通常对应一个目录
- `config_group`：特殊的 `config_item`，下面还能创建子目录
- `subsystem`：configfs 顶层子系统，比如 `/sys/kernel/config/usb_gadget`

USB Gadget configfs 在内核中注册 subsystem 后，用户才能在 `/sys/kernel/config/usb_gadget` 下创建 gadget。

## 9. functionfs 和 ADB

configfs 已经可以灵活选择 function，但 function 的数据传输逻辑仍然在内核 function driver 中。

functionfs 更进一步：它把 Gadget endpoint 暴露给用户态程序，让用户态程序自己提供描述符并读写 endpoint。

ADB 就是典型例子。

### 9.1 ADB 架构

ADB 全称 Android Debug Bridge，由三部分组成：

```text
PC 端 adb client
        |
PC 端 adb server
        |
USB Host
        |
USB Gadget/functionfs
        |
板端 adbd
```

`adb client` 是用户运行的 `adb push`、`adb shell` 等命令。

`adb server` 是 PC 后台进程，负责管理和设备之间的连接。

`adbd` 是开发板上的守护进程，通过 Gadget/functionfs 和 PC 通信。

### 9.2 functionfs 使用流程

先通过 configfs 创建 ffs function：

```sh
modprobe libcomposite
mount -t configfs none /sys/kernel/config

mkdir -p /sys/kernel/config/usb_gadget/g1
mkdir -p /sys/kernel/config/usb_gadget/g1/functions/ffs.adb
```

再挂载 functionfs：

```sh
mkdir -p /dev/usb-ffs/adb
mount -t functionfs adb /dev/usb-ffs/adb
```

挂载后可以看到：

```text
/dev/usb-ffs/adb/ep0
```

一开始只有 `ep0`。用户态程序需要通过 `ep0` 写入描述符，内核再根据描述符创建更多 endpoint 文件。

### 9.3 adbd 如何创建 endpoint

资料分析的是：

```text
adbd-master/adb/usb_linux_client.cpp
```

核心流程：

```text
init_functionfs
    打开 /dev/usb-ffs/adb/ep0
    向 ep0 写入接口描述符
    向 ep0 写入字符串描述符
    functionfs 根据描述符申请 endpoint
    创建 ep1、ep2 等 endpoint 文件
```

对应内核侧，`f_fs.c` 中 ep0 的 write 操作会解析用户态写入的描述符，并调用类似 `ffs_epfiles_create` 的逻辑创建 endpoint 文件。

这就是 ADB 比普通内核 function 更灵活的地方：

- 描述符由用户态 `adbd` 提供
- endpoint 文件由 functionfs 创建
- 数据传输由用户态 `adbd` 读写 endpoint 文件完成

### 9.4 STM32MP157 ADB 脚本要点

STM32MP157 参考脚本的关键动作：

```sh
VENDOR_ID="0x1d6b"
PRODUCT_ID="0x0104"
UDC=`ls /sys/class/udc/ | awk '{print $1}'`

mkdir -p /dev/usb-ffs/adb
mkdir -p /sys/kernel/config/usb_gadget/g1

echo ${VENDOR_ID} > /sys/kernel/config/usb_gadget/g1/idVendor
echo ${PRODUCT_ID} > /sys/kernel/config/usb_gadget/g1/idProduct

mkdir -p /sys/kernel/config/usb_gadget/g1/strings/0x409
echo "0123456789ABCDEF" > /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber
echo "STMicroelectronics" > /sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer
echo "STM32MP1" > /sys/kernel/config/usb_gadget/g1/strings/0x409/product

mkdir -p /sys/kernel/config/usb_gadget/g1/functions/ffs.adb
mkdir -p /sys/kernel/config/usb_gadget/g1/configs/b.1
ln -s /sys/kernel/config/usb_gadget/g1/functions/ffs.adb \
      /sys/kernel/config/usb_gadget/g1/configs/b.1

mount -t functionfs adb /dev/usb-ffs/adb
start-stop-daemon --start --oknodo --pidfile /var/run/adbd.pid \
    --startas /bin/adbd --background

sleep 1
echo $UDC > /sys/kernel/config/usb_gadget/g1/UDC
```

注意顺序：

1. 先创建 configfs gadget 和 ffs function。
2. 再挂载 functionfs。
3. 再启动 `adbd`，让它写 ep0 描述符。
4. 最后绑定 UDC。

如果顺序错了，Host 可能枚举不到完整设备，或者 endpoint 文件还没创建出来。

## 10. legacy、configfs、functionfs 的关系

可以把三者理解成逐步放开灵活度。

### 10.1 legacy gadget

例子：

- `g_zero`
- `g_serial`
- `g_ether`

特点：

```text
设备描述符、配置、function 组合、数据处理逻辑主要写在内核驱动里。
```

优点是简单，适合学习和固定功能。

缺点是不够灵活，换组合通常要换驱动或改内核代码。

### 10.2 configfs gadget

例子：

```text
/sys/kernel/config/usb_gadget/g1
```

特点：

```text
通过 mkdir、write、ln -s 动态创建设备、配置和 function 组合。
```

优点是不用重写一个 legacy gadget，就能组合已有 function。

缺点是 function 的数据处理仍由内核已有 function driver 决定。

### 10.3 functionfs

例子：

- ADB
- 用户态自定义 USB function

特点：

```text
内核提供 functionfs 框架和 endpoint 文件。
用户态程序提供描述符并读写 endpoint。
```

优点是最灵活，协议逻辑可以放在用户态。

缺点是用户态程序需要正确提供描述符、处理 endpoint 数据和时序。

## 11. 建议学习路线

建议按下面顺序学习：

1. 先理解 USB 描述符和枚举流程。
2. 再读 `11_Gadget驱动程序框架`，建立分层模型。
3. 用 `g_zero` 学 endpoint、`usb_request` 和 complete 回调。
4. 用 `g_serial` 学 Gadget 如何接入 Linux TTY/console。
5. 学 configfs，掌握动态创建 Gadget 的方式。
6. 学 functionfs 和 ADB，理解用户态如何参与 USB function 实现。

最关键的三句话：

```text
Gadget 是 Linux 模拟 USB Device 的框架。
描述符决定 Host 看到什么设备。
usb_request 决定 endpoint 上如何收发数据。
```

## 12. 复习检查点

学习完后，可以用这些问题检查自己是否真正理解：

1. Host 第一次为什么只读取 8 字节设备描述符？
2. `SET_ADDRESS` 通常由哪一层处理？
3. `GET_DESCRIPTOR` 最终为什么会进入 composite 框架？
4. Device Descriptor 和 Interface Descriptor 分别通常由谁提供？
5. 为什么 Gadget 不能主动给 Host 发数据？
6. OUT endpoint 对 Host 和 Gadget 分别意味着什么？
7. `usb_request.complete` 是从哪条调用链回调上来的？
8. `g_zero` 的 loopback 和 sourcesink 有什么区别？
9. `/dev/ttyGS0` 和 `/dev/ttyACM0` 分别在哪一端？
10. configfs 和 sysfs 最大区别是什么？
11. functionfs 为什么一开始只有 ep0？
12. ADB 为什么要先让 `adbd` 写 ep0 描述符，再绑定 UDC？

