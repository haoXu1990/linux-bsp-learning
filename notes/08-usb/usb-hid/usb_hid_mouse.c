
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libusb-1.0/libusb.h"

struct usb_mouse {
  struct libusb_device_handle *handle;
  int interface;
  int endpoint;
  unsigned char buf[256];
  int transferred;
  struct libusb_transfer *transfer;
  struct usb_mouse *next;
  int packet_size;
};
static struct usb_mouse *usb_mouse_list;

void free_usb_mouses(struct usb_mouse *list) {
  struct usb_mouse *pnext;

  while (list) {
    pnext = list->next;
    free(list);
    list = pnext;
  }
}

int get_usb_mouses(libusb_device **device, int num_devices,
                   struct usb_mouse **usb_mouse_list) {
  int err;
  libusb_device *dev;
  int endpoint;
  int interface_num;
  struct libusb_config_descriptor *config_desc;
  struct libusb_device_handle *device_handle = NULL;
  struct usb_mouse *pmouse;
  struct usb_mouse *list = NULL;
  int mouse_cnt = 0;
  int i;

  for (i = 0; i < num_devices; i++) {
    dev = device[i];

    err = libusb_get_config_descriptor(dev, 0, &config_desc);
    if (err) {
      fprintf(stderr, "could not get configuration descriptor: %d %s\n", err,
              libusb_strerror(err));
      continue;
    }

    fprintf(stdout, "libusb_get_config_descriptor() ok, bNumInterface = %d\n",
            config_desc->bNumInterfaces);

    int interface = 0;
    for (interface = 0; interface < config_desc->bNumInterfaces; interface++) {
      const struct libusb_interface_descriptor *interface_dsc =
          &config_desc->interface[interface].altsetting[0];
      interface_num = interface_dsc->bInterfaceNumber;

      fprintf(stdout,
              "interface %d: class=0x%02x subclass=0x%02x protocol=0x%02x "
              "endpoints=%d\n",
              interface_num, interface_dsc->bInterfaceClass,
              interface_dsc->bInterfaceSubClass,
              interface_dsc->bInterfaceProtocol, interface_dsc->bNumEndpoints);

      if (interface_dsc->bInterfaceClass != LIBUSB_CLASS_HID ||
          interface_dsc->bInterfaceProtocol != 2) {
        continue;
      }

      fprintf(stdout, "find usb mouse ok, interface = %d\n", interface_num);

      int ep = 0;
      for (; ep < interface_dsc->bNumEndpoints; ep++) {
        const struct libusb_endpoint_descriptor *endpoint_dsc =
            &interface_dsc->endpoint[ep];
        int is_interrupt = (endpoint_dsc->bmAttributes & 3) ==
                           LIBUSB_TRANSFER_TYPE_INTERRUPT;
        int is_in = (endpoint_dsc->bEndpointAddress &
                     LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;

        fprintf(stdout,
                "endpoint 0x%02x: attr=0x%02x max_packet=%d interval=%d\n",
                endpoint_dsc->bEndpointAddress, endpoint_dsc->bmAttributes,
                endpoint_dsc->wMaxPacketSize, endpoint_dsc->bInterval);

        if (!is_interrupt || !is_in) {
          continue;
        }

        fprintf(stdout, "find interrupt in endpoint 0x%02x\n",
                endpoint_dsc->bEndpointAddress);

        endpoint = endpoint_dsc->bEndpointAddress;

        err = libusb_open(dev, &device_handle);
        if (err) {
          fprintf(stderr, "failed to open usb mouse: %d %s\n", err,
                  libusb_strerror(err));
          libusb_free_config_descriptor(config_desc);
          return -1;
        }
        fprintf(stdout, "libusb_open ok\n");

        pmouse = malloc(sizeof(struct usb_mouse));
        if (!pmouse) {
          fprintf(stderr, "can not malloc\n");
          libusb_close(device_handle);
          libusb_free_config_descriptor(config_desc);
          return -1;
        }
        pmouse->endpoint = endpoint;
        pmouse->interface = interface_num;
        pmouse->handle = device_handle;
        pmouse->next = NULL;
        pmouse->packet_size = endpoint_dsc->wMaxPacketSize;

        if (!list) {
          list = pmouse;
        } else {
          pmouse->next = list;
          list = pmouse;
        }
        mouse_cnt++;
        break;
      }
    }

    libusb_free_config_descriptor(config_desc);
  }

  *usb_mouse_list = list;
  return mouse_cnt;
}

static void mouse_irq(struct libusb_transfer *transfer) {
  static int count = 0;
  int err;

  // fprintf(stdout,
  //           "status=%d actual_length=%d requested_length=%d endpoint=0x%02x\n",
  //           transfer->status,
  //           transfer->actual_length,
  //           transfer->length,
  //           transfer->endpoint);

    fprintf(stdout, "%04d datas: ", count++);

  if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
    fprintf(stdout, "%04d datas: ", count++);
    for (int i = 0; i < transfer->actual_length; i++) {
      fprintf(stdout, "%02x ", transfer->buffer[i]);
    }

    static int x = 0;
    static int y = 0;

    int dx = (signed char)transfer->buffer[1];
    int dy = (signed char)transfer->buffer[2];

    x += dx;
    y += dy;

    fprintf(stdout, "dx = %d, dy = %d, x = %d, y = %d\n", dx, dy, x, y);
    fprintf(stdout, "\n");
  } else {
    fprintf(stderr, "transfer status = %d, actual_length = %d\n",
            transfer->status, transfer->actual_length);

  }

  err = libusb_submit_transfer(transfer);
  if (err < 0) {
    fprintf(stderr, "libusb_submit_transfer err: %d %s\n", err,
            libusb_strerror(err));
  }
}

