
/*
 * Gadget Driver for cqlh iAP
 *
 * Copyright (C) 2026 cqlh, Inc.
 * Author: xuhao <cqlh@xuhao.com.cn>
 */

#ifndef _IAP_CORE_H_
#define _IAP_CORE_H_

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb/composite.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define IAP_BULK_BUFFER_SIZE	4096

/* number of tx requests to allocate */
#define TX_REQ_MAX		4

/*
 * This CarPlay iAP channel is a singleton by product design: one gadget
 * function, one /dev/zjinnova_iap endpoint, one userspace iAP service.
 */
struct iap_dev {
	struct usb_function function;
	struct usb_composite_dev *cdev;
	spinlock_t lock;

	struct usb_ep *ep_in;
	struct usb_ep *ep_out;

	int online;
	int error;

	atomic_t read_excl;
	atomic_t write_excl;
	atomic_t open_excl;

	struct list_head tx_idle;

	wait_queue_head_t read_wq;
	wait_queue_head_t write_wq;
	struct usb_request *rx_req;
	int rx_done;

	struct work_struct work;
	struct miscdevice *misc_device;
	int sw_online;
};

static inline struct iap_dev *func_to_iap(struct usb_function *f)
{
	return container_of(f, struct iap_dev, function);
}

static inline int iap_lock(atomic_t *excl)
{
	if (atomic_inc_return(excl) == 1)
		return 0;

	atomic_dec(excl);
	return -1;
}

static inline void iap_unlock(atomic_t *excl)
{
	atomic_dec(excl);
}

int iap_chardev_register(struct iap_dev *dev);
void iap_chardev_unregister(struct iap_dev *dev);

int iap_create_bulk_endpoints(struct iap_dev *dev,
		struct usb_endpoint_descriptor *in_desc,
		struct usb_endpoint_descriptor *out_desc);
void iap_free_bulk_requests(struct iap_dev *dev);
void iap_req_put(struct iap_dev *dev, struct list_head *head,
		struct usb_request *req);
struct usb_request *iap_req_get(struct iap_dev *dev, struct list_head *head);

#endif /* _IAP_CORE_H_ */
