#ifndef DEVICE_WCHIP_MT7601U_H
#define DEVICE_WCHIP_MT7601U_H

#include "wifi.h"
#include "../usb/transfer.h"
#include "../util/io.h"

/* MediaTek/Ralink MT7601U (USB 0x148f:0x7601) backend for wifi_chip_ops.
 *
 * SCAFFOLD: USB open/claim/endpoint discovery + vendor register reads are
 * wired (via usb_device_control_transfer); the MLME the firmware owns (firmware
 * download, MCU command protocol, TXWI/RXWI encap/decap, RF calibration,
 * scan/auth/assoc) is left as marked TODOs. Reference: the Linux `mt7601u`
 * mac80211 driver (drivers/net/wireless/mediatek/mt7601u). */
typedef struct{
  wifi_chip_t base;
  usb_device_t usb_device;
  int if_no;
  uint8_t ep_cmd_resp; // second bulk-IN: MCU command responses
  uint8_t data_tx_ep;  // AC_BE bulk-OUT (out_eps[2]): data/injection
  uint8_t mcu_seq;     // rolling 1..15 sequence for response-bearing cmds
  uint8_t mac_addr[6]; // read from eFUSE/EEPROM
  int8_t  ee_rf_freq_off;
  int8_t  ee_ref_temp;
  int8_t  ee_lna_gain;
  int8_t  ee_rssi_offset[2];
  bool    ee_tssi_enabled;
  uint32_t rf_pa_mode[2];
  int     bw;          // MT_BW_20/40, -1 = unset
  int     temp_mode;   // mt_temp_mode, -1 = unset
  bool    chan_ext_below;
  int     raw_temp, curr_temp, dpd_temp;
  bool    pll_lock_protect;
  uint8_t channel;     // current channel (1-based)
  uint8_t mode;        // wifi_mode_t
  union{
    struct{ usb_transfer_t rx, tx; };
    usb_transfer_t xfers[2];
  };
  uint8_t rx_buff[4096];
  // STA association MLME (managed mode): open auth → assoc → CONNECTED.
  uint8_t    conn_state;      // mt_conn_state_t (0 idle)
  uint8_t    ap_bssid[6];
  char       conn_ssid[33];
  uint8_t    conn_ssid_len;
  uint8_t    assoc_ie[64];    // RSN IE staged via set_appie, echoed in assoc-req
  uint8_t    assoc_ie_len;
  uint16_t   tx_seq;          // 802.11 sequence number generator
  io_timer_t conn_timer;      // auth/assoc retry + timeout
  uint8_t    conn_tries;
  // software CCMP (monitor-mode radio has no HW cipher) — keys from set_key
  uint8_t    tk[16];          // pairwise (PTK TK)
  bool       tk_set;
  uint8_t    gtk[16];         // group key
  uint8_t    gtk_idx;
  bool       gtk_set;
  uint64_t   tx_pn;           // CCMP packet number (TX)
  // active scan: backend channel-hop + beacon collect → SCAN_RESULT (FullMAC-presented).
  bool       scanning;
  uint8_t    scan_max_chan;   // last channel to dwell (req->channel or 13)
  io_timer_t scan_timer;
  uint8_t    n_results;
  struct mt_scan_result{
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;
    uint16_t capinfo;
    uint16_t ie_len;
    uint8_t  ies[256];
  } results[24];
} wchip_mt7601u_t;

extern const wifi_chip_ops_t wchip_mt7601u_ops;

#endif
