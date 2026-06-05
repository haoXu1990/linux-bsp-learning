# IIO Triggered Buffer 源码复习

主要以当前实验驱动`DHT11` 模块来介绍：

```text
notes/04-iio-triggered/source/dht11_iio_triggered_drv.c
```

目标不是只会使用 DHT11，而是借 DHT11 把 IIO 的三条主线理清：

```text
1. IIO 设备模型：驱动如何把一个传感器注册给 IIO core
2. Direct mode：用户 cat sysfs 属性时如何进入 read_raw()
3. Triggered buffer：trigger 如何触发采样，数据如何进入 /dev/iio:deviceX
```

## 1. IIO 是什么

IIO 是 Linux 内核里给传感器、ADC、DAC、IMU 等设备准备的子系统。

字符设备驱动通常是驱动自己设计接口，比如：

```text
/dev/dht11
read(fd, buf, len)
```

 IIO 驱动不是这样。IIO 驱动把自己的能力描述给 IIO core：

```text
我叫什么名字
我有哪些通道
每个通道是什么类型
direct read 时怎么读
buffer 模式下一帧数据怎么排列
trigger 到来时怎么采样
```

然后 IIO core 自动创建统一用户接口：

```text
/sys/bus/iio/devices/iio:deviceX/name
/sys/bus/iio/devices/iio:deviceX/in_temp_input
/sys/bus/iio/devices/iio:deviceX/scan_elements/*
/sys/bus/iio/devices/iio:deviceX/buffer/*
/sys/bus/iio/devices/iio:deviceX/trigger/current_trigger
/dev/iio:deviceX
```

**IIO 的核心不是某一个 API，而是一套对象关系!!!**

```text
iio_dev      一个 IIO 设备
iio_info     IIO 设备操作函数表
iio_chan_spec  通道描述
iio_buffer   buffer 数据缓存
iio_trigger  触发源
iio_poll_func trigger 触发后的采样回调
```

## 2. 当前驱动的关键结构

### 2.1 私有数据

```c
struct dht11_data {
	struct gpio_desc *gpiod;
	struct mutex lock;
	int temperature;
	int humidity;
};
```

这是驱动自己的私有数据，通过：

```c
dht11 = iio_priv(indio_dev);
```

从 `iio_dev` 里取出来。

关系是：

```text
iio_dev
    给 IIO core 看

dht11_data
    给 DHT11 驱动内部逻辑使用
```

### 2.2 buffer 一帧数据

```c
struct dht11_scan {
	s32 temperature;
	s32 humidity;
	s64 timestamp;
};
```

triggered buffer 模式下，每触发一次，驱动会准备这样一帧数据，然后推入 IIO buffer。

对应二进制布局：

```text
4 字节 temperature
4 字节 humidity
8 字节 timestamp
总共 16 字节
```

所以用户态读：

```sh
hexdump -C -n 16 /dev/iio:device1
```

读到的就是一帧。

### 2.3 channel 描述

```c
static const struct iio_chan_spec dht11_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
		.scan_index = 0,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
		.scan_index = 1,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(2),
};
```

这一段同时服务 direct mode 和 buffer mode。

`info_mask_separate` 用来生成 direct mode 的 sysfs 文件：

```text
IIO_TEMP + IIO_CHAN_INFO_PROCESSED
    -> in_temp_input

IIO_HUMIDITYRELATIVE + IIO_CHAN_INFO_PROCESSED
    -> in_humidityrelative_input
```

`scan_index` 和 `scan_type` 用来生成 buffer mode 的 scan_elements：

```text
scan_index = 0
    -> scan_elements/in_temp_en
    -> scan_elements/in_temp_index
    -> scan_elements/in_temp_type

scan_index = 1
    -> scan_elements/in_humidityrelative_en
    -> scan_elements/in_humidityrelative_index
    -> scan_elements/in_humidityrelative_type

IIO_CHAN_SOFT_TIMESTAMP(2)
    -> scan_elements/in_timestamp_en
    -> scan_elements/in_timestamp_index
    -> scan_elements/in_timestamp_type
```

