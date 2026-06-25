# CarPlay iAP Gadget 驱动重构与 configfs 配置说明

本文说明当前目录中的 CarPlay iAP 底层驱动代码和 configfs 自动配置脚本。

目标不是把它改成 `g_zero` 那种 legacy gadget，而是保留座舱业务需要的 configfs 组合能力：

- CarPlay iAP
- ADB
- USB 网络，如 ECM/RNDIS
- 未来其他 USB function

同时把原来 `f_iap.c` 一个文件里混在一起的职责拆开，方便理解和维护。

## 1. 当前业务链路

CarPlay iAP 的数据链路可以理解为：

```text
iPhone / CarPlay Host
        |
USB bulk IN/OUT
        |
Linux USB Gadget composite/configfs
        |
iAP usb_function
        |
/dev/zjinnova_iap
        |
座舱 CarPlay/iAP 用户态服务
```

这里有两个方向容易混：

```text
USB OUT endpoint:
    从 Host 角度看是 Host 写出
    从车机 Gadget 角度看是车机接收

USB IN endpoint:
    从 Host 角度看是 Host 读入
    从车机 Gadget 角度看是车机发送
```

所以驱动中的方向关系是：

```text
用户态 read(/dev/zjinnova_iap)
    -> 驱动向 ep_out 提交 usb_request
    -> 等待 iPhone/Host 写 OUT 数据

用户态 write(/dev/zjinnova_iap)
    -> 驱动向 ep_in 提交 usb_request
    -> 等待 iPhone/Host 发起 IN 读
```

## 2. 为什么不改成 legacy g_iap

老师课程里常见的是 `g_zero`、`g_serial` 这种 legacy gadget：

```text
legacy gadget driver
    usb_composite_driver
        configuration
            usb_function
```

这类驱动加载后通常直接创建完整 USB 设备。

但是座舱业务经常需要把多个 function 组合在同一个 USB 设备里：

```text
CarPlay iAP + ADB + USB 网络 + 其他诊断/日志通道
```

如果改成固定的 `g_iap` legacy gadget，短期看起来简单，但后续组合能力会变差。configfs 更适合产品化场景：

```text
configfs
    动态创建设备描述符
    动态创建配置
    动态选择 function
    动态组合 iap / adb / ecm / rndis
```

因此当前方向是：

```text
保留 f_iap 作为 usb_function
保留 configfs 负责组合
补脚本让系统自动配置，不让业务手动敲 configs/functions/ln -s
```

## 3. 原始 f_iap.c 为什么保留

`f_iap.c` 已恢复在目录中，作为原始单文件版本参考。

它不参与当前编译。

当前 Makefile 是：

```make
obj-m += f_iap.o
f_iap-objs := iap_function.o iap_io.o iap_chardev.o
```

这表示最终仍然生成：

```text
f_iap.ko
```

但是 `f_iap.ko` 由下面三个对象链接得到：

```text
iap_function.o
iap_io.o
iap_chardev.o
```

目录中的 `f_iap.c` 只是保留给对照学习，不会被 Kbuild 编译进模块。

## 4. 拆分后的代码结构

当前代码按职责拆成四个文件。

### 4.1 iap_core.h

公共头文件，包含：

- `IAP_BULK_BUFFER_SIZE`
- `TX_REQ_MAX`
- `struct iap_dev`
- `func_to_iap()`
- `iap_lock()` / `iap_unlock()`
- 各模块之间共享的函数声明

核心结构：

```c
struct iap_dev {
    struct usb_function function;
    struct usb_composite_dev *cdev;

    struct usb_ep *ep_in;
    struct usb_ep *ep_out;

    int online;
    int error;

    atomic_t read_excl;
    atomic_t write_excl;
    atomic_t open_excl;

    struct list_head tx_idle;
    wait_queue_head_t read_wq;
    wait_queue_head_t write_wq;
    struct usb_request *rx_req;
    int rx_done;

    struct work_struct work;
    struct miscdevice *misc_device;
    int sw_online;
};
```

这个结构体就是 iAP function 的运行时状态。

注意：CarPlay iAP 通道按业务设计是单实例：

```text
一个车机系统
一个 CarPlay iAP function
一个 /dev/zjinnova_iap
一个 CarPlay/iAP 用户态服务
```

所以代码保留了单实例用户态设备节点。但底层 complete 回调已经不再直接依赖原来的 `_iap_dev` 全局变量。

### 4.2 iap_function.c

负责 USB function 和 configfs 集成。

对应老师模型中的：

```text
usb_function 层
```

主要内容：

