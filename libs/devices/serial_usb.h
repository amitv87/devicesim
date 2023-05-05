#ifndef DEVICE_SERIAL_USB_H
#define DEVICE_SERIAL_USB_H

#include "../usb/transfer.h"

typedef struct serial_usb_device_s serial_usb_device_t;

typedef int (*serial_output)(serial_usb_device_t* serial_dev, uint8_t* data, size_t length);

typedef struct{
  uint8_t if_no, tx_ep, rx_ep;
} serial_usb_device_descriptor_t;

typedef struct serial_usb_device_s{
  void *usr_data;
  usb_device_t usb_device;
  union{
    struct{
      usb_transfer_t rx, tx;
    };
    usb_transfer_t xfers[2];
  };
  int8_t if_no;
  serial_output output;
  uint8_t buff[1][2048];
} serial_usb_device_t;

bool serial_usb_device_init(serial_usb_device_t* serial_dev, usb_dev_info_t* info, uint8_t if_no);
bool serial_usb_device_deinit(serial_usb_device_t* serial_dev);
void serial_usb_device_input(serial_usb_device_t* serial_dev, uint8_t* data, size_t length);

#endif
