
/*
 * iAP USB endpoint/request handling.
 */

#include <linux/kernel.h>
#include <linux/slab.h>

#include "iap_core.h"

static struct usb_request *iap_request_new(struct usb_ep *ep, int buffer_size)
{
	struct usb_request *req = usb_ep_alloc_request(ep, GFP_KERNEL);

	if (!req)
		return NULL;

	req->buf = kmalloc(buffer_size, GFP_KERNEL);
	if (!req->buf) {
		usb_ep_free_request(ep, req);
		return NULL;
	}

	return req;
}

static void iap_request_free(struct usb_request *req, struct usb_ep *ep)
{
	if (!req)
		return;

	kfree(req->buf);
	usb_ep_free_request(ep, req);
}

void iap_req_put(struct iap_dev *dev, struct list_head *head,
		struct usb_request *req)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	list_add_tail(&req->list, head);
	spin_unlock_irqrestore(&dev->lock, flags);
}

struct usb_request *iap_req_get(struct iap_dev *dev, struct list_head *head)
{
	unsigned long flags;
	struct usb_request *req;

	spin_lock_irqsave(&dev->lock, flags);
	if (list_empty(head)) {
		req = NULL;
	} else {
		req = list_first_entry(head, struct usb_request, list);
		list_del(&req->list);
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	return req;
}

static void iap_complete_in(struct usb_ep *ep, struct usb_request *req)
{
	struct iap_dev *dev = req->context;

	if (req->status != 0)
		dev->error = 1;

	iap_req_put(dev, &dev->tx_idle, req);
	wake_up(&dev->write_wq);
}

static void iap_complete_out(struct usb_ep *ep, struct usb_request *req)
{
	struct iap_dev *dev = req->context;

	printk(KERN_INFO "iap_complete_out: req=%p, status=%d, ep=%s, online=%d\n",
			req, req->status, ep->name, dev->online);

	dev->rx_done = 1;
	if (req->status != 0) {
		printk(KERN_ERR "iap_complete_out: USB request failed, status=%d, ep=%s\n",
				req->status, ep->name);
		dump_stack();
		dev->error = 1;
	}

	wake_up(&dev->read_wq);
}

int iap_create_bulk_endpoints(struct iap_dev *dev,
		struct usb_endpoint_descriptor *in_desc,
		struct usb_endpoint_descriptor *out_desc)
{
	struct usb_composite_dev *cdev = dev->cdev;
	struct usb_request *req;
	struct usb_ep *ep;
	int i;

	printk(KERN_DEBUG "create_bulk_endpoints dev: %p\n", dev);

	ep = usb_ep_autoconfig(cdev->gadget, in_desc);
	if (!ep) {
		printk(KERN_ERR "usb_ep_autoconfig for ep_in failed\n");
		return -ENODEV;
	}
	printk(KERN_DEBUG "usb_ep_autoconfig for ep_in got %s\n", ep->name);
	ep->driver_data = dev;
	dev->ep_in = ep;

	ep = usb_ep_autoconfig(cdev->gadget, out_desc);
	if (!ep) {
		printk(KERN_ERR "usb_ep_autoconfig for ep_out failed\n");
		return -ENODEV;
	}
	printk(KERN_DEBUG "usb_ep_autoconfig for iap ep_out got %s\n", ep->name);
	ep->driver_data = dev;
	dev->ep_out = ep;

	req = iap_request_new(dev->ep_out, IAP_BULK_BUFFER_SIZE);
	if (!req)
		goto fail;
	req->complete = iap_complete_out;
	req->context = dev;
	dev->rx_req = req;

	for (i = 0; i < TX_REQ_MAX; i++) {
		req = iap_request_new(dev->ep_in, IAP_BULK_BUFFER_SIZE);
		if (!req)
			goto fail;
		req->complete = iap_complete_in;
		req->context = dev;
		iap_req_put(dev, &dev->tx_idle, req);
	}

	return 0;

fail:
	iap_free_bulk_requests(dev);
	printk(KERN_ERR "iap_bind() could not allocate requests\n");
	return -ENOMEM;
}

void iap_free_bulk_requests(struct iap_dev *dev)
{
	struct usb_request *req;

	iap_request_free(dev->rx_req, dev->ep_out);
	dev->rx_req = NULL;

	while ((req = iap_req_get(dev, &dev->tx_idle)))
		iap_request_free(req, dev->ep_in);
}