- interface 描述符
- endpoint 描述符
- full-speed/high-speed 描述符表
- 字符串描述符
- `iap_alloc_inst()`
- `iap_alloc()`
- `iap_function_bind()`
- `iap_function_set_alt()`
- `iap_function_disable()`
- `iap_function_unbind()`
- `DECLARE_USB_FUNCTION_INIT(iap, ...)`

最关键的是：

```c
DECLARE_USB_FUNCTION_INIT(iap, iap_alloc_inst, iap_alloc);
```

这表示内核注册了一个 configfs function，名字是：

```text
iap
```

因此用户可以通过 configfs 创建：

```sh
mkdir functions/iap.carplay
```

#### bind 阶段

`iap_function_bind()` 发生在 function 被加入 configuration 并被 composite 框架绑定时。

它做的事：

```text
1. 获取 interface id
2. 获取字符串 id
3. 拷贝 full-speed 描述符
4. 自动申请 bulk IN/OUT endpoint
5. 分配 usb_request
6. 如果支持 high-speed，拷贝 high-speed 描述符
```

对应流程：

```text
configfs ln -s functions/iap.carplay configs/c.1/
    |
composite bind
    |
iap_function_bind
    |
usb_interface_id
usb_string_id
usb_copy_descriptors
usb_ep_autoconfig
iap_create_bulk_endpoints
```

#### set_alt 阶段

`iap_function_set_alt()` 通常在 Host 选择配置/接口 altsetting 后发生。

它做的事：

```text
1. 根据当前 USB 速度配置 endpoint
2. enable ep_in
3. enable ep_out
4. online = 1
5. 唤醒等待 online 的 read/write
6. 发送 uevent 通知用户态状态变化
```

对应：

```text
Host SET_CONFIGURATION / SET_INTERFACE
    |
composite
    |
iap_function_set_alt
    |
usb_ep_enable
    |
/dev/zjinnova_iap 可以收发
```

### 4.3 iap_io.c

负责 endpoint 和 `usb_request`。

对应老师模型中的：

```text
endpoint / usb_request 数据传输层
```

主要内容：

- `iap_request_new()`
- `iap_request_free()`
- `iap_req_put()`
- `iap_req_get()`
- `iap_complete_in()`
- `iap_complete_out()`
- `iap_create_bulk_endpoints()`
- `iap_free_bulk_requests()`

#### endpoint 申请

`iap_create_bulk_endpoints()` 调用：

```c
usb_ep_autoconfig(cdev->gadget, in_desc);
usb_ep_autoconfig(cdev->gadget, out_desc);
```

这说明 iAP function 不指定固定 endpoint 编号，而是让 UDC/composite 自动分配满足描述符要求的 endpoint。

#### request 分配

驱动分配：

```text
1 个 OUT request，用于用户态 read() 时接收 Host 数据
4 个 IN request，用于用户态 write() 时向 Host 提供数据
```

IN request 放在 `tx_idle` 链表中：

```text
tx_idle
    空闲 IN usb_request 队列
```

#### complete 回调

重构前 complete 回调使用全局变量：

```c
struct iap_dev *dev = _iap_dev;
```

重构后改成：

```c
req->context = dev;

static void iap_complete_in(struct usb_ep *ep, struct usb_request *req)
{
    struct iap_dev *dev = req->context;
}
```

这样更符合 Gadget 驱动常见写法：

```text
每个 request 自己携带上下文
complete 回调从 req->context 找回所属设备
```

虽然业务上仍然是单实例，但底层 request 回调不再硬依赖全局 `_iap_dev`。

### 4.4 iap_chardev.c

负责 `/dev/zjinnova_iap`。

对应业务用户态接口层。

主要内容：

- `miscdevice`
- `open`
- `read`
- `write`
- `ioctl`
- `release`
- uevent 状态通知

设备节点名仍然是：

```text
/dev/zjinnova_iap
```

#### open

`open()` 使用 `open_excl` 限制单进程打开：

```text
如果已有 CarPlay/iAP 服务打开了 /dev/zjinnova_iap
第二个进程再打开会返回 -EBUSY
```

这是合理的，因为当前座舱系统理论上只有一个 CarPlay APP/iAP 服务实例。

#### read

用户态调用：

```c
read(fd, buf, len);
```

驱动行为：

```text
1. 如果 function 还没 online，阻塞等待
2. 使用 rx_req
3. 把 rx_req 提交到 ep_out
4. 等待 Host 写 OUT 数据
5. iap_complete_out 唤醒 read
6. copy_to_user 返回给用户态
```

流程：

