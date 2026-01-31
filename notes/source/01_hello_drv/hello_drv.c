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

// 1.  确认主设备号
static int major = 0;
static char kernel_buf[1024];
static struct class *hello_class;

#define MIN(a, b) (a < b ? a : b)

// 3. 实现对应的方法
static ssize_t hello_drv_open(struct inode *inode, struct file *file) {
    printk("100ask, hello_drv_open\n");
    return 0;
}

static ssize_t hello_drv_release(struct inode *inode, struct file *file) {
    // 这里有点不明白为什么内核不直接用 close 命名？
    printk("100ask, hello_drv_release\n");
    return 0;
}

static ssize_t hello_drv_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    int len = strlen(kernel_buf);
    // 把数据从内核空间拷贝到用户空间
    // 这里有个问题，如果数据量大，用户要分次获取数据，那么已经获取的数据还是存放在内核空间？
    //  最后一个参数需要了解下
    int size = MIN(len, count);
    int ret = copy_to_user(buf, kernel_buf, size);
    if (ret == 0) {
        printk("100ask, hell_drv, read %d bytes\n", size);
        *ppos += size;
        return size;
    }
    return -EFAULT;
}

static ssize_t hello_drv_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    // 这种驱动在实际业务情况应该还要结合外设吧？ 例如 uart/spi
    int ret = copy_from_user(kernel_buf , buf, MIN(sizeof(kernel_buf), count));
    if (ret == 0) {
        printk("100ask, hell_drv, write %d bytes\n", MIN(sizeof(kernel_buf), count));
        return MIN(sizeof(kernel_buf), count);
    }
    return -EFAULT;
}

// 2. 创建hello驱动的文件操作结构体， 这里就是定义上层应用使用 open/write 等方法。
static struct file_operations hello_drv = {
    .owner = THIS_MODULE,
    .open = hello_drv_open,
    .release = hello_drv_release,
    .read = hello_drv_read,
    .write = hello_drv_write,
};

// 4. 注册驱动
static int __init hello_init(void) {
    printk("100ask, hello_drv_init \n");
    int err;
    // 注册驱动， 成功后会生成 /dev/hello
    major = register_chrdev(0, "hello", &hello_drv);

    // ? 这个是干什么的？
    hello_class = class_create(THIS_MODULE, "hello_class");
    err = PTR_ERR(hello_class);
    if (IS_ERR(hello_class)) {
        unregister_chrdev(major, "hello");
        return err;
    }

    //? 不懂
    device_create(hello_class, NULL, MKDEV(major, 0), NULL, "hello");

    return 0;
}

static void __exit hello_exit(void)
{
	printk(" 100ask, unregister_chrdev hello.\n");
	device_destroy(hello_class, MKDEV(major, 0));
	class_destroy(hello_class);
	unregister_chrdev(major, "hello");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