`IIO_CHAN_SOFT_TIMESTAMP(2)` 不是结束标记，它是一个真实的 timestamp channel，`2` 表示：

```text
timestamp 的 scan_index = 2
```

## 3. probe 里做了什么

驱动 probe 的核心流程：

```text
devm_iio_device_alloc()
    -> 分配 iio_dev 和私有数据区

iio_priv()
    -> 取出 dht11_data

devm_gpiod_get()
    -> 获取 DHT11 GPIO

填充 indio_dev
    -> name
    -> info
    -> modes
    -> channels
    -> available_scan_masks

iio_triggered_buffer_setup()
    -> 注册 triggered buffer 能力

devm_iio_device_register()
    -> 把 iio_dev 注册给 IIO core
```

对应代码：

```c
indio_dev->name = "dht11_triggered";
indio_dev->info = &dht11_iio_info;
indio_dev->modes = INDIO_DIRECT_MODE;
indio_dev->channels = dht11_channels;
indio_dev->num_channels = ARRAY_SIZE(dht11_channels);
indio_dev->available_scan_masks = dht11_available_scan_masks;
```

注意：

```text
INDIO_DIRECT_MODE
表示设备支持 direct read，不表示禁止 triggered buffer。
```

triggered buffer 能力来自：

```c
iio_triggered_buffer_setup(indio_dev,
			   iio_pollfunc_store_time,
			   dht11_trigger_handler,
			   NULL);
```

所以当前驱动同时支持两条路径：

```text
direct mode:
cat in_temp_input -> dht11_read_raw()

triggered buffer:
trigger_now -> dht11_trigger_handler() -> /dev/iio:deviceX
```

## 4. devm_iio_device_register 注册链路

驱动调用：

```c
ret = devm_iio_device_register(dev, indio_dev);
```

它内部核心进入：

```text
iio_device_register()
    -> __iio_device_register()
```

T113 的内核版本是Linux 5.4.61 ，`__iio_device_register()` 主线可以理解为：

```text
iio_device_register_debugfs(indio_dev)
iio_device_register_sysfs(indio_dev)
iio_device_register_eventset(indio_dev)
iio_buffer_alloc_sysfs_and_mask(indio_dev)
cdev_init(&indio_dev->chrdev, &iio_buffer_fileops)
cdev_device_add(&indio_dev->chrdev, &indio_dev->dev)
```

### 4.1 iio_device_register_debugfs

调试辅助入口。它和 direct read、triggered buffer 主线关系不大，没仔细跟。

### 4.2 iio_device_register_sysfs

负责创建普通 sysfs 属性，比如：

```text
name
in_temp_input
in_humidityrelative_input
```

它根据：

```c
indio_dev->channels
indio_dev->num_channels
indio_dev->info
```

遍历 channel。

当看到：

```c
.type = IIO_TEMP,
.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
```

就生成：

```text
in_temp_input
```

当看到：

```c
.type = IIO_HUMIDITYRELATIVE,
.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
```

就生成：

```text
in_humidityrelative_input
```

这一步只创建文件，不读取硬件。

真正读 DHT11 发生在用户执行：

```sh
cat in_temp_input
```

之后 IIO core 才调用：

```c
indio_dev->info->read_raw()
```

也就是你的：

```c
dht11_read_raw()
```

### 4.3 iio_device_register_eventset

这是 IIO event 机制入口，用于阈值报警、数据 ready 事件等。

当前 DHT11 驱动没有设置：

```c
event_spec
num_event_specs
```

所以这条线也没有仔细跟

### 4.4 iio_buffer_alloc_sysfs_and_mask

这是 buffer 模式的关键注册点。

它做两类事情：

```text
1. 创建 buffer/ 目录
2. 创建 scan_elements/ 目录
```

通用 buffer 属性来自：

```c
iio_buffer_attrs
```

因此生成：

```text
buffer/enable
buffer/length
buffer/watermark
```

这一步不是设置 buffer 大小，只是创建 `buffer/length` 这个文件。