```text
APP read(/dev/zjinnova_iap)
    |
usb_ep_queue(ep_out, rx_req)
    |
iPhone/Host OUT transfer
    |
iap_complete_out
    |
wake_up(read_wq)
    |
copy_to_user
```

#### write

用户态调用：

```c
write(fd, buf, len);
```

驱动行为：

```text
1. 如果 function 还没 online，阻塞等待
2. 从 tx_idle 取一个空闲 IN request
3. copy_from_user 到 request buffer
4. 提交到 ep_in
5. 等待 Host 发起 IN 读
6. iap_complete_in 把 request 放回 tx_idle
```

流程：

```text
APP write(/dev/zjinnova_iap)
    |
iap_req_get(tx_idle)
    |
copy_from_user
    |
usb_ep_queue(ep_in, req)
    |
iPhone/Host IN transfer
    |
iap_complete_in
    |
iap_req_put(tx_idle)
    |
wake_up(write_wq)
```

#### ioctl

当前支持：

```c
#define IAP_GET_DEVICE_STATE _IOW('z', 1, int)
```

返回：

```text
1: online
0: offline
```

用于用户态判断 USB/iAP 通道是否已经被 Host 配置好。

## 5. 当前实现检查点

这次重构保持以下行为不变：

```text
模块输出: f_iap.ko
configfs function 名: iap
用户态设备节点: /dev/zjinnova_iap
interface class: 0xFF
interface subclass: 0xF0
endpoint 数量: 1 IN + 1 OUT
endpoint 类型: bulk
bulk buffer: 4096
tx request 数量: 4
```

这次重构修正/加固了几个点：

```text
1. complete 回调使用 req->context，不再直接使用 _iap_dev。
2. read/write 等待 online 时，只在 wait 返回负数时视为信号中断错误。
3. ioctl 遇到未知命令返回 -EINVAL。
4. 注销 miscdevice 前 cancel_work_sync，避免 work 使用已释放对象。
5. bind 失败路径释放已分配的 request/descriptor。
```

第 2 点尤其需要注意。

原代码中：

```c
ret = wait_event_interruptible(...);
if (ret <= 0)
    return ret;
```

`wait_event_interruptible()` 正常等到条件满足时返回 `0`。因此 `ret <= 0` 会把正常唤醒也直接返回掉。现在改成：

```c
if (ret < 0)
    return ret;
```

这样才表示：

```text
正常等到 online: 继续执行
被信号打断: 返回错误
```

## 6. configfs 脚本

脚本路径：

```text
notes/08-usb/gadget/scripts/iap_configfs.sh
```

默认启动只配置 iAP function：

```sh
sh scripts/iap_configfs.sh start
```

停止：

```sh
sh scripts/iap_configfs.sh stop
```

重启：

```sh
sh scripts/iap_configfs.sh restart
```

查看状态：

```sh
sh scripts/iap_configfs.sh status
```

如果需要可执行权限：

```sh
chmod +x scripts/iap_configfs.sh
```

### 6.1 默认创建的结构

默认会创建：

```text
/sys/kernel/config/usb_gadget/g_carplay
    idVendor
    idProduct
    strings/0x409
        serialnumber
        manufacturer
        product
    configs/c.1
        strings/0x409/configuration
        iap.carplay -> ../../functions/iap.carplay
    functions/iap.carplay
    UDC
```

脚本最后才写入 UDC：

```sh
echo "$udc" > "$GADGET_DIR/UDC"
```

这点很重要。必须先把描述符、配置、function 都建好，再绑定 UDC，否则 Host 可能枚举到半成品设备。

### 6.2 常用变量

可以通过环境变量覆盖：

```sh
GADGET_NAME=g_carplay
CONFIG_NAME=c.1
VID=0x1234
PID=0x5678
SERIAL=0123456789ABCDEF
MANUFACTURER=zjinnova
PRODUCT="CarPlay iAP Gadget"
UDC=49000000.usb-otg
```

示例：

```sh
VID=0x1d6b PID=0x0104 PRODUCT="CarPlay Composite" \
UDC=49000000.usb-otg \
sh scripts/iap_configfs.sh start
```

如果不指定 `UDC`，脚本会自动取：

```sh
ls /sys/class/udc | sed -n '1p'
```

### 6.3 f_iap 模块加载

脚本会尝试：

```sh
modprobe f_iap
```

如果模块没有安装到目标系统模块目录，可以指定 ko 路径：

```sh
IAP_MODULE_PATH=/root/f_iap.ko sh scripts/iap_configfs.sh start
```

脚本会执行：

