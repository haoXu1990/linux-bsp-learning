# USB drv 学习总结

本目录学习的是 Linux 内核里的 USB 设备驱动。这里以 USB 鼠标为例，重点理解：

- USB 设备和驱动是怎么匹配的
- `probe` 里应该完成哪些初始化
- USB 数据传输为什么要用 URB
- USB 鼠标数据怎么通过 input 子系统上报
- 用户态 APP 怎么读取 input 事件

## 1. USB 驱动匹配的是 interface

USB 设备插入后，Linux USB core 会枚举设备，读取各种描述符：

```text
device descriptor
configuration descriptor
interface descriptor
endpoint descriptor
```

USB 驱动通常不是直接匹配整个 USB device，而是匹配某个 interface。

一个 USB 设备可能有多个 interface，例如一个无线键鼠接收器可能同时包含：

```text
interface 0: keyboard
interface 1: mouse
interface 2: vendor specific
```

不同 interface 可以绑定不同驱动。

本驱动通过 `id_table` 匹配 HID Boot Mouse interface：

```c
static struct usb_device_id usb_mouse_id_table[] = {
    { USB_INTERFACE_INFO(USB_INTERFACE_CLASS_HID,
                         USB_INTERFACE_SUBCLASS_BOOT,
                         USB_INTERFACE_PROTOCOL_MOUSE) },
    { }
};
```

含义是：

```text
class    = HID
subclass = Boot
protocol = Mouse
```

匹配流程：

```text
USB 设备插入
    -> USB core 枚举设备
    -> USB core 读取描述符
    -> USB core 遍历已注册的 usb_driver
    -> 用 interface 信息和 id_table 匹配
    -> 匹配成功后调用 probe
```

驱动注册使用：

```c
module_usb_driver(usb_mouse_driver);
```

它会自动生成模块入口和出口，内部完成：

```text
usb_register()
usb_deregister()
```

使用 GPL-only 符号时，模块需要声明：

```c
MODULE_LICENSE("GPL");
```

## 2. probe 的作用

`probe` 表示 USB core 已经把匹配到的 interface 交给当前驱动。

在 `probe` 中通常要做这些事情：

```text
1. 通过 interface 获取 usb_device
2. 获取当前 interface setting
3. 查找需要使用的 endpoint
4. 根据 endpoint 创建 pipe
5. 获取 endpoint 的最大包长度
6. 分配驱动私有结构体
7. 分配 USB 数据 buffer
8. 分配并初始化 input_dev
9. 注册 input 设备
10. 保存私有数据到 interface
```

这里几个结构体很重要：

```c
struct usb_interface
```

表示当前匹配到的 USB interface。

```c
struct usb_device
```

表示整个 USB 设备。

```c
struct usb_host_interface
```

表示当前 interface 的描述符信息。

```c
struct usb_endpoint_descriptor
```

表示 endpoint 描述符。

## 3. endpoint 的查找

USB 鼠标的数据是设备主动上报给主机的，因此驱动需要找到：

```text
interrupt IN endpoint
```

判断函数是：

```c
usb_endpoint_is_int_in(endpoint)
```

它同时判断：

```text
endpoint 类型是 interrupt
endpoint 方向是 IN
```

IN 表示：

```text
device -> host
```

例如 endpoint 地址 `0x81`：

```text
0x80: IN 方向
0x01: endpoint 1
```

简单鼠标接口通常只有一个 interrupt IN endpoint，所以内核示例 `usbmouse.c` 里会直接检查 endpoint 数量是否为 1。

如果想写得更通用，可以遍历所有 endpoint，找到 interrupt IN endpoint。

## 4. pipe 和 maxpacket

找到 endpoint 后，需要构造 pipe：

```c
pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
```

`pipe` 是 Linux USB core 用来描述一次 USB 传输目标的编码值。

它包含：

```text
USB 设备
endpoint 编号
方向
传输类型
```

`usb_rcvintpipe` 表示：

```text
创建一个 interrupt IN 接收 pipe
```

然后获取最大包大小：

```c
maxpacket = usb_maxpacket(dev, pipe, usb_pipeout(pipe));
```

`maxpacket` 表示该 endpoint 一次传输的最大数据长度，对应 endpoint 描述符中的 `wMaxPacketSize`。

这个值通常用来决定接收 buffer 的大小。

## 5. USB 数据传输使用 URB

内核 USB 驱动使用 URB 描述一次 USB 传输。

URB 全称：

```text
USB Request Block
```

可以理解为：

```text
一次 USB 传输请求
```

用户态 libusb 和内核态 USB 驱动的对应关系：

```text
libusb_alloc_transfer()          -> usb_alloc_urb()
libusb_fill_interrupt_transfer() -> usb_fill_int_urb()
libusb_submit_transfer()         -> usb_submit_urb()
libusb callback                  -> URB complete callback
```

一次 interrupt IN 接收流程：

```text
分配 URB
    -> 填充 URB
    -> 提交 URB
    -> 鼠标产生数据
    -> USB core 调用 URB 回调函数
    -> 回调中处理数据
    -> 再次提交 URB
```

需要注意：

```text
URB 提交一次，只接收一次传输结果。
```

如果要持续接收鼠标数据，需要在回调函数中再次调用：

```c
usb_submit_urb(urb, GFP_ATOMIC);
```

## 6. usb_alloc_coherent 的作用

USB 控制器通常通过 DMA 访问内存。

`usb_alloc_coherent` 用来分配一块适合 USB DMA 使用的内存：

```c
mouse->data = usb_alloc_coherent(dev, maxpacket, GFP_KERNEL,
                                 &mouse->data_dma);
```

