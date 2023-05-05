#include "serial_usb.h"

#define FUN_CHK(fun, ...) if(!fun(__VA_ARGS__)){LOG("%s failed", #fun); goto end;}

static void on_transfer(usb_transfer_t* transfer, uint8_t* data, size_t length){
  serial_usb_device_t* serial_dev = transfer->usr_data;
  int rc = serial_dev->output(serial_dev, data, length);
}

static bool init_transfer(serial_usb_device_t* serial_dev, usb_transfer_t* transfer, uint8_t endpoint, int buff_idx){
  bool rc = false;

  *transfer = (usb_transfer_t){
    .usr_data = serial_dev,
    .on_transfer = on_transfer,
    .device = &serial_dev->usb_device,
  };

  FUN_CHK(usb_transfer_init, transfer, endpoint);

  if(buff_idx >= 0){
    FUN_CHK(usb_transfer_submit, transfer, serial_dev->buff[buff_idx] + 1, sizeof(serial_dev->buff[buff_idx]) - 1);
  }

  rc = true;
  end:
  return rc;
}

bool serial_usb_device_init(serial_usb_device_t* serial_dev, usb_dev_info_t* info, uint8_t if_no){
  bool rc = false;
  if(serial_dev->usb_device.usr_data) goto end;
  void* host = serial_dev->usb_device.host;
  serial_dev->if_no = -1;
  serial_dev->usb_device = (usb_device_t){0};
  serial_dev->usb_device.host = host;
  FUN_CHK(usb_device_init, &serial_dev->usb_device, info);
  FUN_CHK(usb_device_open, &serial_dev->usb_device);

  usb_device_endpoint_t eps[4];

  int ep_count = usb_device_find_eps(&serial_dev->usb_device, if_no, eps, countof(eps));

  LOG("usb_device_find_eps: %d", ep_count);

  FUN_CHK(usb_device_claim_iface, &serial_dev->usb_device, if_no);

  serial_dev->if_no = if_no;

  int rx_ep = -1, tx_ep = -1;

  while(ep_count > 0 && (rx_ep < 0 || tx_ep < 0)){
    usb_device_endpoint_t* ep = &eps[ep_count -= 1];

    if(ep->is_input){
      if(rx_ep < 0) rx_ep = ep->num;
    }
    else if(tx_ep < 0) tx_ep = ep->num;
  }

  LOG("rx_ep: %d, tx_ep: %d", rx_ep, tx_ep);

  if(rx_ep >= 0 && tx_ep >= 0){
    FUN_CHK(init_transfer, serial_dev, &serial_dev->rx, rx_ep,  0);
    FUN_CHK(init_transfer, serial_dev, &serial_dev->tx, tx_ep, -1);
    rc = true;
  }

  if(rc) serial_dev->usb_device.usr_data = serial_dev;
  else{
    if(serial_dev->if_no >= 0) usb_device_release_iface(&serial_dev->usb_device, serial_dev->if_no);
    usb_device_deinit(&serial_dev->usb_device);
  }

  end:
  return rc;
}

bool serial_usb_device_deinit(serial_usb_device_t* serial_dev){
  if(!serial_dev->usb_device.usr_data) return false;
  for(int i = 0; i < countof(serial_dev->xfers); i++) usb_transfer_deinit(&serial_dev->xfers[i]);
  usb_device_release_iface(&serial_dev->usb_device, serial_dev->if_no);
  usb_device_close(&serial_dev->usb_device);
  usb_device_deinit(&serial_dev->usb_device);
  serial_dev->usb_device.usr_data = NULL;
  return true;
}

void serial_usb_device_input(serial_usb_device_t* serial_dev, uint8_t* data, size_t length){
  if(!serial_dev->usb_device.usr_data) return;
  size_t sent = 0;
  while(sent < length){
    size_t to_send = min(serial_dev->tx.max_pkt_size, length - sent);
    bool rc = usb_transfer_submit(&serial_dev->tx, data + sent, to_send);
    sent += to_send;
    // LOG("submit rc: %u, to_send: %zu, sent: %zu, pkt_size: %zu", rc, to_send, sent, pkt_size);
  }
}
