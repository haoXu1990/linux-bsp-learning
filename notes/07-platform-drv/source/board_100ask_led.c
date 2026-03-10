#include <linux/module.h>
#include <linux/platform_device.h>

/***
 * board 层， 主要描述当前板载有什么设备
 *
 *  resource
 *     我想先简单点，就吧 resource 直接放在板载里面，等代码能跑起来后，再考虑 resource 到底应该放在哪里，是否应该单独出来
 *
 * */


// 定义资源
static struct resource led_resources[] = {
    {
        .start = 117,
        .end = 117,
        .flags = IORESOURCE_IRQ,
    },
    {
        .start = 118,
        .end = 118,
        .flags = IORESOURCE_IRQ,
    },
};

// 定义 led device
static struct platform_device led_device = {
    .name = "chip_t113_led",
    .num_resources = ARRAY_SIZE(led_resources),
    .resource = led_resources,
};


// 定义 platfo 骨架
static int __init board_t113_init(void) {
    //  注册 led， key device
    platform_device_register(&led_device);
    return 0;
}

static void __exit board_t113_exit(void) {
    platform_device_unregister(&led_device);
}

module_init(board_t113_init);
module_exit(board_t113_exit);

MODULE_LICENSE("GPL");
