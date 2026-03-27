
#include "linux/err.h"
#include "linux/printk.h"
#include "linux/uaccess.h"
#include <linux/module.h>

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/major.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/tty.h>
#include <linux/wait.h>

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
    unsigned long flags;
    local_irq_save(flags);
  // 写时序必须最少支持 60 us
  //  ds18b20 在15 us 后采样
  if (bit) {
    // 写1 时序
    // 拉低总线 15us
    gpiod_direction_output(ds18b20_gpio, 0);
    delay_ktime_us(5);
    // 设置input 后由上拉电阻拉高GPIO
    gpiod_direction_input(ds18b20_gpio);
    local_irq_restore(flags);
    delay_ktime_us(55);
  } else {
    // 写 0 时序， 拉低至少 60us, 最多120us
    //  ds18b20 在15 us 后采样
    gpiod_direction_output(ds18b20_gpio, 0);
    delay_ktime_us(60);
    gpiod_direction_input(ds18b20_gpio);
    local_irq_restore(flags);
    // 让从机读， 感觉可以不要这个 5us
    delay_ktime_us(5);
  }
}

static void ds18b20_write_byte(u8 value) {
  int i;
  for (i = 0; i < 8; i++) {
    ds18b20_write_bit(value & 0x01);
    value >>= 1;
  }
}

static int ds18b20_read_bit(void) {
  int value;
  unsigned long flags;
  // 拉低 1us最低 -> 释放 -> 15us 延时 -> 读数据
  local_irq_save(flags);
  gpiod_direction_output(ds18b20_gpio, 0);
  delay_ktime_us(2);
  gpiod_direction_input(ds18b20_gpio);
  delay_ktime_us(13);
  value = gpiod_get_value(ds18b20_gpio);
  local_irq_restore(flags);
  delay_ktime_us(45);
  return value;
}

static u8 ds18b20_read_byte(void) {
  int i;
  u8 data = 0;

  for (i = 0; i < 8; i++) {
    if (ds18b20_read_bit()) {
        data |= (1 << i);
    }
  }
  return data;
}

// 发送复位信号
static int ds18b20_reset(void) {
  // DS18B20 手册初始化时序 图13
  // 主机拉低最少 480us 然后等待 15-60 us，从机拉低总线 60 - 240 us;
  int value;
  unsigned long flags;

  // 关中断
  local_irq_save(flags);

  // 1. 拉低 480 us
  gpiod_direction_output(ds18b20_gpio, 0);

  // 1.1 保持最低 480us
  delay_ktime_us(500);

  // 2. 释放总线
  gpiod_direction_input(ds18b20_gpio);
  // 2.1 等待 60us
  delay_ktime_us(70);

  value = gpiod_get_value(ds18b20_gpio);

  // 恢复中断
  local_irq_restore(flags);

  printk("ds18b20 reset ack %d \n", value);
  // 如果等待 70 us 后为0，代表有从机响应
  return value == 0 ? 0 : -1;
}

static int ds18b20_read_temp(void) {
  u8 tempL, tempH;

  unsigned int integer;
  unsigned char decimal1,decimal2,decimal;

  // 1. reset
  if (ds18b20_reset()) {
      printk("ds18b20_reset failed. \n");
      return -1;
  }

  //2 0xcc  skip rom, ROM 操作指令
  ds18b20_write_byte(0xCC);

  //3 功能指令, 温度转换指令，ds18b20 温度转换成功后会把数据存入 暂存器
  ds18b20_write_byte(0x44);

  msleep(750); // 等待转换

  // reset

  if (ds18b20_reset()) {
      printk("ds18b20_reset failed. \n");
      return -1;
  }
  // 0xcc skip rom
  ds18b20_write_byte(0xCC);
  // 0xbe
  ds18b20_write_byte(0xBE);

  // 温度寄存器格式
  // LS Byte bit7 bit6 bit5 bit4 bit3 bit2 bit1 bit0
  //          3    2    1    0    -1   -2  -3    -4
  // MS Byte bit15 bit14 bit13 bit12 bit11 bit10 bit9 bit8
  //           S     S     S     S     S    6     5    4

  tempL = ds18b20_read_byte();
  tempH = ds18b20_read_byte();

// 补码表示负数，其实就是取反 + 1，例如 十进制 5 = 0b0101, 那么负数 5 = 0b1010 + 0001 = 0b1011
//  ds18b20 是16bit 数据，那么数据就有2种解析方式，如果温度为正，就正常的二进制，如果温度为负 返回的就是补码
// 问题1 怎么把补码转换成有符号的数据
//  答： 编译器自己搞定 但是要定义有符号类型  int16，如果是uint16 肯定就错误了
//
// 那么这样定义其实已经转换补码了
 int16_t temp16 = (tempH << 8) | tempL;

 // 这里先把小数处理了，为了后续方便，统一把数据转换成正数
 int sign = 1;
 if (temp16 < 0) {
     // 记录符号
     sign = -1;

     temp16 = -temp16;   // 转成正数再算
 }

 int tempInt = temp16 / 16; // 去掉低4位 小数位，小数位精度 0.5， 0.25， 0.125， 0.0625


 //我们默认的使用的是 0.0625 的精度
 //  (temp16 % 16) 取低4位 * 0.0625
 // ！！ 奇怪了，不能直接乘以 0.0625 要报错，搞不懂，只能先乘以再除；

 int tempDec = (temp16 % 16) * 625 / 100;

 // 恢复符号
 tempInt *= sign;

 printk("temperature = %d.%02d\n", tempInt, tempDec);

 //  if(tempH>0x7f)      							//最高位为1时温度是负
	// {
	// 	tempL    = ~tempL;         						//补码转换，取反加一
	// 	tempH    = ~tempH+1;
	// 	integer  = tempL/16+tempH*16;      			//整数部分
	// 	decimal1 = (tempL&0x0f)*10/16; 				//小数第一位
	// 	decimal2 = (tempL&0x0f)*100/16%10;			//小数第二位
	// 	decimal  = decimal1*10+decimal2; 			//小数两位
	// 	printk("temperature = %d.%d \n", integer, decimal);
 //    } else {
 //        integer  = tempL/16+tempH*16;      				//整数部分
 //    	decimal1 = (tempL&0x0f)*10/16; 					//小数第一位
 //    	decimal2 = (tempL&0x0f)*100/16%10;				//小数第二位
 //    	decimal  = decimal1*10+decimal2; 				//小数两位
 //    	printk("temperature = %d.%d\n", integer, decimal);
 //    }



  return tempInt;
}

