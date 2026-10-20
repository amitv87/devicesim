#include "wifi_softmac.h"
#include "ccmp.h"

#define SM_LOG(sm, fmt, ...) LOG("%s: " fmt, (sm)->name, ##__VA_ARGS__)

const uint8_t WIFI_SOFTMAC_STA_MAC[6] = { 0x02, 0x00, 0x00, 0x77, 0x66, 0x01 };
#define STA_MAC WIFI_SOFTMAC_STA_MAC

#define WSM_SCAN_DWELL_MS 300   /* default per-channel dwell when neither OS nor backend picks one */
#define WSM_CONN_TICK_MS  300   /* auth/assoc retransmit + timeout tick */

static inline bool is_5g(uint8_t ch){ return ch > 14; }

static bool radio_tx(wifi_softmac_t* sm, const uint8_t* f, size_t n){
  return sm->radio && sm->radio->tx ? sm->radio->tx(sm->ctx, f, n) : false;
}
static bool radio_tune(wifi_softmac_t* sm, uint8_t ch){
  if(!sm->radio || !sm->radio->set_channel) return false;
  bool ok = sm->radio->set_channel(sm->ctx, ch);
  if(ok) sm->channel = ch;
  return ok;
}
static void radio_bssid(wifi_softmac_t* sm, const uint8_t* bssid){
  if(sm->radio && sm->radio->set_bssid) sm->radio->set_bssid(sm->ctx, bssid);
}

static uint16_t next_seq(wifi_softmac_t* sm){
  uint16_t s = sm->tx_seq; sm->tx_seq = (uint16_t)((sm->tx_seq + 1) & 0xfff); return (uint16_t)(s << 4);
}

/* Common 802.11 mgmt header (addr1=AP, addr2=STA, addr3=BSSID). */
static size_t mgmt_hdr(wifi_softmac_t* sm, uint8_t* f, uint8_t fc0){
  f[0] = fc0; f[1] = 0x00; f[2] = 0; f[3] = 0;
  memcpy(f + 4,  sm->ap_bssid, 6);
  memcpy(f + 10, STA_MAC, 6);
  memcpy(f + 16, sm->ap_bssid, 6);
  uint16_t sc = next_seq(sm); f[22] = sc & 0xff; f[23] = sc >> 8;
  return 24;
}

/* Supported (+ extended) rates for the operating band. Legacy only: we don't
 * advertise HT/VHT, so the AP sends us non-aggregated frames at <= 54 Mbps. */
static size_t put_rates(wifi_softmac_t* sm, uint8_t* f){
  size_t n = 0;
  if(is_5g(sm->channel)){
    static const uint8_t r5[] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c}; // 6B 9 12B 18 24B 36 48 54
    f[n++] = 0x01; f[n++] = sizeof r5; memcpy(f + n, r5, sizeof r5); n += sizeof r5;
  } else {
    static const uint8_t r2[] = {0x82, 0x84, 0x8b, 0x96};                       // 1 2 5.5 11 (basic)
    static const uint8_t x2[] = {0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c}; // 6..54
    f[n++] = 0x01; f[n++] = sizeof r2; memcpy(f + n, r2, sizeof r2); n += sizeof r2;
    f[n++] = 0x32; f[n++] = sizeof x2; memcpy(f + n, x2, sizeof x2); n += sizeof x2;
  }
  return n;
}

static void send_auth(wifi_softmac_t* sm){
  uint8_t f[30]; size_t n = mgmt_hdr(sm, f, 0xB0);
  f[n++] = 0; f[n++] = 0;   // algorithm = open
  f[n++] = 1; f[n++] = 0;   // transaction seq = 1
  f[n++] = 0; f[n++] = 0;   // status
  SM_LOG(sm, "TX auth-req -> %d", radio_tx(sm, f, n));
}

static void send_assoc(wifi_softmac_t* sm){
  uint8_t f[200]; size_t n = mgmt_hdr(sm, f, 0x00);
  f[n++] = 0x11; f[n++] = 0x00;             // capability: ESS | Privacy
  f[n++] = 0x0a; f[n++] = 0x00;             // listen interval
  f[n++] = 0x00; f[n++] = sm->ssid_len;     // SSID IE
  memcpy(f + n, sm->ssid, sm->ssid_len); n += sm->ssid_len;
  n += put_rates(sm, f + n);
  if(sm->assoc_ie_len){ memcpy(f + n, sm->assoc_ie, sm->assoc_ie_len); n += sm->assoc_ie_len; }
  SM_LOG(sm, "TX assoc-req (rsn_ie:%uB) -> %d", sm->assoc_ie_len, radio_tx(sm, f, n));
}

/* Send a robust mgmt frame: CCMP-protected once a PTK exists on a PMF (SAE) link. */
static bool tx_robust_mgmt(wifi_softmac_t* sm, uint8_t* f, size_t n){
  if(sm->tk_set && sm->is_sae){
    uint8_t enc[512];
    if(n + 16 > sizeof enc) return false;
    size_t outn = ccmp_encrypt(sm->tk, f, 24, false, true, sm->tx_pn, 0, f + 24, n - 24, enc);
    sm->tx_pn++;
    return radio_tx(sm, enc, outn);
  }
  return radio_tx(sm, f, n);
}