真正设置大小发生在用户态：

```sh
echo 8 > buffer/length
```

这会进入 `industrialio-buffer.c` 中 length 的 store 函数，最终调用具体 buffer 实现里的：

```c
buffer->access->set_length()
```

对当前 kfifo buffer 来说，具体实现位于：

```text
drivers/iio/buffer/kfifo_buf.c
```

`scan_elements` 来自 channel 中的：

```c
scan_index
scan_type
```

例如：

```c
.scan_index = 0,
.scan_type = {
	.sign = 's',
	.realbits = 32,
	.storagebits = 32,
	.endianness = IIO_CPU,
},
```

生成：

```text
scan_elements/in_temp_en
scan_elements/in_temp_index
scan_elements/in_temp_type
```

### 4.5 cdev_init 和 cdev_device_add

源码：

```c
cdev_init(&indio_dev->chrdev, &iio_buffer_fileops);
indio_dev->chrdev.owner = this_mod;
ret = cdev_device_add(&indio_dev->chrdev, &indio_dev->dev);
```

这几行创建：

```text
/dev/iio:deviceX
```

这个字符设备用于读取 buffer 数据。

它绑定的是：

```c
iio_buffer_fileops
```

所以：

```sh
hexdump -C -n 16 /dev/iio:device1
```

走的是：

```text
/dev/iio:device1
    -> iio_buffer_fileops.read
    -> 从 IIO buffer 读取二进制帧
```

而 direct mode：

```sh
cat in_temp_input
```

走的是 sysfs，不走这个字符设备。

## 5. Direct mode 完整链路

用户态：

```sh
cat /sys/bus/iio/devices/iio:device1/in_temp_input
```

内核路径：

```text
sysfs show
    -> IIO core 找到 IIO_TEMP channel
    -> 识别 IIO_CHAN_INFO_PROCESSED
    -> 调用 indio_dev->info->read_raw()
    -> dht11_read_raw()
    -> dht11_read_measurement()
    -> 返回 IIO_VAL_INT
    -> IIO core 把 val 格式化成文本
```

驱动里：

```c
*val = dht11->temperature;
return IIO_VAL_INT;
```

`IIO_VAL_INT` 的含义不是温度值，而是告诉 IIO core：

```text
val 是整数，请按整数格式输出
```

所以最终看到：

```text
26
```

Direct mode 的本质：

```text
用户 read sysfs 的时候，驱动现场采样一次。
```

## 6. available_scan_masks 和 scan_mask

驱动设置：

```c
static const unsigned long dht11_available_scan_masks[] = {
	GENMASK(1, 0),
	0,
};

indio_dev->available_scan_masks = dht11_available_scan_masks;
```

`available_scan_masks` 是驱动声明给 IIO core 的规则：

```text
buffer 模式允许哪些真实数据通道组合
```

`GENMASK(1, 0)` 等价于：

```text
0b11
```

也就是：

```text
bit0 = 1 -> scan_index 0 -> temperature
bit1 = 1 -> scan_index 1 -> humidity
```

所以当前规则是：

```text
温度和湿度必须一起启用
```

`buffer->scan_mask` 是运行时当前用户启用了哪些通道。

例如用户执行：

```sh
echo 1 > scan_elements/in_temp_en
echo 1 > scan_elements/in_humidityrelative_en
```

当前 scan_mask 就是：

```text
0b11
```

如果用户只启用温度：

```text
0b01
```

就不符合当前 `available_scan_masks` 规则。

timestamp 比较特殊。`IIO_CHAN_SOFT_TIMESTAMP(2)` 是 timestamp channel，但通常不写进 `available_scan_masks`。是否把 timestamp 放进输出帧，由用户态控制：

```sh
echo 1 > scan_elements/in_timestamp_en
```

所以：

```text
available_scan_masks 限制真实数据通道组合
in_timestamp_en 决定是否附加 timestamp
```

## 7. Trigger 创建与绑定

Triggered buffer 涉及两个角色：

