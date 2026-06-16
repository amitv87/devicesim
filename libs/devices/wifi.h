#ifndef DEVICE_WIFI_H
#define DEVICE_WIFI_H

#include "../usb/host.h"
#include "../common.h"

/* ---------------------------------------------------------------------------
 * Virtual WiFi controller exposed over a pty.
 *
 * This is a direct projection of ESP-IDF's full-MAC boundary onto a byte
 * stream. On real ESP32 silicon the supplicant (wpa_supplicant) registers a
 * `struct wpa_funcs` of up-calls into the radio blob and drives it back via a
 * handful of `esp_wifi_*_internal()` down-calls (see
 * components/wpa_supplicant/esp_supplicant/src/esp_wifi_driver.h).
 *
 * Here, devicesim plays the radio blob: it owns the MLME the firmware normally
 * owns (scan, auth, assoc, and the SIFS-timing lower MAC that can never leave
 * the chip), and exposes the *same* boundary to your OS over the pty. Your OS
 * runs the supplicant — the 4-way handshake, key derivation, RSN IE, SAE body,
 * action frames — exactly as ESP-IDF's port of hostap does.
 *
 *   [ your OS / wpa_supplicant ]  <-- pty (/tmp/tty.wifi) -->  [ devicesim ]
 *        wpa_funcs / esp_wifi_*          this protocol         wifi_chip_ops
 *                                                                   |
 *                                                          sim  or  mt7601u (USB)
 *
 * The chip-specific backend (MT7601U firmware download, MCU protocol,
 * TXWI/RXWI) lives behind `wifi_chip_ops_t` so the wire protocol stays radio
 * independent. A `wchip_sim` backend implements the same vtable with no
 * hardware so the OS side can be developed before the dongle is involved.
 *
 * ---------------------------------------------------------------------------
 * Wire framing (one pty byte stream, H4-style multiplex):
 *
 *     +-------+-------+--------+--------+===============+
 *     | plane |  op   | len_lo | len_hi |    payload    |
 *     +-------+-------+--------+--------+===============+
 *        u8      u8      u8       u8        len bytes
 *
 *   len   = uint16 little-endian = payload length (bytes after the 4B header)
 *   plane = WIFI_PLANE_*
 *   op    = on CTRL plane, a WIFI_CMD_* (OS->dev) or WIFI_EV_* (dev->OS);
 *           ignored (0) on DATA / EAPOL planes.
 *
 * DATA  plane payload  = one 802.3 Ethernet frame (post-decrypt, both ways).
 * EAPOL plane payload  = one 802.3 Ethernet frame with ethertype 0x888E.
 *                        devicesim splits inbound 802.11 data by ethertype so
 *                        the supplicant gets a clean rx_eapol channel (the
 *                        wpa_sta_rx_eapol analog) even before keys are set.
 * ------------------------------------------------------------------------- */

#define WIFI_ETHERTYPE_EAPOL 0x888E

typedef enum{
  WIFI_PLANE_CTRL     = 0,
  WIFI_PLANE_DATA     = 1, // 802.3 ethernet
  WIFI_PLANE_EAPOL    = 2, // 802.3 ethernet, ethertype 0x888E
  WIFI_PLANE_RAW80211 = 3, // [wifi_rx80211_t] + raw 802.11 frame (SoftMAC/monitor)
} wifi_plane_t;

/* CTRL ops: OS -> devicesim (down-calls, the esp_wifi_*_internal analogs) */
typedef enum{
  WIFI_CMD_SET_MODE     = 0x01, // u8 mode (WIFI_MODE_*)
  WIFI_CMD_SCAN         = 0x02, // wifi_scan_req_t
  WIFI_CMD_CONNECT      = 0x03, // wifi_connect_req_t   (esp_wifi_sta_connect_internal)
  WIFI_CMD_DISCONNECT   = 0x04, // u8 reason            (esp_wifi_deauthenticate_internal)
  WIFI_CMD_SET_APPIE    = 0x05, // wifi_appie_t + ie    (esp_wifi_set_appie_internal)
  WIFI_CMD_SET_KEY      = 0x06, // wifi_key_t + key     (esp_wifi_set_sta_key_internal)
  WIFI_CMD_SET_IGTK     = 0x07, // wifi_igtk_t          (esp_wifi_set_igtk_internal)
  WIFI_CMD_SEND_MGMT    = 0x08, // wifi_mgmt_tx_t + frm (esp_wifi_send_mgmt_frm_internal)
  WIFI_CMD_SAE_MSG      = 0x09, // wifi_sae_t + body    (reply to WIFI_EV_SAE_BUILD)
  WIFI_CMD_CONFIG_DONE  = 0x0A, // empty                (wpa_config_done: profile ready)
  WIFI_CMD_SET_CHANNEL  = 0x0B, // u8 channel           (monitor/SoftMAC tuning)
} wifi_cmd_t;