static void conn_fail(wifi_softmac_t* sm, const char* why){
  SM_LOG(sm, "connect failed: %s", why);
  io_rem_timer(&sm->conn_timer);
  sm->state = WSM_IDLE;
  radio_bssid(sm, NULL);
  wifi_dev_on_disconnected(sm->chip->dev, 1);
}

static void conn_tick(io_timer_t* t){
  wifi_softmac_t* sm = t->usr_data;
  /* an SA-Query comeback (status 30) needs the AP a few seconds to evict its stale SA */
  if(++sm->conn_tries > (sm->assoc_comeback ? 15 : 5)){ conn_fail(sm, "auth/assoc timeout"); return; }
  if(sm->state == WSM_AUTH)       send_auth(sm);
  else if(sm->state == WSM_ASSOC) send_assoc(sm);
  else if(sm->state == WSM_SAE_COMMIT || sm->state == WSM_SAE_CONFIRM){
    if(sm->sae_tx_len) radio_tx(sm, sm->sae_tx, sm->sae_tx_len);   // retransmit last SAE auth
  }
  else io_rem_timer(&sm->conn_timer);
}

/* ---- link watchdog: beacon loss while connected ----------------------------- */
static void link_down(wifi_softmac_t* sm, uint8_t reason){
  if(sm->rx_dups) SM_LOG(sm, "link: %u duplicate (MAC retry) frames dropped", sm->rx_dups);
  io_rem_timer(&sm->link_timer);
  sm->state = WSM_IDLE;
  sm->tk_set = sm->gtk_set = sm->igtk_set = false;
  radio_bssid(sm, NULL);
  wifi_dev_on_disconnected(sm->chip->dev, reason);
}

static void link_tick(io_timer_t* t){
  wifi_softmac_t* sm = t->usr_data;
  if(sm->state != WSM_CONNECTED){ io_rem_timer(&sm->link_timer); return; }
  if(sm->scanning){ sm->last_ap_rx_ms = sys_now(); return; }   // off-channel by choice
  if(sys_now() - sm->last_ap_rx_ms > WIFI_SOFTMAC_LINK_TIMEOUT_MS){
    SM_LOG(sm, "no frames from AP for %ums - link lost (beacon timeout)", WIFI_SOFTMAC_LINK_TIMEOUT_MS);
    link_down(sm, WIFI_SOFTMAC_REASON_BEACON_TIMEOUT);
  }
}

static void link_up(wifi_softmac_t* sm){
  sm->last_ap_rx_ms = sys_now();
  io_rem_timer(&sm->link_timer);
  sm->link_timer = (io_timer_t){ .repeat = true, .run_now = false, .interval_ms = 1000, .usr_data = sm, .cb = link_tick };
  io_add_timer(&sm->link_timer);
}

/* ---- scan ---------------------------------------------------------------- */
static void scan_collect(wifi_softmac_t* sm, const uint8_t* f, size_t len, int8_t rssi, uint8_t rx_ch){
  if(len < 38) return;
  const uint8_t* bssid = f + 16;
  uint16_t capinfo = f[34] | (f[35] << 8);
  const uint8_t* ies = f + 36;
  size_t avail = len - 36, ie_len = 0;
  while(ie_len + 2 <= avail){                       // trim FCS / junk by walking TLVs
    uint8_t l = ies[ie_len + 1];
    if(ie_len + 2 + l > avail) break;
    if(ie_len + 2 + l > sizeof sm->results[0].ies) break;   // cap on a whole-element boundary
    ie_len += 2 + l;
  }
  /* Real channel: DS Parameter Set (2.4G) or HT Operation primary channel (5G).
   * USB RX batching can deliver a frame after we've hopped, so prefer the IEs. */
  uint8_t ch = 0, ht_ch = 0;
  for(size_t q = 0; q + 2 <= ie_len && q + 2 + ies[q + 1] <= ie_len; q += 2 + ies[q + 1]){
    if(ies[q] == 3 && ies[q + 1] == 1) ch = ies[q + 2];
    else if(ies[q] == 61 && ies[q + 1] >= 1) ht_ch = ies[q + 2];
  }
  if(!ch) ch = ht_ch ? ht_ch : rx_ch;
  for(uint8_t i = 0; i < sm->n_results; i++){
    if(memcmp(sm->results[i].bssid, bssid, 6) == 0){
      if(rssi > sm->results[i].rssi) sm->results[i].rssi = rssi;
      return;
    }
  }
  if(sm->n_results >= countof(sm->results)) return;
  wifi_softmac_bss_t* r = &sm->results[sm->n_results++];
  memcpy(r->bssid, bssid, 6);
  r->channel = ch; r->rssi = rssi; r->capinfo = capinfo;
  memcpy(r->ies, ies, ie_len); r->ie_len = (uint16_t)ie_len;
}

