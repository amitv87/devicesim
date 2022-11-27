#ifndef USB_TRANSFER_H
#define USB_TRANSFER_H

#include "device.h"

typedef struct usb_transfer_s usb_transfer_t;

typedef void (*usb_on_transfer)(usb_transfer_t* transfer, uint8_t* data, size_t length);

typedef struct usb_transfer_s{
  void* usr_data;
  int stop_flag;
  bool is_tx_active;
  size_t max_pkt_size;
  usb_device_t* device;
  usb_on_transfer on_transfer;
  struct libusb_transfer* xfer;
} usb_transfer_t;

bool usb_transfer_init(usb_transfer_t* transfer, uint8_t endpoint);
bool usb_transfer_deinit(usb_transfer_t* transfer);
bool usb_transfer_submit(usb_transfer_t* transfer, uint8_t* data, size_t length);

#endif
