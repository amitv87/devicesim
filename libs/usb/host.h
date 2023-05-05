#ifndef USB_HOST_H
#define USB_HOST_H

#include <libusb.h>
#include "../util/io.h"

#define CHK_USB_ERR(func, ...) if((ret = func(__VA_ARGS__))){LOG(#func " ret: %d, err: %s", ret, libusb_strerror(ret));}

#define ENUM_TO_STR_FUNC_NAME(enum_name) enum_name##_to_str
#define ENUM_TO_STR(enum_name, ...) ENUM_TO_STR_FUNC_NAME(enum_name)(__VA_ARGS__)
#define ENUM_TO_STR_FUNC(enum_name, ...) const char* ENUM_TO_STR_FUNC_NAME(enum_name)(enum enum_name val)

ENUM_TO_STR_FUNC(libusb_class_code);
ENUM_TO_STR_FUNC(libusb_descriptor_type);
ENUM_TO_STR_FUNC(libusb_endpoint_direction);
ENUM_TO_STR_FUNC(libusb_endpoint_transfer_type);
ENUM_TO_STR_FUNC(libusb_standard_request);
ENUM_TO_STR_FUNC(libusb_request_type);
ENUM_TO_STR_FUNC(libusb_request_recipient);
ENUM_TO_STR_FUNC(libusb_iso_sync_type);
ENUM_TO_STR_FUNC(libusb_iso_usage_type);
ENUM_TO_STR_FUNC(libusb_supported_speed);
ENUM_TO_STR_FUNC(libusb_bos_type);
ENUM_TO_STR_FUNC(libusb_speed);

typedef struct usb_host_s usb_host_t;

typedef struct{
  const char* name;
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