static void scan_finish(wifi_softmac_t* sm){
  sm->scanning = false;
  io_rem_timer(&sm->scan_timer);
  for(uint8_t i = 0; i < sm->n_results; i++){
    wifi_softmac_bss_t* r = &sm->results[i];
    wifi_scan_result_t sr; memset(&sr, 0, sizeof sr);
    memcpy(sr.bssid, r->bssid, 6);
    sr.channel = r->channel; sr.rssi = r->rssi; sr.capinfo = r->capinfo;
    wifi_dev_on_scan_result(sm->chip->dev, &sr, r->ies, r->ie_len);
  }
  wifi_dev_on_scan_done(sm->chip->dev);
  SM_LOG(sm, "scan done: %u BSS(s)", sm->n_results);
  if(sm->state != WSM_IDLE && sm->link_channel && sm->channel != sm->link_channel){
    radio_tune(sm, sm->link_channel);             // back to the AP after a scan while linked
    sm->last_ap_rx_ms = sys_now();
  }
}

static void scan_tick(io_timer_t* t){
  wifi_softmac_t* sm = t->usr_data;
  if(!sm->scanning) return;
  if(sm->scan_single || sm->scan_idx + 1 >= sm->scan_last){ scan_finish(sm); return; }
  sm->scan_idx++;
  radio_tune(sm, sm->scan_chans[sm->scan_idx]);
}

bool wifi_softmac_scan(wifi_softmac_t* sm, wifi_scan_req_t* req){
  if(sm->scanning) return true;
  if(sm->state != WSM_IDLE && sm->state != WSM_CONNECTED){ wifi_dev_on_scan_done(sm->chip->dev); return true; }
  uint16_t dwell = (req && req->dwell_ms) ? req->dwell_ms : (sm->scan_dwell_ms ? sm->scan_dwell_ms : WSM_SCAN_DWELL_MS);
  sm->n_results = 0;
  sm->scanning = true;
  sm->scan_single = (req && req->channel) ? req->channel : 0;
  sm->scan_idx = 0;
  sm->scan_last = sm->n_scan_chans;
  radio_tune(sm, sm->scan_single ? sm->scan_single : sm->scan_chans[0]);
  sm->scan_timer = (io_timer_t){ .repeat = true, .run_now = false, .interval_ms = dwell, .usr_data = sm, .cb = scan_tick };
  io_add_timer(&sm->scan_timer);
  if(sm->scan_single){ SM_LOG(sm, "scan: ch%u (%ums)", sm->scan_single, dwell); }
  else { SM_LOG(sm, "scan: %u channels (%u..%u), %ums dwell", sm->n_scan_chans, sm->scan_chans[0], sm->scan_chans[sm->n_scan_chans - 1], dwell); }
  return true;
}

/* ---- auth / assoc RX ----------------------------------------------------- */
static void conn_rx(wifi_softmac_t* sm, const uint8_t* f, size_t len){
  if(len < 30 || memcmp(f + 10, sm->ap_bssid, 6) != 0) return;   // from our AP; auth/assoc rsp body >= 6
  if(memcmp(f + 4, STA_MAC, 6) != 0) return;                       // ... and to us
  /* SAE auth (algorithm 3): deliver the body to the OS supplicant + drive the sequence. */
  if((sm->state == WSM_SAE_COMMIT || sm->state == WSM_SAE_CONFIRM)
     && f[0] == 0xB0 && len >= 30 && (f[24] | (f[25] << 8)) == 3){
    uint16_t seq = f[26] | (f[27] << 8), status = f[28] | (f[29] << 8);
    SM_LOG(sm, "SAE rx seq=%u status=%u len=%zu", seq, status, len);
    bool ok = (seq == 1) ? (status == 0 || status == WIFI_SAE_STATUS_H2E) : (status == 0);
    if(!ok){ char b[32]; snprintf(b, sizeof b, "SAE status %u", status); conn_fail(sm, b); return; }
    wifi_sae_t ev; memset(&ev, 0, sizeof ev); memcpy(ev.bssid, sm->ap_bssid, 6);
    uint8_t* bd = (uint8_t*)f + 30; uint16_t bl = (uint16_t)(len - 30);
    if(seq == 1 && sm->state == WSM_SAE_COMMIT){
      ev.sae_type = WIFI_SAE_COMMIT; wifi_dev_on_sae_rx(sm->chip->dev, &ev, bd, bl);
      sm->state = WSM_SAE_CONFIRM; sm->conn_tries = 0; sm->sae_tx_len = 0;
      wifi_sae_t b2; memset(&b2, 0, sizeof b2); memcpy(b2.bssid, sm->ap_bssid, 6); b2.sae_type = WIFI_SAE_CONFIRM;
      wifi_dev_on_sae_build(sm->chip->dev, &b2);           // ask OS for the confirm
      SM_LOG(sm, "SAE commit rx -> building confirm");
    } else if(seq == 2 && sm->state == WSM_SAE_CONFIRM){
      ev.sae_type = WIFI_SAE_CONFIRM; wifi_dev_on_sae_rx(sm->chip->dev, &ev, bd, bl);
      sm->state = WSM_ASSOC; sm->conn_tries = 0;           // assoc sent by conn_tick
      SM_LOG(sm, "SAE confirm rx -> assoc");
    }
    return;
  }
  if(sm->state == WSM_AUTH && f[0] == 0xB0){              // auth response
    uint16_t status = f[28] | (f[29] << 8);
    if(status != 0){ char b[32]; snprintf(b, sizeof b, "auth status %u", status); conn_fail(sm, b); return; }
    SM_LOG(sm, "auth OK -> assoc");
    /* TX from the conn timer, not this RX completion (keeps USB backends simple). */
    sm->state = WSM_ASSOC; sm->conn_tries = 0;
  } else if(sm->state == WSM_ASSOC && f[0] == 0x10){      // assoc response
    uint16_t status = f[26] | (f[27] << 8);
    if(status == 30){
      /* 802.11w: the AP still holds our prior PMF SA and runs SA Query against it;
       * it evicts the stale SA once that times out. Stay in ASSOC and retry. */
      sm->assoc_comeback = true; sm->conn_tries = 0;
      SM_LOG(sm, "assoc rejected (status 30, SA Query) -> retrying until AP evicts stale SA");
      return;
    }
    if(status != 0){ char b[32]; snprintf(b, sizeof b, "assoc status %u", status); conn_fail(sm, b); return; }
    uint16_t aid = (uint16_t)((f[28] | (f[29] << 8)) & 0x3fff);
    SM_LOG(sm, "ASSOCIATED (aid=%u) - link up, awaiting 4-way", aid);
    io_rem_timer(&sm->conn_timer);
    sm->assoc_comeback = false;
    sm->state = WSM_CONNECTED;
    link_up(sm);
    wifi_dev_on_connect(sm->chip->dev, sm->ap_bssid);
    wifi_dev_on_connected(sm->chip->dev, sm->ap_bssid);
  }
}

