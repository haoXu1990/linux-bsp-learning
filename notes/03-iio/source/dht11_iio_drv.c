#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/ktime.h>

#define DHT11_START_LOW_MS	20
#define DHT11_START_HIGH_US	30
#define DHT11_WAIT_TIMEOUT_US	200
#define DHT11_BIT_HIGH_US	300
#define DHT11_THRESHOLD_US	50

// 定义一个自己的 data
struct dht11_data {
	struct gpio_desc *gpiod;
	struct mutex lock;
	int temperature;
	int humidity;
};

static int dht11_wait_level(struct dht11_data *dht11, int level,
			    unsigned int timeout_us)
{
	ktime_t start;

	start = ktime_get();
	while (gpiod_get_value(dht11->gpiod) != level) {
		if (ktime_to_us(ktime_sub(ktime_get(), start)) > timeout_us)
			return -ETIMEDOUT;
		cpu_relax();
		udelay(1);
	}

	return 0;
}

static int dht11_read_bit(struct dht11_data *dht11)
{
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

static int dht11_start(struct dht11_data *dht11)
{
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

	ret = dht11_wait_level(dht11, 1, DHT11_WAIT_TIMEOUT_US);
	if (ret)
		return ret;

	return 0;
}

static int dht11_read_frame(struct dht11_data *dht11, u8 data[5])
{
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

static int dht11_decode(struct dht11_data *dht11, u8 data[5])
{
	dht11->humidity = data[0];

	if (data[2] & BIT(7))
		dht11->temperature = -data[2];
	else
		dht11->temperature = data[2];

	return 0;
}

static int dht11_read_measurement(struct dht11_data *dht11)
{
	u8 data[5];
	int ret;

	ret = dht11_read_frame(dht11, data);
	if (ret)
		return ret;

	return dht11_decode(dht11, data);
}

static int dht11_read_raw(struct iio_dev *indio_dev,
			  const struct iio_chan_spec *chan,
			  int *val, int *val2, long mask)
{
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
	mutex_unlock(&dht11->lock);
	*val2 = 0;
	return ret;
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
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct dht11_data *dht11;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*dht11));
	if (!indio_dev)
		return -ENOMEM;

	dht11 = iio_priv(indio_dev);
	mutex_init(&dht11->lock);

	dht11->gpiod = devm_gpiod_get(dev, NULL, GPIOD_OUT_HIGH);
	if (IS_ERR(dht11->gpiod)) {
        return PTR_ERR(dht11->gpiod);
	}

	if (gpiod_cansleep(dht11->gpiod))
		return PTR_ERR(dht11->gpiod);

	ret = gpiod_direction_output(dht11->gpiod, 1);
	if (ret)
		return  PTR_ERR(dht11->gpiod);

	indio_dev->name = "dht11";
	indio_dev->info = &dht11_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = dht11_channels;
	indio_dev->num_channels = ARRAY_SIZE(dht11_channels);

	platform_set_drvdata(pdev, indio_dev);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id dht11_of_match[] = {
	{ .compatible = "100ask,dht11" },
	{ }
};
MODULE_DEVICE_TABLE(of, dht11_of_match);

static struct platform_driver dht11_driver = {
	.probe = dht11_probe,
	.driver = {
		.name = "dht11-iio",
		.of_match_table = dht11_of_match,
	},
};
module_platform_driver(dht11_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XuHao");
MODULE_DESCRIPTION("DHT11 IIO driver");
