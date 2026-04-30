# DHT11 IIO 学习笔记

## 1. 学习目标

这份笔记的目标不是再写一个私有的 `dht11` 字符设备驱动，而是学会：

1. 什么是 IIO 子系统
2. DHT11 为什么适合挂到 IIO
3. 字符设备版 DHT11 和 IIO 版 DHT11 的区别
4. IIO 版 DHT11 驱动的核心代码结构
5. 设备树怎么写
6. 用户态怎么读取温湿度


## 2. 先看你仓库里已有的 DHT11 驱动

你已经有一个基于 GPIO 位操作的 DHT11 驱动：

- [notes/01-gpio/03-dht110-drv/source/dht11_drv.c](</\\?\UNC\192.168.10.208\xuhao\work\100ask\linux-bsp-learning\notes\01-gpio\03-dht110-drv\source\dht11_drv.c>)
- [notes/01-gpio/03-dht110-drv/source/dht11_test.c](</\\?\UNC\192.168.10.208\xuhao\work\100ask\linux-bsp-learning\notes\01-gpio\03-dht110-drv\source\dht11_test.c>)

这份代码已经把 DHT11 协议最难的部分做了出来：

1. 主机拉低总线一段时间，发起开始信号
2. 切换 GPIO 为输入
3. 等待 DHT11 响应
4. 读取 40bit 数据
5. 校验和检查
6. 返回湿度和温度

也就是说，你现在已经会了：

- DHT11 时序
- GPIO 输入输出切换
- 用延时和电平检测解码 bit 流

下一步只是把“驱动框架”从字符设备换成 IIO。


## 3. 什么是 IIO

IIO 是 `Industrial I/O`，Linux 内核里专门给传感器、ADC、DAC、IMU、温湿度等设备准备的子系统。

常见特征：

1. 一个设备可以暴露多个通道 `channel`
2. 每个通道有标准属性
3. 用户态通常通过 `sysfs` 读取
4. 复杂场景还可以用 buffer/trigger 做批量采样

对 DHT11 这类传感器来说，IIO 很合适，因为它天然就有两个数据通道：

- 温度
- 相对湿度


## 4. 为什么不建议继续做字符设备版

字符设备版也能用，但问题是接口不标准。

比如你现在的方式是：

```c
open("/dev/dht11")
read(fd, buf, 2)
```

这个接口只有你自己的程序知道怎么解析，别人接手代码时要重新学习。

IIO 版的优势是：

1. 驱动模型标准
2. 用户接口标准
3. 温度、湿度通道命名标准
4. 方便和其他传感器驱动统一管理
5. 更符合 Linux 内核里“传感器驱动”的写法


## 5. 字符设备版 vs IIO 版

### 5.1 你现在的字符设备版

核心 API：

- `register_chrdev`
- `class_create`
- `device_create`
- `file_operations`
- `read`
- `copy_to_user`

用户看到的是：

- `/dev/dht11`

### 5.2 IIO 版

核心 API：

- `devm_iio_device_alloc`
- `iio_priv`
- `struct iio_chan_spec`
- `struct iio_info`
- `devm_iio_device_register`

用户看到的是：

- `/sys/bus/iio/devices/iio:deviceX/name`
- `/sys/bus/iio/devices/iio:deviceX/in_temp_input`
- `/sys/bus/iio/devices/iio:deviceX/in_humidityrelative_input`


## 6. DHT11 放到 IIO 后，思路怎么变

要注意，变的是“框架”，不是“协议”。

### 6.1 不变的部分

下面这些业务逻辑仍然存在：

1. GPIO 拉低 18ms 左右
2. GPIO 拉高 20~40us
3. 切成输入
4. 等待传感器响应
5. 接收 40bit
6. 判断每一 bit 是 0 还是 1
7. 校验 checksum

### 6.2 改变的部分

字符设备版是在 `read()` 里直接采数据并返回给用户。

IIO 版通常会：

