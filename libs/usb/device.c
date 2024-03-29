#include "device.h"

static bool is_device(libusb_device* dev, usb_dev_info_t* info){
  struct libusb_device_descriptor dd = {};
  uint8_t bus = libusb_get_bus_number(dev);
  uint8_t addr = libusb_get_device_address(dev);
  libusb_get_device_descriptor(dev, &dd);
  if(info->bus && info->bus != bus) return false;
  if(info->addr && info->addr != addr) return false;
  if(info->vid && info->vid != dd.idVendor) return false;
  if(info->pid && info->pid != dd.idProduct) return false;
  return true;
}

bool usb_device_init(usb_device_t* device, usb_dev_info_t* info){
  bool rc = false;
  if(device->dev) goto end;
  if(info->dev) device->dev = libusb_ref_device(info->dev);
  else{
    libusb_device* dev = NULL;
    libusb_device** devs = NULL;
    int rc = libusb_get_device_list(device->host->context, &devs);
    for(uint8_t i = 0; i < rc; i++){
      dev = devs[i];
      if(!is_device(dev, info)) continue;
      device->dev = libusb_ref_device(dev);
      break;
    }
    if(devs) libusb_free_device_list(devs, true);
  }
  rc = device->dev != NULL;
  end:
  return rc;
}

void usb_device_deinit(usb_device_t* device){
  if(!device->dev) return;
  usb_device_close(device);
  libusb_unref_device(device->dev);
  device->dev = NULL;
}

bool usb_device_open(usb_device_t* device){
  bool rc = false;
  if(!device->dev || device->devh) goto end;
  int ret;
  CHK_USB_ERR(libusb_open, device->dev, &device->devh);
  CHK_USB_ERR(libusb_set_auto_detach_kernel_driver, device->devh, 1);
  rc = device->devh != NULL;
  end:
  return rc;
}

bool usb_device_close(usb_device_t* device){
  bool rc = false;
  if(!device->devh) goto end;
  libusb_close(device->devh);
  device->devh = NULL;
  rc = device->devh == NULL;
  end:
  return rc;
}

bool usb_device_claim_iface(usb_device_t* device, uint8_t idx){
  bool rc = false;
  if(!device->devh) goto end;
  int ret;
  CHK_USB_ERR(libusb_claim_interface, device->devh, idx);
  rc = ret == 0;
  end:
  return rc;
}

bool usb_device_release_iface(usb_device_t* device, uint8_t idx){
  bool rc = false;
  if(!device->devh) goto end;
  int ret;
  CHK_USB_ERR(libusb_release_interface, device->devh, idx);
  rc = ret == 0;
  end:
  return rc;
}

bool usb_device_match(usb_device_t* device, usb_dev_info_t* info){
  if(!device->dev) return false;
  return is_device(device->dev, info);
}

int usb_device_find_eps(usb_device_t* device, uint8_t if_no, usb_device_endpoint_t eps[], size_t ep_count){
  int rc = -1;
  if(!device->devh || ep_count <= 0) goto end;
  struct libusb_device_descriptor dd = {0};
  libusb_get_device_descriptor(device->dev, &dd);
  for(uint8_t cf_num = 0; cf_num < dd.bNumConfigurations; cf_num++){
    bool should_break = false;
    struct libusb_config_descriptor* cdesc = NULL;
    libusb_get_config_descriptor(device->dev, cf_num, &cdesc);
    if(!cdesc) continue;
    for(int if_num = 0; if_num < cdesc->bNumInterfaces; if_num++){
      if(if_no != cdesc->interface[if_num].altsetting[0].bInterfaceNumber) continue;
      rc = 0;
      for(int ep_num = 0; ep_num < cdesc->interface[if_num].altsetting[0].bNumEndpoints; ep_num++){
        const struct libusb_endpoint_descriptor *endpoint = NULL;
        endpoint = &cdesc->interface[if_num].altsetting[0].endpoint[ep_num];
        eps[rc++] = (usb_device_endpoint_t){
          .num = endpoint->bEndpointAddress,
          .is_input = (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) > 0,
        };
        if(rc >= ep_count) break;
      }
      should_break = true;
      break;
    }

    libusb_free_config_descriptor(cdesc);
    if(should_break) break;
  }

  end:
  return rc;
}
