#include "hci_usb.h"

#define DEFAULT_HCI_IF_NO (0x00)

#define FUN_CHK(fun, ...) if(!fun(__VA_ARGS__)){LOG("%s failed", #fun); goto end;}

#define ELIF_PKT else IF_PKT
#define IF_PKT(typ, pkt_struct, xyz, ...) if(pkt->type == typ){       \
  if(buff->w_idx >= sizeof(hci_pkt_t) + sizeof(pkt_struct)){          \
    pkt_struct* xyz = (pkt_struct*)(pkt->data);                       \
    pkt_length = sizeof(hci_pkt_t) + sizeof(pkt_struct) + xyz->length;\
    if(buff->w_idx >= pkt_length){__VA_ARGS__}                        \
  }                                                                   \
}

typedef enum{
  HCI_UNK = 0x00,
  HCI_CMD = 0x01,
  HCI_ACL = 0x02,
  HCI_SCO = 0x03,
  HCI_EVT = 0x04,
  HCI_ECMD = 0x09,
} hci_pkt_type;

typedef struct{
  uint8_t type;
  uint8_t data[];
} __PACKED__ hci_pkt_t;

typedef struct{
  uint16_t opcode;
  uint8_t length;
} __PACKED__ hci_cmd_t;

typedef struct{
  uint16_t handle : 12;
  uint8_t pbflag  : 2;
  uint8_t bcflag  : 2;
  uint16_t length;
} __PACKED__ hci_acl_t;

typedef struct{
  uint16_t opcode  : 12;
  uint8_t reserved : 4;
  uint8_t length;
} __PACKED__ hci_sco_t;

typedef struct{
  uint8_t opcode;
  uint8_t length;
} __PACKED__ hci_evt_t;

static void on_transfer(usb_transfer_t* transfer, uint8_t* data, size_t length){
  hci_usb_device_t* hci_dev = transfer->usr_data;

  hci_pkt_type type = data[-1];
  // LOG("transfer: %p, type: %u, length: %zu", transfer, type, length);

  switch(type){
    case HCI_ACL:{
      hci_acl_t* pkt = (hci_acl_t*)(data);
      if(hci_dev->rem_acl_bytes == 0){
        hci_dev->rem_acl_bytes = sizeof(hci_acl_t) + pkt->length - length;
        data -= 1, length += 1;
      }
      else hci_dev->rem_acl_bytes -= length;
      break;
    }
    case HCI_EVT:{
      hci_evt_t* pkt = (hci_evt_t*)(data);
      if(hci_dev->rem_evt_bytes == 0){
        hci_dev->rem_evt_bytes = sizeof(hci_evt_t) + pkt->length - length;
        data -= 1, length += 1;
      }
      else hci_dev->rem_evt_bytes -= length;
      break;
    }
    default: return;
  }

  int rc = hci_dev->output(hci_dev, data, length);
}

static bool init_transfer(hci_usb_device_t* hci_dev, usb_transfer_t* transfer, hci_pkt_type type, uint8_t endpoint, int buff_idx){
  bool rc = false;

  *transfer = (usb_transfer_t){
    .usr_data = hci_dev,
    .on_transfer = on_transfer,
    .device = &hci_dev->usb_device,
  };

  FUN_CHK(usb_transfer_init, transfer, endpoint);

  if(buff_idx >= 0){
    hci_dev->buff[buff_idx][0] = type;
    FUN_CHK(usb_transfer_submit, transfer, hci_dev->buff[buff_idx] + 1, sizeof(hci_dev->buff[buff_idx]) - 1);
  }

  rc = true;
  end:
  return rc;
}

