#include "asm-generic/gpio.h"
#include "asm-generic/int-ll64.h"
#include "asm/gpio.h"
#include "asm/stat.h"
#include "linux/irqreturn.h"
#include "linux/jiffies.h"
#include "linux/key.h"
#include "linux/printk.h"
#include "linux/spinlock.h"
#include "linux/wait.h"
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

static struct class *gpio_irq_drv_class;

struct gpio_desc {
    int gpio;
    int irq;
    char *name;
    unsigned long last_jiffies; // 上一次中断的时间
    int last_level; // 上一次中断的电平
};

static struct gpio_desc gpios[] = {
    {103, 0, "gpio_key1", 0 ,0},
    {117, 0, "gpio_led1", 0, 0}
};


typedef enum  {
    KEY_UP = 0,
    KEY_DOWN = 1
} key_state_t;

struct key_event {
    int gpio;
    key_state_t state; // 0, 抬起， 1 按下
};


u8 led_status = 0;

// 环形缓冲区
// 环形缓冲区其实就是3个重要数据
// 1. buffer
// 2. head 指针
// 3. tail 指针

#define RING_BUFFER_SIZE 64
struct ev_queue {
    struct key_event buffer[RING_BUFFER_SIZE];
    u32 head;
    u32 tail;

    // 数据入栈/出栈肯定是需要锁的，我对内核锁不是特别了解，简单百度了搜索选择了下面这个
    // ！！我记得我们业务中有人在 中断中用了可重入锁，导致死锁而出现重大安全事故
    spinlock_t lock;
    wait_queue_head_t wq;

    u32 drop_cnt; // 丢弃计数， 实际业务中调试这个很有用
};

static struct ev_queue ring_buffer;

static u8 evq_next(u32 i) {
    return (i + 1) & (RING_BUFFER_SIZE -1);
}

// 入队
void ev_push_irq(struct ev_queue *q, const struct key_event *ev) {
    unsigned long flags;

    spin_lock_irqsave(&q->lock, flags);

    if (evq_next(q->head) == q->tail) {
        // 记录丢弃数量
        q->drop_cnt++;

        spin_unlock_irqrestore(&q->lock, flags);
        return;
    }

    q->buffer[q->head] = *ev;
    q->head = evq_next(q->head);

    spin_unlock_irqrestore(&q->lock, flags);

    wake_up_interruptible(&q->wq);
}

//  出队
int evq_pop(struct ev_queue *q, struct key_event *out) {
    unsigned long flags;

    spin_lock_irqsave(&q->lock, flags);

    if (q->head == q->tail) {
        printk("ev_queue is empty.\n");
        spin_unlock_irqrestore(&q->lock, flags);
        return -ENOENT;
    }
    *out = q->buffer[q->tail];
    q->tail = evq_next(q->tail);
    spin_unlock_irqrestore(&q->lock, flags);
    return 0;
}


// 设置用户LED状态
// 0 亮， 1 灭
static void setUserLEDStatue(u8 status) {
    gpio_set_value(gpios[1].gpio, status);
}



static irqreturn_t gpio_irq_drv_irq_handler(int irq, void *dev_id) {
    struct gpio_desc *gpio = dev_id;

    // 获取系统节拍， 这个比延时好用
    unsigned long current_jiffies = jiffies;

    // 去抖动 20ms
    //  msecs_to_jiffies(20) 把 20ms 转换成系统 节拍数；
    //  gpio->last_jiffies + msecs_to_jiffies(20) 就是下一次中断时间，如果小于那么就忽略了
    if (time_before(current_jiffies, gpio->last_jiffies + msecs_to_jiffies(20))) {
        return IRQ_HANDLED;
    }

    // 读取电平
    int value = gpio_get_value(gpio->gpio);

    // 如果当前电平与上一次电平相同证明是长按， 这里也忽略掉
    if (value == gpio->last_level) {
        return IRQ_HANDLED;
    }

    // 更新 gpio 时间也电平数据
    gpio->last_jiffies = current_jiffies;
    gpio->last_level = value;

    // 存储到缓冲区
    struct key_event ev = {
        .gpio = gpio->gpio,
        .state = value ? KEY_DOWN : KEY_UP
    };

    // 根据按键事件设置 LED 状态
    setUserLEDStatue(value ? 0 : 1);

    ev_push_irq(&ring_buffer, &ev);

    // 唤醒read 中的休眠，告诉 app 有事件产生

    printk("gpio_irq_drv_irq_handler\n");
    return IRQ_HANDLED;
}


