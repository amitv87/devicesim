#ifndef DEVICE_WCHIP_RTL8821CU_H
#define DEVICE_WCHIP_RTL8821CU_H

#include "wifi.h"
#include "wifi_softmac.h"
#include "../usb/transfer.h"
#include "../util/io.h"

/* Realtek RTL8821CU (USB 0x0bda:0xc820, 1T1R 802.11ac + BT 4.2 combo) WLAN
 * backend for wifi_chip_ops. Interface 2 is the vendor-class WLAN function
 * (interface 0/1 are the BT HCI function, driven separately by hci_usb).
 *
 * Bring-up is transcribed from the Realtek vendor driver (8821cu-20210916:
 * halmac power sequence / firmware DDMA download / MAC init, phydm BB+AGC+RF
 * tables and channel switch) with rtw88 as a cross-reference. The radio runs
 * as a SoftMAC (no hardware crypto, fixed-rate TX); the 802.11 STA MLME and
 * software CCMP live in the shared wifi_softmac layer. */

#define RTL_RX_XFERS   4
#define RTL_RX_BUF_SZ  32768
#define RTL_TX_XFERS   8
#define RTL_TX_BUF_SZ  2400
#define RTL_EFUSE_SZ   512

typedef struct wchip_rtl8821cu_s wchip_rtl8821cu_t;

typedef struct{
  wchip_rtl8821cu_t* rtl;
  struct libusb_transfer* xfer;
  bool busy;
  uint8_t buf[RTL_TX_BUF_SZ];
} rtl_tx_slot_t;

typedef struct{
  wchip_rtl8821cu_t* rtl;
  struct libusb_transfer* xfer;
  bool active;
  bool stalled;             // last completion was a STALL: clear the halt before resubmitting
  uint8_t buf[RTL_RX_BUF_SZ];
} rtl_rx_slot_t;

typedef struct wchip_rtl8821cu_s{
  wifi_chip_t base;
  usb_device_t usb_device;
  int if_no;
  bool up;                  // MAC+PHY initialised, RX running
  uint8_t ep_in, ep_int;    // bulk-IN (RX), interrupt-IN
  uint8_t ep_out[4];        // bulk-OUT in descriptor order: [0]=HQ [1]=NQ [2]=LQ
  uint8_t n_out;
  uint8_t cut;              // chip cut (0=A ... 4=E)
  // logical efuse (eeprom) map + parsed fields
  uint8_t efuse[RTL_EFUSE_SZ];
  uint8_t mac_addr[6];
  uint8_t rfe_expand;       // raw efuse 0xCA
  uint8_t rfe_type;         // phydm table rfe (= expand >> 3)
  uint8_t pkg_type;
  uint8_t xtal_cap;
  uint8_t thermal;
  uint8_t board_opt, bt_setting;
  bool    btg;              // 2.4G front end on the BT-shared (BTG) path
  // fw / tx fifo
  uint16_t fw_ver; uint8_t fw_sub;
  uint16_t rsvd_bndy;       // reserved-page boundary (TX FIFO page), 0 until TRX init
  uint16_t h2c_seq;         // FW-offload H2C packet sequence
  uint8_t  h2c_box;         // next HMEBOX mailbox (0..3)
  // phy shadow
  uint32_t rega24, rega28, regaac;
  uint8_t channel;
  uint8_t mode;             // wifi_mode_t (MONITOR: raw 802.11 RX on the RAW80211 plane)
  uint8_t band;             // 0 = 2.4G, 1 = 5G, 0xff = unset
  // async TX/RX
  rtl_tx_slot_t tx[RTL_TX_XFERS];
  rtl_rx_slot_t rx[RTL_RX_XFERS];
  uint32_t rx_pkts, rx_crc_err, tx_ok, tx_err, tx_drop;
  uint32_t tx_acked, tx_noack;  // firmware CCX reports for unicast mgmt
  bool in_usb_cb;           // inside a libusb completion: no sync register I/O allowed
  uint8_t bssid[6];         // BSS programmed for HW ACK/filtering (zero = no link)
  io_timer_t bssid_timer;   // deferred apply when set from a USB callback
  io_timer_t stats_timer;   // RTL8821CU_DEBUG: 1 Hz RX/TX counters
  io_timer_t rx_retry_timer;// resubmits RX slots that ended on a transient USB error
  uint16_t rx_err_streak;   // consecutive failed RX completions (reset by a good one)
  // shared STA MLME (scan/auth/assoc/SAE/CCMP/PMF)
  wifi_softmac_t sm;
} wchip_rtl8821cu_t;

extern const wifi_chip_ops_t wchip_rtl8821cu_ops;

#endif
