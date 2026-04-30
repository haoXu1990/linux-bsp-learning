#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define DHT11_START_LOW_MS 20
#define DHT11_START_HIGH_US 30
#define DHT11_WAIT_TIMEOUT_US 200
#define DHT11_BIT_HIGH_US 300
#define DHT11_THRESHOLD_US 50

/**
 * 定义 IIO 的私有数据区
 */
struct dht11_data {
  struct gpio_desc *gpiod;
  struct mutex lock;
  int temperature;
  int humidity;
};

/*
 * triggered-buffer 模式下推入 /dev/iio:deviceX 的一帧二进制数据。
 * 这个布局必须和下面 dht11_channels[] 里的 scan_index/scan_type 对应：
 *   index 0: temperature, s32
 *   index 1: humidity, s32
 *   index 2: timestamp, s64
 */
struct dht11_scan {
  s32 temperature;
  s32 humidity;
  s64 timestamp;
};

static int dht11_wait_level(struct dht11_data *dht11, int level,
                            unsigned int timeout_us) {
  ktime_t start = ktime_get();

  while (gpiod_get_value(dht11->gpiod) != level) {
    if (ktime_to_us(ktime_sub(ktime_get(), start)) > timeout_us)
      return -ETIMEDOUT;
    cpu_relax();
    udelay(1);
  }

  return 0;
}

static int dht11_read_bit(struct dht11_data *dht11) {
  ktime_t start;
  long high_us;
  int ret;

  ret = dht11_wait_level(dht11, 0, DHT11_WAIT_TIMEOUT_US);
  if (ret)
    return ret;

  ret = dht11_wait_level(dht11, 1, DHT11_WAIT_TIMEOUT_US);
  if (ret)
    return ret;

  start = ktime_get();
  ret = dht11_wait_level(dht11, 0, DHT11_BIT_HIGH_US);
  if (ret)
    return ret;

  high_us = ktime_to_us(ktime_sub(ktime_get(), start));

  return high_us > DHT11_THRESHOLD_US ? 1 : 0;
}

static int dht11_start(struct dht11_data *dht11) {
  int ret;

  ret = gpiod_direction_output(dht11->gpiod, 0);
  if (ret)
    return ret;

  msleep(DHT11_START_LOW_MS);

  ret = gpiod_direction_output(dht11->gpiod, 1);
  if (ret)
    return ret;

  udelay(DHT11_START_HIGH_US);

  ret = gpiod_direction_input(dht11->gpiod);
  if (ret)
    return ret;

  ret = dht11_wait_level(dht11, 0, DHT11_WAIT_TIMEOUT_US);
  if (ret)
    return ret;

  return dht11_wait_level(dht11, 1, DHT11_WAIT_TIMEOUT_US);
}

static int dht11_read_frame(struct dht11_data *dht11, u8 data[5]) {
  int bit;
  int i;
  int ret;

  memset(data, 0, 5);

  ret = dht11_start(dht11);
  if (ret)
    return ret;

  for (i = 0; i < 40; i++) {
    bit = dht11_read_bit(dht11);
    if (bit < 0)
      return bit;

    data[i / 8] <<= 1;
    data[i / 8] |= bit;
  }

  if (((u8)(data[0] + data[1] + data[2] + data[3])) != data[4])
    return -EIO;

  return 0;
}

static int dht11_decode(struct dht11_data *dht11, u8 data[5]) {
  dht11->humidity = data[0];

  if (data[2] & BIT(7))
    dht11->temperature = -data[2];
  else
    dht11->temperature = data[2];

  return 0;
}

static int dht11_read_measurement(struct dht11_data *dht11) {
  u8 data[5];
  int ret;

  ret = dht11_read_frame(dht11, data);
  if (ret)
    return ret;

  return dht11_decode(dht11, data);
}

/*
 * direct 模式路径：
 *   cat in_temp_input / in_humidityrelative_input
 *       -> IIO core
 *       -> dht11_read_raw()
 *       -> 立刻读取一次 DHT11
 *
 * 这条路径和 03-iio 实验里的用户态访问方式一样。
 */
static int dht11_read_raw(struct iio_dev *indio_dev,
                          const struct iio_chan_spec *chan, int *val, int *val2,
                          long mask) {
  struct dht11_data *dht11 = iio_priv(indio_dev);
  int ret;

  if (mask != IIO_CHAN_INFO_PROCESSED)
    return -EINVAL;

  mutex_lock(&dht11->lock);

  ret = dht11_read_measurement(dht11);
  if (ret)
    goto out_unlock;

  switch (chan->type) {
  case IIO_TEMP:
    *val = dht11->temperature;
    ret = IIO_VAL_INT;
    break;
  case IIO_HUMIDITYRELATIVE:
    *val = dht11->humidity;
    ret = IIO_VAL_INT;
    break;
  default:
    ret = -EINVAL;
    break;
  }

out_unlock:
  *val2 = 0;
  mutex_unlock(&dht11->lock);
  return ret;
}

/*
 * triggered-buffer 模式路径：
 *   trigger 触发
 *       -> IIO core 调用这个 poll function
 *       -> 读取一次 DHT11
 *       -> 把一帧二进制数据推入 /dev/iio:deviceX
 *
 * 这个模式下，用户态不是从 sysfs 的 in_temp_input 取 buffer 数据，
 * 而是在启用 buffer 并绑定 trigger 后，从 /dev/iio:deviceX 读取二进制帧。
 */
