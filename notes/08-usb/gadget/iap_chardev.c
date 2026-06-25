
/*
 * iAP userspace bridge: /dev/cqlh_iap.
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/usb/composite.h>

#include "iap_core.h"

#define IAP_GET_DEVICE_STATE	_IOW('z', 1, int)

static const char iap_shortname[] = "cqlh_iap";
static struct iap_dev *iap_active_dev;

static ssize_t iap_read(struct file *fp, char __user *buf,
		size_t count, loff_t *pos)
{
	struct iap_dev *dev = fp->private_data;
	struct usb_request *req;
	int r = count;
	int xfer;
	int ret;

	if (!dev)
		return -ENODEV;

	printk(KERN_DEBUG "iap_read: count=%zu, online=%d, error=%d\n",
			count, dev->online, dev->error);

	if (count > IAP_BULK_BUFFER_SIZE)
		return -EINVAL;

	if (iap_lock(&dev->read_excl))
		return -EBUSY;

	while (!(dev->online || dev->error)) {
		printk(KERN_DEBUG "iap_read: waiting for online state\n");
		ret = wait_event_interruptible(dev->read_wq,
				dev->online || dev->error);
		if (ret < 0) {
			iap_unlock(&dev->read_excl);
			return ret;
		}
	}

	if (dev->error) {
		printk(KERN_DEBUG "iap_read: error\n");
		r = -EIO;
		goto done;
	}

requeue_req:
	req = dev->rx_req;
	req->length = count;
	dev->rx_done = 0;

	printk(KERN_DEBUG "iap_read: ep_out=%s, ep->enabled=%d\n",
			dev->ep_out->name, dev->ep_out->enabled);

	ret = usb_ep_queue(dev->ep_out, req, GFP_ATOMIC);
	if (ret < 0) {
		printk(KERN_ERR "iap_read: failed to queue req %p (%d)\n",
				req, ret);
		r = -EIO;
		dev->error = 1;
		goto done;
	}

	printk(KERN_DEBUG "rx %p queue\n", req);

	ret = wait_event_interruptible_timeout(dev->read_wq, dev->rx_done,
			msecs_to_jiffies(1000));
	if (ret < 0) {
		dev->error = 1;
		r = ret;
		usb_ep_dequeue(dev->ep_out, req);
		goto done;
	} else if (ret == 0) {
		r = 0;
		usb_ep_dequeue(dev->ep_out, req);
		goto done;
	}

	if (!dev->error) {
		if (req->actual == 0)
			goto requeue_req;

		printk(KERN_DEBUG "rx %p %d\n", req, req->actual);
		xfer = (req->actual < count) ? req->actual : count;
		if (copy_to_user(buf, req->buf, xfer))
			r = -EFAULT;
	} else {
		r = -EIO;
	}

done:
	iap_unlock(&dev->read_excl);
	return r;
}

static ssize_t iap_write(struct file *fp, const char __user *buf,
		size_t count, loff_t *pos)
{
	struct iap_dev *dev = fp->private_data;
	struct usb_request *req = NULL;
	int r = count;
	int xfer;
	int ret;

	if (!dev)
		return -ENODEV;

	if (iap_lock(&dev->write_excl))
		return -EBUSY;

	while (!(dev->online || dev->error)) {
		printk(KERN_DEBUG "iap_write: waiting for online state\n");
		ret = wait_event_interruptible(dev->write_wq,
				dev->online || dev->error);
		if (ret < 0) {
			iap_unlock(&dev->write_excl);
			return ret;
		}
	}

	if (dev->error) {
		r = -EIO;
		goto done;
	}

	while (count > 0) {
		if (dev->error) {
			printk(KERN_ERR "iap_write dev->error\n");
			r = -EIO;
			break;
		}

		req = NULL;
		ret = wait_event_interruptible(dev->write_wq,
				(req = iap_req_get(dev, &dev->tx_idle)) ||
				dev->error);
		if (ret < 0) {
			r = ret;
			break;
		}

		if (!req)
			continue;

		xfer = min_t(size_t, count, IAP_BULK_BUFFER_SIZE);
		if (copy_from_user(req->buf, buf, xfer)) {
			r = -EFAULT;
			break;
		}

		req->length = xfer;
		ret = usb_ep_queue(dev->ep_in, req, GFP_ATOMIC);
		if (ret < 0) {
			dev->error = 1;
			r = -EIO;
			break;
		}

		buf += xfer;
		count -= xfer;
		req = NULL;
	}

	if (req)
		iap_req_put(dev, &dev->tx_idle, req);

done:
	iap_unlock(&dev->write_excl);
	return r;
}

static int iap_open(struct inode *ip, struct file *fp)
{
	struct iap_dev *dev = iap_active_dev;

	if (!dev)
		return -ENODEV;

	if (iap_lock(&dev->open_excl))
		return -EBUSY;

	fp->private_data = dev;
	dev->error = 0;

	return 0;
}

static int iap_release(struct inode *ip, struct file *fp)
{
	struct iap_dev *dev = fp->private_data;

	if (dev)
		iap_unlock(&dev->open_excl);

	return 0;
}

static long iap_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct iap_dev *dev = file->private_data;
	int online = 0;

	switch (cmd) {
	case IAP_GET_DEVICE_STATE:
		online = dev->online ? 1 : 0;
		break;
	default:
		return -EINVAL;
	}

	if (copy_to_user((void __user *)arg, &online, sizeof(online)))
		return -EFAULT;

	printk("zjinnova: iap iap_ioctl >>>>>>>>>>>>>>>>>>>>>>>>>>\n");
	return 0;
}

static const struct file_operations iap_fops = {
	.owner = THIS_MODULE,
	.read = iap_read,
	.write = iap_write,
	.open = iap_open,
	.unlocked_ioctl = iap_ioctl,
	.release = iap_release,
};

static struct miscdevice iap_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = iap_shortname,
	.fops = &iap_fops,
};

static void iap_work(struct work_struct *data)
{
	struct iap_dev *dev = container_of(data, struct iap_dev, work);
	char *disconnected[2] = { "IAP_STATE=DISCONNECTED", NULL };
	char *connected[2] = { "IAP_STATE=CONNECTED", NULL };
	char **uevent_envp = NULL;

	if (dev->online != dev->sw_online) {
		if (dev->online)
			uevent_envp = connected;
		else
			uevent_envp = disconnected;
	}
	dev->sw_online = dev->online;

	if (uevent_envp) {
		kobject_uevent_env(&dev->misc_device->this_device->kobj,
				KOBJ_CHANGE, uevent_envp);
		printk(KERN_INFO "%s: sent uevent %s\n", __func__,
				uevent_envp[0]);
	} else {
		printk(KERN_ERR "%s: did not send uevent (%d %p)\n",
				__func__, dev->sw_online, uevent_envp);
	}
}

int iap_chardev_register(struct iap_dev *dev)
{
	int ret;

	if (iap_active_dev)
		return -EBUSY;

	INIT_WORK(&dev->work, iap_work);
	dev->misc_device = &iap_device;
	iap_active_dev = dev;

	ret = misc_register(&iap_device);
	if (ret) {
		iap_active_dev = NULL;
		dev->misc_device = NULL;
	}

	return ret;
}

void iap_chardev_unregister(struct iap_dev *dev)
{
	if (iap_active_dev != dev)
		return;

	cancel_work_sync(&dev->work);
	misc_deregister(&iap_device);
	iap_active_dev = NULL;
	dev->misc_device = NULL;
}