static int gpio_irq_drv_open(struct inode *inode, struct file *file) {
    printk("gpio_irq_drv_open\n");
    u32  i;
    int ret;
    // set gpio
    for (i = 0; i <ARRAY_SIZE(gpios); i++) {
        ret = gpio_request(gpios[i].gpio, gpios[i].name);
        if (ret < 0) {
            printk("gpio_request failed\n");
            return ret;
        }
        gpios[i].irq = gpio_to_irq(gpios[i].gpio);

        if (gpios[i].irq < 0) {
            printk("gpio_to_irq failed\n");
            gpio_free(gpios[i].gpio);
            return -EINVAL;
        }
    }

    // set user_led gpio output
    gpio_direction_output(gpios[1].gpio, 0);
    // 设置 user_key gpio 为输入
    gpio_direction_input(gpios[0].gpio);

    // 设置 中断
    ret = request_irq(gpios[0].irq, gpio_irq_drv_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "gpio_irq_drv", &gpios[0]);
    if (ret < 0) {
        printk("user key gpio request_irq failed\n");
        gpio_free(gpios[0].gpio);
        return -EINVAL;
    }
    return 0;
}

static int gpio_irq_drv_release(struct inode *inode, struct file *file) {
    printk("gpio_irq_drv_release\n");
    int i;
     for (i = 0; i <ARRAY_SIZE(gpios); i++) {
         gpio_free(gpios[i].gpio);
     }

     // release irq
     free_irq(gpios[0].irq, &gpios[0]);
    return 0;
}

static ssize_t gpio_irq_drv_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    printk("gpio_irq_drv_read\n");

    struct key_event ev;
    u8 state;
    int ret;

    if (count != 1) {
        return -EINVAL;
    }

    ret = wait_event_interruptible(ring_buffer.wq, ring_buffer.head != ring_buffer.tail);
    if (ret) {
        return ret;
    }

    // 这里感觉有问题，如果是线程竞争， 驱动需要考虑吗？
    ret = evq_pop(&ring_buffer, &ev);
    if (ret) {
        return -EAGAIN;
    }

    state = (u8)ev.state;
    if (copy_to_user(buf, &state, 1)) {
        return -EFAULT;
    }

    return 1;
}

static unsigned int gpio_irq_drv_poll(struct file *file, poll_table *wait) {
    unsigned int mask = 0;

    poll_wait(file, &ring_buffer.wq, wait);

    if (ring_buffer.head != ring_buffer.tail) {
        mask |= POLLIN | POLLRDNORM;
    }

    return mask;
}

// 2. 定义字符设备
static struct file_operations  gpio_irq_drv_fops = {
    .owner = THIS_MODULE,
    .open = gpio_irq_drv_open,
    .release = gpio_irq_drv_release,
    .read = gpio_irq_drv_read,
    .poll = gpio_irq_drv_poll,
};


static int __init gpio_irq_drv_init(void)  {

    // 初始化 环形缓冲区

    ring_buffer.head = 0;
    ring_buffer.tail = 0;
    ring_buffer.drop_cnt = 0;
    spin_lock_init(&ring_buffer.lock);
    init_waitqueue_head(&ring_buffer.wq);

    major = register_chrdev(0, "gpio_irq", &gpio_irq_drv_fops);
    if (major < 0) {
        printk("register_chrdev failed\n");
        return major;
    }

    // 创建 class
    gpio_irq_drv_class = class_create(THIS_MODULE, "gpio_irq_drv");
    if (IS_ERR(gpio_irq_drv_class)) {
        unregister_chrdev(major, "gpio_irq");
        printk("class_create failed\n");
        return PTR_ERR(gpio_irq_drv_class);
    }

    // 创建 device
    device_create(gpio_irq_drv_class, NULL, MKDEV(major, 0), NULL, "gpio_irq_drv");

    return 0;
}

static void __exit gpio_irq_drv_exit(void) {
    device_destroy(gpio_irq_drv_class, MKDEV(major, 0));
    class_destroy(gpio_irq_drv_class);
    unregister_chrdev(major, "gpio_irq");
}

module_init(gpio_irq_drv_init);
module_exit(gpio_irq_drv_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("XuHao");
MODULE_DESCRIPTION("GPIO IRQ Driver");
