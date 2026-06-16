#include "wchip_sim.h"

#define SIM(chip) ((wchip_sim_t*)(chip))

/* a couple of canned APs returned on scan: one open, one WPA2 */
typedef struct{
  uint8_t bssid[6];
  uint8_t channel;
  int8_t  rssi;
  const char* ssid;
  const uint8_t* rsn_ie; // NULL = open
  uint8_t rsn_ie_len;
} fake_ap_t;

// minimal RSN IE: version 1, CCMP group + pairwise, PSK akm
static const uint8_t rsn_ccmp_psk[] = {
  0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
  0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
  0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
};

static const fake_ap_t fake_aps[] = {
  {{0x02,0x00,0x00,0x00,0x00,0x01}, 1,  -42, "devicesim-open", NULL, 0},
  {{0x02,0x00,0x00,0x00,0x00,0x02}, 6,  -58, "devicesim-wpa2", rsn_ccmp_psk, sizeof(rsn_ccmp_psk)},
};

static void emit_ssid_rsn(uint8_t* buf, size_t* len, const fake_ap_t* ap){
  size_t n = 0;
  size_t sl = strlen(ap->ssid);
  buf[n++] = 0x00;            // SSID element id
  buf[n++] = (uint8_t)sl;
  memcpy(buf + n, ap->ssid, sl); n += sl;
  if(ap->rsn_ie){ memcpy(buf + n, ap->rsn_ie, ap->rsn_ie_len); n += ap->rsn_ie_len; }
  *len = n;
}

static bool sim_init(wifi_chip_t* chip, usb_dev_info_t* info){
  SIM(chip)->connected = false;
  LOG("wchip_sim online (no hardware)");
  return true;
}
static bool sim_deinit(wifi_chip_t* chip){ return true; }

static bool sim_set_mode(wifi_chip_t* chip, uint8_t mode){
  SIM(chip)->mode = mode;
  LOG("sim set_mode: %u", mode);
  return true;
}

static bool sim_scan(wifi_chip_t* chip, wifi_scan_req_t* req){
  LOG("sim scan ch:%u passive:%u", req->channel, req->passive);
  for(int i = 0; i < countof(fake_aps); i++){
    const fake_ap_t* ap = &fake_aps[i];
    if(req->channel && req->channel != ap->channel) continue;
    wifi_scan_result_t r = {.channel = ap->channel, .rssi = ap->rssi, .capinfo = 0x0411};
    memcpy(r.bssid, ap->bssid, 6);
    uint8_t ies[64]; size_t ie_len;
    emit_ssid_rsn(ies, &ie_len, ap);
    wifi_dev_on_scan_result(chip->dev, &r, ies, ie_len);
  }
  wifi_dev_on_scan_done(chip->dev);
  return true;
}

static bool sim_connect(wifi_chip_t* chip, wifi_connect_req_t* req){
  memcpy(SIM(chip)->bssid, req->bssid, 6);
  LOG("sim connect ssid:%.*s ch:%u", req->ssid_len, req->ssid, req->channel);
  // mirror the blob: announce assoc start, then assoc done -> OS starts 4-way
  wifi_dev_on_connect(chip->dev, req->bssid);
  wifi_dev_on_connected(chip->dev, req->bssid);
  SIM(chip)->connected = true;
  return true;
}

static bool sim_disconnect(wifi_chip_t* chip, uint8_t reason){
  SIM(chip)->connected = false;
  wifi_dev_on_disconnected(chip->dev, reason);
  return true;
}

static bool sim_set_appie(wifi_chip_t* chip, uint8_t type, uint8_t* ie, size_t len){
  LOG("sim set_appie type:%u len:%zu", type, len);
  return true;
}
static bool sim_set_key(wifi_chip_t* chip, wifi_key_t* k, uint8_t* key){
  LOG("sim set_key alg:%u idx:%u flag:0x%02x key_len:%u", k->alg, k->key_idx, k->key_flag, k->key_len);
  return true;
}
static bool sim_set_igtk(wifi_chip_t* chip, wifi_igtk_t* igtk){ return true; }
static bool sim_send_mgmt(wifi_chip_t* chip, wifi_mgmt_tx_t* m, uint8_t* frame, size_t len){
  LOG("sim send_mgmt subtype:0x%02x len:%zu", m->subtype, len);
  return true;
}
static bool sim_config_done(wifi_chip_t* chip){
  LOG("sim config_done (data path open)");
  return true;
}
static bool sim_tx_data(wifi_chip_t* chip, uint8_t* eth, size_t len){
  // TODO Phase 0.5: bridge to host network (libs/net) instead of dropping.
  // hexdump(eth, len, "sim tx_data %zu", len);
  return true;
}

const wifi_chip_ops_t wchip_sim_ops = {
  .init        = sim_init,
  .deinit      = sim_deinit,
  .set_mode    = sim_set_mode,
  .scan        = sim_scan,
  .connect     = sim_connect,
  .disconnect  = sim_disconnect,
  .set_appie   = sim_set_appie,
  .set_key     = sim_set_key,
  .set_igtk    = sim_set_igtk,
  .send_mgmt   = sim_send_mgmt,
  .sae_msg     = NULL, // WPA3 not simulated
  .config_done = sim_config_done,
  .tx_data     = sim_tx_data,
};
