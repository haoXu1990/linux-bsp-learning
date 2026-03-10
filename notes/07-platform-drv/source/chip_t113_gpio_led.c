#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>

static int led_gpio;

int chip_led_set(int value) {
    gpio_set_value(led_gpio, value);
    return 0;
}
// 这里要导出，这样 字符设备驱动层才可以使用
EXPORT_SYMBOL(chip_led_set);

static int chip_t113_led_probe(struct platform_device *pdev) {
    struct resource *res;

    res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);

    led_gpio = res->start;

    gpio_request(led_gpio, "led");

    gpio_direction_output(led_gpio, 0);

    printk("chip_t113_led device init. \n");

    return 0;
}

static struct platform_driver chip_t113_led_driver = {
    .probe = chip_t113_led_probe,
    .driver = {
        .name = "chip_t113_led"
    }
};

module_platform_driver(chip_t113_led_driver);

MODULE_LICENSE("GPL");
