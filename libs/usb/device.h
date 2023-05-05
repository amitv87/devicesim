#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "host.h"

typedef struct{
  uint8_t num;
  bool is_input;
} usb_device_endpoint_t;

typedef struct usb_device_s{
  void* usr_data;
  usb_host_t* host;
  libusb_device* dev;
  libusb_device_handle* devh;
} usb_device_t;

bool usb_device_init(usb_device_t* device, usb_dev_info_t* info);
void usb_device_deinit(usb_device_t* device);
bool usb_device_open(usb_device_t* device);
bool usb_device_close(usb_device_t* device);
bool usb_device_claim_iface(usb_device_t* device, uint8_t idx);
bool usb_device_release_iface(usb_device_t* device, uint8_t idx);
bool usb_device_match(usb_device_t* device, usb_dev_info_t* info);
int usb_device_find_eps(usb_device_t* device, uint8_t if_no, usb_device_endpoint_t eps[], size_t ep_count);

#endif
