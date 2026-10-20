#include <stdlib.h>
#include "bt_rtl.h"
#define INCBIN_PREFIX  fw_
#define INCBIN_STYLE   INCBIN_STYLE_SNAKE
#include "../util/incbin.h"

/* linux-firmware rtl_bt/rtl8821c_fw.bin + rtl_bt/rtl8821c_config.bin
 * sha256 fw     dfe7ef772d078ec09490c02190a32fcd40b3eaf3b040049d8930eb80e6d5743c
 * sha256 config 292717b0610ce920f66b734c299d349ea59a4c4400044078d8fc65625f17e382 */
INCBIN(rtl8821c_bt, DEVICES_FW_DIR "/rtl8821c_bt_fw.bin");
INCBIN(rtl8821c_bt_config, DEVICES_FW_DIR "/rtl8821c_bt_config.bin");

#define RTL_ROM_LMP_8821A     0x8821
#define RTL_PROJECT_8821C     10
#define RTL_FRAG_LEN          252
#define RTL_CMD_TIMEOUT_MS    1000

static inline uint16_t le16(const uint8_t* p){ return p[0] | (p[1] << 8); }
static inline uint32_t le32(const uint8_t* p){ return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Send one HCI command (class request on the interface) and wait for its Command
 * Complete. Returns the number of return-parameter bytes copied to `rsp` (status
 * first), or -1. Events arrive on the interrupt EP in <= wMaxPacketSize chunks. */
static int hci_cmd(usb_device_t* dev, uint8_t if_no, uint8_t evt_ep, uint16_t op,
                   const uint8_t* p, uint8_t plen, uint8_t* rsp, int rsp_max){
  uint8_t cmd[3 + 255];
  cmd[0] = op & 0xff; cmd[1] = op >> 8; cmd[2] = plen;
  if(plen) memcpy(cmd + 3, p, plen);
  int rc = usb_device_control_transfer(dev, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
                                       0, 0, if_no, cmd, 3 + plen, RTL_CMD_TIMEOUT_MS);
  if(rc != 3 + plen){ LOG("bt_rtl: cmd 0x%04x send failed (%d)", op, rc); return -1; }

  uint8_t ev[2 + 255]; int have = 0;
  uint64_t end = sys_now() + RTL_CMD_TIMEOUT_MS;
  while(sys_now() < end){
    int n = usb_device_interrupt_transfer(dev, evt_ep, ev + have, sizeof(ev) - have, 200);
    if(n < 0){ if(n == LIBUSB_ERROR_TIMEOUT) continue; return -1; }
    have += n;
    while(have >= 2 && have >= 2 + ev[1]){              // one complete event
      int elen = 2 + ev[1];
      if(ev[0] == 0x0e && elen >= 5 && le16(ev + 3) == op){        // Command Complete
        int n_ret = min(elen - 5, rsp_max);
        if(rsp && n_ret > 0) memcpy(rsp, ev + 5, n_ret);
        return n_ret;
      }
      if(ev[0] == 0x0f && elen >= 6 && le16(ev + 4) == op){        // Command Status
        if(rsp && rsp_max > 0) rsp[0] = ev[2];
        return 1;
      }
      memmove(ev, ev + elen, have - elen); have -= elen;          // unrelated event: skip
    }
    if(have >= (int)sizeof(ev)) have = 0;
  }
  LOG("bt_rtl: cmd 0x%04x timed out", op);
  return -1;
}

/* rtlbt_parse_firmware (epatch v1 "Realtech"): pick the patch for this ROM, end it with
 * the firmware version word, append the config blob. Returns a malloc'd image. */
static uint8_t* build_patch(uint8_t rom_version, bool with_config, uint32_t* out_len, uint32_t* fw_version){
  const uint8_t* fw = fw_rtl8821c_bt_data; uint32_t len = fw_rtl8821c_bt_size;
  static const uint8_t ext_sig[4] = {0x51, 0x04, 0xfd, 0x77};
  if(len < 14 + 4 || memcmp(fw, "Realtech", 8) || memcmp(fw + len - 4, ext_sig, 4)){
    LOG("bt_rtl: bad epatch image"); return NULL;
  }
  int project = -1;                                   // walk the trailing TLVs backwards
  for(const uint8_t* p = fw + len - 4; p >= fw + 14 + 3; ){
    uint8_t op = *--p, l = *--p, data = *--p;
    if(op == 0xff) break;
    if(l == 0) break;
    if(op == 0 && l == 1){ project = data; break; }
    p -= l;
  }
  if(project != RTL_PROJECT_8821C){ LOG("bt_rtl: firmware project %d is not 8821C", project); return NULL; }
  *fw_version = le32(fw + 8);
  uint16_t n = le16(fw + 12);
  if(len < 14 + 8u * n) return NULL;
  uint32_t plen = 0, poff = 0;
  for(uint16_t i = 0; i < n; i++){
    if(le16(fw + 14 + 2 * i) == rom_version + 1){
      plen = le16(fw + 14 + 2 * n + 2 * i);
      poff = le32(fw + 14 + 4 * n + 4 * i);
      break;
    }
  }
  if(!poff || plen < 4 || poff > len || plen > len - poff){ LOG("bt_rtl: no patch for rom %u", rom_version); return NULL; }
  uint32_t cfg = with_config ? fw_rtl8821c_bt_config_size : 0;
  uint8_t* img = malloc(plen + cfg);
  if(!img) return NULL;
  memcpy(img, fw + poff, plen - 4);
  memcpy(img + plen - 4, fw + 8, 4);                  // fw_version replaces the last word
  if(cfg) memcpy(img + plen, fw_rtl8821c_bt_config_data, cfg);
  *out_len = plen + cfg;
  return img;
}

bool bt_rtl_load_patch(usb_device_t* dev, uint8_t if_no, uint8_t evt_ep){
  uint8_t r[64];
  // drain any stale event (e.g. a reset completion from a previous session)
  while(usb_device_interrupt_transfer(dev, evt_ep, r, sizeof r, 50) > 0);

  if(hci_cmd(dev, if_no, evt_ep, 0x1001, NULL, 0, r, sizeof r) < 9 || r[0]){ LOG("bt_rtl: read local version failed"); return false; }
  uint16_t hci_rev = le16(r + 2), lmp_subver = le16(r + 7);
  uint32_t patched = le32(fw_rtl8821c_bt_data + 8);
  if(((uint32_t)hci_rev << 16 | lmp_subver) == patched){
    LOG("bt_rtl: already patched (fw 0x%08x)", patched); return true;
  }
  if(lmp_subver != RTL_ROM_LMP_8821A || hci_rev != 0x000c || r[1] != 0x08){
    LOG("bt_rtl: not an RTL8821C in ROM state (hci_ver 0x%02x hci_rev 0x%04x lmp_subver 0x%04x), leaving as is", r[1], hci_rev, lmp_subver);
    return false;
  }
  if(hci_cmd(dev, if_no, evt_ep, 0xfc6d, NULL, 0, r, sizeof r) < 2 || r[0]){ LOG("bt_rtl: read rom version failed"); return false; }
  uint8_t rom = r[1];
  static const uint8_t sec_proj[5] = {0x10, 0xA4, 0xAD, 0x00, 0xb0};    // RTL_SEC_PROJ
  uint8_t key_id = 0;
  if(hci_cmd(dev, if_no, evt_ep, 0xfc61, sec_proj, 5, r, sizeof r) >= 3 && !r[0]) key_id = r[1];

  uint32_t img_len = 0, fw_ver = 0;
  uint8_t* img = build_patch(rom, key_id == 0, &img_len, &fw_ver);
  if(!img) return false;
  LOG("bt_rtl: RTL8821C rom %u key %u: uploading patch 0x%08x (%u bytes)", rom, key_id, fw_ver, img_len);

  // rtl_download_firmware: 0xFC20 [index][<=252 data], index wraps 0x7f -> 1, last |= 0x80
  uint64_t t0 = sys_now();
  int frags = img_len / RTL_FRAG_LEN + 1;
  bool ok = true;
  for(int i = 0, j = 0; i < frags && ok; i++){
    uint8_t cmd[1 + RTL_FRAG_LEN];
    uint8_t idx = j++;
    if(idx == 0x7f) j = 1;
    int flen = RTL_FRAG_LEN;
    if(i == frags - 1){ idx |= 0x80; flen = img_len % RTL_FRAG_LEN; }
    cmd[0] = idx;
    memcpy(cmd + 1, img + i * RTL_FRAG_LEN, flen);
    int n = hci_cmd(dev, if_no, evt_ep, 0xfc20, cmd, 1 + flen, r, sizeof r);
    if(n != 2 || r[0]){ LOG("bt_rtl: download failed at fragment %d/%d (n %d status 0x%02x)", i, frags, n, n > 0 ? r[0] : 0xff); ok = false; }
  }
  free(img);
  if(!ok) return false;

  if(hci_cmd(dev, if_no, evt_ep, 0x1001, NULL, 0, r, sizeof r) < 9 || r[0]){ LOG("bt_rtl: version check after download failed"); return false; }
  LOG("bt_rtl: patched in %llums: fw version 0x%04x%04x", (unsigned long long)(sys_now() - t0), le16(r + 2), le16(r + 7));
  return true;
}
