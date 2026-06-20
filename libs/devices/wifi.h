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

/* The on-the-wire ABI (planes, opcodes, packed payload structs) lives in a
 * dependency-free header so an OS can include it directly without dragging in
 * libusb / the devicesim io+common macros pulled by this file. */
#include "wifi_proto.h"

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
