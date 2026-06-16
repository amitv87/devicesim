#ifndef DEVICE_WCHIP_SIM_H
#define DEVICE_WCHIP_SIM_H

#include "wifi.h"

/* A hardware-free wifi_chip_ops backend. It fakes the MLME the radio firmware
 * normally owns (scan, auth, assoc) so the OS-side supplicant + IP stack can be
 * brought up before any MT7601U is involved. Open networks complete end to end;
 * WPA assoc succeeds but the 4-way handshake has no AP authenticator behind it
 * (tx_data just logs) -- swap in wchip_mt7601u for real RF. */
typedef struct{
  wifi_chip_t base;
  uint8_t mode;
  uint8_t bssid[6];
  bool connected;
} wchip_sim_t;

extern const wifi_chip_ops_t wchip_sim_ops;

#endif
