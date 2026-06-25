/*
 * iAP USB function/configfs integration.
 */

#include <linux/configfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/composite.h>

#include "iap_core.h"

#define DRIVER_NAME		"iap"
#define MAX_INST_NAME_LEN	40

struct iap_instance {
	struct usb_function_instance func_inst;
	const char *name;
	struct iap_dev *dev;
	char iap_ext_compat_id[16];
	struct usb_os_desc iap_os_desc;
};

static struct usb_interface_descriptor iap_interface_desc = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = 0xFF,
	.bInterfaceSubClass = 0xF0,
	.bInterfaceProtocol = 0,
};

static struct usb_endpoint_descriptor iap_highspeed_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(512),
};

static struct usb_endpoint_descriptor iap_highspeed_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize = __constant_cpu_to_le16(512),
};

static struct usb_endpoint_descriptor iap_fullspeed_in_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor iap_fullspeed_out_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_OUT,
	.bmAttributes = USB_ENDPOINT_XFER_BULK,
};

static struct usb_descriptor_header *fs_iap_descs[] = {
	(struct usb_descriptor_header *)&iap_interface_desc,
	(struct usb_descriptor_header *)&iap_fullspeed_in_desc,
	(struct usb_descriptor_header *)&iap_fullspeed_out_desc,
	NULL,
};

static struct usb_descriptor_header *hs_iap_descs[] = {
	(struct usb_descriptor_header *)&iap_interface_desc,
	(struct usb_descriptor_header *)&iap_highspeed_in_desc,
	(struct usb_descriptor_header *)&iap_highspeed_out_desc,
	NULL,
};

static struct usb_string iap_string_defs[] = {
	[0].s = "iAP Interface",
	{  }
};

static struct usb_gadget_strings iap_string_table = {
	.language = 0x0409,
	.strings = iap_string_defs,
};

static struct usb_gadget_strings *iap_strings[] = {
	&iap_string_table,
	NULL,
};

static struct iap_instance *to_iap_instance(struct config_item *item)
{
	return container_of(to_config_group(item), struct iap_instance,
			func_inst.group);
}

static struct iap_instance *to_fi_iap(struct usb_function_instance *fi)
{
	return container_of(fi, struct iap_instance, func_inst);
}

static int iap_dev_setup(struct iap_instance *fi_iap)
{
	struct iap_dev *dev;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	fi_iap->dev = dev;

	spin_lock_init(&dev->lock);
	init_waitqueue_head(&dev->read_wq);
	init_waitqueue_head(&dev->write_wq);

	atomic_set(&dev->open_excl, 0);
	atomic_set(&dev->read_excl, 0);
	atomic_set(&dev->write_excl, 0);

	INIT_LIST_HEAD(&dev->tx_idle);

	ret = iap_chardev_register(dev);
	if (ret) {
		kfree(dev);
		fi_iap->dev = NULL;
		printk(KERN_ERR "iap gadget driver failed to initialize\n");
		return ret;
	}

	return 0;
}

static void iap_dev_cleanup(struct iap_instance *fi_iap)
{
	struct iap_dev *dev = fi_iap->dev;

	if (!dev)
		return;

	iap_chardev_unregister(dev);
	kfree(dev);
	fi_iap->dev = NULL;
}

