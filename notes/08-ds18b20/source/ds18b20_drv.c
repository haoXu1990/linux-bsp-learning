#include <cstddef>
#include <linux/module.h>

#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/kmod.h>
#include <linux/gfp.h>
#include <linux/gpio/consumer.h>

// 01
static int major;

static struct class *ds18b20_drv_class;

struct gpio_desc *ds18b20_gpio;

static DECLARE_WAIT_QUEUE_HEAD(gpio_wait);

void delay_ktime_us(int us) {
    // 先用 udelay 测试
    udelay(us);
}

static void ds18b20_write_bit(int bit) {
    // 写时序必须最少支持 60 us
    //  ds18b20 在15 us 后采样
    if (bit) {
        // 写1 时序
        // 拉低总线 15us
        gpiod_direction_output(ds18b20_gpio, 0);
        delay_ktime_us(5);
        // 设置input 后由上拉电阻拉高GPIO
        gpiod_direction_input(ds18b20_gpio);
        delay_ktime_us(55);
    } else {
        // 写 0 时序， 拉低至少 60us, 最多120us
        //  ds18b20 在15 us 后采样
        gpiod_direction_output(ds18b20_gpio, 0);
        delay_ktime_us(60);
        gpiod_direction_input(ds18b20_gpio);
        // 让从机读， 感觉可以不要这个 5us
        delay_ktime_us(5);
    }
}

static void ds18b20_write_byte(u8 value) {
    int i;
    for (i = 0; i < 8; i++) {
        // 这里会不会由问题，写失败怎么知道？
        ds18b20_write_bit(value & 0x01);
        value >>=1;
    }
}

static int ds18b20_read_bit() {
    int value;
    // 拉低 1us最低 -> 释放 -> 15us 延时 -> 读数据
    gpiod_direction_output(ds18b20_gpio, 0);
    delay_ktime_us(2);
    gpiod_direction_input(ds18b20_gpio);
    delay_ktime_us(13);
    value = gpiod_get_value(ds18b20_gpio);
    return value;
}

static u8 ds18b20_read_byte(){
    int i;
    u8 data = 0;

    for (i = 0; i < 8; i++) {
    data >>= 1;
    if (ds18b20_read_bit())
        data |= 0x80;
    }

    return data;
}

// 发送复位信号
static int ds18b20_reset() {

    // DS18B20 手册初始化时序 图13
    // 主机拉低最少 480us 然后等待 15-60 us，从机拉低总线 60 - 240 us;
    int value;

    // 1. 拉低 480 us
    gpiod_direction_output(ds18b20_gpio, 0);

    // 1.1 保持最低 480us
    delay_ktime_us(500);

    // 2. 释放总线
    gpiod_direction_input(ds18b20_gpio);
    // 2.1 等待 60us
    delay_ktime_us(70);

    value = gpiod_get_value(ds18b20_gpio);

    // 如果等待 70 us 后为0，代表有从机响应
    return value == 0 ? 0 : -1;
}

static int ds18b20_read_temp() {
    u8 temp_l, temp_h;
    int temp;

    //1. reset
    if (ds18b20_reset())
        return -1;
    // 0xcc  skip rom
    ds18b20_write_byte(0xCC);

    ds18b20_write_byte(0x44);

    msleep(750); // 等待转换

    // reset

    if (ds18b20_reset())
        return -1;
    // 0xcc skip rom
    ds18b20_write_byte(0xCC);
    // 0xbe
    ds18b20_write_byte(0xBE);

    temp_l = ds18b20_read_byte();
    temp_h = ds18b20_read_byte();

    temp = (temp_h << 8) | temp_l;

     // 校验数据
}


static ssize_t ds18b20_read(struct file *file, char __user *buf, size_t size, loff_t *ppos) {

    unsigned long flags;
    // 在读取操作的时候要关闭中断

    local_irq_save(flags);


    // 2.


}

static int ds18b20_open(struct inode *inode, struct file *file){

}

static unsigned int ds18b20_poll(struct file *fp, poll_table * wait)
{
    poll_wait(fp, &gpio_wait, wait);
    // TODO: 这里要判断是否有温度值, 后面在改
    return is_key_buf_empty() ? 0 : POLLIN | POLLRDNORM;
}

// 02
static struct file_operations ds18b20_fops = {
    .owner = THIS_MODULE,
    .read = ds18b20_read,
    .open = ds18b20_open,
    .poll = ds18b20_poll,
};

static int ds18b20_probe(struct platform_device *pdev)
{
    int error = 0;
    int i;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	printk("%s %s line %d\n", __FILE__, __FUNCTION__, __LINE__);


	// 从设备树获取引脚信息
	ds18b20_gpio = gpiod_get(&pdev->DEV, NULL, GPIOD_OUT_LOW);

	major = register_chrdev(0, "ds18b20", &ds18b20_fops);

	if (major < 0) {
        printk("Failed to register char device\n");
        return major;
	}

    ds18b20_drv_class = class_create(THIS_MODULE, "ds18b20");
    if (IS_ERR(ds18b20_drv_class)) {
		unregister_chrdev(major, "ds18b20");
		return PTR_ERR(ds18b20_drv_class);
	}

    device_create(ds18b20_drv_class, NULL, MKDEV(major, 0), NULL, "ds18b20");
    return error;
}

static int ds18b20_remove(struct platform_device *pdev){

    device_destroy(ds18b20_drv_class, MKDEV(major, 0));
	class_destroy(ds18b20_drv_class);
	unregister_chrdev(major, "ds18b20");
}

static const struct of_device_id ds18b20[] = {
    { .compatible = "100ask,ds18b20"},
};

static struct platform_driver ds18b20_driver = {
    .probe = ds18b20_probe,
    .remove = ds18b20_remove,
    .driver = {
        .name = "ds18b20",
        .of_match_table = ds18b20_of_match,
    }
}

static int __init ds18b20_drv_init(void)  {

    return platform_driver_register(&ds18b20_driver);
}
static void __exit ds18b20_drv_exit(void) {
    platform_driver_unregister(&ds18b20_driver);
}

module_init(ds18b20_drv_init);
module_exit(ds18b20_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIO IRQ Driver");