```sh
insmod /root/f_iap.ko
```

## 7. 组合 ADB

启用 ADB：

```sh
ENABLE_ADB=1 sh scripts/iap_configfs.sh start
```

脚本会创建：

```text
functions/ffs.adb
/dev/usb-ffs/adb
```

并挂载：

```sh
mount -t functionfs adb /dev/usb-ffs/adb
```

如果存在 `/bin/adbd`，脚本会启动：

```sh
start-stop-daemon --start --oknodo \
    --pidfile /var/run/adbd.pid \
    --startas /bin/adbd \
    --background
```

注意 ADB/functionfs 的顺序：

```text
1. 创建 functions/ffs.adb
2. 挂载 /dev/usb-ffs/adb
3. 启动 adbd，让 adbd 写 ep0 描述符
4. 最后绑定 UDC
```

这和资料中 ADB 章节讲的一致。

## 8. 组合 USB 网络

脚本预留两种网络 function：

### 8.1 ECM

```sh
ENABLE_ECM=1 sh scripts/iap_configfs.sh start
```

会创建：

```text
functions/ecm.usb0
```

并设置：

```text
host_addr
dev_addr
```

### 8.2 RNDIS

```sh
ENABLE_RNDIS=1 sh scripts/iap_configfs.sh start
```

会创建：

```text
functions/rndis.usb0
```

ECM/RNDIS 使用哪个，要看目标系统、Host 端驱动和车厂平台要求。

可以同时配置：

```sh
ENABLE_ADB=1 ENABLE_ECM=1 sh scripts/iap_configfs.sh start
```

实际产品里是否允许同时出现多个 function，需要结合 iPhone/CarPlay 枚举要求、VID/PID、接口组合和认证要求确认。

## 9. 使用建议

### 9.1 开发板调试

先只启用 iAP：

```sh
IAP_MODULE_PATH=/root/f_iap.ko \
VID=0x1234 PID=0x5678 \
sh scripts/iap_configfs.sh start
```

确认：

```sh
ls /dev/zjinnova_iap
cat /sys/kernel/config/usb_gadget/g_carplay/UDC
dmesg | grep -i iap
```

### 9.2 加 ADB

```sh
ENABLE_ADB=1 \
IAP_MODULE_PATH=/root/f_iap.ko \
sh scripts/iap_configfs.sh start
```

确认：

```sh
mount | grep functionfs
ls /dev/usb-ffs/adb
ps | grep adbd
```

### 9.3 加网络

```sh
ENABLE_ECM=1 \
IAP_MODULE_PATH=/root/f_iap.ko \
sh scripts/iap_configfs.sh start
```

确认：

```sh
ip link
dmesg | grep -i ecm
```

## 10. 和老师模型的对应关系

老师讲的分层：

```text
UDC 驱动
    usb_gadget / usb_udc
        usb_gadget_driver
            composite
                configuration
                    function
                        endpoint / request
```

当前项目对应：

```text
UDC 驱动
    由 SoC 内核提供，如 dwc2/chipidea

usb_gadget / usb_udc
    由内核 gadget 框架提供

usb_gadget_driver / composite
    由 libcomposite/configfs 提供

configuration
    由 iap_configfs.sh 创建 configs/c.1

function
    由 f_iap.ko 注册 iap function
    代码在 iap_function.c

endpoint / request
    代码在 iap_io.c

用户态业务接口
    /dev/zjinnova_iap
    代码在 iap_chardev.c
```

所以它不是没有按分层做，而是：

```text
完整设备装配层交给 configfs 和脚本
iAP function 层由 f_iap.ko 实现
用户态桥接层通过 miscdevice 暴露给 CarPlay 服务
```

## 11. 后续可继续优化的点

当前重构主要是职责拆分，没有改变业务协议。

后续可以考虑：

```text
1. 给 /dev/zjinnova_iap 增加更明确的状态机注释。
2. 把 VID/PID、product string 固化到产品配置文件中。
3. 根据实际 CarPlay 枚举要求，确认 interface class/subclass/protocol 是否符合认证要求。
4. 如果 read 超时 1 秒是业务需求，保留；如果不是，需要和上层 APP 确认。
5. 若需要热插拔更稳，可以进一步梳理 disconnect/disable/unbind 时 pending request 的处理。
```

特别是第 4 点：

```c
wait_event_interruptible_timeout(..., msecs_to_jiffies(1000));
```

这意味着用户态 `read()` 最多等 1 秒。超时后返回 0。这个行为是否符合 CarPlay 服务预期，需要结合上层协议确认。
