#ifndef DEVICE_WIFI_SOFTMAC_H
#define DEVICE_WIFI_SOFTMAC_H

#include "wifi.h"
#include "../util/io.h"

/* Radio-agnostic 802.11 STA MLME for SoftMAC backends.
 *
 * The OS sees a FullMAC-style device on the pty (SCAN/CONNECT/keys on CTRL,
 * 802.3 on DATA/EAPOL), while the radio only moves raw 802.11 frames. This
 * layer bridges the two: backend-owned channel-hop scan, open + SAE (WPA3)
 * authentication, association (incl. the 802.11w SA-Query comeback), 802.3 <->
 * 802.11 data framing, software CCMP for pairwise/group data, and PMF (CCMP
 * protected unicast deauth, BIP-CMAC-128 verified group deauth).
 *
 * Ported from the MT7601U backend's in-tree MLME; a chip backend supplies the
 * radio hooks below and feeds received MPDUs to wifi_softmac_rx(). */

/* Harness STA MAC. The OS uses the same address as its netif hwaddr (the
 * supplicant's SPA in PTK derivation), so every backend programs this into the
 * radio instead of its factory address. */
extern const uint8_t WIFI_SOFTMAC_STA_MAC[6];

typedef struct{
  // Queue one 802.11 MPDU for transmission (no FCS). Must be safe to call from
  // the backend's RX completion and from io timers.
  bool (*tx)(void* ctx, const uint8_t* frame, size_t len);
  // Tune the radio (and keep RX running) on `channel` (2.4 GHz 1-14, 5 GHz 36+).
  bool (*set_channel)(void* ctx, uint8_t channel);
  // Program the BSS the hardware should ACK/filter for; NULL = no link.
  void (*set_bssid)(void* ctx, const uint8_t* bssid);
  // Optional: per-link RF calibration (e.g. IQK) on the tuned channel, run before auth.
  void (*prepare_link)(void* ctx, uint8_t channel);
} wifi_softmac_radio_t;

enum { WSM_IDLE = 0, WSM_AUTH, WSM_SAE_COMMIT, WSM_SAE_CONFIRM, WSM_ASSOC, WSM_CONNECTED };

#define WIFI_SOFTMAC_REASON_BEACON_TIMEOUT 200   /* ESP-IDF WIFI_REASON_BEACON_TIMEOUT */
#define WIFI_SOFTMAC_LINK_TIMEOUT_MS      8000   /* no frame from the AP for this long -> link lost */

typedef struct{
  uint8_t  bssid[6];
  uint8_t  channel;
  int8_t   rssi;
  uint16_t capinfo;
  uint16_t ie_len;
  uint8_t  ies[512];   // whole elements only (HT+VHT+HE+vendor beacons exceed 320)
} wifi_softmac_bss_t;

typedef struct wifi_softmac_s{
  wifi_chip_t* chip;              // up-calls go to chip->dev
  const wifi_softmac_radio_t* radio;
  void* ctx;                      // passed to radio hooks
  const char* name;               // log prefix
  const uint8_t* scan_chans;      // channels hopped for a full scan
  uint8_t n_scan_chans;
  uint16_t scan_dwell_ms;         // default per-channel dwell
  uint8_t channel;                // current channel (as last tuned by us)
  // STA association MLME
  uint8_t    state;
  uint8_t    ap_bssid[6];
  char       ssid[33];
  uint8_t    ssid_len;
  uint8_t    assoc_ie[96];        // RSN IE staged via set_appie, echoed in assoc-req
  uint8_t    assoc_ie_len;
  uint16_t   tx_seq;
  io_timer_t conn_timer;
  uint8_t    conn_tries;
  uint8_t    link_channel;        // AP channel (scans while connected return here)
  uint64_t   last_ap_rx_ms;       // last frame from the AP (beacon-loss watchdog)
  io_timer_t link_timer;
  uint16_t   rx_sc[17];           // duplicate detection: last seq-ctrl from the AP [0]=mgmt/non-QoS, [1+tid]=QoS
  uint32_t   rx_sc_valid;         // bit n: rx_sc[n] holds a value
  uint32_t   rx_dups;             // retries dropped this association
  bool       assoc_comeback;      // status-30 (SA-Query) reject pending an assoc retry
  // software CCMP keys
  uint8_t    tk[16];  bool tk_set;
  uint8_t    gtk[16]; uint8_t gtk_idx; bool gtk_set;
  uint64_t   tx_pn;
  // 802.11w BIP-CMAC-128
  uint8_t    igtk[16]; uint8_t igtk_id; uint8_t igtk_ipn[6]; bool igtk_set;
  // WPA3 SAE (algorithm 3): OS builds commit/confirm bodies
  bool       is_sae;
  uint8_t    sae_tx[400];
  uint16_t   sae_tx_len;
  // channel-hop scan
  bool       scanning;
  uint8_t    scan_idx, scan_last;
  uint8_t    scan_single;         // nonzero: dwell only on this channel
  io_timer_t scan_timer;
  uint8_t    n_results;
  wifi_softmac_bss_t results[32];
} wifi_softmac_t;

void wifi_softmac_init(wifi_softmac_t* sm, wifi_chip_t* chip, const wifi_softmac_radio_t* radio, void* ctx, const char* name);
void wifi_softmac_stop(wifi_softmac_t* sm);   // cancel timers, forget link (no TX)

/* RX entry: one received 802.11 MPDU (FCS stripped or not; CCMP tolerates both). */
void wifi_softmac_rx(wifi_softmac_t* sm, const uint8_t* frame, size_t len, int8_t rssi, uint8_t channel);

/* wifi_chip_ops implementations (call from the backend's op wrappers). */
bool wifi_softmac_scan(wifi_softmac_t* sm, wifi_scan_req_t* req);
bool wifi_softmac_connect(wifi_softmac_t* sm, wifi_connect_req_t* req);
bool wifi_softmac_disconnect(wifi_softmac_t* sm, uint8_t reason);
bool wifi_softmac_reset(wifi_softmac_t* sm, uint8_t home_channel);
bool wifi_softmac_set_appie(wifi_softmac_t* sm, uint8_t type, uint8_t* ie, size_t len);
bool wifi_softmac_set_key(wifi_softmac_t* sm, wifi_key_t* k, uint8_t* key);
bool wifi_softmac_set_igtk(wifi_softmac_t* sm, wifi_igtk_t* igtk);
bool wifi_softmac_sae_msg(wifi_softmac_t* sm, wifi_sae_t* s, uint8_t* body, size_t len);
bool wifi_softmac_send_mgmt(wifi_softmac_t* sm, wifi_mgmt_tx_t* m, uint8_t* body, size_t len);
bool wifi_softmac_tx_data(wifi_softmac_t* sm, uint8_t* eth, size_t len);

#endif