static ssize_t ds18b20_read(struct file *file, char __user *buf, size_t size,
                            loff_t *ppos) {

  int value;

  if (size < sizeof(value)) {
      printk("input size error \n");
      return -EINVAL;
  }

  value = ds18b20_read_temp();
  if (value < 0) {
      printk("read temp failed.\n");
      return -EIO;
  }

  printk("ds18b20 temp = %d", value);

  // 2.
  if (copy_to_user(buf, &value, sizeof(value))) {
      return -EIO;
  }

  return sizeof(value);
}

static int ds18b20_open(struct inode *inode, struct file *file) { return 0; }

static unsigned int ds18b20_poll(struct file *fp, poll_table *wait) {
  poll_wait(fp, &gpio_wait, wait);
  // TODO: 这里要判断是否有温度值, 后面在改 is_key_buf_empty() ? 0 :
  return POLLIN | POLLRDNORM;
}

// 02
static struct file_operations ds18b20_fops = {
    .owner = THIS_MODULE,
    .read = ds18b20_read,
    .open = ds18b20_open,
   // .poll = ds18b20_poll,  先测试通过，后续在添加
};

static int ds18b20_probe(struct platform_device *pdev) {
  int error = 0;

  // printk("%s %s line %d\n", __FILE__, __FUNCTION__, __LINE__);
  printk("ds18b20_prob ok.\n");
  // 从设备树获取引脚信息
  ds18b20_gpio = gpiod_get(&pdev->dev, NULL, GPIOD_OUT_LOW);

  if (IS_ERR(ds18b20_gpio)) {
    return PTR_ERR(ds18b20_gpio);
  }

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

static int ds18b20_remove(struct platform_device *pdev) {

  gpiod_put(ds18b20_gpio);
  device_destroy(ds18b20_drv_class, MKDEV(major, 0));
  class_destroy(ds18b20_drv_class);
  unregister_chrdev(major, "ds18b20");

  return 0;
}

static const struct of_device_id ds18b20_of_match[] = {
    {.compatible = "100ask,ds18b20"},
    {},
    // 简单跟了下代码，这里要添加 {} 是 因为内核在match 的时候有个判断  match->compatible[0]
    // 这样设计的目的应该是为了节省空间和灵活处理
};

MODULE_DEVICE_TABLE(of, ds18b20_of_match);

static struct platform_driver ds18b20_driver = {.probe = ds18b20_probe,
                                                .remove = ds18b20_remove,
                                                .driver = {
                                                    .name = "ds18b20",
                                                    .of_match_table = ds18b20_of_match,
                                                }};

module_platform_driver(ds18b20_driver);

// 下面这个初始化感觉不合适了，用platform 更方便
//  简单跟了 platform 代码；
// #define module_platform_driver(__platform_driver) \
	// module_driver(__platform_driver, platform_driver_register, \
	// 		platform_driver_unregister)
	// 		#define module_driver(__driver, __register, __unregister, ...) \

// static int __init __driver##_init(void) \
// { \
// 				return __register(&(__driver) , ##__VA_ARGS__); \
// } \
// module_init(__driver##_init); \
// static void __exit __driver##_exit(void) \
// { \
// 				__unregister(&(__driver) , ##__VA_ARGS__); \
// } \
// module_exit(__driver##_exit);

// static int __init ds18b20_drv_init(void) {

//   return platform_driver_register(&ds18b20_driver);
// }

// static void __exit ds18b20_drv_exit(void) {
//   platform_driver_unregister(&ds18b20_driver);
// }

// module_init(ds18b20_drv_init);
// module_exit(ds18b20_drv_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIO IRQ Driver");
