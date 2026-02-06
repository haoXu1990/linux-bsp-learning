#include <linux/module.h>
#include <linux/poll.h>

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
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/fcntl.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>


// 1. 定义主设备号
static int major = 0;
static struct class *dht11_class;

struct gpio_desc{
	int gpio;
	int irq;
    char *name;
};

static struct gpio_desc gpios = { 139, 0, "dht11" };


///
// 等待指定GPIO 在指定的 时间范围内，电平变为 指定的 level;
static int wait_level(int gpio, int level, unsigned int timeout_us) {
    unsigned int waited = 0;

    while (gpio_get_value(gpio) != level) {
        if (waited++ > timeout_us)
            return -ETIMEDOUT;
        udelay(1);
    }
    return 0;
}

static long wait_level_with_time(int gpio, int level, unsigned int timeout_us)
{
    ktime_t start = ktime_get();
    ktime_t now;

    while (gpio_get_value(gpio) != level) {
        now = ktime_get();
        if (ktime_to_us(ktime_sub(now, start)) > timeout_us)
            return -ETIMEDOUT;

        cpu_relax();
        udelay(1);
    }

    return ktime_to_us(ktime_sub(ktime_get(), start));
}


static int dht11_read_bit(int gpio) {
    ktime_t t_start, t_end;
    long high_us;

    // 1. wait 50us low, 注意这里的目的只是判断电平变化是否到来
    if (wait_level(gpio, 0, 200)) return -1;

    // 2. wait high,  注意这里的目的只是判断电平变化是否到来
    if (wait_level(gpio, 1, 200)) return -1;

    // record start time;
    t_start = ktime_get();

    // wait gpio to  low
    if (wait_level(gpio, 0, 300)) return -1;

    t_end = ktime_get();

    high_us = ktime_to_us(ktime_sub(t_end, t_start));

    return high_us > 50 ? 1 : 0;

}

// 测算宽度
static long measure_width(int gpio, int level, unsigned long timeout_us) {

    int ret;
    u64 t0,t1;

    ret = wait_level(gpio, level, timeout_us * 0.5);
    if (ret)
        return ret;

    t0 = ktime_get_ns();
    while (gpio_get_value(gpio) == level) {
        t1 = ktime_get_ns();
        if ((t1 - t0) > timeout_us * 1000ULL)
            return -ETIMEDOUT;
    }

    return (long)((t1 - t0));
}

// 4. 定义设备操作函数
static int dht11_open(struct inode *inode, struct file *file) {

    int err;
    // 初始化 gpio
    err = gpio_request(gpios.gpio, gpios.name);

    if (err < 0) {
        printk("gpio_request failed\n");
        return err;
    }

    gpios.irq = gpio_to_irq(gpios.gpio);

    // 设置GPIO 初始状态为 output， 高电平
    gpio_direction_output(gpios.gpio, 1);

    printk("gpio %d dht11_open\n", gpios.gpio);
    return 0;
}

static int dht11_release(struct inode *inode, struct file *file) {

    // 释放
    gpio_free(gpios.gpio);

    return 0;
}

static ssize_t dht11_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {

    int err;
    int i;
    // 40bit数据,也就是5个字节
    u8 data[5] = {0};
    u8 crc;
    if (count != 2)
        return -EINVAL;

    printk("dht11_read called\n");
    // 1. send start signal
    // 1.1 set output direction and set low level
    gpio_direction_output(gpios.gpio, 0);
    // 1.2 set low level
    // gpio_set_value(gpios.gpio, 0);
    // 1.3 wait 20ms
    msleep(20);
    gpio_set_value(gpios.gpio, 1);
    // 1.4 wait 20~40us, hight
    udelay(30);
    // 1.4 set input direction
    gpio_direction_input(gpios.gpio);

    // set a delay of 40us, wait for dht11 response
    //udelay(40);
    // 2. wait dht11 response
    // 2.1 check low level
    // !!!!! 注意这里的目的不是检测电平的持续时间,而是在单位时间内等待电平变化,只要有变化就进行下一步
    long low_us_l = wait_level_with_time(gpios.gpio, 0, 200);
    //printk("gpio %d wait %d us low_us received\n", gpios.gpio, low_us_l);
    // gpio 139 wait 1 us low_us received

    if (low_us_l < 0) return -EIO;

    long high_us_l = wait_level_with_time(gpios.gpio, 1, 200);
    if (high_us_l < 0) return -EIO;
    // gpio 139 wait 0 us high_us_l received
   // printk("gpio %d wait %d us high_us_l received\n", gpios.gpio, high_us_l);

   // 由上面的打印日志可以看出,在等待响应数据的 低电平的时候,当我们去检测的时候等了 1us 就收到了拉低信号
   // 然后在等待高电平的时候几乎进去方法就获取到了高电平, 可能是时序太快,我们打印添加了打印语句,造成了CPU调度;

   //  感觉这里需要对齐一下第一个bit 数据?
   //  是否可以先 read一个bit的数据扔掉的方式来对齐,等我写完总结文档在来考虑这个事情
   //  当前程序还有一个问题,应用程序第一次读的时候会失败 ,这个问题应该好解决,先写文档后在来处理;
    // read 40bit ;
    for (i = 0; i < 40; i++) {
        // read bit
        int bit = dht11_read_bit(gpios.gpio);
        if (bit < 0) {
            printk("read a bit data failed.\n");
            return -EIO;
        }
        // set bit data
        data[i / 8] <<= 1;
        data[i / 8] |= bit;
    }
    printk("dht11 data read\n");
    printk("dht11 data: %02x %02x %02x %02x %02x\n", data[0], data[1], data[2], data[3], data[4]);
    // check checksum crc
    crc = data[0] + data[1] + data[2] + data[3];
    if (crc != data[4]) return -EIO;

    printk("dht11 data verified\n");
    // return data;
    u8 outdata[2] = {data[0], data[2]};
    if (copy_to_user(buf, outdata, sizeof(outdata))) return -EFAULT;

    gpio_direction_output(gpios.gpio, 1);

    return sizeof(outdata);
}


// 5. 定义设备操作结构体
static const struct file_operations dht11_fops = {
    .owner = THIS_MODULE,
    .open = dht11_open,
    .release = dht11_release,
    .read = dht11_read
};


// 10. 定义设备驱动注册函数
static int __init  dht11_init(void)
{
    int ret;

    // 注册文件设备驱动
   major = register_chrdev(0, "dht11", &dht11_fops);
   if (major < 0) {
    printk("register_chrdev failed\n");
    return major;
   }

   // 创建 class
   dht11_class = class_create(THIS_MODULE, "dht11_class");
   if (IS_ERR(dht11_class)){
       printk("class_create failed \n");
       unregister_chrdev(major, "dht11");
       return PTR_ERR(dht11_class);
   }

   // 创建device
   device_create(dht11_class, NULL, MKDEV(major, 0), NULL, "dht11");

    return 0;
}

// 11. 定义设备驱动注销函数
static void dht11_exit(void)
{
    // 6. 删除设备节点
    device_destroy(dht11_class, MKDEV(major, 0));

    // 7. 删除类
    class_destroy(dht11_class);

    // 8. 注销字符设备驱动
    unregister_chrdev(major, "dht11");
}

module_init(dht11_init);
module_exit(dht11_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("DHT11 Driver");
