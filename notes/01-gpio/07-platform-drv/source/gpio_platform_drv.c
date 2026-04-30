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

#define DEV_NAME "gpio_key"


// 导出 chip 层的接口
extern int chip_led_set(int val);
extern int chip_key_read(void);

static int major;
static struct class *gpio_class;
static struct device *gpio_dev;

static ssize_t drv_key_read(struct file *file, char __user *buf, size_t size, loff_t *ppos) {
    char ret;

    ret = chip_key_read();

    copy_to_user(buf, &ret, 1);

    return 1;
}


static ssize_t drv_led_set(struct file *file, const char __user *buf, size_t size, loff_t *ppos) {
    char ret;
    copy_from_user(&ret, buf, 1);

    chip_led_set(ret);

    return 1;
}

static struct file_operations gpio_platform_fops = {
    .owner = THIS_MODULE,
    .read = drv_key_read,
    .write = drv_led_set,
};


static int __init drv_init(void) {
    major = register_chrdev(0, DEV_NAME, &gpio_platform_fops);
    gpio_class = class_create(THIS_MODULE, DEV_NAME);


    gpio_dev =  device_create(gpio_class,
                      NULL,
                      MKDEV(major,0),
                      NULL,
                      DEV_NAME);
    return 0;
}

static void __exit drv_exit(void) {
    device_destroy(gpio_class, MKDEV(major,0));
    class_destroy(gpio_class);
    unregister_chrdev(major, DEV_NAME);
}

module_init(drv_init);
module_exit(drv_exit);

MODULE_LICENSE("GPL");