/* ---- 802.11w ------------------------------------------------------------- */
/* BIP-CMAC-128: verify the MME on a group-addressed robust mgmt frame with the IGTK.
 * MME = EID 0x4c, len 16: KeyID(2) IPN(6) MIC(8). AAD = masked FC || A1 || A2 || A3. */
static bool bip_verify(wifi_softmac_t* sm, const uint8_t* f, size_t len){
  if(!sm->igtk_set || len < 24 + 2 + 18) return false;
  int mme = -1;
  for(size_t p = 24; p + 2 <= len; ){
    uint8_t id = f[p], l = f[p + 1];
    if(p + 2u + l > len) break;
    if(id == 0x4c && l == 16){ mme = (int)p; break; }
    p += 2u + l;
  }
  if(mme < 0) return false;
  uint64_t ipn = 0, last = 0;                                  // replay check: 48-bit LE IPN must increase
  for(int i = 5; i >= 0; i--){ ipn = ipn << 8 | f[mme + 4 + i]; last = last << 8 | sm->igtk_ipn[i]; }
  if(ipn <= last){ SM_LOG(sm, "BIP: replayed IPN %llu (last %llu)", (unsigned long long)ipn, (unsigned long long)last); return false; }
  uint8_t buf[512];
  size_t body_len = len - 24;
  if(20 + body_len > sizeof buf) return false;
  uint16_t fc = (uint16_t)((f[0] | (f[1] << 8)) & ~0x3800);   // mask Retry/PwrMgt/MoreData
  size_t bn = 0;
  buf[bn++] = (uint8_t)fc; buf[bn++] = (uint8_t)(fc >> 8);
  memcpy(buf + bn, f + 4, 18); bn += 18;
  memcpy(buf + bn, f + 24, body_len);
  size_t mic_off = bn + ((size_t)mme - 24) + 2 + 2 + 6;        // EID+len+KeyID+IPN -> MIC
  uint8_t rx_mic[8]; memcpy(rx_mic, buf + mic_off, 8);
  memset(buf + mic_off, 0, 8);
  bn += body_len;
  uint8_t mac[16]; aes_cmac128(sm->igtk, buf, bn, mac);
  if(memcmp(mac, rx_mic, 8) != 0) return false;
  memcpy(sm->igtk_ipn, f + mme + 4, 6);                        // advance the replay counter
  return true;
}

/* Unicast robust mgmt body: CCMP-decrypt (PMF) or take as-is (no PMF). Returns body len or -1. */
static int robust_body(wifi_softmac_t* sm, const uint8_t* f, size_t len, uint8_t* out, size_t out_sz, const char* kind){
  if(!sm->is_sae){
    if(len - 24 > out_sz) return -1;
    memcpy(out, f + 24, len - 24); return (int)(len - 24);
  }
  if(!(f[1] & 0x40) || !sm->tk_set){ SM_LOG(sm, "unprotected unicast %s ignored (PMF)", kind); return -1; }
  if(len - 24 > out_sz) return -1;
  int pl = ccmp_decrypt(sm->tk, f, 24, false, true, f + 24, len - 24, out);
  if(pl < 0 && len >= 24 + 16 + 4) pl = ccmp_decrypt(sm->tk, f, 24, false, true, f + 24, len - 24 - 4, out);
  if(pl < 0) SM_LOG(sm, "unicast %s CCMP verify failed - ignored", kind);
  return pl;
}

