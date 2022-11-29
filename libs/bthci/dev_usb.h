#ifndef HCI_DEVICE_H
#define HCI_DEVICE_H

#include "../usb/transfer.h"

typedef struct hci_usb_device_s hci_usb_device_t;

typedef int (*hci_output)(hci_usb_device_t* hci_dev, uint8_t* data, size_t length);

typedef struct hci_usb_device_s{
  void *usr_data;
  usb_device_t usb_device;
  union{
    struct{
      usb_transfer_t tx_cmd, tx_acl, rx_evt, rx_acl;
    };
    usb_transfer_t xfers[4];
  };
  hci_output output;
  io_buff_t input_buff;
  size_t rem_acl_bytes, rem_evt_bytes;
  uint8_t buff[2][2048];
  uint8_t __input_buff[2048];
} hci_usb_device_t;

bool hci_usb_device_init(hci_usb_device_t* hci_dev, usb_dev_info_t* info);
bool hci_usb_device_deinit(hci_usb_device_t* hci_dev);
void hci_usb_device_input(hci_usb_device_t* hci_dev, uint8_t* data, size_t length);

#endif