1. 注册一个 `iio_dev`
2. 定义两个 channel
3. 实现 `read_raw()`
4. 用户读 `in_temp_input` 或 `in_humidityrelative_input` 时，驱动在 `read_raw()` 里返回结果

所以可以把它理解成：

- 字符设备版：你自己设计接口
- IIO 版：你填 Linux 已经定义好的传感器接口


## 7. IIO 驱动里最重要的 4 个结构

### 7.1 私有数据

驱动总要有自己的私有数据，比如 GPIO、锁、缓存值：

```c
struct dht11 {
	struct gpio_desc *gpiod;
	struct mutex lock;
	int temperature;
	int humidity;
};
```

这里的 `temperature` 和 `humidity` 一般存“处理后的值”。


### 7.2 channel 定义

IIO 要先告诉内核：我有哪些测量通道。

```c
static const struct iio_chan_spec dht11_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};
```

这表示：

1. 设备有温度通道
2. 设备有湿度通道
3. 支持读取“处理后的值”


### 7.3 `iio_info`

驱动要告诉 IIO core：读通道时该调哪个函数。

```c
static const struct iio_info dht11_iio_info = {
	.read_raw = dht11_read_raw,
};
```


### 7.4 `iio_dev`

探测函数里要分配并注册 IIO 设备：

```c
indio_dev = devm_iio_device_alloc(dev, sizeof(struct dht11));
if (!indio_dev)
	return -ENOMEM;

dht11 = iio_priv(indio_dev);

indio_dev->name = "dht11";
indio_dev->info = &dht11_iio_info;
indio_dev->modes = INDIO_DIRECT_MODE;
indio_dev->channels = dht11_channels;
indio_dev->num_channels = ARRAY_SIZE(dht11_channels);

return devm_iio_device_register(dev, indio_dev);
```


## 8. `read_raw()` 是 IIO 版的核心入口

对 DHT11 来说，最重要的是 `read_raw()`。

它的职责通常是：

1. 判断当前读的是温度还是湿度
2. 触发一次 DHT11 采样
3. 拿到原始 5 字节数据
4. 校验 checksum
5. 返回处理后的值

典型框架如下：

```c
static int dht11_read_raw(struct iio_dev *indio_dev,
			  const struct iio_chan_spec *chan,
			  int *val, int *val2, long mask)
{
	struct dht11 *dht11 = iio_priv(indio_dev);
	int ret;

	if (mask != IIO_CHAN_INFO_PROCESSED)
		return -EINVAL;

	mutex_lock(&dht11->lock);
	ret = dht11_read_measurement(dht11);
	if (ret) {
		mutex_unlock(&dht11->lock);
		return ret;
	}

	switch (chan->type) {
	case IIO_TEMP:
		*val = dht11->temperature;
		break;
	case IIO_HUMIDITYRELATIVE:
		*val = dht11->humidity;
		break;
	default:
		mutex_unlock(&dht11->lock);
		return -EINVAL;
	}

	mutex_unlock(&dht11->lock);
	return IIO_VAL_INT;
}
```

这里的 `dht11_read_measurement()` 就相当于把你现在字符设备版 `dht11_read()` 里的协议部分抽出来。


## 9. 你现有代码迁移到 IIO 时，建议怎么拆

最稳妥的迁移方式不是重写全部逻辑，而是拆层。

### 9.1 协议层

保留你现有的这些函数思路：

- `wait_level`
- `wait_level_with_time`
- `dht11_read_bit`

然后把它们整理成：

```c
static int dht11_start(struct dht11 *dht11);
static int dht11_read_byte(struct dht11 *dht11, u8 *data);
static int dht11_read_frame(struct dht11 *dht11, u8 data[5]);
static int dht11_decode(struct dht11 *dht11, u8 data[5]);
```

### 9.2 框架层

新增 IIO 相关逻辑：

1. `probe`
2. `remove` 或 `devm_*`
3. `iio_chan_spec`
4. `iio_info`
5. `read_raw`

### 9.3 设备树层

把 GPIO 从写死的编号改成设备树获取：

```c
dht11->gpiod = devm_gpiod_get(dev, NULL, GPIOD_IN);
```

