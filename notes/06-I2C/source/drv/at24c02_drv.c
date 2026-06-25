#include "linux/cdev.h"
#include "linux/device.h"
#include "linux/export.h"
#include "linux/fs.h"
#include "linux/gfp.h"
#include "linux/i2c.h"
#include "linux/kernel.h"
#include "linux/mod_devicetable.h"
#include "linux/mutex.h"
#include "linux/printk.h"
#include "linux/types.h"
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

struct at24c02_dev{

    struct i2c_client *client;

    struct mutex lock;

    dev_t devt;

    struct cdev cdev;

    struct class *class;

    struct device *device;

};


#define AT24C02_SIZE       256
#define AT24C02_PAGE_SIZE  8



static int at24c02_i2c_read(struct at24c02_dev *at24,
			    u8 addr, u8 *buf, int len)
{
	struct i2c_msg msgs[2];
	int ret;

	// 发起一个写地址操作，
	msgs[0].addr  = at24->client->addr;
	msgs[0].flags = 0;
	msgs[0].len   = 1;
	msgs[0].buf   = &addr;

	// 发读取
	msgs[1].addr  = at24->client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len   = len;
	msgs[1].buf   = buf;

	ret = i2c_transfer(at24->client->adapter, msgs, 2);
	if (ret != 2)
		return ret < 0 ? ret : -EIO;

	return len;
}


static int at24c02_i2c_write_page(struct at24c02_dev *at24,
				  u8 addr, const u8 *buf, int len)
{
	u8 tmp[AT24C02_PAGE_SIZE + 1];
	struct i2c_msg msg;
	int ret;

	tmp[0] = addr;
	memcpy(&tmp[1], buf, len);

	msg.addr  = at24->client->addr;
	msg.flags = 0;
	msg.len   = len + 1;
	msg.buf   = tmp;

	ret = i2c_transfer(at24->client->adapter, &msg, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	msleep(10);
	return len;
}

static ssize_t at24c02_read(struct file *file, char __user *buf,
			    size_t count, loff_t *offset) {
	struct at24c02_dev *at24 = file->private_data;
	unsigned char *kernel_buf;
	size_t len;
	int ret;

	if (*offset >= AT24C02_SIZE) {
	    return 0;
	}

	// 做个判断防止 读取的字节数 超过了容量
	len = min_t(size_t, count, AT24C02_SIZE - *offset);
    if (!len)
    	return 0;

    kernel_buf = kmalloc(len, GFP_KERNEL);
    if (!kernel_buf)
    	return -EIO;

	mutex_lock(&at24->lock);
	ret = at24c02_i2c_read(at24, *offset, kernel_buf, len);
	mutex_unlock(&at24->lock);


	if (ret < 0) {
			kfree(kernel_buf);
			return ret;
	}
    if (copy_to_user(buf, kernel_buf, len)) {
    	kfree(kernel_buf);
    	return -EFAULT;
    }

    kfree(kernel_buf);
    *offset += len;


	return len;
}

static ssize_t at24c02_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *offset) {
	struct at24c02_dev *at24 = file->private_data;
	int ret;
	unsigned char kernel_buf[AT24C02_PAGE_SIZE];
	size_t written_size = 0;
	size_t len = 0, page_left = 0;

	// 检测地址是否越界
	if (*offset >= AT24C02_SIZE) {
	    return -EIO;
	}

	// 限制下实际读取的大小
	count = min_t(size_t, count, AT24C02_SIZE - *offset);

	mutex_lock(&at24->lock);

	while (written_size < count) {
	// 不能跨页写，所以这里处理每一页
	    page_left = AT24C02_PAGE_SIZE - (*offset % AT24C02_PAGE_SIZE);
		len = min_t(size_t, count - written_size, page_left);

		if (copy_from_user(kernel_buf, buf + written_size, len)) {
            ret = -1;
            return ret;
		}
// 写数据
		ret = at24c02_i2c_write_page(at24, *offset, kernel_buf, len);
		if (ret < 0) {
		    return ret;
		}
// 更新，移动 offset 和记录写入成功大小
		*offset += len;
		written_size += len;

	}

	mutex_unlock(&at24->lock);

	return written_size;
}
static int at24c02_open(struct inode *innode, struct file *file) {

   	struct at24c02_dev *at24;

	at24 = container_of(innode->i_cdev, struct at24c02_dev, cdev);
	file->private_data = at24;

    return 0;
}