```text
trigger provider
    触发源，比如 sysfstrig0

trigger consumer
    使用触发源的 IIO 设备，比如 iio:device1
```

### 7.1 创建 sysfs trigger

用户态：

```sh
cd /sys/bus/iio/devices/iio_sysfs_trigger
echo 0 > add_trigger
```

源码位置：

```text
drivers/iio/trigger/iio-trig-sysfs.c
```

效果：

```text
创建 struct iio_sysfs_trig
创建 struct iio_trigger，名字为 sysfstrig0
初始化 sysfs_trig->work，绑定 irq_work 回调
注册 trigger 设备，生成 /sys/bus/iio/devices/trigger0
生成 trigger0/trigger_now 属性
```

注意：

```text
echo 0 > add_trigger
```

这里的 `0` 是 trigger id，一般对应：

```text
sysfstrig0
trigger0
```

### 7.2 绑定 current_trigger

用户态：

```sh
echo sysfstrig0 > /sys/bus/iio/devices/iio:device1/trigger/current_trigger
```

源码：

```c
iio_trigger_write_current()
```

关键流程：

```text
dev_to_iio_dev(dev)
    -> 根据 sysfs 路径中的 iio:device1 得到 indio_dev

iio_trigger_acquire_by_name("sysfstrig0")
    -> 根据写入内容找到 struct iio_trigger

indio_dev->trig = trig
    -> 把 iio:device1 绑定到 sysfstrig0
```

这里要分清：

```text
绑定哪个 device？
由路径 /sys/bus/iio/devices/iio:device1 决定。

绑定哪个 trigger？
由 echo 进去的 sysfstrig0 决定。
```

这一步只选择触发源，还不会调用 `dht11_trigger_handler()`。

## 8. buffer/enable 和 pollfunc 关联

驱动调用：

```c
iio_triggered_buffer_setup(indio_dev,
			   iio_pollfunc_store_time,
			   dht11_trigger_handler,
			   NULL);
```

这一步创建并保存 pollfunc。

可以理解成：

```text
indio_dev->pollfunc
    top half    = iio_pollfunc_store_time
    bottom half = dht11_trigger_handler
```

但此时 pollfunc 还没有挂到 trigger 上。

真正挂接发生在：

```sh
echo 1 > /sys/bus/iio/devices/iio:device1/buffer/enable
```

这条链路会进入：

```text
industrialio-buffer.c
industrialio-triggered-buffer.c
```

核心动作是：

```c
iio_trigger_attach_poll_func(indio_dev->trig, indio_dev->pollfunc);
```

这时才形成：

```text
sysfstrig0
    -> iio:device1 的 pollfunc
        -> iio_pollfunc_store_time
        -> dht11_trigger_handler
```

所以：

```text
current_trigger
    只设置 indio_dev->trig

buffer/enable = 1
    才把 pollfunc attach 到 trigger 上
```

## 9. trigger_now 触发链路

用户态：

```sh
echo 1 > /sys/bus/iio/devices/trigger0/trigger_now
```

源码入口：

```c
static ssize_t iio_sysfs_trigger_poll(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t count)
{
	struct iio_trigger *trig = to_iio_trigger(dev);
	struct iio_sysfs_trig *sysfs_trig = iio_trigger_get_drvdata(trig);

	irq_work_queue(&sysfs_trig->work);

	return count;
}
```

这里：

```text
dev
    来自 /sys/bus/iio/devices/trigger0

to_iio_trigger(dev)
    得到 trigger0 对应的 struct iio_trigger

iio_trigger_get_drvdata(trig)
    得到 sysfs trigger 私有数据 struct iio_sysfs_trig

irq_work_queue(&sysfs_trig->work)
    把 add_trigger 阶段初始化好的 irq_work 加入队列
```

`trigger_now` 这一步不会直接调用 DHT11 驱动。它只是把 work 投递出去。

之后 Linux 通用 irq_work 框架会执行：

```text
work->func(work)
```

这个回调在 add_trigger 阶段已经设置好，通常类似：

```c
iio_sysfs_trigger_work()
```