这样驱动就不会把 `139` 这种板级数字写死在代码里。


## 10. 设备树应该怎么写

对于 DHT11，设备树节点通常很简单：

```dts
humidity_sensor: dht11 {
	compatible = "dht11";
	gpios = <&pio 4 11 GPIO_ACTIVE_HIGH>; /* 例子：PE11 */
	status = "okay";
};
```

说明：

1. `compatible = "dht11"` 用来和驱动匹配
2. `gpios` 指定数据引脚
3. DHT11 通常只需要 1 根数据线

你的旧代码里把 GPIO 写死成 `139`，这适合学习协议，不适合标准驱动。

标准驱动应该从设备树拿 GPIO。


## 11. 用户态怎么读 IIO 设备

驱动注册成功后，用户态一般这样查看：

```sh
cd /sys/bus/iio/devices/
ls
cat iio:device0/name
cat iio:device0/in_temp_input
cat iio:device0/in_humidityrelative_input
```

常见理解方式：

1. `in_temp_input` 表示温度
2. `in_humidityrelative_input` 表示相对湿度

很多 IIO 传感器会使用“毫单位”：

- `25000` 表示 `25.000` 摄氏度
- `60000` 表示 `60.000%`

具体单位要看驱动实现。


## 12. DHT11 在 IIO 里常见的两种实现方式

### 12.1 轮询方式

像你现在这份代码一样：

1. 发开始信号
2. 切输入
3. 循环等电平变化
4. 用高电平时间判断 0/1

优点：

1. 好理解
2. 容易从现有代码迁移

缺点：

1. 对时序敏感
2. CPU 忙等较多
3. 系统调度影响大


### 12.2 中断采样方式

上游 DHT11 IIO 驱动更接近这种思路：

1. 发开始信号
2. 申请 GPIO 边沿中断
3. 每次边沿到来时记录时间戳
4. 采样完成后统一解析整个波形

优点：

1. 更符合内核驱动思路
2. 不用一直在一个循环里忙等所有边沿

缺点：

1. 实现更复杂
2. 对中断和状态管理要求更高

学习顺序建议：

1. 先把轮询版迁移到 IIO
2. 再去看中断版的上游实现


## 13. 一个最小 IIO DHT11 骨架

下面这个骨架不是完整驱动，但可以帮你建立结构感。

```c
#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/iio/iio.h>

struct dht11 {
	struct gpio_desc *gpiod;
	struct mutex lock;
	int temperature;
	int humidity;
};

static int dht11_read_measurement(struct dht11 *dht11)
{
	/* 这里填你现有的 DHT11 协议读取逻辑 */
	/* 成功后把 dht11->temperature 和 dht11->humidity 更新好 */
	return 0;
}

static int dht11_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct dht11 *dht11 = iio_priv(indio_dev);
	int ret;

	if (mask != IIO_CHAN_INFO_PROCESSED)
		return -EINVAL;

	mutex_lock(&dht11->lock);
	ret = dht11_read_measurement(dht11);
	if (ret) {
		mutex_unlock(&dht11->lock);
		return ret;
	}

	switch (chan->type) {
	case IIO_TEMP:
		*val = dht11->temperature;
		break;
	case IIO_HUMIDITYRELATIVE:
		*val = dht11->humidity;
		break;
	default:
		mutex_unlock(&dht11->lock);
		return -EINVAL;
	}

	mutex_unlock(&dht11->lock);
	return IIO_VAL_INT;
}

static const struct iio_info dht11_iio_info = {
	.read_raw = dht11_read_raw,
};

static const struct iio_chan_spec dht11_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static int dht11_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct dht11 *dht11;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*dht11));
	if (!indio_dev)
		return -ENOMEM;

	dht11 = iio_priv(indio_dev);
	mutex_init(&dht11->lock);

	dht11->gpiod = devm_gpiod_get(&pdev->dev, NULL, GPIOD_IN);
	if (IS_ERR(dht11->gpiod))
		return PTR_ERR(dht11->gpiod);

	indio_dev->name = "dht11";
	indio_dev->info = &dht11_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = dht11_channels;
	indio_dev->num_channels = ARRAY_SIZE(dht11_channels);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}
```


