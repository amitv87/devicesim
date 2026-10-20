#ifndef DEVICE_BT_RTL_H
#define DEVICE_BT_RTL_H

#include "../usb/device.h"

/* Realtek USB Bluetooth firmware patch download (port of Linux btrtl.c for the
 * RTL8821C, the BT half of the RTL8821CU combo).
 *
 * The controller boots a ROM image that answers HCI but is incomplete (e.g. LE
 * advertising reports carry bogus RSSI). This uploads the linux-firmware
 * rtl_bt/rtl8821c_fw.bin patch + rtl8821c_config.bin with vendor command 0xFC20.
 *
 * Must run synchronously on a claimed HCI interface before the async event/ACL
 * transfers are armed (it reads HCI events itself from `evt_ep`). Returns true
 * when the controller ends up patched (or already was); false when it is not an
 * RTL8821C in ROM state or the download failed (the controller then stays in ROM,
 * which still works for basic HCI). */
bool bt_rtl_load_patch(usb_device_t* dev, uint8_t if_no, uint8_t evt_ep);

#endif