它会调用：

```c
iio_trigger_poll(sysfs_trig->trig);
```

然后进入 IIO trigger core：

```text
iio_trigger_poll(sysfstrig0)
    -> 遍历 attach 到 sysfstrig0 的 pollfunc
    -> 调用 pollfunc top half
    -> 调用 pollfunc bottom half
```

最终进入：

```c
dht11_trigger_handler()
```

## 10. 数据如何进入 /dev/iio:deviceX

trigger 触发后进入：

```c
dht11_trigger_handler()
```

核心代码：

```c
ret = dht11_read_measurement(dht11);
if (!ret) {
	scan.temperature = dht11->temperature;
	scan.humidity = dht11->humidity;
	iio_push_to_buffers_with_timestamp(indio_dev, &scan, pf->timestamp);
}
```

`scan` 是栈上的临时变量。

真正把数据交给 IIO buffer 的是：

```c
iio_push_to_buffers_with_timestamp()
```

它会把启用的 scan elements 拷贝到 IIO core 管理的 buffer 中。

用户态读取：

```sh
hexdump -C -n 16 /dev/iio:device1
```

例如：

```text
00000000  16 00 00 00 3e 00 00 00 30 a2 29 15 6d 4c 00 00
```

按小端解析：

```text
16 00 00 00 -> 0x00000016 -> 22
3e 00 00 00 -> 0x0000003e -> 62
30 a2 29 15 6d 4c 00 00 -> timestamp
```

也就是：

```text
temperature = 22
humidity = 62
timestamp = 软件时间戳
```

## 11. 用户态完整测试流程

假设 DHT11 是 `iio:device1`。

### 11.1 确认设备

```sh
for i in /sys/bus/iio/devices/iio:device*; do
	echo "== $i =="
	cat $i/name
done
```

### 11.2 创建 sysfs trigger

```sh
cd /sys/bus/iio/devices/iio_sysfs_trigger
echo 0 > add_trigger
cat /sys/bus/iio/devices/trigger0/name
```

期望：

```text
sysfstrig0
```

### 11.3 配置 buffer

```sh
cd /sys/bus/iio/devices/iio:device1

echo 0 > buffer/enable
echo 1 > scan_elements/in_temp_en
echo 1 > scan_elements/in_humidityrelative_en
echo 1 > scan_elements/in_timestamp_en
echo 8 > buffer/length
echo sysfstrig0 > trigger/current_trigger
echo 1 > buffer/enable
```

### 11.4 触发并读取

触发一次：

```sh
echo 1 > /sys/bus/iio/devices/trigger0/trigger_now
```

读取一帧：

```sh
hexdump -C -n 16 /dev/iio:device1
```

如果 `hexdump` 卡住，说明 buffer 里没有数据。常见原因：

```text
1. trigger 没触发
2. buffer 没 enable
3. current_trigger 没绑定
4. DHT11 读取失败，handler 没有 push 数据
```

DHT11 不能太频繁触发，建议触发间隔至少 2 秒。

## 12. 三条主线总结

### 12.1 IIO 设备注册主线

```text
dht11_probe()
    -> devm_iio_device_alloc()
    -> 填 indio_dev
    -> iio_triggered_buffer_setup()
    -> devm_iio_device_register()
        -> __iio_device_register()
        -> iio_device_register_sysfs()
        -> iio_buffer_alloc_sysfs_and_mask()
        -> cdev_device_add()
```

结果：

```text
生成 /sys/bus/iio/devices/iio:deviceX
生成 direct sysfs 属性
生成 buffer/ 和 scan_elements/
生成 /dev/iio:deviceX
```

### 12.2 Direct mode 主线

```text
cat in_temp_input
    -> IIO sysfs show
    -> indio_dev->info->read_raw()
    -> dht11_read_raw()
    -> dht11_read_measurement()
    -> 文本输出
```

关键词：

```text
sysfs
read_raw
IIO_VAL_INT
文本数值
```

### 12.3 Triggered buffer 主线