## 14. 学习时最容易卡住的点

### 14.1 误以为 IIO 会帮你实现协议

不会。

IIO 只是传感器驱动框架，不会替你实现 DHT11 的单总线时序。

真正和 DHT11 协议相关的部分，仍然需要你自己写。


### 14.2 把 GPIO 编号写死

学习阶段可以写死，正式驱动不建议。

应该改成：

1. 设备树描述硬件
2. 驱动从设备树取 GPIO


### 14.3 不清楚返回值单位

IIO 里一定要想清楚：

1. 返回整数还是整数加小数
2. 返回原始值还是处理后值
3. 温度湿度的单位是什么

最简单的方式是：

- 直接返回整型摄氏度和整型湿度

更标准一些的方式是：

- 返回毫摄氏度、千分比湿度


### 14.4 在 `read_raw()` 里没有加锁

DHT11 一次采样不是原子操作，最好加 `mutex`，避免并发读取把时序打乱。


## 15. 建议你的实操顺序

### 第一阶段：只理解，不动代码

1. 重新读一遍 [dht11_drv.c](</\\?\UNC\192.168.10.208\xuhao\work\100ask\linux-bsp-learning\notes\01-gpio\03-dht110-drv\source\dht11_drv.c>)
2. 把 `dht11_read()` 里的协议流程画成时序图
3. 分清哪些代码属于“协议层”，哪些属于“字符设备框架层”

### 第二阶段：做最小 IIO 迁移

1. 去掉 `register_chrdev`
2. 新增 `iio_dev`
3. 新增两个 channel
4. 把原来的读取流程搬到 `read_raw()`
5. 先用轮询实现跑通

### 第三阶段：做标准化

1. GPIO 改为设备树获取
2. 返回值单位标准化
3. 处理错误码
4. 加锁

### 第四阶段：对照上游驱动

1. 阅读上游 `drivers/iio/humidity/dht11.c`
2. 理解它为什么采用边沿中断加时间戳的方案
3. 再决定是否把自己的轮询版升级成中断版


## 16. 你可以把这件事总结成一句话

学习 DHT11 的 IIO 驱动，本质上是在做两件事：

1. 保留 DHT11 协议读取逻辑
2. 把输出接口从私有字符设备改成标准 IIO 通道


## 17. 推荐你现在就做的练习

如果你准备开始写代码，建议按这个顺序练：

1. 先把现有 `dht11_read()` 里的协议代码单独抽成 `dht11_read_measurement()`
2. 再写一个最小 `platform + iio` 驱动壳子
3. 让用户态先能通过 `in_temp_input` 读到温度
4. 再补 `in_humidityrelative_input`
5. 最后再处理设备树和单位规范


## 18. 参考资料

1. Linux IIO 简介  
   [https://docs.kernel.org/driver-api/iio/intro.html](https://docs.kernel.org/driver-api/iio/intro.html)

2. Linux IIO Core  
   [https://docs.kernel.org/driver-api/iio/core.html](https://docs.kernel.org/driver-api/iio/core.html)

3. Linux 上游 DHT11 IIO 驱动源码  
   [https://codebrowser.dev/linux/linux/drivers/iio/humidity/dht11.c.html](https://codebrowser.dev/linux/linux/drivers/iio/humidity/dht11.c.html)

4. DHT11 设备树绑定示例  
   [https://kernel.googlesource.com/pub/scm/linux/kernel/git/ericvh/v9fs/%2B/0ba3307a8ec35252f7b1e222e32889a6f3d9ceb3/Documentation/devicetree/bindings/iio/humidity/dht11.txt](https://kernel.googlesource.com/pub/scm/linux/kernel/git/ericvh/v9fs/%2B/0ba3307a8ec35252f7b1e222e32889a6f3d9ceb3/Documentation/devicetree/bindings/iio/humidity/dht11.txt)
