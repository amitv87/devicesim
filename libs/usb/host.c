#include <stdlib.h>
#include "host.h"

// #define USB_DEBUG

#define PRINT_DESC(fmt, ...) fprintf(stderr, "[USB]" fmt "\r\n", ##__VA_ARGS__)
#define IF_READ_STRING(idx) if(libusb_get_string_descriptor_ascii(device_handle, idx, string_desc, sizeof(string_desc)) >= 0)

static struct timeval zero_tv = {0};

#ifdef USB_DEBUG
static void print_desc(libusb_device_handle *device_handle, struct libusb_device_descriptor* dd, struct libusb_config_descriptor* cdesc){
  unsigned char string_desc[256];
  IF_READ_STRING(dd->iManufacturer)
    PRINT_DESC("              manufacturer: %s", string_desc);
  IF_READ_STRING(dd->iProduct)
    PRINT_DESC("                   product: %s", string_desc);
  IF_READ_STRING(dd->iSerialNumber)
    PRINT_DESC("                    serial: %s", string_desc);
  IF_READ_STRING(cdesc->iConfiguration)
    PRINT_DESC("                descriptor: %s", string_desc);

  for(int if_num = 0; if_num < cdesc->bNumInterfaces; if_num++){
    PRINT_DESC("              interface[%d]: id = %d", if_num, cdesc->interface[if_num].altsetting[0].bInterfaceNumber);
    for(int alt_num=0; alt_num<cdesc->interface[if_num].num_altsetting; alt_num++){
      PRINT_DESC("interface[%d].altsetting[%d]: num endpoints = %d", if_num, alt_num, cdesc->interface[if_num].altsetting[alt_num].bNumEndpoints);
      PRINT_DESC("   Class.SubClass.Protocol: %02X.%02X.%02X", cdesc->interface[if_num].altsetting[alt_num].bInterfaceClass,
        cdesc->interface[if_num].altsetting[alt_num].bInterfaceSubClass, cdesc->interface[if_num].altsetting[alt_num].bInterfaceProtocol);

      IF_READ_STRING(cdesc->interface[if_num].altsetting[alt_num].iInterface)
        PRINT_DESC("               string_desc: %s", string_desc);

      for(int ep_num=0; ep_num<cdesc->interface[if_num].altsetting[alt_num].bNumEndpoints; ep_num++){
        const struct libusb_endpoint_descriptor *endpoint = NULL;
        struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
        endpoint = &cdesc->interface[if_num].altsetting[alt_num].endpoint[ep_num];
        PRINT_DESC("       endpoint[%d].address: 0x%02X", ep_num, endpoint->bEndpointAddress);
        PRINT_DESC("           descriptor type: 0x%02X", endpoint->bDescriptorType);
        PRINT_DESC("             transfer type: 0x%02X", endpoint->bmAttributes & 0x03);
        PRINT_DESC("           max packet size: 0x%04X", endpoint->wMaxPacketSize);
        PRINT_DESC("          polling interval: 0x%02X", endpoint->bInterval);
        libusb_get_ss_endpoint_companion_descriptor(NULL, endpoint, &ep_comp);
        if(ep_comp){
          PRINT_DESC("                 max burst: 0x%02X   (USB 3.0)", ep_comp->bMaxBurst);
          PRINT_DESC("        bytes per interval: 0x%04X (USB 3.0)", ep_comp->wBytesPerInterval);
          libusb_free_ss_endpoint_companion_descriptor(ep_comp);
        }
      }
    }
  }
}
#endif

static void on_hotplug_safe(usb_host_t* host, libusb_device *dev, libusb_hotplug_event event){
  uint8_t bus = libusb_get_bus_number(dev);
  uint8_t addr = libusb_get_device_address(dev);
  struct libusb_device_descriptor dd = {};
  libusb_get_device_descriptor(dev, &dd);

  #ifdef USB_DEBUG
  const char* evt = event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED ? "added" : "removed";
  LOG("%s %p -> bus: %hhu, addr: %hhu, vid: %04x, pid: %04x, bus: %hhu, addr: %hhu", evt, dev, bus, addr, dd.idVendor, dd.idProduct, bus, addr);
  if(event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED){
    struct libusb_config_descriptor* cdesc = NULL;
    libusb_device_handle* device_handle = NULL;
    int ret;
    CHK_USB_ERR(libusb_open, dev, &device_handle);
    for(uint8_t if_num = 0; if_num < dd.bNumConfigurations; if_num++){
      cdesc = NULL;
      CHK_USB_ERR(libusb_get_config_descriptor, dev, if_num, &cdesc);
      if(cdesc){
        print_desc(device_handle, &dd, cdesc);
        libusb_free_config_descriptor(cdesc);
      }
    }
    if(device_handle) libusb_close(device_handle);
  }
  #endif

  usb_dev_info_t info = {
    .dev = dev,
    .bus = bus,
    .addr = addr,
    .vid = dd.idVendor,
    .pid = dd.idProduct,
  };

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