bool hci_usb_device_init(hci_usb_device_t* hci_dev, usb_dev_info_t* info){
  bool rc = false;
  if(hci_dev->usb_device.usr_data) goto end;
  void* host = hci_dev->usb_device.host;
  hci_dev->usb_device = (usb_device_t){0};
  hci_dev->usb_device.host = host;
  FUN_CHK(usb_device_init, &hci_dev->usb_device, info);
  FUN_CHK(usb_device_open, &hci_dev->usb_device);
  FUN_CHK(usb_device_claim_iface, &hci_dev->usb_device, DEFAULT_HCI_IF_NO);

  hci_dev->input_buff = (io_buff_t){
    .bytes = hci_dev->__input_buff,
    .len = sizeof(hci_dev->__input_buff),
  };

  hci_dev->rem_acl_bytes = hci_dev->rem_evt_bytes = 0;

  FUN_CHK(init_transfer, hci_dev, &hci_dev->tx_cmd, HCI_CMD, 0x00, -1);
  FUN_CHK(init_transfer, hci_dev, &hci_dev->tx_acl, HCI_ACL, 0x02, -1);
  FUN_CHK(init_transfer, hci_dev, &hci_dev->rx_evt, HCI_EVT, 0x81, 0);
  FUN_CHK(init_transfer, hci_dev, &hci_dev->rx_acl, HCI_ACL, 0x82, 1);

  rc = true;

  if(rc) hci_dev->usb_device.usr_data = hci_dev;

  end:
  return rc;
}

bool hci_usb_device_deinit(hci_usb_device_t* hci_dev){
  if(!hci_dev->usb_device.usr_data) return false;
  for(int i = 0; i < countof(hci_dev->xfers); i++) usb_transfer_deinit(&hci_dev->xfers[i]);
  usb_device_release_iface(&hci_dev->usb_device, DEFAULT_HCI_IF_NO);
  usb_device_close(&hci_dev->usb_device);
  usb_device_deinit(&hci_dev->usb_device);
  hci_dev->usb_device.usr_data = NULL;
  return true;
}

void hci_usb_device_input(hci_usb_device_t* hci_dev, uint8_t* data, size_t length){
  if(!hci_dev->usb_device.usr_data) return;
  io_buff_t* buff = &hci_dev->input_buff;
  while(length > 0){
    int bytes_to_read = buff->len - buff->w_idx;
    if(bytes_to_read > length) bytes_to_read = length;
    else if(bytes_to_read <= 0){
      buff->w_idx = 0;
      continue;
    }
    memcpy(buff->bytes + buff->w_idx, data, bytes_to_read);
    buff->w_idx += bytes_to_read, data += bytes_to_read, length -= bytes_to_read;

    check_again:
    if(buff->w_idx >= sizeof(hci_pkt_t)){
      hci_pkt_t* pkt = (hci_pkt_t*)(buff->bytes + buff->r_idx);
      size_t pkt_length = 0;

      usb_transfer_t* ep_tx = NULL;

      IF_PKT(HCI_CMD, hci_cmd_t, cmd, {ep_tx = &hci_dev->tx_cmd;})
      ELIF_PKT(HCI_EVT, hci_evt_t, evt, {})
      ELIF_PKT(HCI_ACL, hci_acl_t, acl, {ep_tx = &hci_dev->tx_acl;})
      ELIF_PKT(HCI_SCO, hci_sco_t, sco, {})
      else goto resetRead;

      if(!pkt_length) goto read_again;
      else if(pkt_length > buff->len - 4) goto resetRead;

      if(buff->w_idx >= pkt_length){
        // LOG("got hci pkt: %hhu, length: %lu", pkt->type, pkt_length);

        if(ep_tx){
          size_t sent = 0, pkt_size = pkt_length - sizeof(hci_pkt_t);
          while(sent < pkt_size){
            size_t to_send = min(ep_tx->max_pkt_size, pkt_size - sent);
            bool rc = usb_transfer_submit(ep_tx, pkt->data + sent, to_send);
            sent += to_send;
            // LOG("submit rc: %u, to_send: %zu, sent: %zu, pkt_size: %zu", rc, to_send, sent, pkt_size);
          }
        }
        else LOG("unknown hci pkt");

        buff->w_idx -= pkt_length;
        if(buff->w_idx > 0){
          buff->r_idx += pkt_length;
          goto check_again;
        }
      }
    }

    read_again:
    if(buff->r_idx > 0){
      memmove(buff->bytes, buff->bytes + buff->r_idx, buff->w_idx);
      buff->r_idx = 0;
    }
    continue;

    resetRead:
    buff->r_idx = buff->w_idx = 0;
  }
}
