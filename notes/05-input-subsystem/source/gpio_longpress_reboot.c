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

#define DEBOUNCE_MS 20

/*
 * 驱动数据结构。
 */
struct input_dev_demo {
  int gpio;
  int irq;
  bool active_low;
  bool pressed;
  unsigned long last_jiffies;
  struct input_dev *input_dev;
};

static struct input_dev *g_input_dev;
static int g_irq;

// 读取 GPIO 电平，然后转换成“按键是否按下”
static bool input_dev_key_is_pressed(struct input_dev_demo *demo) {
  int value = gpio_get_value(demo->gpio);

  // 如果设备树里写的是低电平有效，那么读到 0 才表示按下
  //  这里一定要注意设备树中的配置
  return demo->active_low ? !value : !!value;
}

//GPIO 中断处理函数：按下和松开都会进来
static irqreturn_t input_dev_irq_handler(int irq, void *dev_id) {
  struct input_dev_demo *demo = dev_id;
  unsigned long now = jiffies;
  bool pressed = input_dev_key_is_pressed(demo);

  // 简单去抖，20ms 内的变化先忽略掉
  if (time_before(now, demo->last_jiffies + msecs_to_jiffies(DEBOUNCE_MS)))
    return IRQ_HANDLED;

  /* 如果状态没变化，可能是抖动或者重复中断，直接忽略 */
  if (pressed == demo->pressed)
    return IRQ_HANDLED;

  demo->last_jiffies = now;
  demo->pressed = pressed;

  // 上报 input 事件
  input_report_key(demo->input_dev, KEY_POWER, pressed);

  printk("input_dev_demo: report inpt key ret = %d\n", pressed);
  // sync 事件
  input_sync(demo->input_dev);

  return IRQ_HANDLED;
}

/* 设备树匹配成功后，内核会调用 probe */
static int input_dev_probe(struct platform_device *pdev) {
  struct input_dev_demo *demo;
  enum of_gpio_flags flags;
  int ret;

  /* 申请驱动数据结构 */
  demo = kzalloc(sizeof(*demo), GFP_KERNEL);
  if (!demo)
    return -EINVAL;

  // 从设备树的 gpios 属性里拿到 GPIO
  demo->gpio = of_get_named_gpio_flags(pdev->dev.of_node, "gpios", 0, &flags);
  if (!gpio_is_valid(demo->gpio)) {
    ret = demo->gpio;
    printk("input_dev_demo: get gpio failed, ret = %d\n", ret);
    return  -EINVAL;
  }

  demo->active_low = flags & OF_GPIO_ACTIVE_LOW;


  // 申请 GPIO
  ret = gpio_request(demo->gpio, "input_dev_demo_key");
  if (ret) {
    printk("input_dev_demo: gpio_request failed, ret = %d\n", ret);
    return -EINVAL;
  }

  // 设置方向
  ret = gpio_direction_input(demo->gpio);
  if (ret) {
    printk("input_dev_demo: gpio_direction_input failed, ret = %d\n", ret);
    return -EINVAL;
  }

  // 获取 中端
  demo->irq = platform_get_irq(pdev, 0);
  if (demo->irq < 0) {
    ret = demo->irq;
    printk("input_dev_demo: get irq failed, ret = %d\n", ret);
    return -EINVAL;
  }

  // 创建一个 input 设备
  // devm 开头的好像是自动管理资源
  demo->input_dev = input_allocate_device();
  if (!demo->input_dev) {
      return -EINVAL;
  }

  // 可有可无
  demo->input_dev->name = "input_dev_demo";
  demo->input_dev->phys = "input_dev_demo/input0";
  demo->input_dev->id.bustype = BUS_HOST;

  // 设置支持的输入事件：电源键
  input_set_capability(demo->input_dev, EV_KEY, KEY_POWER);

  // 注册 input 设备
  ret = input_register_device(demo->input_dev);
  if (ret) {
    printk("input_dev_demo: input_register_device failed, ret = %d\n", ret);
    return -EINVAL;
  }

  demo->pressed = input_dev_key_is_pressed(demo);
  demo->last_jiffies = jiffies;

  // 申请 GPIO 中断
  ret = request_irq(demo->irq, input_dev_irq_handler,
                    IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                    "input_dev_demo", demo);
  if (ret) {
    printk("input_dev_demo: request_irq failed, ret = %d\n", ret);
    return -EINVAL;
  }

  /* 保存一下，remove 的时候可以取回来 */
  g_input_dev = demo->input_dev;
  g_irq = demo->irq;
  platform_set_drvdata(pdev, demo);

  printk("input_dev_demo: probe ok, gpio = %d, irq = %d\n", demo->gpio,
         demo->irq);

  return 0;
}


static int input_dev_remove(struct platform_device *pdev) {
  struct input_dev_demo *demo = platform_get_drvdata(pdev);

  // 释放probe 中申请的资源
  free_irq(demo->irq, demo);
  input_unregister_device(demo->input_dev);
  gpio_free(demo->gpio);
  kfree(demo);

  g_input_dev = NULL;
  g_irq = 0;

  return 0;
}


static const struct of_device_id input_dev_of_match[] = {
    {.compatible = "100ask,input_dev_demo"}, {}};

MODULE_DEVICE_TABLE(of, input_dev_of_match);

// 注册 platform_drive
static struct platform_driver input_dev_driver = {
    .probe = input_dev_probe,
    .remove = input_dev_remove,
    .driver = {
        .name = "input_dev_demo",
        .of_match_table = input_dev_of_match,
    }};

static int __init input_dev_init(void) {
  return platform_driver_register(&input_dev_driver);
}

static void __exit input_dev_exit(void) {

  platform_driver_unregister(&input_dev_driver);
}

module_init(input_dev_init);
module_exit(input_dev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XuHao");
MODULE_DESCRIPTION("input driver");