/* SA Query (category 8): answer requests so a PMF AP keeps the association. */
static void sa_query_rx(wifi_softmac_t* sm, const uint8_t* body, int bl){
  if(bl < 4 || body[0] != 8 || body[1] != 0) return;      // category SA Query, action request
  uint8_t f[64]; size_t n = mgmt_hdr(sm, f, 0xD0);        // action
  f[n++] = 8; f[n++] = 1;                                  // SA Query response
  f[n++] = body[2]; f[n++] = body[3];                      // transaction id
  SM_LOG(sm, "SA Query req id=%02x%02x -> response %d", body[2], body[3], tx_robust_mgmt(sm, f, n));
}

/* ---- data RX ------------------------------------------------------------- */
/* Inbound 802.11 data -> (decrypt if Protected) -> 802.3 -> generic layer. */
static void data_rx(wifi_softmac_t* sm, const uint8_t* f, size_t len){
  if((f[1] & 0x03) != 0x02) return;                        // FromDS only (AP -> STA)
  uint8_t stype = f[0] & 0xf0;
  if(stype & 0x40) return;                                 // null / no-data subtypes
  bool qos = (stype & 0x80) != 0;
  size_t hdr = 24u + (qos ? 2u : 0u);
  if(f[1] & 0x80) hdr += 4;                                // +HTC
  if(len < hdr + 8) return;
  const uint8_t* da = f + 4;                               // addr1=DA(us), addr2=BSSID, addr3=SA
  const uint8_t* sa = f + 16;
  bool group = (da[0] & 1);
  if(memcmp(f + 10, sm->ap_bssid, 6) != 0) return;         // not our BSS
  if(!group && memcmp(da, STA_MAC, 6) != 0) return;        // unicast to another STA
  if(qos && (f[24] & 0x80)) return;                        // A-MSDU: never negotiated (no HT)
  uint8_t body[2400]; const uint8_t* snap; size_t snap_len;
  if(f[1] & 0x40){                                         // Protected -> CCMP
    const uint8_t* key = group ? sm->gtk : sm->tk;
    if(group ? !sm->gtk_set : !sm->tk_set) return;
    if(len - hdr > sizeof body) return;
    /* Some RX paths leave the 4-byte FCS on the MPDU: try FCS-stripped first, then as-is. */
    int pl = -1;
    if(len >= hdr + 8 + 8 + 4) pl = ccmp_decrypt(key, f, hdr, qos, false, f + hdr, len - 4 - hdr, body);
    if(pl < 0) pl = ccmp_decrypt(key, f, hdr, qos, false, f + hdr, len - hdr, body);
    if(pl < 0){
      const uint8_t* cc = f + hdr;
      SM_LOG(sm, "CCMP FAIL %s len:%zu keyid:%u pn:%02x%02x%02x%02x%02x%02x", group ? "group" : "pairwise",
             len, (cc[3] >> 6), cc[7], cc[6], cc[5], cc[4], cc[1], cc[0]);
      return;
    }
    snap = body; snap_len = (size_t)pl;
  } else {
    snap = f + hdr; snap_len = len - hdr;
  }
  if(snap_len < 8 || snap[0] != 0xAA || snap[1] != 0xAA || snap[2] != 0x03) return;
  size_t payload = snap_len - 8;
  uint8_t eth[2400];
  if(14 + payload > sizeof eth) return;
  memcpy(eth, da, 6); memcpy(eth + 6, sa, 6);
  eth[12] = snap[6]; eth[13] = snap[7];
  memcpy(eth + 14, snap + 8, payload);
  wifi_dev_on_rx_data(sm->chip->dev, eth, 14 + payload);
}

/* ---- connected-state mgmt ------------------------------------------------ */
static void link_mgmt_rx(wifi_softmac_t* sm, const uint8_t* f, size_t len){
  if(len < 24 || memcmp(f + 16, sm->ap_bssid, 6) != 0) return;
  uint8_t st = f[0];
  bool group = (f[4] & 1);
  if(st == 0xc0 || st == 0xa0){                            // deauth / disassoc
    /* 802.11w (PMF, WPA3/SAE only): honored only if protected - unicast via CCMP,
     * group via BIP - so a spoofed deauth can't knock us off. WPA2: honored as-is. */
    const char* kind = st == 0xc0 ? "deauth" : "disassoc";
    bool honored = false; uint16_t reason = 0;
    if(!group){
      if(memcmp(f + 4, STA_MAC, 6) != 0) return;
      uint8_t b[64]; int bl = robust_body(sm, f, len, b, sizeof b, kind);
      if(bl >= 2){ reason = b[0] | (b[1] << 8); honored = true; }
    } else if(!sm->is_sae){
      reason = len >= 26 ? (f[24] | (f[25] << 8)) : 0; honored = true;
    } else if(bip_verify(sm, f, len)){
      reason = len >= 26 ? (f[24] | (f[25] << 8)) : 0; honored = true;
    } else SM_LOG(sm, "group %s BIP verify failed/absent - ignored (PMF)", kind);
    if(honored){
      SM_LOG(sm, "%s from AP reason=%u - disconnecting", kind, reason);
      link_down(sm, (uint8_t)reason);
    }
  } else if(st == 0xd0 && !group && memcmp(f + 4, STA_MAC, 6) == 0){   // unicast action
    uint8_t b[256];
    bool prot = (f[1] & 0x40) != 0;
    int bl = prot ? robust_body(sm, f, len, b, sizeof b, "action") : (int)min(len - 24, sizeof b);
    if(bl < 0) return;
    if(!prot) memcpy(b, f + 24, (size_t)bl);
    if(bl >= 1 && b[0] == 8){ sa_query_rx(sm, b, bl); return; }
    /* other categories -> OS (wpa_sta_rx_mgmt analog), plaintext body */
    uint8_t fr[24 + 256]; memcpy(fr, f, 24); fr[1] &= ~0x40; memcpy(fr + 24, b, (size_t)bl);
    wifi_mgmt_rx_t m; memset(&m, 0, sizeof m);
    m.subtype = 13; memcpy(m.sa, f + 10, 6); m.channel = sm->channel;
    wifi_dev_on_rx_mgmt(sm->chip->dev, &m, fr, 24 + (size_t)bl);
  }
}

