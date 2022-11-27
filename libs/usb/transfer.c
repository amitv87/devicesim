#include "transfer.h"

static void on_transfer_complete(struct libusb_transfer *xfer){
  usb_transfer_t* transfer = xfer->user_data;
  if(transfer->is_tx_active){
    if(transfer->on_transfer) transfer->on_transfer(transfer, xfer->buffer, xfer->actual_length);
    if(xfer->endpoint & LIBUSB_ENDPOINT_IN) transfer->is_tx_active = libusb_submit_transfer(xfer) == 0;
  }

  // LOG("ep: %02x, status: %u, active: %hhu", xfer->endpoint, xfer->status, transfer->is_tx_active);

  transfer->stop_flag = 1;
}

static bool get_transfer_info(usb_transfer_t* transfer){
  int ret;
  bool rc = false;
  struct libusb_config_descriptor* conf_desc = NULL;
  CHK_USB_ERR(libusb_get_active_config_descriptor, transfer->device->dev, &conf_desc);
  if(ret || !conf_desc){
    rc = false;
    goto end;
  }
  
  for(uint8_t i = 0; i < conf_desc->bNumInterfaces; i++){
    for(uint8_t j = 0; j < conf_desc->interface[i].num_altsetting; j++){
      for(uint8_t k = 0; k < conf_desc->interface[i].altsetting[j].bNumEndpoints; k++){
        const struct libusb_endpoint_descriptor* endpoint = &conf_desc->interface[i].altsetting[j].endpoint[k];
        if(transfer->xfer->endpoint == endpoint->bEndpointAddress){
          transfer->xfer->type = endpoint->bmAttributes & 0x03;
          transfer->max_pkt_size = endpoint->wMaxPacketSize;
          rc = true;
          goto done;
        }
      }
    }
  }
  done:
  libusb_free_config_descriptor(conf_desc);

  end:
  return rc;
}

bool usb_transfer_init(usb_transfer_t* transfer, uint8_t endpoint){
  bool rc = false;
  struct libusb_transfer* xfer = transfer->xfer;
  if(xfer) goto end;
  transfer->xfer = xfer = libusb_alloc_transfer(0);
  if(!xfer) goto end;

  transfer->is_tx_active = false;

  xfer->dev_handle = transfer->device->devh;
  xfer->callback = on_transfer_complete;
  xfer->user_data = transfer;
  xfer->timeout = 0;
  xfer->endpoint = endpoint;
  xfer->flags = 0;
  xfer->num_iso_packets = 0;

  if(xfer->endpoint != 0) rc = get_transfer_info(transfer);
  else{
    rc = true;
    transfer->max_pkt_size = 64;
    xfer->timeout = 1000;
    xfer->type = LIBUSB_TRANSFER_TYPE_CONTROL;
  }

  // LOG("ep 0x%02X, rc: %d, ttype: %hhu, max_pkt_size: %zu", xfer->endpoint, rc, xfer->type, transfer->max_pkt_size);

  if(!rc){
    libusb_free_transfer(xfer);
    transfer->xfer = xfer = NULL;
  }

  end:
  return rc;
}

bool usb_transfer_deinit(usb_transfer_t* transfer){
  bool rc = false;
  struct libusb_transfer* xfer = transfer->xfer;
  if(!xfer) goto end;

  // LOG("ep: %02x, status: %u, active: %hhu", xfer->endpoint, transfer->xfer->status, transfer->is_tx_active);

  if(transfer->is_tx_active && xfer->endpoint & LIBUSB_ENDPOINT_IN){
    transfer->is_tx_active = false, transfer->stop_flag = 0;
    int ret;
    CHK_USB_ERR(libusb_cancel_transfer, xfer);
    if(ret == 0){
      CHK_USB_ERR(libusb_handle_events_completed, transfer->device->host->context, &transfer->stop_flag);
    }
    else{
      struct timeval tv = {.tv_sec = 1};
      CHK_USB_ERR(libusb_handle_events_timeout, transfer->device->host->context, &tv);
    }
  }
  libusb_free_transfer(xfer);
  transfer->xfer = NULL;

  end:
  rc = transfer->xfer == NULL;
  return rc;
}

bool usb_transfer_submit(usb_transfer_t* transfer, uint8_t* data, size_t length){
  bool rc = false;
  int ret = -1;
  int out_len = 0;
  struct libusb_transfer* xfer = transfer->xfer;
  if(!xfer) goto end;

  if((xfer->endpoint & LIBUSB_ENDPOINT_IN)){
    xfer->buffer = data;
    xfer->length = length;
    CHK_USB_ERR(libusb_submit_transfer, xfer);
    transfer->is_tx_active = ret == 0;
  }
  else if(xfer->type == LIBUSB_TRANSFER_TYPE_CONTROL){
    ret = libusb_control_transfer(xfer->dev_handle, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE, 0, 0, 0, data, length, xfer->timeout);
    if(ret < 0){
      LOG("libusb_control_transfer ret: %d, err: %s", ret, libusb_strerror(ret));
    }
    else out_len = ret;
  }
  else if(xfer->type == LIBUSB_TRANSFER_TYPE_BULK){
    CHK_USB_ERR(libusb_bulk_transfer, xfer->dev_handle, xfer->endpoint, data, length, &out_len, xfer->timeout);
  }
  else if(xfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT){
    CHK_USB_ERR(libusb_interrupt_transfer, xfer->dev_handle, xfer->endpoint, data, length, &out_len, xfer->timeout);
  }

  rc = ret >= 0;

  end:
  // LOG("libusb_submit_transfer ep: %02X, out_len: %d, rc: %d", xfer->endpoint, out_len, rc);
  return rc;
}
