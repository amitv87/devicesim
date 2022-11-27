#ifndef USB_HOST_H
#define USB_HOST_H

#include <libusb.h>
#include "../util/io.h"

#define CHK_USB_ERR(func, ...) if((ret = func(__VA_ARGS__))){LOG(#func " ret: %d, err: %s", ret, libusb_strerror(ret));}

typedef struct usb_host_s usb_host_t;

typedef struct{
  libusb_device *dev;
  uint8_t bus, addr;
  uint16_t vid, pid;
} usb_dev_info_t;

typedef struct{
  list_item_t item;
  libusb_device *dev;
  libusb_hotplug_event event;
} libusb_device_wrapped_t;

typedef void (*on_usb_device)(usb_host_t* host, usb_dev_info_t *dev_info, bool added);

typedef struct usb_host_s{
  void* usr_data;
  bool is_in_cb;
  io_handle_t input_handle;
  on_usb_device on_device;
  libusb_context* context;
  libusb_hotplug_callback_handle hotplug_handle;
  libusb_device_wrapped_t* hotplug_list;
} usb_host_t;

void usb_host_init(usb_host_t* host);
void usb_host_start(usb_host_t* host);
void usb_host_deinit(usb_host_t* host);

#endif
