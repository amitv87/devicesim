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
bool usb_device_reset(usb_device_t* device);
bool usb_device_claim_iface(usb_device_t* device, uint8_t idx);
bool usb_device_release_iface(usb_device_t* device, uint8_t idx);
bool usb_device_match(usb_device_t* device, usb_dev_info_t* info);
int usb_device_find_eps(usb_device_t* device, uint8_t if_no, usb_device_endpoint_t eps[], size_t ep_count);

// Synchronous control transfer with a caller-supplied setup packet (vendor /
// class / standard). `request_type` is the bmRequestType (direction | type |
// recipient). Returns bytes transferred (>= 0) or a negative libusb error.
int usb_device_control_transfer(usb_device_t* device, uint8_t request_type, uint8_t request,
  uint16_t value, uint16_t index, uint8_t* data, uint16_t length, unsigned int timeout);

// Synchronous bulk transfer (direction implied by endpoint's IN/OUT bit).
// Returns bytes transferred (>= 0) or a negative libusb error.
int usb_device_bulk_transfer(usb_device_t* device, uint8_t endpoint,
  uint8_t* data, int length, unsigned int timeout);

#endif
