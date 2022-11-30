#include <stdlib.h>
#include "host.h"

// #define PRINT_DEV_DESC

static struct timeval zero_tv = {0};

#ifdef PRINT_DEV_DESC

#define PRINT_DESC(space_width, fmt, ...) fprintf(stderr, "[USB]" "%" #space_width "s" fmt "\r\n", "", ##__VA_ARGS__)
#define IF_READ_STRING(idx) if((libusb_get_string_descriptor_ascii(device_handle, idx, string_desc, sizeof(string_desc)) >= 0 && string_desc[0] != 0) || (string_desc[0] = 0))

static const char* bcd_version_to_string(uint16_t bcdUSB){
  static char string[3 + 1 + 2 + 1 + 2 + 1];
  char *p = string;
  p += sprintf(string, "%u.%u", (bcdUSB >> 8) & 0xff, (bcdUSB >> 4) & 0xf);
  if(bcdUSB & 0xf) sprintf(p, ".%u", bcdUSB & 31);
  return string;
}

static const char* libusb_speed_to_speed(enum libusb_speed speed){
  switch(speed){
    case LIBUSB_SPEED_LOW: return "1.5M";
    case LIBUSB_SPEED_FULL: return "12M";
    case LIBUSB_SPEED_HIGH: return "480M";
    case LIBUSB_SPEED_SUPER: return "5G";
    case LIBUSB_SPEED_SUPER_PLUS: return "10G";
  }
  return "?";
}

static void print_desc(libusb_device *dev){
  int ret;
  unsigned char string_desc[256];
  struct libusb_device_descriptor dd = {};
  libusb_device_handle* device_handle = NULL;

  CHK_USB_ERR(libusb_get_device_descriptor, dev, &dd); if(ret != 0 ) goto end;

  uint8_t bus = libusb_get_bus_number(dev), addr = libusb_get_device_address(dev), speed = libusb_get_device_speed(dev);

  PRINT_DESC(1, "Bus[%hhu].Addr[%hhu]: vid 0x%04x, pid 0x%04x, USB%s @ %sbps (%s speed)", bus, addr,
    dd.idVendor, dd.idProduct, bcd_version_to_string(dd.bcdUSB), libusb_speed_to_speed(speed), ENUM_TO_STR(libusb_speed, speed));

  CHK_USB_ERR(libusb_open, dev, &device_handle); if(ret != 0 ) goto end;

  IF_READ_STRING(dd.iManufacturer)  PRINT_DESC(3, "Manufacturer: %s", string_desc);
  IF_READ_STRING(dd.iProduct)       PRINT_DESC(3, "Product: %s", string_desc);
  IF_READ_STRING(dd.iSerialNumber)  PRINT_DESC(3, "SerialNumber: %s", string_desc);

  for(uint8_t cf_num = 0; cf_num < dd.bNumConfigurations; cf_num++){
    struct libusb_config_descriptor* cdesc = NULL;
    CHK_USB_ERR(libusb_get_config_descriptor, dev, cf_num, &cdesc);
    if(!cdesc) continue;
    uint16_t max_current = cdesc->MaxPower * (dd.bcdUSB >= 0x0300 ? 8 : 2);
    IF_READ_STRING(cdesc->iConfiguration){}
    PRINT_DESC(3, "Configuration[%u]: %s @ %umA", cf_num, string_desc, max_current);
    for(int if_num = 0; if_num < cdesc->bNumInterfaces; if_num++){
      PRINT_DESC(5, "Interface[%d]: ", if_num);
      for(int alt_num = 0; alt_num < cdesc->interface[if_num].num_altsetting; alt_num++){
        IF_READ_STRING(cdesc->interface[if_num].altsetting[alt_num].iInterface){}
        PRINT_DESC(7, "Altsetting[%d]: %s", alt_num, string_desc);
        PRINT_DESC(9, "InterfaceClass: %s", ENUM_TO_STR(libusb_class_code, cdesc->interface[if_num].altsetting[alt_num].bInterfaceClass));
        PRINT_DESC(9, "Class.SubClass.Protocol: %02X.%02X.%02X", cdesc->interface[if_num].altsetting[alt_num].bInterfaceClass,
          cdesc->interface[if_num].altsetting[alt_num].bInterfaceSubClass, cdesc->interface[if_num].altsetting[alt_num].bInterfaceProtocol);
        for(int ep_num = 0; ep_num < cdesc->interface[if_num].altsetting[alt_num].bNumEndpoints; ep_num++){
          const struct libusb_endpoint_descriptor *endpoint = NULL;
          endpoint = &cdesc->interface[if_num].altsetting[alt_num].endpoint[ep_num];
          PRINT_DESC(11, "Endpoint[%d]: 0x%02X %s %s %u bytes", ep_num, endpoint->bEndpointAddress,
            ENUM_TO_STR(libusb_endpoint_direction, endpoint->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK),
            ENUM_TO_STR(libusb_endpoint_transfer_type, endpoint->bmAttributes & 0x03),
            endpoint->wMaxPacketSize
          );
          // PRINT_DESC(11, "Interval: %u", endpoint->bInterval);
          struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
          libusb_get_ss_endpoint_companion_descriptor(NULL, endpoint, &ep_comp);
          if(ep_comp){
            PRINT_DESC(11, "MaxBurst: 0x%02X   (USB 3.0)", ep_comp->bMaxBurst);
            PRINT_DESC(11, "BytesPerInterval: 0x%04X (USB 3.0)", ep_comp->wBytesPerInterval);
            libusb_free_ss_endpoint_companion_descriptor(ep_comp);
          }
        }
      }
    }

    libusb_free_config_descriptor(cdesc);
  }

  end:
  if(device_handle) libusb_close(device_handle);
}
#endif

