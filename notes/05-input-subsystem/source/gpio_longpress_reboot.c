#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/workqueue.h>

static int key_gpio = 103;
module_param(key_gpio, int, 0644);
MODULE_PARM_DESC(key_gpio, "GPIO number used as the power key");

static bool active_low;
module_param(active_low, bool, 0644);
MODULE_PARM_DESC(active_low, "Set to true when the key is active low");

static unsigned int press_ms = 3000;
module_param(press_ms, uint, 0644);
MODULE_PARM_DESC(press_ms, "Long press timeout in milliseconds");

struct gpio_power_key {
	int gpio;
	int irq;
	bool pressed;
	bool reboot_sent;
	struct input_dev *input;
	struct delayed_work longpress_work;
};

static struct gpio_power_key power_key;

static bool gpio_power_key_is_pressed(struct gpio_power_key *key)
{
	int value = gpio_get_value(key->gpio);

	return active_low ? !value : !!value;
}

static void gpio_power_key_report(struct gpio_power_key *key, bool pressed)
{
	input_report_key(key->input, KEY_POWER, pressed);
	input_sync(key->input);
}

static void gpio_power_key_longpress_work(struct work_struct *work)
{
	struct gpio_power_key *key =
		container_of(to_delayed_work(work), struct gpio_power_key,
			     longpress_work);

	if (!gpio_power_key_is_pressed(key))
		return;

	if (key->reboot_sent)
		return;

	key->reboot_sent = true;

	pr_info("gpio_longpress_reboot: long press detected, call ctrl_alt_del\n");

	gpio_power_key_report(key, true);
	gpio_power_key_report(key, false);

	ctrl_alt_del();
}

static irqreturn_t gpio_power_key_irq(int irq, void *dev_id)
{
	struct gpio_power_key *key = dev_id;
	bool pressed = gpio_power_key_is_pressed(key);

	if (pressed == key->pressed)
		return IRQ_HANDLED;

	key->pressed = pressed;

	gpio_power_key_report(key, pressed);

	if (pressed) {
		key->reboot_sent = false;
		schedule_delayed_work(&key->longpress_work,
				      msecs_to_jiffies(press_ms));
	} else {
		cancel_delayed_work(&key->longpress_work);
	}

	return IRQ_HANDLED;
}

static int __init gpio_longpress_reboot_init(void)
{
	int ret;

	memset(&power_key, 0, sizeof(power_key));
	power_key.gpio = key_gpio;

	ret = gpio_request(power_key.gpio, "gpio_longpress_reboot");
	if (ret) {
		pr_err("gpio_longpress_reboot: gpio_request(%d) failed: %d\n",
		       power_key.gpio, ret);
		return ret;
	}

	ret = gpio_direction_input(power_key.gpio);
	if (ret) {
		pr_err("gpio_longpress_reboot: gpio_direction_input failed: %d\n",
		       ret);
		goto err_free_gpio;
	}

	power_key.irq = gpio_to_irq(power_key.gpio);
	if (power_key.irq < 0) {
		ret = power_key.irq;
		pr_err("gpio_longpress_reboot: gpio_to_irq failed: %d\n", ret);
		goto err_free_gpio;
	}

	power_key.input = input_allocate_device();
	if (!power_key.input) {
		ret = -ENOMEM;
		goto err_free_gpio;
	}

	power_key.input->name = "gpio-longpress-reboot";
	power_key.input->phys = "gpio-longpress-reboot/input0";
	power_key.input->id.bustype = BUS_HOST;
	input_set_capability(power_key.input, EV_KEY, KEY_POWER);

	ret = input_register_device(power_key.input);
	if (ret) {
		pr_err("gpio_longpress_reboot: input_register_device failed: %d\n",
		       ret);
		goto err_free_input;
	}

	INIT_DELAYED_WORK(&power_key.longpress_work,
			  gpio_power_key_longpress_work);

	power_key.pressed = gpio_power_key_is_pressed(&power_key);

	ret = request_irq(power_key.irq, gpio_power_key_irq,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  "gpio_longpress_reboot", &power_key);
	if (ret) {
		pr_err("gpio_longpress_reboot: request_irq failed: %d\n", ret);
		goto err_unregister_input;
	}

	pr_info("gpio_longpress_reboot: gpio=%d irq=%d active_low=%d press_ms=%u\n",
		power_key.gpio, power_key.irq, active_low, press_ms);

	return 0;

err_unregister_input:
	input_unregister_device(power_key.input);
	power_key.input = NULL;
	goto err_free_gpio;
err_free_input:
	input_free_device(power_key.input);
	power_key.input = NULL;
err_free_gpio:
	gpio_free(power_key.gpio);
	return ret;
}

static void __exit gpio_longpress_reboot_exit(void)
{
	free_irq(power_key.irq, &power_key);
	cancel_delayed_work_sync(&power_key.longpress_work);

	if (power_key.input)
		input_unregister_device(power_key.input);

	gpio_free(power_key.gpio);

	pr_info("gpio_longpress_reboot: exit\n");
}

module_init(gpio_longpress_reboot_init);
module_exit(gpio_longpress_reboot_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XuHao");
MODULE_DESCRIPTION("GPIO long press reboot input driver");
