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

// 01
static int major;

static struct class *ds18b20_drv_class;

static DECLARE_WAIT_QUEUE_HEAD(gpio_wait);

void delay_ktime_us(int us) {

}

static void write_bit(int bit) {

}
static void read_bit() {

}


static ssize_t ds18b20_read(struct file *file, char __user *buf, size_t size, loff_t *ppos) {


}

static int ds18b20_open(struct inode *inode, struct file *file){

}

static unsigned int ds18b20_poll(struct file *fp, poll_table * wait)
{
    poll_wait(fp, &gpio_wait, wait);
    return is_key_buf_empty() ? 0 : POLLIN | POLLRDNORM;
}

// 02
static struct file_operations ds18b20_fops = {
    .owner = THIS_MODULE,
    .read = ds18b20_read,
    .open = ds18b20_open,
    .poll = ds18b20_poll,
}

static int gpio_drv_probe(struct platform_device *pdev)
{
    int error = 0;
    int i;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	printk("%s %s line %d\n", __FILE__, __FUNCTION__, __LINE__);


	major = register_chrdev(0, "ds18b20", &ds18b20_fops);

    ds18b20_drv_class = class_create(THIS_MODULE, "ds18b20");
    if (IS_ERR(ds18b20_drv_class)) {

		unregister_chrdev(major, "ds18b20");
		return PTR_ERR(ds18b20_drv_class);
	}

    device_create(ds18b20_drv_class, NULL, MKDEV(major, 0), NULL, "ds18b20")
    return error;
}

static int ds18b20_remove(struct platform_device *pdev){

    device_destroy(ds18b20_drv_class, MKDEV(major, 0));
	class_destroy(ds18b20_drv_class);
	unregister_chrdev(major, "ds18b20");
}

static const struct of_device_id ds18b20[] = {
    { .compatible = "100ask,ds18b20"}
}

static struct platform_driver ds18b20_driver = {
    .probe = ds18b20_probe,
    .remove = ds18b20_remove,
    .driver = {
        .name = "ds18b20",
        .of_match_table = ds18b20,
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