/* 802.11 duplicate detection (IEEE 802.11-2016 10.3.2.14): a unicast frame from the AP with
 * Retry set and the same sequence control as the last one in its class is a MAC retry sent
 * because our ACK was lost (e.g. BT holding the shared antenna). Drop it so the OS sees each
 * MSDU once. Classes: management + non-QoS data share one cache, QoS data has one per TID. */
static bool rx_is_dup(wifi_softmac_t* sm, const uint8_t* f, size_t len){
  uint8_t type = f[0] & 0x0c;
  if(len < 24 || type == 0x04 || (f[4] & 1)) return false;  // control / group addressed: no cache
  if(memcmp(f + 10, sm->ap_bssid, 6) != 0) return false;
  int slot = 0;
  if(type == 0x08 && (f[0] & 0x80) && len >= 26) slot = 1 + (f[24] & 0x0f);
  uint16_t sc = f[22] | (f[23] << 8);
  bool dup = (f[1] & 0x08) && (sm->rx_sc_valid & (1u << slot)) && sm->rx_sc[slot] == sc;
  sm->rx_sc[slot] = sc; sm->rx_sc_valid |= 1u << slot;
  return dup;
}

void wifi_softmac_rx(wifi_softmac_t* sm, const uint8_t* f, size_t len, int8_t rssi, uint8_t channel){
  if(len < 10) return;
  uint8_t type = f[0] & 0x0c;                              // 0x00 mgmt, 0x04 ctrl, 0x08 data
  if(sm->scanning && type == 0x00 && (f[0] == 0x80 || f[0] == 0x50)){
    scan_collect(sm, f, len, rssi, channel);
    if(sm->state != WSM_CONNECTED) return;
  }
  if(sm->state != WSM_IDLE && rx_is_dup(sm, f, len)){ sm->rx_dups++; return; }
  if(sm->state == WSM_CONNECTED){
    if(len >= 16 && type != 0x04 && memcmp(f + 10, sm->ap_bssid, 6) == 0) sm->last_ap_rx_ms = sys_now();
    if(type == 0x08) data_rx(sm, f, len);
    else if(type == 0x00) link_mgmt_rx(sm, f, len);
  } else if(sm->state != WSM_IDLE && type == 0x00){
    conn_rx(sm, f, len);
  }
}

/* ---- ops ----------------------------------------------------------------- */
bool wifi_softmac_sae_msg(wifi_softmac_t* sm, wifi_sae_t* s, uint8_t* body, size_t len){
  uint8_t f[sizeof sm->sae_tx]; size_t n = mgmt_hdr(sm, f, 0xB0);
  uint16_t seq = (s->sae_type == WIFI_SAE_COMMIT) ? 1 : 2;
  f[n++] = 3; f[n++] = 0;                                  // auth algorithm = SAE
  f[n++] = (uint8_t)seq; f[n++] = (uint8_t)(seq >> 8);
  f[n++] = (uint8_t)s->status; f[n++] = (uint8_t)(s->status >> 8);   // 126 = H2E on commit
  if(n + len > sizeof f) return false;
  memcpy(f + n, body, len); n += len;
  memcpy(sm->sae_tx, f, n); sm->sae_tx_len = (uint16_t)n;
  SM_LOG(sm, "TX SAE %s (%zuB) -> %d", seq == 1 ? "commit" : "confirm", len, radio_tx(sm, f, n));
  return true;
}