static void on_hotplug_safe(usb_host_t* host, libusb_device *dev, libusb_hotplug_event event){
  uint8_t bus = libusb_get_bus_number(dev);
  uint8_t addr = libusb_get_device_address(dev);
  struct libusb_device_descriptor dd = {};
  int ret;
  CHK_USB_ERR(libusb_get_device_descriptor, dev, &dd); if(ret != 0 ) return;
  usb_dev_info_t info = {
    .dev = dev,
    .bus = bus,
    .addr = addr,
    .vid = dd.idVendor,
    .pid = dd.idProduct,
  };

  #ifdef PRINT_DEV_DESC
  if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) print_desc(dev);
  #endif

  host->on_device(host, &info, event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
}

static int on_hotplug(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data){
  libusb_device_wrapped_t* wdev = malloc(sizeof(libusb_device_wrapped_t));
  *wdev = (libusb_device_wrapped_t){
    .dev = libusb_ref_device(dev),
    .event = event,
  };
  usb_host_t* host = user_data;
  LIST_ADD(&host->hotplug_list, wdev);
  return 0;
}

static void on_pollfd_removed(int fd, void *user_data){
  LOG("fd: %d", fd);
  usb_host_t* host = user_data;
  host->input_handle.fd = fd;
  io_dereg_handle(&host->input_handle);
}

static void on_pollfd_added(int fd, short events, void *user_data){
  LOG("fd: %d, events: %hd", fd, events);
  usb_host_t* host = user_data;
  host->input_handle.fd = fd;
  io_reg_handle(&host->input_handle);
}

static void on_usb_fd_activity(io_handle_t* handle, uint8_t fd_mode_mask){
  usb_host_t* host = handle->usr_data;
  libusb_handle_events_timeout(host->context, &zero_tv);

  while(host->hotplug_list){
    libusb_device_wrapped_t* wdev = host->hotplug_list;
    host->hotplug_list = wdev->item.next;
    on_hotplug_safe(host, wdev->dev, wdev->event);
    libusb_unref_device(wdev->dev);
    free(wdev);
  }
}

void usb_host_init(usb_host_t* host){
  if(host->context) return;
  int ret;
  host->input_handle = (io_handle_t){.cb = on_usb_fd_activity, .fd_mode_mask = FD_READ | FD_EXCEPT, .usr_data = host};
  CHK_USB_ERR(libusb_init, &host->context);
}

void usb_host_start(usb_host_t* host){
  if(host->hotplug_handle) return;
  libusb_set_pollfd_notifiers(host->context, on_pollfd_added, on_pollfd_removed, host);
  const struct libusb_pollfd** pollfds = libusb_get_pollfds(host->context);
  for(const struct libusb_pollfd** if_num = pollfds; *if_num; if_num++) on_pollfd_added((*if_num)->fd, 0, host);
  free(pollfds);

  int ret;
  CHK_USB_ERR(libusb_pollfds_handle_timeouts, host->context);
  CHK_USB_ERR(libusb_hotplug_register_callback, host->context, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
    LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, on_hotplug, host, &host->hotplug_handle);
}

void usb_host_deinit(usb_host_t* host){
  if(!host->context) return;
  if(host->hotplug_handle) libusb_hotplug_deregister_callback(host->context, host->hotplug_handle);
  libusb_exit(host->context);
  host->context = 0;
  host->hotplug_handle = 0;
}
