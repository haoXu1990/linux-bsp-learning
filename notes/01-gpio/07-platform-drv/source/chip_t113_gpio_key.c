#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>

static int key_gpio;

int chip_key_read(int gpio) {
    return gpio_get_value(key_gpio);
}


// 这里要导出，这样 字符设备驱动层才可以使用
EXPORT_SYMBOL(chip_key_read);

static int chip_t113_key_probe(struct platform_device *pdev) {
    struct resource *res;

    res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);

    key_gpio = res->start;

    gpio_request(key_gpio, "key");

    gpio_direction_input(key_gpio);

    printk("chip_t113_key device init. \n");

    return 0;
}

static struct platform_driver chip_t113_key_driver = {
    .probe = chip_t113_key_probe,
    .driver = {
        .name = "chip_t113_key"
    }
};

module_platform_driver(chip_t113_key_driver);

MODULE_LICENSE("GPL");