bool wifi_softmac_connect(wifi_softmac_t* sm, wifi_connect_req_t* req){
  if(sm->scanning){                                  // connect aborts the scan; the OS still gets SCAN_DONE
    sm->scanning = false; io_rem_timer(&sm->scan_timer);
    wifi_dev_on_scan_done(sm->chip->dev);
    SM_LOG(sm, "scan aborted by connect");
  }
  io_rem_timer(&sm->conn_timer);
  memcpy(sm->ap_bssid, req->bssid, 6);
  sm->ssid_len = req->ssid_len > 32 ? 32 : req->ssid_len;
  memcpy(sm->ssid, req->ssid, sm->ssid_len); sm->ssid[sm->ssid_len] = 0;
  uint8_t ch = req->channel ? req->channel : sm->channel;
  io_rem_timer(&sm->link_timer);
  radio_tune(sm, ch);
  sm->link_channel = sm->channel;
  if(sm->radio && sm->radio->prepare_link) sm->radio->prepare_link(sm->ctx, sm->channel);
  radio_bssid(sm, req->bssid);
  sm->tk_set = sm->gtk_set = sm->igtk_set = false;
  sm->conn_tries = 0; sm->sae_tx_len = 0; sm->assoc_comeback = false;
  sm->rx_sc_valid = 0; sm->rx_dups = 0;                    // new association: fresh duplicate cache
  if(sm->is_sae){
    /* WPA3: ask the OS for the SAE commit; wifi_softmac_sae_msg sends it as algo-3 auth. */
    sm->state = WSM_SAE_COMMIT;
    wifi_sae_t s; memset(&s, 0, sizeof s); memcpy(s.bssid, sm->ap_bssid, 6); s.sae_type = WIFI_SAE_COMMIT;
    wifi_dev_on_sae_build(sm->chip->dev, &s);
  } else {
    sm->state = WSM_AUTH;
    send_auth(sm);
  }
  sm->conn_timer = (io_timer_t){ .repeat = true, .run_now = false, .interval_ms = WSM_CONN_TICK_MS, .usr_data = sm, .cb = conn_tick };
  io_add_timer(&sm->conn_timer);
  SM_LOG(sm, "connecting '%s' (%s) ch%u %02x:%02x:%02x:%02x:%02x:%02x", sm->ssid,
         sm->is_sae ? "WPA3/SAE" : "open/WPA2", ch,
         req->bssid[0], req->bssid[1], req->bssid[2], req->bssid[3], req->bssid[4], req->bssid[5]);
  return true;
}

bool wifi_softmac_disconnect(wifi_softmac_t* sm, uint8_t reason){
  if(sm->state != WSM_IDLE){
    io_rem_timer(&sm->conn_timer);
    uint8_t f[26]; size_t n = mgmt_hdr(sm, f, 0xC0);       // deauth - free our slot at the AP
    f[n++] = reason ? reason : 3; f[n++] = 0;
    /* PMF: once a PTK is installed a deauth is robust and MUST be CCMP-protected, or the AP
     * ignores it, keeps our SA, and the next assoc gets status 30 (SA Query). */
    bool prot = sm->tk_set && sm->is_sae;
    bool ok = tx_robust_mgmt(sm, f, n);
    SM_LOG(sm, "TX deauth (%s, reason=%u) -> %d", prot ? "CCMP-protected" : "clear", f[24], ok);
  }
  if(sm->state != WSM_IDLE && sm->rx_dups) SM_LOG(sm, "link: %u duplicate (MAC retry) frames dropped", sm->rx_dups);
  io_rem_timer(&sm->link_timer);
  sm->link_channel = 0;
  sm->state = WSM_IDLE;
  sm->tk_set = sm->gtk_set = sm->igtk_set = false;
  radio_bssid(sm, NULL);
  return true;
}

bool wifi_softmac_reset(wifi_softmac_t* sm, uint8_t home_channel){
  if(sm->scanning){ sm->scanning = false; io_rem_timer(&sm->scan_timer); }
  wifi_softmac_disconnect(sm, 3);
  sm->n_results = 0;
  sm->tx_seq = 0; sm->tx_pn = 0;
  sm->assoc_ie_len = 0; sm->is_sae = false;
  radio_tune(sm, home_channel);
  SM_LOG(sm, "radio reset (state cleared, ch%u)", home_channel);
  return true;
}

bool wifi_softmac_set_appie(wifi_softmac_t* sm, uint8_t type, uint8_t* ie, size_t len){
  if(type == WIFI_APPIE_RSN && len <= sizeof sm->assoc_ie){ memcpy(sm->assoc_ie, ie, len); sm->assoc_ie_len = (uint8_t)len; }
  /* AKM=SAE (00-0f-ac:8) in the RSN IE -> SAE auth. ver(2) group(4) pair_cnt(2)+n*4 akm_cnt(2)+n*4 */
  if(type != WIFI_APPIE_RSN){ SM_LOG(sm, "set_appie type:%u len:%zu (ignored)", type, len); return true; }
  sm->is_sae = false;
  if(len >= 4 && ie[0] == 0x30){
    const uint8_t* r = ie + 2; size_t rl = ie[1] < len - 2 ? ie[1] : len - 2, q = 2 + 4;
    if(q + 2 <= rl){ uint16_t pc = r[q] | (r[q+1] << 8); q += 2 + (size_t)pc * 4;
      if(q + 2 <= rl){ uint16_t ac = r[q] | (r[q+1] << 8); q += 2;
        for(uint16_t k = 0; k < ac && q + 4 <= rl; k++, q += 4)
          if(r[q] == 0x00 && r[q+1] == 0x0f && r[q+2] == 0xac && r[q+3] == 0x08){ sm->is_sae = true; break; }
      }
    }
  }
  SM_LOG(sm, "set_appie type:%u len:%zu sae:%d", type, len, sm->is_sae);
  return true;
}