// lseek,
static loff_t at24c02_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t new_pos;

	switch (whence) {
	case SEEK_SET:
		new_pos = offset;
		break;

	case SEEK_CUR:
		new_pos = file->f_pos + offset;
		break;

	case SEEK_END:
		new_pos = AT24C02_SIZE + offset;
		break;

	default:
		return -EINVAL;
	}

	if (new_pos < 0 || new_pos > AT24C02_SIZE)
		return -EINVAL;

	file->f_pos = new_pos;
	return new_pos;
}

// file_operations结构体
static struct file_operations at24c02_fops = {
	.owner	 = THIS_MODULE,
	.read    = at24c02_read,
	.write   = at24c02_write,
	.open    = at24c02_open,
	.llseek  = at24c02_llseek,
	// .poll    = i2c_drv_poll,
	// .fasync  = i2c_drv_fasync,
};
// 后面两个就懒得写了

int at24c02_probe(struct i2c_client *client) {

    struct at24c02_dev *dev_at24c02;
    int ret;

    printk("at24c02 probe, addr = 0x%x\n", client->addr);
    //1. 先给结构体分配空间

    dev_at24c02 = kzalloc(sizeof(*dev_at24c02), GFP_KERNEL);
    if (!dev_at24c02) {
        return -EIO;
    }
    dev_at24c02->client = client;
    mutex_init(&dev_at24c02->lock);

    // 试试这个看行不行
    //2. 把结构体数据挂到 client 的私有空间里面
    i2c_set_clientdata(client, dev_at24c02);


    //3. 注册字符设备
    // 新用一种方式试试，我看好多 文章和我们的业务驱动都用的这种
    ret = alloc_chrdev_region(&dev_at24c02->devt, 0, 1, "at24c02");
    if (ret) {
        printk("alloc_chardev error.\n");
        return -EIO;
    }

    cdev_init(&dev_at24c02->cdev, &at24c02_fops);
    dev_at24c02->cdev.owner = THIS_MODULE;

    ret = cdev_add(&dev_at24c02->cdev, dev_at24c02->devt, 1);
    if (ret) {
        printk("cdev_add erro ...\n");
        return -EIO;
    }

    // 释放的时候用
    dev_at24c02->class = class_create(THIS_MODULE, "at24c02_class");

    dev_at24c02->device = device_create(dev_at24c02->class, NULL, dev_at24c02->devt, NULL, "at24c02");

    dev_info(&client->dev, "at24c02 driver ready.... \n");
    return 0;
}

int at24c02_remove(struct i2c_client *client) {

    // 从私有数据区中获取结构体，
    struct at24c02_dev *dev_at24c02 = i2c_get_clientdata(client);

    // 释放资源
    device_destroy(dev_at24c02->class, dev_at24c02->devt);
	class_destroy(dev_at24c02->class);
	cdev_del(&dev_at24c02->cdev);
	unregister_chrdev_region(dev_at24c02->devt, 1);

	kfree(dev_at24c02);

	dev_info(&client->dev, "at24c02 removed\n");
}



static const struct of_device_id at24c02_of_match[] = {
    {.compatible = "100ask,at24c02"},
    {}
};

static struct i2c_driver at24c02_driver = {
    .driver = {
        .name = "100ask_at24c02",
        .owner = THIS_MODULE,
        .of_match_table = at24c02_of_match,
    },
    .probe_new = at24c02_probe, // 试试这个心的方法
    .remove = at24c02_remove,
};



// 也可以用下面这个宏来注册，本质上是一样的
//module_i2c_driver(&at24c02_driver);

static int __init i2c_drv_init(void) {
    return i2c_add_driver(&at24c02_driver);
}

static void __exit i2c_drv_exit(void) {
    return i2c_del_driver(&at24c02_driver);
}

module_init(i2c_drv_init);
module_exit(i2c_drv_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XuHao");
MODULE_DESCRIPTION("input driver");