它返回两种地址：

```text
mouse->data
```

CPU 使用的虚拟地址。

```text
mouse->data_dma
```

USB 控制器 DMA 使用的地址。

配合 URB 使用时，需要设置：

```c
urb->transfer_dma = mouse->data_dma;
urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
```

含义是：

```text
这个 buffer 已经有 DMA 地址，USB core 不需要再次映射。
```

释放时必须使用：

```c
usb_free_coherent(...)
```

不能用 `kfree` 释放。

## 7. input 子系统上报

USB 鼠标驱动一般不直接创建字符设备给 APP 读取，而是注册为 input 设备。

驱动通过 input 子系统上报：

```text
按键事件
相对移动事件
滚轮事件
```

input 设备需要先声明自己支持哪些事件。

`evbit` 表示支持哪些事件大类，例如：

```text
EV_KEY: 按键事件
EV_REL: 相对位移事件
```

`keybit` 表示支持哪些具体按键，例如：

```text
BTN_LEFT
BTN_RIGHT
BTN_MIDDLE
```

`relbit` 表示支持哪些相对轴，例如：

```text
REL_X
REL_Y
REL_WHEEL
```

所以一个鼠标 input 设备通常需要设置：

```text
EV_KEY
EV_REL
BTN_LEFT / BTN_RIGHT / BTN_MIDDLE
REL_X / REL_Y / REL_WHEEL
```

收到 USB 数据后，驱动调用：

```c
input_report_key(...)
input_report_rel(...)
input_sync(...)
```

`input_sync` 表示一组 input 事件上报完成。

完整数据路径：

```text
USB 鼠标
    -> interrupt IN endpoint
    -> URB 回调函数
    -> input_report_key / input_report_rel
    -> input_sync
    -> input core
    -> /dev/input/eventX
```

如果系统有图形环境，例如 Xorg、Wayland、Qt、Weston，那么图形系统会读取 `/dev/input/eventX`，并根据 `REL_X`、`REL_Y` 移动光标。

如果系统只有串口或命令行，没有图形环境，那么 input 事件仍然存在，但屏幕上不会自动显示鼠标光标。

## 8. usb_set_intfdata 的作用

`probe` 中会分配驱动私有结构体，例如：

```c
struct usb_mouse
```

里面保存：

```text
usb_device
usb_interface
input_dev
urb
data buffer
DMA 地址
pipe
maxpacket
```

需要把这个私有结构体保存到当前 interface：

```c
usb_set_intfdata(intf, mouse);
```

拔掉设备时，USB core 会调用 `disconnect`，并传入同一个 `intf`。

此时可以取回私有数据：

```c
mouse = usb_get_intfdata(intf);
```

然后释放资源。

这样做的好处是：

```text
一个驱动可以同时支持多个 USB 鼠标。
每个 interface 都有自己的私有数据。
```

不要用一个全局变量保存设备数据，否则多个设备会互相覆盖。

## 9. disconnect 的清理流程

设备拔出后，USB core 调用 `disconnect`。

驱动需要停止传输并释放资源：

```text
1. usb_get_intfdata 取回私有数据
2. usb_set_intfdata(intf, NULL)
3. usb_kill_urb 停止 URB
4. usb_free_urb 释放 URB
5. usb_free_coherent 释放 DMA buffer
6. input_unregister_device 注销 input 设备
7. kfree 释放私有结构体
```

注意：

```text
usb_kill_urb 会等待正在进行的 URB 完成或取消。
```

拔设备时不能让回调继续访问已经释放的内存。

## 10. 用户态 APP 验证

本目录下的测试 APP：

```text
usb_mouse_app.c
```

它读取 input 子系统导出的事件节点：

```text
/dev/input/eventX
```

使用流程：

```sh
insmod usb-mouse-drv.ko
cat /proc/bus/input/devices
./usb_mouse_app /dev/input/eventX
```

如果驱动正常上报事件，APP 可以看到：

```text
EV_KEY
EV_REL
EV_SYN
```

对于鼠标移动，重点看：

```text
EV_REL REL_X
EV_REL REL_Y
```

对于按键，重点看：

```text
EV_KEY BTN_LEFT
EV_KEY BTN_RIGHT
EV_KEY BTN_MIDDLE
```

## 11. 总体流程

USB 鼠标驱动完整流程：

```text
加载模块
    -> usb_register
    -> 插入 USB 鼠标
    -> USB core 枚举设备
    -> id_table 匹配 interface
    -> 调用 probe
    -> 查找 interrupt IN endpoint
    -> 创建 pipe
    -> 分配私有数据、buffer、input_dev
    -> 注册 input 设备
    -> 用户空间打开 /dev/input/eventX
    -> input_dev open
    -> 分配并提交 URB
    -> 鼠标产生数据
    -> URB 回调
    -> input 子系统上报事件
    -> APP 或图形系统读取事件
    -> 回调中再次提交 URB
    -> 持续接收
```

拔出设备：

```text
USB 鼠标拔出
    -> USB core 调用 disconnect
    -> 停止 URB
    -> 注销 input 设备
    -> 释放 DMA buffer
    -> 释放私有数据
```

## 12. 关键理解

USB 驱动这部分可以抓住一条主线：

```text
匹配 interface
    -> probe 初始化
    -> endpoint 决定数据从哪里来
    -> pipe 描述传输目标
    -> URB 描述一次传输
    -> 回调处理数据
    -> input 子系统上报事件
    -> /dev/input/eventX 给用户态读取
```

对于 USB 鼠标来说，最核心的是：

```text
interrupt IN endpoint + URB + input_report_rel/input_report_key
```