int main(int argc, char **argv) {
  int err;
  libusb_device **devs;
  int num_devices, num_mouse;
  struct usb_mouse *pmouse;

  err = libusb_init(NULL);
  if (err < 0) {
    fprintf(stderr, "failed to initialise libusb %d - %s\n", err,
            libusb_strerror(err));
    exit(1);
  }

  num_devices = libusb_get_device_list(NULL, &devs);
  if (num_devices < 0) {
    fprintf(stderr, "libusb_get_device_list() failed: %d %s\n", num_devices,
            libusb_strerror(num_devices));
    libusb_exit(NULL);
    exit(1);
  }
  fprintf(stdout, "libusb_get_device_list() ok\n");

  num_mouse = get_usb_mouses(devs, num_devices, &usb_mouse_list);
  if (num_mouse <= 0) {
    libusb_free_device_list(devs, 1);
    libusb_exit(NULL);
    exit(1);
  }

  fprintf(stdout, "get %d mouses\n", num_mouse);

  libusb_free_device_list(devs, 1);

  pmouse = usb_mouse_list;
  while (pmouse) {
    libusb_set_auto_detach_kernel_driver(pmouse->handle, 1);
    err = libusb_claim_interface(pmouse->handle, pmouse->interface);
    if (err) {
      fprintf(stderr, "failed to libusb_claim_interface(%d): %d %s\n",
              pmouse->interface, err, libusb_strerror(err));
      exit(1);
    }
    fprintf(stdout, "libusb_claim_interface(%d) ok\n", pmouse->interface);
    pmouse = pmouse->next;
  }

  pmouse = usb_mouse_list;
  while (pmouse) {
    pmouse->transfer = libusb_alloc_transfer(0);
    if (!pmouse->transfer) {
      fprintf(stderr, "libusb_alloc_transfer failed\n");
      exit(1);
    }

    libusb_fill_interrupt_transfer(pmouse->transfer,
                                    pmouse->handle,
                                   pmouse->endpoint,
                                   pmouse->buf,
                                   pmouse->packet_size,
                                   mouse_irq, pmouse, 0);

    err = libusb_submit_transfer(pmouse->transfer);
    if (err < 0) {
      fprintf(stderr, "libusb_submit_transfer endpoint 0x%02x err: %d %s\n",
              pmouse->endpoint, err, libusb_strerror(err));
      exit(1);
    }
    fprintf(stdout, "libusb_submit_transfer endpoint 0x%02x ok\n",
            pmouse->endpoint);

    pmouse = pmouse->next;
  }

  while (1) {
    struct timeval tv = {5, 0};
    int r;

    r = libusb_handle_events_timeout(NULL, &tv);
    if (r < 0) {
      fprintf(stderr, "libusb_handle_events_timeout err: %d %s\n", r,
              libusb_strerror(r));
      break;
    }
  }

  pmouse = usb_mouse_list;
  while (pmouse) {
    libusb_release_interface(pmouse->handle, pmouse->interface);
    libusb_close(pmouse->handle);
    pmouse = pmouse->next;
  }

  free_usb_mouses(usb_mouse_list);

  libusb_exit(NULL);
}