/* CTRL ops: devicesim -> OS (up-calls, the wpa_funcs callback analogs) */
typedef enum{
  WIFI_EV_CONNECT       = 0x81, // u8 bssid[6]          (wpa_sta_connect: prep profile)
  WIFI_EV_CONNECTED     = 0x82, // u8 bssid[6]          (wpa_sta_connected_cb: assoc done)
  WIFI_EV_DISCONNECTED  = 0x83, // u8 reason            (wpa_sta_disconnected_cb)
  WIFI_EV_RX_MGMT       = 0x84, // wifi_mgmt_rx_t + frm (wpa_sta_rx_mgmt: action/auth)
  WIFI_EV_SCAN_RESULT   = 0x85, // wifi_scan_result_t + ies
  WIFI_EV_SCAN_DONE     = 0x86, // empty
  WIFI_EV_SAE_BUILD     = 0x87, // wifi_sae_t           (wpa3_build_sae_msg request)
  WIFI_EV_SAE_RX        = 0x88, // wifi_sae_t + body    (wpa3_parse_sae_msg)
  WIFI_EV_MIC_FAILURE   = 0x89, // u8 is_unicast        (wpa_michael_mic_failure)
} wifi_ev_t;

typedef enum{
  WIFI_MODE_NULL    = 0,
  WIFI_MODE_STA     = 1,
  WIFI_MODE_AP      = 2,
  WIFI_MODE_MONITOR = 3,
} wifi_mode_t;

// RAW80211 plane RX header (radiotap-ish), prepended to each captured frame.
typedef struct{
  uint8_t  channel;
  int8_t   rssi;     // dBm
  uint16_t rate;     // raw RXWI rate word
  uint8_t  flags;
} __PACKED__ wifi_rx80211_t;

/* appie types, mirrors wifi_appie_t in esp_wifi_types */
typedef enum{
  WIFI_APPIE_WPA      = 0,
  WIFI_APPIE_RSN      = 1,
  WIFI_APPIE_ASSOC_REQ= 2,
} wifi_appie_type_t;

/* ---- CTRL payload structs (all little-endian, packed) ------------------- */

typedef struct{
  uint8_t channel;   // 0 = scan all channels
  uint8_t passive;   // 0 active, 1 passive
} __PACKED__ wifi_scan_req_t;

typedef struct{
  uint8_t bssid[6];
  uint8_t channel;
  uint8_t ssid_len;
  uint8_t ssid[32];
} __PACKED__ wifi_connect_req_t;

typedef struct{
  uint8_t  type;     // wifi_appie_type_t
  uint16_t ie_len;   // ie bytes follow
} __PACKED__ wifi_appie_t;

typedef struct{
  uint8_t alg;       // WIFI_WPA_ALG_* (CCMP/TKIP/IGTK/PMK ...)
  uint8_t addr[6];   // peer (PTK) or broadcast (GTK)
  uint8_t key_idx;
  uint8_t set_tx;
  uint8_t key_flag;  // KEY_FLAG_PAIRWISE / GROUP / RX / TX ...
  uint8_t seq_len;
  uint8_t seq[8];
  uint8_t key_len;   // key bytes follow
} __PACKED__ wifi_key_t;

typedef struct{
  uint8_t keyid[2];
  uint8_t pn[6];
  uint8_t igtk[32];
} __PACKED__ wifi_igtk_t;

typedef struct{
  uint8_t  subtype;  // WLAN_FC_STYPE_* (e.g. ACTION)
  uint8_t  da[6];
  uint32_t freq;     // 0 = current
  // 802.11 frame body follows
} __PACKED__ wifi_mgmt_tx_t;

typedef struct{
  uint8_t  subtype;
  uint8_t  sa[6];
  int8_t   rssi;
  uint8_t  channel;
  uint64_t tsf;
  // 802.11 frame follows
} __PACKED__ wifi_mgmt_rx_t;

typedef struct{
  uint8_t  bssid[6];
  uint8_t  channel;
  int8_t   rssi;
  uint16_t capinfo;
  // raw IEs follow (ssid, wpa, rsn, ... as seen on air)
} __PACKED__ wifi_scan_result_t;