bool wifi_softmac_set_key(wifi_softmac_t* sm, wifi_key_t* k, uint8_t* key){
  if(k->key_len != 16){ SM_LOG(sm, "set_key: unexpected key_len %u", k->key_len); return false; }
  if(k->key_flag & WIFI_KEY_FLAG_PAIRWISE){ memcpy(sm->tk, key, 16); sm->tk_set = true; sm->tx_pn = 1; }
  else if(k->key_flag & WIFI_KEY_FLAG_GROUP){ memcpy(sm->gtk, key, 16); sm->gtk_idx = k->key_idx; sm->gtk_set = true; }
  SM_LOG(sm, "set_key %s idx:%u (sw CCMP)", (k->key_flag & WIFI_KEY_FLAG_PAIRWISE) ? "pairwise" : "group", k->key_idx);
  return true;
}

bool wifi_softmac_set_igtk(wifi_softmac_t* sm, wifi_igtk_t* igtk){
  memcpy(sm->igtk, igtk->igtk, 16);                        // BIP-CMAC-128 uses the first 16 bytes
  sm->igtk_id = (uint8_t)(igtk->keyid[0] | (igtk->keyid[1] << 8));
  memcpy(sm->igtk_ipn, igtk->pn, 6);
  sm->igtk_set = true;
  SM_LOG(sm, "set_igtk id:%u (BIP-CMAC-128)", sm->igtk_id);
  return true;
}

/* OS-originated mgmt frame (action etc.): body after the 24B header. */
bool wifi_softmac_send_mgmt(wifi_softmac_t* sm, wifi_mgmt_tx_t* m, uint8_t* body, size_t len){
  uint8_t f[600];
  if(24 + len > sizeof f) return false;
  f[0] = (uint8_t)((m->subtype & 0x0f) << 4); f[1] = 0; f[2] = 0; f[3] = 0;
  memcpy(f + 4, m->da, 6);
  memcpy(f + 10, STA_MAC, 6);
  memcpy(f + 16, sm->state == WSM_IDLE ? m->da : sm->ap_bssid, 6);
  uint16_t sc = next_seq(sm); f[22] = sc & 0xff; f[23] = sc >> 8;
  memcpy(f + 24, body, len);
  /* robust action categories (not public=4 / HT=7 / self-protected=15 / vendor=127) are protected under PMF */
  bool robust = m->subtype == 13 && len >= 1 && body[0] != 4 && body[0] != 7 && body[0] != 15 && body[0] != 127;
  bool ok = robust && !(m->da[0] & 1) ? tx_robust_mgmt(sm, f, 24 + len) : radio_tx(sm, f, 24 + len);
  SM_LOG(sm, "TX mgmt subtype:%u len:%zu -> %d", m->subtype, len, ok);
  return ok;
}

/* Outbound 802.3 (EAPOL/data from the OS) -> 802.11 data (ToDS) + LLC/SNAP. */
bool wifi_softmac_tx_data(wifi_softmac_t* sm, uint8_t* eth, size_t len){
  if(sm->state != WSM_CONNECTED || len < 14) return false;
  uint8_t f[2400]; size_t n = 0;
  f[0] = 0x08; f[1] = 0x01;                 // data, ToDS=1
  f[2] = 0; f[3] = 0;
  memcpy(f + 4,  sm->ap_bssid, 6);          // addr1 = BSSID (RA)
  memcpy(f + 10, STA_MAC, 6);               // addr2 = SA (TA)
  memcpy(f + 16, eth, 6);                   // addr3 = DA
  uint16_t sc = next_seq(sm); f[22] = sc & 0xff; f[23] = sc >> 8;
  n = 24;
  f[n++] = 0xAA; f[n++] = 0xAA; f[n++] = 0x03; f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0x00;
  f[n++] = eth[12]; f[n++] = eth[13];
  size_t payload = len - 14;
  if(n + payload > sizeof f) return false;
  memcpy(f + n, eth + 14, payload); n += payload;
  /* CCMP once the PTK is installed; EAPOL (the 4-way) always goes in the clear, matching
   * the OS supplicant's key-install ordering that the MT7601U path was validated against. */
  uint16_t etype = (uint16_t)((eth[12] << 8) | eth[13]);
  if(sm->tk_set && etype != WIFI_ETHERTYPE_EAPOL){
    uint8_t enc[2400 + 16];
    size_t outn = ccmp_encrypt(sm->tk, f, 24, false, false, sm->tx_pn, 0, f + 24, n - 24, enc);
    sm->tx_pn++;
    return radio_tx(sm, enc, outn);
  }
  return radio_tx(sm, f, n);
}

void wifi_softmac_init(wifi_softmac_t* sm, wifi_chip_t* chip, const wifi_softmac_radio_t* radio, void* ctx, const char* name){
  const uint8_t* chans = sm->scan_chans; uint8_t n = sm->n_scan_chans; uint16_t dwell = sm->scan_dwell_ms;
  memset(sm, 0, sizeof *sm);
  sm->chip = chip; sm->radio = radio; sm->ctx = ctx; sm->name = name;
  sm->scan_chans = chans; sm->n_scan_chans = n; sm->scan_dwell_ms = dwell;
}

void wifi_softmac_stop(wifi_softmac_t* sm){
  io_rem_timer(&sm->conn_timer);
  io_rem_timer(&sm->link_timer);
  io_rem_timer(&sm->scan_timer);
  sm->scanning = false;
  sm->state = WSM_IDLE;
  sm->tk_set = sm->gtk_set = sm->igtk_set = false;
}