static int iap_function_set_alt(struct usb_function *f,
		unsigned intf, unsigned alt)
{
	struct iap_dev *dev = func_to_iap(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	printk("iap_function_set_alt\n");

	if (config_ep_by_speed(cdev->gadget, f, dev->ep_in) ||
			config_ep_by_speed(cdev->gadget, f, dev->ep_out)) {
		printk(KERN_ERR "iap_function_set_alt fail\n");
		return -EINVAL;
	}

	usb_ep_enable(dev->ep_in);
	usb_ep_enable(dev->ep_out);

	dev->online = 1;
	printk("iap_function_set_alt: online\n");

	wake_up(&dev->read_wq);
	wake_up(&dev->write_wq);
	schedule_work(&dev->work);

	return 0;
}

static int iap_function_bind(struct usb_configuration *c,
		struct usb_function *f)
{
	struct usb_composite_dev *cdev = c->cdev;
	struct iap_dev *dev = func_to_iap(f);
	int id;
	int ret;
	int status;

	printk("iap_function_bind\n");

	dev->cdev = cdev;
	printk(KERN_DEBUG "iap_function_bind dev: %p\n", dev);

	id = usb_interface_id(c, f);
	if (id < 0)
		return id;
	iap_interface_desc.bInterfaceNumber = id;

	status = usb_string_id(cdev);
	if (status < 0)
		return status;
	iap_string_defs[0].id = status;
	iap_interface_desc.iInterface = status;

	dev->function.fs_descriptors = usb_copy_descriptors(fs_iap_descs);
	if (!dev->function.fs_descriptors)
		return -ENOMEM;

	ret = iap_create_bulk_endpoints(dev, &iap_fullspeed_in_desc,
			&iap_fullspeed_out_desc);
	if (ret) {
		usb_free_all_descriptors(f);
		return ret;
	}

	if (gadget_is_dualspeed(c->cdev->gadget)) {
		iap_highspeed_in_desc.bEndpointAddress =
			iap_fullspeed_in_desc.bEndpointAddress;
		iap_highspeed_out_desc.bEndpointAddress =
			iap_fullspeed_out_desc.bEndpointAddress;

		dev->function.hs_descriptors = usb_copy_descriptors(hs_iap_descs);
		if (!dev->function.hs_descriptors) {
			iap_free_bulk_requests(dev);
			usb_free_all_descriptors(f);
			return -ENOMEM;
		}
	}

	printk(KERN_DEBUG "%s speed %s: IN/%s, OUT/%s\n",
			gadget_is_dualspeed(c->cdev->gadget) ? "dual" : "full",
			f->name, dev->ep_in->name, dev->ep_out->name);

	return 0;
}

static void iap_function_unbind(struct usb_configuration *c,
		struct usb_function *f)
{
	struct iap_dev *dev = func_to_iap(f);

	printk(KERN_ERR "iap_function_unbind\n");

	dev->online = 0;
	dev->error = 1;

	wake_up(&dev->read_wq);
	wake_up(&dev->write_wq);
	schedule_work(&dev->work);

	iap_free_bulk_requests(dev);
	usb_free_all_descriptors(f);
}

static void iap_function_disable(struct usb_function *f)
{
	struct iap_dev *dev = func_to_iap(f);

	printk(KERN_ERR "iap_function_disable\n");

	dev->online = 0;
	dev->error = 1;
	usb_ep_disable(dev->ep_in);
	usb_ep_disable(dev->ep_out);

	wake_up(&dev->read_wq);
	wake_up(&dev->write_wq);
	schedule_work(&dev->work);
}

static void iap_free(struct usb_function *f)
{
}

static void iap_attr_release(struct config_item *item)
{
	struct iap_instance *fi_iap = to_iap_instance(item);

	usb_put_function_instance(&fi_iap->func_inst);
}

static struct configfs_item_operations iap_item_ops = {
	.release = iap_attr_release,
};

static struct config_item_type iap_func_type = {
	.ct_item_ops = &iap_item_ops,
	.ct_owner = THIS_MODULE,
};

static int iap_set_inst_name(struct usb_function_instance *fi,
		const char *name)
{
	struct iap_instance *fi_iap = to_fi_iap(fi);
	char *ptr;
	int name_len;

	name_len = strlen(name) + 1;
	if (name_len > MAX_INST_NAME_LEN)
		return -ENAMETOOLONG;

	ptr = kstrndup(name, name_len, GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	fi_iap->name = ptr;
	return 0;
}

static void iap_free_inst(struct usb_function_instance *fi)
{
	struct iap_instance *fi_iap = to_fi_iap(fi);

	printk("zjinnova: iap free >>>>>>>>>>>>>>>>>>>>>>>\n");

	kfree(fi_iap->name);
	iap_dev_cleanup(fi_iap);
	kfree(fi_iap);
}

static struct usb_function_instance *iap_alloc_inst(void)
{
	struct iap_instance *fi_iap;
	int ret;

	printk("zjinnova: iap alloc_inst >>>>>>>>>>>>>>>>>>>>>>>>>>\n");

	fi_iap = kzalloc(sizeof(*fi_iap), GFP_KERNEL);
	if (!fi_iap)
		return ERR_PTR(-ENOMEM);

	fi_iap->func_inst.set_inst_name = iap_set_inst_name;
	fi_iap->func_inst.free_func_inst = iap_free_inst;

	ret = iap_dev_setup(fi_iap);
	if (ret) {
		kfree(fi_iap);
		printk(KERN_ERR "Error setting IAP\n");
		return ERR_PTR(ret);
	}

	config_group_init_type_name(&fi_iap->func_inst.group,
			"", &iap_func_type);

	return &fi_iap->func_inst;
}

static struct usb_function *iap_alloc(struct usb_function_instance *fi)
{
	struct iap_instance *fi_iap = to_fi_iap(fi);
	struct iap_dev *dev;

	printk("zjinnova: iap alloc >>>>>>>>>>>>>>>>>>>>>>>>>>\n");

	if (!fi_iap->dev)
		return ERR_PTR(-EINVAL);

	dev = fi_iap->dev;
	dev->function.name = DRIVER_NAME;
	dev->function.strings = iap_strings;
	dev->function.bind = iap_function_bind;
	dev->function.unbind = iap_function_unbind;
	dev->function.free_func = iap_free;
	dev->function.set_alt = iap_function_set_alt;
	dev->function.disable = iap_function_disable;

	return &dev->function;
}

DECLARE_USB_FUNCTION_INIT(iap, iap_alloc_inst, iap_alloc);
MODULE_LICENSE("GPL");
