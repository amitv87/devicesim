#include "wifi.h"

#define WIFI_HDR_LEN 4

#define CALL(chip, fn, ...) ((chip) && (chip)->ops && (chip)->ops->fn ? (chip)->ops->fn((chip), ##__VA_ARGS__) : false)

/* ---- emit: build [plane, op, len_lo, len_hi] then a/b payload chunks ----- */
static void emit(wifi_dev_t* dev, uint8_t plane, uint8_t op,
                 const void* a, size_t alen, const void* b, size_t blen){
  size_t len = alen + blen;
  uint8_t hdr[WIFI_HDR_LEN] = {plane, op, (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff)};
  if(!dev->output) return;
  dev->output(dev, hdr, sizeof(hdr));
  if(alen) dev->output(dev, (uint8_t*)a, alen);
  if(blen) dev->output(dev, (uint8_t*)b, blen);
}

/* ---- up-calls (backend -> OS): the wpa_funcs callback analogs ----------- */
void wifi_dev_on_connect(wifi_dev_t* dev, uint8_t bssid[6]){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_CONNECT, bssid, 6, NULL, 0);
}
void wifi_dev_on_connected(wifi_dev_t* dev, uint8_t bssid[6]){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_CONNECTED, bssid, 6, NULL, 0);
}
void wifi_dev_on_disconnected(wifi_dev_t* dev, uint8_t reason){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_DISCONNECTED, &reason, 1, NULL, 0);
}
void wifi_dev_on_rx_mgmt(wifi_dev_t* dev, wifi_mgmt_rx_t* m, uint8_t* frame, size_t len){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_RX_MGMT, m, sizeof(*m), frame, len);
}
void wifi_dev_on_scan_result(wifi_dev_t* dev, wifi_scan_result_t* r, uint8_t* ies, size_t len){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_SCAN_RESULT, r, sizeof(*r), ies, len);
}
void wifi_dev_on_scan_done(wifi_dev_t* dev){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_SCAN_DONE, NULL, 0, NULL, 0);
}
void wifi_dev_on_sae_build(wifi_dev_t* dev, wifi_sae_t* s){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_SAE_BUILD, s, sizeof(*s), NULL, 0);
}
void wifi_dev_on_sae_rx(wifi_dev_t* dev, wifi_sae_t* s, uint8_t* body, size_t len){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_SAE_RX, s, sizeof(*s), body, len);
}
void wifi_dev_on_mic_failure(wifi_dev_t* dev, uint8_t is_unicast){
  emit(dev, WIFI_PLANE_CTRL, WIFI_EV_MIC_FAILURE, &is_unicast, 1, NULL, 0);
}
void wifi_dev_on_rx_data(wifi_dev_t* dev, uint8_t* eth, size_t len){
  // split by ethertype so the supplicant gets a clean rx_eapol channel
  uint8_t plane = WIFI_PLANE_DATA;
  if(len >= 14 && ((eth[12] << 8) | eth[13]) == WIFI_ETHERTYPE_EAPOL) plane = WIFI_PLANE_EAPOL;
  emit(dev, plane, 0, eth, len, NULL, 0);
}
void wifi_dev_on_rx_80211(wifi_dev_t* dev, uint8_t* frame, size_t len, uint8_t channel, int8_t rssi, uint16_t rate){
  wifi_rx80211_t h = {.channel = channel, .rssi = rssi, .rate = rate, .flags = 0};
  emit(dev, WIFI_PLANE_RAW80211, 0, &h, sizeof(h), frame, len);
}