static irqreturn_t dht11_trigger_handler(int irq, void *p) {
  struct iio_poll_func *pf = p;
  struct iio_dev *indio_dev = pf->indio_dev;
  struct dht11_data *dht11 = iio_priv(indio_dev);
  struct dht11_scan scan;
  int ret;

  memset(&scan, 0, sizeof(scan));

  mutex_lock(&dht11->lock);
  ret = dht11_read_measurement(dht11);
  if (!ret) {
    scan.temperature = dht11->temperature;
    scan.humidity = dht11->humidity;
    /*
     * 这里才是 triggered-buffer 模式真正的数据输出点。
     * IIO core 会把启用的 scan element 拷贝进设备 buffer，
     * 用户态随后从 /dev/iio:deviceX 读取这些数据。
     */
    iio_push_to_buffers_with_timestamp(indio_dev, &scan, pf->timestamp);
  }
  mutex_unlock(&dht11->lock);

  iio_trigger_notify_done(indio_dev->trig);

  return IRQ_HANDLED;
}

static const struct iio_info dht11_iio_info = {
    .read_raw = dht11_read_raw,
};

/*
 * channel 同时服务两种接口：
 *   - info_mask_separate 用来生成 direct 模式的 sysfs 文件，
 *     比如 in_temp_input 和 in_humidityrelative_input。
 *   - scan_index / scan_type 用来生成 buffer 模式的 scan_elements 文件。
 */
static const struct iio_chan_spec dht11_channels[] = {
    {
        .type = IIO_TEMP,
        .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
        .scan_index = 0,
        .scan_type =
            {
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
        .scan_type =
            {
                .sign = 's',
                .realbits = 32,
                .storagebits = 32,
                .endianness = IIO_CPU,
            },
    },
    IIO_CHAN_SOFT_TIMESTAMP(2),
};

/*
 * 限制 buffer 里温度和湿度必须成对启用。
 * 这样这个实验里的二进制帧布局就固定为：
 *   s32 temperature, s32 humidity, s64 timestamp
 */
static const unsigned long dht11_available_scan_masks[] = {
    GENMASK(1, 0),//GENMASK 的意思是 重 bitX-bitX 的掩码为1， 这里就是3， 我们只启用 dht11_channels 前面2个通道
    0,
};

static int dht11_probe(struct platform_device *pdev) {
  struct device *dev = &pdev->dev;

  // IIO 设备
  struct iio_dev *indio_dev;
  // IIO 私有数据区
  struct dht11_data *dht11;
  int ret;

  // 创建一个IIO 设备
  indio_dev = devm_iio_device_alloc(dev, sizeof(*dht11));
  if (!indio_dev)
    return -ENOMEM;

  // 去读自己的私有数据区
  dht11 = iio_priv(indio_dev);
  mutex_init(&dht11->lock);

  // 设置GPIO
  dht11->gpiod = devm_gpiod_get(dev, NULL, GPIOD_OUT_HIGH);
  if (IS_ERR(dht11->gpiod))
    return PTR_ERR(dht11->gpiod);

  if (gpiod_cansleep(dht11->gpiod))
    return -EINVAL;

  ret = gpiod_direction_output(dht11->gpiod, 1);
  if (ret)
    return ret;

  // 填充IIO 设备
  // 设置name 决定 /sys/bus/iio/devices/iio:devices1/name 显示内容
  indio_dev->name = "dht11_triggered";

  // 设置info， 也就是 iio dirct 模式的时候会调用这个来读取数据
  // 当执行 cat /sys/bus/iio/devices/iio:device1/in_temp_input 就是调用这里
  indio_dev->info = &dht11_iio_info;

  // 这里只是表示当前 iio 设备支持  Direct 模式
  indio_dev->modes = INDIO_DIRECT_MODE;

  // 设置IIO 设备有哪些通道， 这个是给 IIO core 用的
  indio_dev->channels = dht11_channels;

  indio_dev->num_channels = ARRAY_SIZE(dht11_channels);

  // 告诉 IIO core 当前buffer 模式允许有哪些通道组合
  // 这里也比较重要，开始理解错了,  注意这里只限制真实的通道
  // 这里表达了允许哪些 scan 一起启用；
  // 例如当前我们的
  indio_dev->available_scan_masks = dht11_available_scan_masks;

  /*
   * 注册 IIO triggered-buffer
   * 当然这里和上面的 indio_dev->modes， 这里也表示当前iio_dev 注册后支持
   * triggered/buffer 模式 iio_pollfunc_store_time 是 中断上半部：记录 trigger
   * 触发时间。 dht11_trigger_handler 是 中断下半部：真正读取 DHT11，
   * 并把一帧数据推入 IIO buffer。
   */
  ret = iio_triggered_buffer_setup(indio_dev, iio_pollfunc_store_time,
                                   dht11_trigger_handler, NULL);
  if (ret)
    return ret;

  platform_set_drvdata(pdev, indio_dev);

  ret = devm_iio_device_register(dev, indio_dev);
  if (ret)
    iio_triggered_buffer_cleanup(indio_dev);

  return ret;
}

static int dht11_remove(struct platform_device *pdev) {
  struct iio_dev *indio_dev = platform_get_drvdata(pdev);

  iio_triggered_buffer_cleanup(indio_dev);

  return 0;
}

static const struct of_device_id dht11_of_match[] = {
    {.compatible = "100ask,dht11-triggered"}, {}};
MODULE_DEVICE_TABLE(of, dht11_of_match);

static struct platform_driver dht11_driver = {
    .probe = dht11_probe,
    .remove = dht11_remove,
    .driver =
        {
            .name = "dht11-iio-triggered",
            .of_match_table = dht11_of_match,
        },
};
module_platform_driver(dht11_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XuHao");
MODULE_DESCRIPTION("DHT11 IIO triggered buffer demo driver");