```text
echo 0 > iio_sysfs_trigger/add_trigger
    -> 创建 sysfstrig0 / trigger0

echo sysfstrig0 > iio:device1/trigger/current_trigger
    -> indio_dev->trig = sysfstrig0

echo 1 > iio:device1/buffer/enable
    -> attach pollfunc 到 trigger

echo 1 > trigger0/trigger_now
    -> irq_work_queue()
    -> iio_sysfs_trigger_work()
    -> iio_trigger_poll()
    -> dht11_trigger_handler()
    -> iio_push_to_buffers_with_timestamp()

read /dev/iio:device1
    -> 读取 buffer 二进制帧
```

关键词：

```text
trigger provider
trigger consumer
current_trigger
buffer enable
pollfunc
iio_push_to_buffers_with_timestamp
/dev/iio:deviceX
```

## 13. Direct mode 和 Triggered buffer 的区别

Direct mode：

```text
读的时候采样
接口是 sysfs
输出是文本
适合低速、简单传感器
```

Triggered buffer：

```text
trigger 触发时采样
接口是 /dev/iio:deviceX
输出是二进制帧
适合连续采样、多通道同步、需要 timestamp 的设备
```

DHT11 工程上用 direct mode 更简单，因为它低速、数据量小。

本实验使用 triggered buffer 的意义是学习 IIO 架构：

```text
trigger 如何作为触发源
buffer 如何作为数据队列
pollfunc 如何连接 trigger 和驱动采样函数
sysfs 如何把 provider 和 consumer 串起来
```

## 14. 常见问题

### 14.1 为什么 `in_temp_input` 还能读

因为当前驱动同时支持 direct mode 和 triggered buffer。

```text
in_temp_input
    -> direct mode

/dev/iio:deviceX
    -> triggered buffer mode
```

### 14.2 为什么 `hexdump` 会卡住

`/dev/iio:deviceX` 是 buffer 数据出口。如果 buffer 中没有数据，`read()` 会阻塞。

需要先：

```text
启用 scan_elements
设置 buffer/length
绑定 current_trigger
启用 buffer
触发 trigger_now
```

### 14.3 为什么 timestamp 会读出来

因为启用了：

```sh
echo 1 > scan_elements/in_timestamp_en
```

timestamp 来自：

```c
IIO_CHAN_SOFT_TIMESTAMP(2)
```

以及：

```c
iio_push_to_buffers_with_timestamp(indio_dev, &scan, pf->timestamp);
```

### 14.4 一个 trigger 能不能触发多个设备

可以。

一个 trigger 可以 attach 多个 pollfunc，也就是触发多个 IIO consumer。

数量受内核配置限制：

```text
CONFIG_IIO_CONSUMERS_PER_TRIGGER
```

### 14.5 为什么需要 CONFIG_IIO_TRIGGERED_BUFFER

因为这些 API 来自 triggered buffer 框架：

```text
iio_triggered_buffer_setup
iio_triggered_buffer_cleanup
devm_iio_triggered_buffer_setup
```

如果内核没开启：

```text
CONFIG_IIO_TRIGGERED_BUFFER
```

外部模块在 MODPOST 时会报 undefined symbol。

## 15. 最终理解

IIO 的精妙之处在于：

```text
驱动只描述能力
IIO core 创建统一接口
sysfs 负责控制面
/dev/iio:deviceX 负责数据面
trigger 负责采样时机
buffer 负责缓存数据帧
```

当前实验最终闭环：

```text
DHT11 驱动
    -> 描述 temp/humidity/timestamp channel
    -> 提供 read_raw
    -> 提供 trigger handler

IIO core
    -> 生成 sysfs direct 接口
    -> 生成 scan_elements 和 buffer 控制接口
    -> 生成 /dev/iio:deviceX

sysfs trigger
    -> 提供 sysfstrig0
    -> trigger_now 手动触发

用户态
    -> current_trigger 绑定触发源
    -> buffer/enable 启用 buffer
    -> trigger_now 触发采样
    -> read /dev/iio:deviceX 读取二进制帧
```