/* ---- dispatch one complete CTRL frame to the backend -------------------- */
static void on_ctrl(wifi_dev_t* dev, uint8_t op, uint8_t* p, size_t len){
  wifi_chip_t* chip = dev->chip;
  switch(op){
    case WIFI_CMD_SET_MODE:
      if(len >= 1) CALL(chip, set_mode, p[0]);
      break;
    case WIFI_CMD_SET_CHANNEL:
      if(len >= 1) CALL(chip, set_channel, p[0]);
      break;
    case WIFI_CMD_RESET:
      CALL(chip, reset);
      break;
    case WIFI_CMD_SCAN:
      if(len >= sizeof(wifi_scan_req_t)) CALL(chip, scan, (wifi_scan_req_t*)p);
      break;
    case WIFI_CMD_CONNECT:
      if(len >= sizeof(wifi_connect_req_t)) CALL(chip, connect, (wifi_connect_req_t*)p);
      break;
    case WIFI_CMD_DISCONNECT:
      CALL(chip, disconnect, len >= 1 ? p[0] : 0);
      break;
    case WIFI_CMD_SET_APPIE:{
      wifi_appie_t* ie = (wifi_appie_t*)p;
      if(len >= sizeof(*ie) && len >= sizeof(*ie) + ie->ie_len)
        CALL(chip, set_appie, ie->type, p + sizeof(*ie), ie->ie_len);
      break;
    }
    case WIFI_CMD_SET_KEY:{
      wifi_key_t* k = (wifi_key_t*)p;
      if(len >= sizeof(*k) && len >= sizeof(*k) + k->key_len)
        CALL(chip, set_key, k, p + sizeof(*k));
      break;
    }
    case WIFI_CMD_SET_IGTK:
      if(len >= sizeof(wifi_igtk_t)) CALL(chip, set_igtk, (wifi_igtk_t*)p);
      break;
    case WIFI_CMD_SEND_MGMT:{
      wifi_mgmt_tx_t* m = (wifi_mgmt_tx_t*)p;
      if(len >= sizeof(*m)) CALL(chip, send_mgmt, m, p + sizeof(*m), len - sizeof(*m));
      break;
    }
    case WIFI_CMD_SAE_MSG:{
      wifi_sae_t* s = (wifi_sae_t*)p;
      if(len >= sizeof(*s)) CALL(chip, sae_msg, s, p + sizeof(*s), len - sizeof(*s));
      break;
    }
    case WIFI_CMD_CONFIG_DONE:
      CALL(chip, config_done);
      break;
    default:
      LOG("unknown wifi ctrl op: 0x%02x, len: %zu", op, len);
  }
}

static void on_frame(wifi_dev_t* dev, uint8_t plane, uint8_t op, uint8_t* p, size_t len){
  switch(plane){
    case WIFI_PLANE_CTRL:     on_ctrl(dev, op, p, len); break;
    case WIFI_PLANE_DATA:
    case WIFI_PLANE_EAPOL:    CALL(dev->chip, tx_data, p, len); break;
    case WIFI_PLANE_RAW80211: CALL(dev->chip, tx_raw80211, p, len); break; // OS injects a raw 802.11 frame
    default: LOG("unknown wifi plane: %u", plane);
  }
}

/* ---- pty -> device: reassemble framed messages -------------------------- */
void wifi_dev_input(wifi_dev_t* dev, uint8_t* data, size_t length){
  io_buff_t* buff = &dev->input_buff;
  while(length > 0){
    size_t space = buff->len - buff->w_idx;
    if(space == 0){ // overflow: drop and resync
      LOG("wifi input overflow, resetting");
      buff->r_idx = buff->w_idx = 0;
      space = buff->len;
    }
    size_t n = min(space, length);
    memcpy(buff->bytes + buff->w_idx, data, n);
    buff->w_idx += n, data += n, length -= n;

    while(buff->w_idx - buff->r_idx >= WIFI_HDR_LEN){
      uint8_t* h = buff->bytes + buff->r_idx;
      size_t plen = h[2] | (h[3] << 8);
      size_t flen = WIFI_HDR_LEN + plen;
      if(flen > buff->len){ // frame can never fit: resync
        LOG("wifi frame too big: %zu, resetting", flen);
        buff->r_idx = buff->w_idx = 0;
        break;
      }
      if(buff->w_idx - buff->r_idx < flen) break; // need more bytes
      on_frame(dev, h[0], h[1], h + WIFI_HDR_LEN, plen);
      buff->r_idx += flen;
    }

    if(buff->r_idx > 0){ // compact
      buff->w_idx -= buff->r_idx;
      if(buff->w_idx) memmove(buff->bytes, buff->bytes + buff->r_idx, buff->w_idx);
      buff->r_idx = 0;
    }
  }
}

bool wifi_dev_init(wifi_dev_t* dev, wifi_chip_t* chip, usb_dev_info_t* info){
  dev->chip = chip;
  chip->dev = dev;
  dev->input_buff = (io_buff_t){.bytes = dev->__input_buff, .len = sizeof(dev->__input_buff)};
  return CALL(chip, init, info);
}

bool wifi_dev_deinit(wifi_dev_t* dev){
  return CALL(dev->chip, deinit);
}
