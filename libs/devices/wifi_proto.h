#ifndef DEVICE_WIFI_PROTO_H
#define DEVICE_WIFI_PROTO_H

/* ---------------------------------------------------------------------------
 * devicesim WiFi pty WIRE ABI — the on-the-wire framing the OS and devicesim
 * agree on. Deliberately dependency-free (only <stdint.h>) so an OS can include
 * THIS header directly (e.g. by absolute path) without dragging in libusb / the
 * devicesim io/common macros that wifi.h pulls. wifi.h includes this and adds
 * the devicesim-internal backend abstraction (wifi_chip_ops / wifi_dev).
 *
 * Wire framing (one pty byte stream, H4-style multiplex):
 *
 *     +-------+-------+--------+--------+===============+
 *     | plane |  op   | len_lo | len_hi |    payload    |
 *     +-------+-------+--------+--------+===============+
 *        u8      u8      u8       u8        len bytes
 *
 *   len   = uint16 little-endian = payload length (bytes after the 4B header)
 *   plane = wifi_plane_t
 *   op    = on CTRL plane, a wifi_cmd_t (OS->dev) or wifi_ev_t (dev->OS);
 *           ignored (0) on DATA / EAPOL / RAW80211 planes.
 *
 * DATA  plane payload  = one 802.3 Ethernet frame (post-decrypt, both ways).
 * EAPOL plane payload  = one 802.3 Ethernet frame, ethertype 0x888E (the 4-way).
 * ------------------------------------------------------------------------- */

#include <stdint.h>

#ifndef __PACKED__
#define __PACKED__ __attribute__((__packed__))
#endif

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
  WIFI_CMD_RESET        = 0x0C, // empty: clear radio state (deauth, keys, scan) — OS startup
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

/* SAE message type (wifi_sae_t.sae_type) — matches the 802.11 auth transaction seq */
typedef enum{
  WIFI_SAE_COMMIT  = 1,
  WIFI_SAE_CONFIRM = 2,
} wifi_sae_type_t;

#define WIFI_SAE_GROUP_P256 19   /* finite cyclic group 19 = NIST P-256 */

/* key cipher (wifi_key_t.alg), mirrors esp wifi_wpa_alg_t */
typedef enum{
  WIFI_WPA_ALG_NONE  = 0,
  WIFI_WPA_ALG_WEP40 = 1,
  WIFI_WPA_ALG_TKIP  = 2,
  WIFI_WPA_ALG_CCMP  = 3,
  WIFI_WPA_ALG_IGTK  = 4,
} wifi_wpa_alg_t;

/* key install flags (wifi_key_t.key_flag), bitmask */
enum{
  WIFI_KEY_FLAG_PAIRWISE = 1u << 0,
  WIFI_KEY_FLAG_GROUP    = 1u << 1,
  WIFI_KEY_FLAG_TX       = 1u << 2,
  WIFI_KEY_FLAG_RX       = 1u << 3,
};

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

#endif /* DEVICE_WIFI_PROTO_H */
