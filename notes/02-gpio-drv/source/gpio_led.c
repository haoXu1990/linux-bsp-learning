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

//


// 1. 定义主设备号
static int major = 0;
static struct class *gpio_led_class;

struct gpio_desc{
	int gpio;
	int irq;
    char *name;
} ;

static struct gpio_desc gpios[1] = {
    {117, 0 , "gpio_led_1"}
};
// {117, 0 , "gpio_led_1"}, 由于PD22 引脚已经有其它模块在使用，这里只做一个引脚的控制
static ssize_t gpio_led_open(struct inode *inode, struct file *file)
{
    // GPIO 配置
    printk("gpio_led_open\n");
    return 0;
}

static ssize_t gpio_led_release(struct inode *inode, struct file *file)
{
    printk("gpio_led_release\n");
    return 0;
}

static ssize_t gpio_led_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
    // 读取当前 GPIO 状态， 高低电平
    char tmp_buf[2];
    int err;

    if (size != 2) {
        return -EINVAL;
    }

    // 读取用户空间输入数据
    // 用户输入2个字节数据，第0个字节是需要读取哪个LED，第1个字节由驱动返回gpio电平数据，高还是低
    err = copy_from_user(tmp_buf, buf, 1);


    // 读取 gpio 电平
    tmp_buf[1] = gpio_get_value(gpios[tmp_buf[0]].gpio);

    // 给用户返回 gpio 电平
    err = copy_to_user(buf, tmp_buf, 2);



    printk("gpio_led_read\n");
    return 1;
}

static ssize_t gpio_led_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos)
{
    // 写 GPIO

    unsigned char tmp_buf[2];
   int err;
   err = copy_from_user(tmp_buf, buf, sizeof(tmp_buf));
   if (err < 0) {
       printk("copy_from_user failed\n");
       return err;
   }
    gpio_set_value(gpios[tmp_buf[0]].gpio, tmp_buf[1]);

    printk("gpio_led_write\n");
    return 2;
}

// 2. 创建结构体
static struct file_operations gpio_led_fops = {
    .owner = THIS_MODULE,
    .open = gpio_led_open,
    .release = gpio_led_release,
    .read = gpio_led_read,
    .write = gpio_led_write,
};

static int __init gpio_drv_init(void)
{
    int ret = 0;
    int err = 0;
    int i;
    int count = sizeof(gpios) / sizeof(gpios[0]);
    //  初始化 GPIO， 这一步感觉可以放在 open 方法中, 并且用户在 close 的时候 驱动模块可以释放资源
    for (i = 0; i < count; i++) {
        err = gpio_request(gpios[i].gpio, gpios[i].name);
        if (err < 0) {
            printk("gpio_request failed\n");
            return err;
        }
        err = gpio_direction_output(gpios[i].gpio, 0);
        if (err < 0) {
            printk("gpio_direction_output failed\n");
            return err;
        }
    }

    // 3. 注册字符设备驱动
    major = register_chrdev(0, "gpio_led", &gpio_led_fops);
    if (major < 0) {
        printk("register_chrdev failed\n");
        return major;
    }

    // 4. 创建类
    gpio_led_class = class_create(THIS_MODULE, "gpio_led_class");
    if (IS_ERR(gpio_led_class)) {
        unregister_chrdev(major, "gpio_led");
        printk("class_create failed\n");
        return PTR_ERR(gpio_led_class);
    }

    // 5. 创建设备节点
    device_create(gpio_led_class, NULL, MKDEV(major, 0), NULL, "gpio_led");

    printk("gpio_drv_init success\n");
    return 0;
}

static void __exit gpio_drv_exit(void)
{
    int i;
    int count = sizeof(gpios) / sizeof(gpios[0]);
    // 6. 删除设备节点
    device_destroy(gpio_led_class, MKDEV(major, 0));

    // 7. 删除类
    class_destroy(gpio_led_class);

    // 8. 注销字符设备驱动
    unregister_chrdev(major, "gpio_led");

    for (i = 0; i < count; i++)
	{
		gpio_free(gpios[i].gpio);
	}
    printk("gpio_drv_exit success\n");
}

module_init(gpio_drv_init);
module_exit(gpio_drv_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("GPIO LED Driver");
MODULE_VERSION("1.0");
