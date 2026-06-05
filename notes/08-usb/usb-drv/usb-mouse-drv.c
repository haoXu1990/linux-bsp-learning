#include "asm-generic/errno.h"
#include "asm/stat.h"
#include "linux/gfp.h"
#include "linux/mod_devicetable.h"
#include "linux/types.h"
#include "linux/usb/ch9.h"
// #include <cerrno>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/hid.h>

struct usb_mouse {

    struct usb_device *usb_dev;
    struct usb_interface *intf;
    struct input_dev *input_dev;


    struct urb *irq;
    unsigned char *data;

    dma_addr_t data_dma; //dma buffer
    int pipe, maxpacket;

    int bInterval;

};

static void my_mouse_irq(struct urb *urb) {
    struct usb_mouse *my_mouse = urb->context;
    signed char *data = my_mouse->data;
    int status = urb->status;

    // int i;
    // int error;
    switch (status) {
        case 0:
            break;
        case -ECONNRESET:
        case -ENOENT:
        case -ESHUTDOWN:
            return;
        default:
            goto resubmit;
    }
    // 处理输入的数据
    input_report_key(my_mouse->input_dev, KEY_L, data[1] & 0x01);
    input_report_key(my_mouse->input_dev, KEY_S, data[1] & 0x02);
    input_report_key(my_mouse->input_dev, KEY_ENTER, data[1] & 0x04);

    input_sync(my_mouse->input_dev);
resubmit:
    status = usb_submit_urb (urb, GFP_ATOMIC);
}


static int usb_mouse_input_dev_open(struct input_dev *dev) {
    struct usb_mouse *mymouse = input_get_drvdata(dev);
    int error;

    printk("usb mouse input dev open . \n");
      // 1.4 创建 URB
      mymouse->irq = usb_alloc_urb(0, GFP_KERNEL);

      if (!mymouse->irq) {
          error = -ENOMEM;
        //  goto error_buffer;
      }

      // 1.5 填充 URB
      usb_fill_int_urb(mymouse->irq, mymouse->usb_dev, mymouse->pipe, mymouse->data, mymouse->maxpacket, my_mouse_irq,mymouse,  mymouse->bInterval);

      // 使用 DMA
      mymouse->irq->transfer_dma = mymouse->data_dma;
      mymouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

       // 1.6 提交 URB
       error = usb_submit_urb(mymouse->irq, GFP_KERNEL);
       if (error) {
       //    goto error_urb;
       }
       printk("mouse_key probe ok. \n");

    return 0;
}

static void usb_mouse_input_dev_close(struct input_dev *dev) {
    struct usb_mouse *mymouse = input_get_drvdata(dev);

    usb_kill_urb(mymouse->irq);
    usb_free_urb(mymouse->irq);

}

static int usb_mouse_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    struct usb_device *dev = interface_to_usbdev(intf);
    struct usb_host_interface *interface;
    struct usb_endpoint_descriptor *endpoint;
    struct input_dev *input_dev;
    struct usb_mouse *mouse_key;
    int pipe, maxpacket;
    int error, i;


    // 1. USB
    interface = intf->cur_altsetting;
    if (interface->desc.bNumEndpoints != 1) {
        return -ENODEV;
    }

    // 1.1 先找到对应的 endpoint
    for (i = 0; i < interface->desc.bNumEndpoints; i++) {
         endpoint = &interface->endpoint[i].desc;
         // 判断 是否 IN
         if (usb_endpoint_is_int_in(endpoint)) {
             break;
         }
    }

    //  创建一个 PIPE
    pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);

    // 创建 maxp 相当于一次传输的最大包大小; endpoint descriptor 中的 wMaxPacketSize
    maxpacket = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

    // 1.2 分配私有数据
    mouse_key = kzalloc(sizeof( struct usb_mouse ), GFP_KERNEL);

    //  创建一个输入设备
    input_dev = input_allocate_device();

    // 设置私有数据
    mouse_key->usb_dev = dev;
    mouse_key->input_dev = input_dev;
    mouse_key->maxpacket = maxpacket;
    mouse_key->intf = intf;
    mouse_key->pipe = pipe;
    mouse_key->bInterval = endpoint->bInterval;

    mouse_key->data = usb_alloc_coherent(dev, maxpacket, GFP_ATOMIC, &mouse_key->data_dma);

    input_set_drvdata(input_dev, mouse_key);

    // 设置支持的事件

    __set_bit(KEY_L, input_dev->keybit);
    __set_bit(KEY_S, input_dev->keybit);
    __set_bit(KEY_ENTER, input_dev->keybit);


    // 在 input open 中 创建urb, 填充urb, 提交urb
    input_dev->open = usb_mouse_input_dev_open;
    input_dev->close = usb_mouse_input_dev_close;

    error = input_register_device(input_dev);

    usb_set_intfdata(intf, mouse_key);

    if (!error) {
        printk("usb mouse, input register ok. \n");
    }

    printk(" usb mouse, probe ok. \n");
    return 0;
}

static void usb_mouse_disconnect(struct usb_interface *intf) {
    struct usb_mouse *mouse = usb_get_intfdata(intf);

    usb_set_intfdata(intf, NULL);

    if (!mouse)
        return;

    usb_kill_urb(mouse->irq);
    usb_free_urb(mouse->irq);

    usb_free_coherent(mouse->usb_dev,
                        mouse->maxpacket,
                        mouse->data,
                        mouse->data_dma);

    usb_put_dev(mouse->usb_dev);

    input_unregister_device(mouse->input_dev);
    kfree(mouse);

    printk("my usb mouse disconnected\n");
}


static struct usb_device_id usb_mouse_id_table [] = {
    { USB_INTERFACE_INFO(USB_INTERFACE_CLASS_HID, USB_INTERFACE_SUBCLASS_BOOT,
    USB_INTERFACE_PROTOCOL_MOUSE) },
    { }	/* Terminating entry */
};

static struct usb_driver usb_mouse_driver = {
    .name = "usbmouse",
    .probe = usb_mouse_probe,
    .disconnect =usb_mouse_disconnect,
    .id_table =usb_mouse_id_table,
};


module_usb_driver(usb_mouse_driver);

MODULE_LICENSE("GPL");