typedef struct{
  uint8_t  bssid[6];
  uint32_t sae_type; // commit / confirm
  uint16_t status;
  // SAE message body follows
} __PACKED__ wifi_sae_t;

/* ---- backend abstraction (the "hide chip specifics" boundary) ----------- */

typedef struct wifi_dev_s  wifi_dev_t;
typedef struct wifi_chip_s wifi_chip_t;

// Down-calls: generic layer -> radio backend. NULL ops are treated as no-ops.
typedef struct{
  bool (*init)(wifi_chip_t* chip, usb_dev_info_t* info); // info NULL for sim
  bool (*deinit)(wifi_chip_t* chip);
  bool (*set_mode)(wifi_chip_t* chip, uint8_t mode);
  bool (*set_channel)(wifi_chip_t* chip, uint8_t channel);
  bool (*scan)(wifi_chip_t* chip, wifi_scan_req_t* req);
  bool (*connect)(wifi_chip_t* chip, wifi_connect_req_t* req); // drive auth+assoc
  bool (*disconnect)(wifi_chip_t* chip, uint8_t reason);
  bool (*set_appie)(wifi_chip_t* chip, uint8_t type, uint8_t* ie, size_t len);
  bool (*set_key)(wifi_chip_t* chip, wifi_key_t* k, uint8_t* key);
  bool (*set_igtk)(wifi_chip_t* chip, wifi_igtk_t* igtk);
  bool (*send_mgmt)(wifi_chip_t* chip, wifi_mgmt_tx_t* m, uint8_t* frame, size_t len);
  bool (*sae_msg)(wifi_chip_t* chip, wifi_sae_t* s, uint8_t* body, size_t len);
  bool (*config_done)(wifi_chip_t* chip);
  bool (*tx_data)(wifi_chip_t* chip, uint8_t* eth, size_t len);     // DATA or EAPOL eth frame
  bool (*tx_raw80211)(wifi_chip_t* chip, uint8_t* frame, size_t len); // inject raw 802.11 frame
} wifi_chip_ops_t;

// Base mixed into each concrete backend (embed as first member).
typedef struct wifi_chip_s{
  wifi_dev_t* dev;            // back-pointer for up-calls
  const wifi_chip_ops_t* ops;
} wifi_chip_t;

typedef int (*wifi_output)(wifi_dev_t* dev, uint8_t* data, size_t length);

typedef struct wifi_dev_s{
  void* usr_data;
  wifi_output output;        // -> pty
  wifi_chip_t* chip;         // active backend
  io_buff_t input_buff;
  uint8_t __input_buff[4096];
} wifi_dev_t;

/* Lifecycle + pty -> device path (call wifi_dev_input with bytes off the pty) */
bool wifi_dev_init(wifi_dev_t* dev, wifi_chip_t* chip, usb_dev_info_t* info);
bool wifi_dev_deinit(wifi_dev_t* dev);
void wifi_dev_input(wifi_dev_t* dev, uint8_t* data, size_t length);

/* Up-calls: backend -> generic layer -> pty (the wpa_funcs callback analogs) */
void wifi_dev_on_connect(wifi_dev_t* dev, uint8_t bssid[6]);
void wifi_dev_on_connected(wifi_dev_t* dev, uint8_t bssid[6]);
void wifi_dev_on_disconnected(wifi_dev_t* dev, uint8_t reason);
void wifi_dev_on_rx_mgmt(wifi_dev_t* dev, wifi_mgmt_rx_t* m, uint8_t* frame, size_t len);
void wifi_dev_on_scan_result(wifi_dev_t* dev, wifi_scan_result_t* r, uint8_t* ies, size_t len);
void wifi_dev_on_scan_done(wifi_dev_t* dev);
void wifi_dev_on_sae_build(wifi_dev_t* dev, wifi_sae_t* s);
void wifi_dev_on_sae_rx(wifi_dev_t* dev, wifi_sae_t* s, uint8_t* body, size_t len);
void wifi_dev_on_mic_failure(wifi_dev_t* dev, uint8_t is_unicast);
// Deliver an inbound 802.3 frame; routed to EAPOL plane if ethertype 0x888E.
void wifi_dev_on_rx_data(wifi_dev_t* dev, uint8_t* eth, size_t len);
// Deliver a raw 802.11 frame (monitor/SoftMAC) on the RAW80211 plane.
void wifi_dev_on_rx_80211(wifi_dev_t* dev, uint8_t* frame, size_t len, uint8_t channel, int8_t rssi, uint16_t rate);

#endif
