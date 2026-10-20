#include <stdlib.h>
#include <unistd.h>
#include "wchip_rtl8821cu.h"
#include "rtl8821c_tables.h"
#define INCBIN_PREFIX  fw_
#define INCBIN_STYLE   INCBIN_STYLE_SNAKE
#include "../util/incbin.h"

/* linux-firmware rtw88/rtw8821c_fw.bin (v24.11.0 NIC image)
 * sha256 2ef409bc418549fcf294061dd0cae1fc22fd9da79b60524950b25de18732f3f0 */
INCBIN(rtl8821c, DEVICES_FW_DIR "/rtw8821c_fw.bin");

/* RTL8821CU WLAN bring-up, transcribed from the Realtek vendor driver
 * (8821cu-20210916): halmac_8821c power sequence / init_*_cfg, halmac_fw_88xx
 * DDMA firmware download, phydm_hal_api8821c channel switch + halhwimg tables.
 * Function names below cite the vendor routine each block mirrors. */

#define RTL(chip) ((wchip_rtl8821cu_t*)(chip))
#define RTL_IFACE 2
#define BIT(n) (1u << (n))
#define FUN_CHK(fun, ...) if(!fun(__VA_ARGS__)){LOG("%s failed", #fun); goto end;}

static bool rtl_debug(void);
static void rtl_coex_band(struct wchip_rtl8821cu_s* rtl, bool is5g);
static inline uint16_t le16(const uint8_t* p){ return p[0] | (p[1] << 8); }
static inline uint32_t le32(const uint8_t* p){ return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void put_le32(uint8_t* p, uint32_t v){ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* ---- register I/O (usb_ops_linux.c usbctrl_vendorreq) --------------------
 * Vendor request 0x05; wValue = register offset. On 8821C an access to the
 * always-on section (0x00-0xFF, 0x1000-0x10FF) must be followed by a 1-byte
 * write to 0x4E0 (both the vendor driver and rtw88 do this). */
#define RTL_VENQT_READ  (LIBUSB_ENDPOINT_IN  | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define RTL_VENQT_WRITE (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define RTL_VENQT_REQ   0x05
#define RTL_IO_TOUT_MS  500

static int rtl_xfer(wchip_rtl8821cu_t* rtl, bool rd, uint16_t addr, uint8_t* buf, uint16_t len){
  libusb_device_handle* h = rtl->usb_device.devh;
  if(!h) return LIBUSB_ERROR_NO_DEVICE;
  int r = -1;
  for(int t = 0; t < 3; t++){
    r = libusb_control_transfer(h, rd ? RTL_VENQT_READ : RTL_VENQT_WRITE, RTL_VENQT_REQ, addr, 0, buf, len, RTL_IO_TOUT_MS);
    if(r == len || r == LIBUSB_ERROR_NO_DEVICE) break;
  }
  if(r != len){
    static int nerr;
    if(nerr++ < 16) LOG("rtl8821cu: %s 0x%04x/%u failed: %d %s", rd ? "read" : "write", addr, len, r, r < 0 ? libusb_strerror(r) : "");
  }
  if(addr <= 0xff || (addr >= 0x1000 && addr <= 0x10ff)){
    uint8_t z = buf[0];
    libusb_control_transfer(h, RTL_VENQT_WRITE, RTL_VENQT_REQ, 0x4e0, 0, &z, 1, RTL_IO_TOUT_MS);
  }
  return r;
}
static uint8_t  r8 (wchip_rtl8821cu_t* rtl, uint16_t a){ uint8_t b[1] = {0}; rtl_xfer(rtl, true, a, b, 1); return b[0]; }
static uint16_t r16(wchip_rtl8821cu_t* rtl, uint16_t a){ uint8_t b[2] = {0}; rtl_xfer(rtl, true, a, b, 2); return le16(b); }
static uint32_t r32(wchip_rtl8821cu_t* rtl, uint16_t a){ uint8_t b[4] = {0}; rtl_xfer(rtl, true, a, b, 4); return le32(b); }
static void w8 (wchip_rtl8821cu_t* rtl, uint16_t a, uint8_t v){ uint8_t b[1] = {v}; rtl_xfer(rtl, false, a, b, 1); }
static void w16(wchip_rtl8821cu_t* rtl, uint16_t a, uint16_t v){ uint8_t b[2] = {v, v >> 8}; rtl_xfer(rtl, false, a, b, 2); }
static void w32(wchip_rtl8821cu_t* rtl, uint16_t a, uint32_t v){ uint8_t b[4]; put_le32(b, v); rtl_xfer(rtl, false, a, b, 4); }
static void w8_set(wchip_rtl8821cu_t* rtl, uint16_t a, uint8_t m){ w8(rtl, a, r8(rtl, a) | m); }
static void w8_clr(wchip_rtl8821cu_t* rtl, uint16_t a, uint8_t m){ w8(rtl, a, r8(rtl, a) & ~m); }
static void w16_set(wchip_rtl8821cu_t* rtl, uint16_t a, uint16_t m){ w16(rtl, a, r16(rtl, a) | m); }
static void w32_set(wchip_rtl8821cu_t* rtl, uint16_t a, uint32_t m){ w32(rtl, a, r32(rtl, a) | m); }

static int mask_shift(uint32_t m){ int s = 0; while(s < 32 && !(m & (1u << s))) s++; return s; }
/* odm_set_bb_reg / rtl8821c_write_bb_reg: read-modify-write unless the mask is the full dword. */
static void bb_w(wchip_rtl8821cu_t* rtl, uint16_t a, uint32_t mask, uint32_t v){
  if(mask != 0xffffffffu) v = (r32(rtl, a) & ~mask) | ((v << mask_shift(mask)) & mask);
  w32(rtl, a, v);
}
static uint32_t bb_r(wchip_rtl8821cu_t* rtl, uint16_t a, uint32_t mask){ return (r32(rtl, a) & mask) >> mask_shift(mask); }

static bool poll8(wchip_rtl8821cu_t* rtl, uint16_t a, uint8_t mask, uint8_t val, int timeout_ms){
  uint64_t end = sys_now() + timeout_ms;
  do{ if((r8(rtl, a) & mask) == val) return true; usleep(50); } while(sys_now() < end);
  return (r8(rtl, a) & mask) == val;
}

/* ---- RF register access (config_phydm_{read,write}_rf_reg_8821c) --------
 * Path A only. Read is a direct BB window at 0x2800 + addr*4; write goes
 * through the 3-wire LSSI port at 0xc90 as (addr << 20 | data[19:0]). */
#define RF_MASK 0xfffffu
static uint32_t rf_r(wchip_rtl8821cu_t* rtl, uint8_t addr, uint32_t mask){
  return bb_r(rtl, 0x2800 + ((uint16_t)addr << 2), mask & RF_MASK);
}
static void rf_w(wchip_rtl8821cu_t* rtl, uint8_t addr, uint32_t mask, uint32_t v){
  mask &= RF_MASK;
  if(mask != RF_MASK) v = (rf_r(rtl, addr, RF_MASK) & ~mask) | ((v << mask_shift(mask)) & mask);
  w32(rtl, 0xc90, (((uint32_t)addr << 20) | (v & RF_MASK)) & 0x0fffffff);
}

/* ======================================================================== */
/* power (halmac_pwr_seq_8821c.c, USB-applicable entries only)               */
/* ======================================================================== */
enum { PWR_W = 1, PWR_POLL = 2, PWR_DELAY_MS = 3, PWR_END = 4 };
typedef struct{ uint16_t off; uint8_t cmd, msk, val; } rtl_pwr_t;

static const rtl_pwr_t pwr_on_seq[] = {
  /* TRANS_CARDDIS_TO_CARDEMU */
  {0x004A, PWR_W, BIT(0), 0},
  {0x0005, PWR_W, BIT(3) | BIT(4) | BIT(7), 0},
  /* TRANS_CARDEMU_TO_ACT */
  {0x0020, PWR_W, BIT(0), BIT(0)},
  {0x0001, PWR_DELAY_MS, 0, 0},
  {0x0000, PWR_W, BIT(5), 0},
  {0x0005, PWR_W, BIT(4) | BIT(3) | BIT(2), 0},
  {0x0006, PWR_POLL, BIT(1), BIT(1)},
  {0x0006, PWR_W, BIT(0), BIT(0)},
  {0x0005, PWR_W, BIT(7), 0},
  {0x0005, PWR_W, BIT(4) | BIT(3), 0},
  {0x10C3, PWR_W, BIT(0), BIT(0)},
  {0x0005, PWR_W, BIT(0), BIT(0)},
  {0x0005, PWR_POLL, BIT(0), 0},
  {0x0020, PWR_W, BIT(3), BIT(3)},
  {0x007C, PWR_W, BIT(1), 0},
  {0, PWR_END, 0, 0},
};
static const rtl_pwr_t pwr_off_seq[] = {
  /* TRANS_ACT_TO_CARDEMU */
  {0x0093, PWR_W, 0xFF, 0xC4},
  {0x001F, PWR_W, 0xFF, 0},
  {0x0049, PWR_W, BIT(1), 0},
  {0x0006, PWR_W, BIT(0), BIT(0)},
  {0x0002, PWR_W, BIT(1), 0},
  {0x10C3, PWR_W, BIT(0), 0},
  {0x0005, PWR_W, BIT(1), BIT(1)},
  {0x0005, PWR_POLL, BIT(1), 0},
  {0x0020, PWR_W, BIT(3), 0},
  {0x0000, PWR_W, BIT(5), BIT(5)},
  /* TRANS_CARDEMU_TO_CARDDIS */
  {0x0007, PWR_W, 0xFF, 0x20},
  {0x0067, PWR_W, BIT(5), 0},
  {0x004A, PWR_W, BIT(0), 0},
  {0x0081, PWR_W, BIT(7) | BIT(6), 0},
  {0x0005, PWR_W, BIT(3) | BIT(4), BIT(3)},
  {0x0090, PWR_W, BIT(1), 0},
  {0, PWR_END, 0, 0},
};

static bool pwr_seq(wchip_rtl8821cu_t* rtl, const rtl_pwr_t* s){
  for(; s->cmd != PWR_END; s++){
    switch(s->cmd){
      case PWR_W: w8(rtl, s->off, (r8(rtl, s->off) & ~s->msk) | (s->val & s->msk)); break;
      case PWR_DELAY_MS: usleep(1000 * s->off); break;
      case PWR_POLL:
        if(!poll8(rtl, s->off, s->msk, s->val & s->msk, 1000)){
          LOG("rtl8821cu: pwr poll 0x%04x msk 0x%02x != 0x%02x (is 0x%02x)", s->off, s->msk, s->val, r8(rtl, s->off));
          return false;
        }
        break;
    }
  }
  return true;
}

/* MAC is off when CR reads 0xEA (unclocked) or SYS_STATUS1+1[0] is set */
static bool rtl_is_powered(wchip_rtl8821cu_t* rtl){
  return !(r8(rtl, 0x0100 /*CR*/) == 0xEA || (r8(rtl, 0x00F5 /*SYS_STATUS1+1*/) & BIT(0)));
}

/* mac_pwr_switch_usb_8821c */
static bool rtl_mac_pwr(wchip_rtl8821cu_t* rtl, bool on){
  uint8_t rpwm = r8(rtl, 0xFE58);
  if(r16(rtl, 0x0080 /*MCUFW_CTRL*/) == 0xC078) w8(rtl, 0xFE58, (rpwm ^ BIT(7)) & 0x80);  // FW alive: leave 32K
  if(on && rtl_is_powered(rtl)) return true;    // caller power-cycles a warm chip first
  if(!on) return pwr_seq(rtl, pwr_off_seq);
  if(!pwr_seq(rtl, pwr_on_seq)) return false;
  w8_clr(rtl, 0x00F5, BIT(0));
  return true;
}

/* pre_init_system_cfg_8821c + rtw_halmac_poweron + init_system_cfg_8821c */
static bool rtl_power_on(wchip_rtl8821cu_t* rtl){
  w8(rtl, 0x001C /*RSV_CTRL*/, 0);
  if(r8(rtl, 0x00FF /*SYS_CFG2+3*/) == 0x20) w8(rtl, 0xFE5B, r8(rtl, 0xFE5B) | BIT(4));   // USB3
  bb_w(rtl, 0x0064 /*PAD_CTRL1*/, BIT(28) | BIT(29), 0x3);                                  // pin mux
  bb_w(rtl, 0x004C /*LED_CFG*/, BIT(25) | BIT(26), 0x0);
  bb_w(rtl, 0x0040 /*GPIO_MUXCFG*/, BIT(2), 0x1);
  // enable_bb_rf_88xx(0)
  w8_clr(rtl, 0x0002 /*SYS_FUNC_EN*/, BIT(0) | BIT(1));
  w8_clr(rtl, 0x001F /*RF_CTRL*/, BIT(0) | BIT(1) | BIT(2));
  w32(rtl, 0x00EC /*WLRF1*/, r32(rtl, 0x00EC) & ~(BIT(24) | BIT(25) | BIT(26)));
  if(r8(rtl, 0x00F2 /*SYS_CFG1+2*/) & BIT(4)) LOG("rtl8821cu: chip reports test mode");

  if(rtl_is_powered(rtl)){
    // warm restart without a power cycle (previous run): halmac powers off then on
    LOG("rtl8821cu: MAC already on, power-cycling");
    rtl_mac_pwr(rtl, false);
  }
  if(!rtl_mac_pwr(rtl, true)) return false;

  // init_system_cfg_8821c
  w32_set(rtl, 0x1080 /*CPU_DMEM_CON*/, BIT(16) /*WL_PLATFORM_RST*/);
  w8_set(rtl, 0x0003 /*SYS_FUNC_EN+1*/, 0xD8);
  uint32_t fw = r32(rtl, 0x0080);
  if(fw & BIT(20) /*BOOT_FSPI_EN*/){
    w32(rtl, 0x0080, fw & ~BIT(20));
    w32(rtl, 0x0040, r32(rtl, 0x0040) & ~BIT(19) /*FSPI_EN*/);
  }
  return true;
}

/* ======================================================================== */
/* efuse (halmac_efuse_88xx.c read_hw_efuse + eeprom_parser)                 */
/* ======================================================================== */
#define EFUSE_PHY_SZ    512
#define EFUSE_PRTCT_SZ  96

static bool rtl_efuse_read(wchip_rtl8821cu_t* rtl){
  static uint8_t phy[EFUSE_PHY_SZ];
  // switch to the WiFi efuse bank, disable 2.5V LDO (read only)
  uint8_t bank = r8(rtl, 0x0035);
  if((bank & 0x3) != 0) w8(rtl, 0x0035, bank & ~0x3);
  w8_clr(rtl, 0x0037 /*LDO_EFUSE_CTRL+3*/, BIT(7));
  uint32_t ctl = r32(rtl, 0x0030 /*EFUSE_CTRL*/);
  for(uint32_t a = 0; a < EFUSE_PHY_SZ; a++){
    ctl &= ~(0xffu | (0x3ffu << 8));
    ctl |= (a & 0x3ff) << 8;
    w32(rtl, 0x0030, ctl & ~BIT(31));
    uint64_t end = sys_now() + 100;
    do{ ctl = r32(rtl, 0x0030); } while(!(ctl & BIT(31)) && sys_now() < end);
    if(!(ctl & BIT(31))){ LOG("rtl8821cu: efuse read timeout @%u", a); return false; }
    phy[a] = ctl & 0xff;
  }
  // logical map: [hdr][2-byte words per enabled bit] blocks (eeprom_parser_88xx)
  uint8_t* log = rtl->efuse;
  memset(log, 0xff, RTL_EFUSE_SZ);
  uint32_t i = 0, lim = EFUSE_PHY_SZ - EFUSE_PRTCT_SZ;
  while(i < lim){
    uint8_t hdr = phy[i], blk, wen;
    if(hdr == 0xff) break;
    if((hdr & 0x1f) == 0x0f){
      uint8_t hdr2 = phy[++i];
      if(hdr2 == 0xff) break;
      blk = ((hdr2 & 0xf0) >> 1) | ((hdr >> 5) & 0x07);
      wen = hdr2 & 0x0f;
    } else {
      blk = (hdr & 0xf0) >> 4;
      wen = hdr & 0x0f;
    }
    i++;
    for(int w = 0; w < 4; w++){
      if(wen & (1 << w)) continue;              // 0 = word present
      uint32_t e = ((uint32_t)blk << 3) + (w << 1);
      if(e + 1 >= RTL_EFUSE_SZ || i + 1 >= lim){ LOG("rtl8821cu: efuse parse overflow"); return false; }
      log[e] = phy[i++]; log[e + 1] = phy[i++];
    }
  }
  LOG("rtl8821cu: efuse %u/%u bytes used, id 0x%04x", i, lim, le16(log));
  return true;
}

/* rtl8821c_read_efuse: the fields this driver consumes */
static void rtl_efuse_parse(wchip_rtl8821cu_t* rtl){
  const uint8_t* m = rtl->efuse;
  bool valid = le16(m) == 0x8129;
  memcpy(rtl->mac_addr, m + 0x107, 6);
  rtl->xtal_cap  = (valid && m[0xB9] != 0xff) ? m[0xB9] : 0x20;
  rtl->thermal   = (valid && m[0xBA] != 0xff) ? m[0xBA] : 0x18;
  rtl->board_opt = m[0xC1];
  rtl->bt_setting = m[0xC3];
  rtl->rfe_expand = (valid && m[0xCA] != 0xff) ? m[0xCA] : 0;
  /* phydm_init_hw_info_by_rfe_type_8821c: tables key on rfe>>3; 0x2x expands set pkg 1 */
  rtl->rfe_type = rtl->rfe_expand >> 3;
  rtl->pkg_type = (rtl->rfe_expand >= 0x20 && rtl->rfe_expand <= 0x2f) ? 1 : 0;
  uint8_t e = rtl->rfe_expand;
  rtl->btg = (e == 2 || e == 4 || e == 7 || e == 0x22 || e == 0x24 || e == 0x27 || e == 0x2a || e == 0x2c || e == 0x2f);
  LOG("rtl8821cu: efuse %s mac %02x:%02x:%02x:%02x:%02x:%02x rfe 0x%02x (tbl %u pkg %u, 2G %s) xtal 0x%02x thermal 0x%02x board 0x%02x bt 0x%02x",
      valid ? "ok" : "INVALID", rtl->mac_addr[0], rtl->mac_addr[1], rtl->mac_addr[2], rtl->mac_addr[3], rtl->mac_addr[4], rtl->mac_addr[5],
      rtl->rfe_expand, rtl->rfe_type, rtl->pkg_type, rtl->btg ? "BTG" : "WLG", rtl->xtal_cap, rtl->thermal, rtl->board_opt, rtl->bt_setting);
}

/* ======================================================================== */
/* TX descriptor (halmac_tx_desc_nic.h, 48 bytes, 8821C field map)           */
/* ======================================================================== */
#define TXDESC_SZ 48
#define QSEL_BE   0x00
#define QSEL_VO   0x06
#define QSEL_BCN  0x10
#define QSEL_MGT  0x12
#define QSEL_H2C  0x13

static inline void desc_set(uint8_t* d, int dw, int sh, int len, uint32_t v){
  uint32_t m = (len == 32 ? 0xffffffffu : ((1u << len) - 1)) << sh;
  uint32_t x = le32(d + dw * 4);
  put_le32(d + dw * 4, (x & ~m) | ((v << sh) & m));
}
/* fill_txdesc_check_sum_8821c: XOR of the first 32 bytes as 16-bit words */
static void desc_chksum(uint8_t* d){
  desc_set(d, 7, 0, 16, 0);
  uint16_t c = 0;
  for(int i = 0; i < 16; i++) c ^= le16(d + 2 * i);
  desc_set(d, 7, 0, 16, c);
}

/* bulk-OUT endpoint for a queue (get_usb_bulkout_id_88xx; id 0=HQ 1=NQ 2=LQ). RQPN NORMAL:
 * 3 bulk-out: VO/VI->NQ, BE/BK->LQ, MG/HI/BCN/H2C->HQ; 2 bulk-out: VO/VI/BE->NQ, BK/MG/HI->HQ */
static uint8_t qsel_ep(wchip_rtl8821cu_t* rtl, uint8_t qsel){
  bool vovi = qsel >= 4 && qsel <= 7, be = qsel == 0 || qsel == 3, bk = qsel == 1 || qsel == 2;
  int id = 0;
  if(rtl->n_out >= 3) id = vovi ? 1 : (be || bk) ? 2 : 0;
  else                id = (vovi || be) ? 1 : 0;
  return rtl->ep_out[id];
}

/* ======================================================================== */
/* firmware download (download_firmware_88xx)                                 */
/* ======================================================================== */
#define OCPBASE_TXBUF  0x18780000u
#define OCPBASE_DMEM   0x00200000u
#define DLFW_PKT_SZ    0x1000        // rtw_halmac_init_adapter: USB max dl size
#define FWHDR_SZ       64
#define FWHDR_CHKSUM   8

/* usb_write_data_not_xmitframe(QSEL_BEACON) + dl_rsvd_page_88xx */
static bool rtl_send_rsvd_page(wchip_rtl8821cu_t* rtl, uint16_t pg, const uint8_t* data, uint32_t len){
  static uint8_t buf[TXDESC_SZ + 8 + DLFW_PKT_SZ + 8];
  uint32_t total = TXDESC_SZ + len, off = TXDESC_SZ;
  bool pad = (total % 512) == 0;                 // avoid a ZLP: shift by an 8-byte packet offset
  if(pad){ total += 8; off += 8; }
  if(total > sizeof buf) return false;
  memset(buf, 0, off);
  desc_set(buf, 0, 0, 16, len);                  // TXPKTSIZE
  desc_set(buf, 0, 16, 8, off);                  // OFFSET
  if(pad) desc_set(buf, 1, 24, 5, 1);            // PKT_OFFSET
  desc_set(buf, 1, 8, 5, QSEL_BCN);
  desc_chksum(buf);
  memcpy(buf + off, data, len);

  w16(rtl, 0x0204 /*FIFOPAGE_CTRL_2*/, (pg & 0xfff) | BIT(15));
  uint8_t cr1 = r8(rtl, 0x0101), txq2 = r8(rtl, 0x0422);
  w8(rtl, 0x0101, cr1 | BIT(0));                 // CR+1: enable SW beacon
  w8(rtl, 0x0422, txq2 & ~BIT(6));               // FWHW_TXQ_CTRL+2: stop HW beacon
  int rc = usb_device_bulk_transfer(&rtl->usb_device, qsel_ep(rtl, QSEL_BCN), buf, total, 1000);
  bool ok = rc == (int)total;
  if(ok){
    uint64_t end = sys_now() + 100;
    while(!(r8(rtl, 0x0205) & BIT(7)) && sys_now() < end);   // BCN_VALID
    ok = (r8(rtl, 0x0205) & BIT(7)) != 0;
    if(!ok) LOG("rtl8821cu: rsvd page: bcn valid timeout");
  } else LOG("rtl8821cu: rsvd page bulk-out %d/%u", rc, total);
  w16(rtl, 0x0204, rtl->rsvd_bndy | BIT(15));   // back to the reserved-page boundary (0 before TRX init)
  w8(rtl, 0x0422, txq2);
  w8(rtl, 0x0101, cr1);
  return ok;
}

static bool rtl_iddma(wchip_rtl8821cu_t* rtl, uint32_t src, uint32_t dst, uint32_t len, bool first){
  uint64_t end = sys_now() + 100;
  while((r32(rtl, 0x1208) & BIT(31)) && sys_now() < end);
  if(r32(rtl, 0x1208) & BIT(31)){ LOG("rtl8821cu: ddma ch0 busy"); return false; }
  uint32_t ctrl = BIT(29) /*CHKSUM_EN*/ | BIT(31) /*OWN*/ | (len & 0x3ffff);
  if(!first) ctrl |= BIT(24);                    // CHKSUM_CONT
  w32(rtl, 0x1200 /*DDMA_CH0SA*/, src);
  w32(rtl, 0x1204 /*DDMA_CH0DA*/, dst);
  w32(rtl, 0x1208 /*DDMA_CH0CTRL*/, ctrl);
  end = sys_now() + 100;
  while((r32(rtl, 0x1208) & BIT(31)) && sys_now() < end);
  return !(r32(rtl, 0x1208) & BIT(31));
}

static bool rtl_dlfw_mem(wchip_rtl8821cu_t* rtl, const uint8_t* fw, uint32_t dst, uint32_t size){
  w32_set(rtl, 0x1208, BIT(25));                 // DDMACH0_RESET_CHKSUM_STS
  for(uint32_t off = 0, first = 1; off < size; first = 0){
    uint32_t n = min(size - off, (uint32_t)DLFW_PKT_SZ);
    if(!rtl_send_rsvd_page(rtl, 0, fw + off, n)) return false;
    if(!rtl_iddma(rtl, OCPBASE_TXBUF + TXDESC_SZ, dst + off, n, first)){ LOG("rtl8821cu: iddma failed @0x%x", dst + off); return false; }
    off += n;
  }
  // check_fw_chksum_88xx
  uint8_t fwc = r8(rtl, 0x0080);
  bool bad = (r32(rtl, 0x1208) & BIT(27)) != 0;  // DDMACH0_CHKSUM_STS
  if(dst < OCPBASE_DMEM) fwc = bad ? ((fwc | BIT(3)) & ~BIT(4)) : (fwc | BIT(3) | BIT(4));
  else                   fwc = bad ? ((fwc | BIT(5)) & ~BIT(6)) : (fwc | BIT(5) | BIT(6));
  w8(rtl, 0x0080, fwc);
  if(bad) LOG("rtl8821cu: fw checksum error @0x%x", dst);
  return !bad;
}

static void wlan_cpu_en(wchip_rtl8821cu_t* rtl, bool en){
  if(en){ w8_set(rtl, 0x001D /*RSV_CTRL+1*/, BIT(0)); w8_set(rtl, 0x0003, BIT(2)); }
  else  { w8_clr(rtl, 0x0003, BIT(2)); w8_clr(rtl, 0x001D, BIT(0)); }
}

static uint32_t ltecoex_r(wchip_rtl8821cu_t* rtl, uint16_t off){
  poll8(rtl, 0x1703, BIT(5), BIT(5), 50);
  w32(rtl, 0x1700, 0x800F0000 | off);
  return r32(rtl, 0x1708);
}
static void ltecoex_w(wchip_rtl8821cu_t* rtl, uint16_t off, uint32_t v){
  poll8(rtl, 0x1703, BIT(5), BIT(5), 50);
  w32(rtl, 0x1704, v);
  w32(rtl, 0x1700, 0xC00F0000 | off);
}

static bool rtl_download_fw(wchip_rtl8821cu_t* rtl){
  const uint8_t* fw = fw_rtl8821c_data;
  uint32_t dmem_sz = le32(fw + 36) + FWHDR_CHKSUM, imem_sz = le32(fw + 48) + FWHDR_CHKSUM;
  uint32_t emem_sz = (fw[24] & BIT(4)) ? le32(fw + 52) + FWHDR_CHKSUM : 0;
  if(fw_rtl8821c_size != FWHDR_SZ + dmem_sz + imem_sz + emem_sz){ LOG("rtl8821cu: bad firmware image size"); return false; }
  rtl->fw_ver = le16(fw + 4); rtl->fw_sub = fw[6];
  LOG("rtl8821cu: fw v%u.%u.%u (%04u-%02u-%02u) dmem %u imem %u emem %u", rtl->fw_ver, fw[6], fw[7],
      le16(fw + 20), fw[16], fw[17], dmem_sz, imem_sz, emem_sz);

  uint32_t lte = ltecoex_r(rtl, 0x38);
  wlan_cpu_en(rtl, false);
  // backups (restore_mac_reg_88xx order)
  uint8_t  b_pq = r8(rtl, 0x010D), b_cr = r8(rtl, 0x0100), b_bcn = r8(rtl, 0x0550);
  uint16_t b_fp1 = r16(rtl, 0x0230);
  uint32_t b_rqpn = r32(rtl, 0x022C) | BIT(31);
  w8(rtl, 0x010D, 3 << 6);                       // HIQ -> high priority (TXDMA_PQ_MAP+1)
  w8(rtl, 0x0100, BIT(0) | BIT(2));              // HCI_TXDMA_EN | TXDMA_EN
  w32(rtl, 0x1330 /*H2CQ_CSR*/, BIT(31));
  w16(rtl, 0x0230, 0x200);                       // HQ pages
  w32(rtl, 0x022C, b_rqpn);
  w8(rtl, 0x0550, (b_bcn & ~BIT(3)) | BIT(4));   // disable beacon functions
  // pltfm_reset_88xx (incl. 8821C clock sync fix)
  w8(rtl, 0x1082, r8(rtl, 0x1082) & ~BIT(0));
  w8(rtl, 0x0009, r8(rtl, 0x0009) & ~BIT(6));
  w8(rtl, 0x1082, r8(rtl, 0x1082) | BIT(0));
  w8(rtl, 0x0009, r8(rtl, 0x0009) | BIT(6));

  // start_dlfw_88xx: DMEM, IMEM, then EMEM
  w16(rtl, 0x0080, (r16(rtl, 0x0080) & 0x3800) | BIT(0));
  const uint8_t* p = fw + FWHDR_SZ;
  bool ok = rtl_dlfw_mem(rtl, p, le32(fw + 32) & ~BIT(31), dmem_sz);
  p += dmem_sz;
  if(ok) ok = rtl_dlfw_mem(rtl, p, le32(fw + 60) & ~BIT(31), imem_sz);
  p += imem_sz;
  if(ok && emem_sz) ok = rtl_dlfw_mem(rtl, p, le32(fw + 56) & ~BIT(31), emem_sz);

  w8(rtl, 0x010D, b_pq); w8(rtl, 0x0100, b_cr); w32(rtl, 0x1330, BIT(31));
  w16(rtl, 0x0230, b_fp1); w32(rtl, 0x022C, b_rqpn); w8(rtl, 0x0550, b_bcn);
  if(!ok) goto fail;

  // dlfw_end_flow_88xx
  w32(rtl, 0x0210 /*TXDMA_STATUS*/, BIT(2));
  uint16_t fwc = r16(rtl, 0x0080);
  if((fwc & 0x50) != 0x50){ LOG("rtl8821cu: IMEM/DMEM checksum not ok (0x%04x)", fwc); goto fail; }
  w16(rtl, 0x0080, (fwc | BIT(14) /*FW_DW_RDY*/) & ~BIT(0));
  wlan_cpu_en(rtl, true);
  uint64_t end = sys_now() + 1000;
  while(r16(rtl, 0x0080) != 0xC078 && sys_now() < end) usleep(500);
  if(r16(rtl, 0x0080) != 0xC078){
    LOG("rtl8821cu: fw not ready: MCUFW_CTRL=0x%04x FW_DBG7=0x%08x", r16(rtl, 0x0080), r32(rtl, 0x10FC));
    goto fail;
  }
  ltecoex_w(rtl, 0x38, lte);
  LOG("rtl8821cu: firmware running (MCUFW_CTRL=0xC078)");
  return true;
fail:
  w8(rtl, 0x0080, r8(rtl, 0x0080) & ~BIT(0));
  w8_set(rtl, 0x0003, BIT(2));
  ltecoex_w(rtl, 0x38, lte);
  return false;
}

/* ======================================================================== */
/* H2C: firmware commands                                                    */
/* ======================================================================== */
#define RSVD_FW_TXBUF_PG  (512 - 0 - 4)    // tx fifo pages - csi buf - fw txbuf (rsvd_fw_txbuf_addr)
#define H2C_PKT_SZ        32
#define H2C_PKT_HDR_SZ    8
#define H2C_SUB_GENERAL_INFO 0x0D
#define H2C_SUB_PHYDM_INFO   0x11
#define H2C_SUB_IQK          0x0E

/* FW-offload H2C packet (send_h2c_pkt_88xx / usb_write_data_not_xmitframe QSEL_H2C):
 * 48B TX desc (TXPKTSIZE + QSEL only) + [cat 1][cmd 0xFF][sub id(2)][total len(2)][seq(2)] + content */
static bool rtl_h2c_pkt(wchip_rtl8821cu_t* rtl, uint8_t sub_id, const uint8_t* content, uint8_t len){
  uint8_t buf[TXDESC_SZ + H2C_PKT_SZ];
  memset(buf, 0, sizeof buf);
  desc_set(buf, 0, 0, 16, H2C_PKT_SZ);
  desc_set(buf, 1, 8, 5, QSEL_H2C);
  desc_chksum(buf);
  if(len > H2C_PKT_SZ - H2C_PKT_HDR_SZ) return false;
  uint8_t* h = buf + TXDESC_SZ;
  h[0] = 0x01; h[1] = 0xFF; h[2] = sub_id; h[3] = 0;
  h[4] = H2C_PKT_HDR_SZ + len; h[5] = 0;
  h[6] = rtl->h2c_seq; h[7] = rtl->h2c_seq >> 8; rtl->h2c_seq++;
  memcpy(h + H2C_PKT_HDR_SZ, content, len);
  int rc = usb_device_bulk_transfer(&rtl->usb_device, qsel_ep(rtl, QSEL_H2C), buf, sizeof buf, 500);
  if(rc != (int)sizeof buf){ LOG("rtl8821cu: h2c pkt 0x%02x failed (%d)", sub_id, rc); return false; }
  return true;
}

/* Classic 8-byte H2C through the HMEBOX mailboxes (rtw_halmac_send_h2c). */
static bool rtl_h2c_reg(wchip_rtl8821cu_t* rtl, const uint8_t h2c[8]){
  uint8_t box = rtl->h2c_box;
  uint64_t end = sys_now() + 100;
  while((r8(rtl, 0x01CC /*HMETFR*/) & BIT(box)) && sys_now() < end) usleep(1000);
  if(r8(rtl, 0x01CC) & BIT(box)){ LOG("rtl8821cu: h2c mailbox %u busy", box); return false; }
  w32(rtl, 0x01F0 + box * 4, le32(h2c + 4));    // HMEBOX_E<box>: bytes 4..7 first
  w32(rtl, 0x01D0 + box * 4, le32(h2c));        // HMEBOX<box>: bytes 0..3 (triggers)
  rtl->h2c_box = (box + 1) % 4;
  return true;
}

/* _send_general_info (halmac general info + phydm info packets, then the register copy) */
static void rtl_send_general_info(wchip_rtl8821cu_t* rtl){
  uint8_t gi[4] = {0};
  gi[2] = (uint8_t)(RSVD_FW_TXBUF_PG - rtl->rsvd_bndy);           // FW_TX_BOUNDARY [23:16]
  rtl_h2c_pkt(rtl, H2C_SUB_GENERAL_INFO, gi, sizeof gi);
  uint8_t pi[8] = {0};
  pi[0] = rtl->rfe_expand;                      // REF(RFE)_TYPE
  pi[1] = 4;                                    // RF_TYPE: HALMAC_RF_1T1R
  pi[2] = rtl->cut;                             // CUT_VER (chip_ver)
  pi[3] = 0x1 | (0x1 << 4);                     // RX/TX ant status = BB_PATH_A
  pi[5] = 0;                                    // PACKAGE_TYPE: vendor leaves hal->PackageType 0 on 8821C
  rtl_h2c_pkt(rtl, H2C_SUB_PHYDM_INFO, pi, sizeof pi);
  uint8_t r[8] = {0};
  r[0] = 0x0C | (0x02 << 5);                    // CMD_ID_GENERAL_INFO_REG, CLASS_GENERAL_INFO_REG
  r[1] = rtl->rfe_expand; r[2] = 0 /*RF_1T1R*/; r[3] = rtl->cut;
  r[4] = 0x1 | (0x1 << 4);
  rtl_h2c_reg(rtl, r);
  LOG("rtl8821cu: sent general/phydm info to fw (rfe 0x%02x cut %u)", rtl->rfe_expand, rtl->cut);
}

/* FW-offloaded IQK (rtw8821c_do_iqk): H2C IQK, firmware writes RF 0x08 = 0xabcde when done. */
static bool rtl_iqk(wchip_rtl8821cu_t* rtl){
  uint64_t t0 = sys_now();
  uint8_t c[1] = { 0x1 /*clear*/ };
  if(!rtl_h2c_pkt(rtl, H2C_SUB_IQK, c, 1)) return false;
  bool done = false;
  for(int i = 0; i < 300 && !done; i++){ usleep(20000); done = rf_r(rtl, 0x08, RF_MASK) == 0xabcde; }
  rf_w(rtl, 0x08, RF_MASK, 0x0);
  uint32_t fail = r32(rtl, 0x1bf0);
  LOG("rtl8821cu: IQK ch%u %s in %llums (reload %u, fail mask 0x%02x)", rtl->channel, done ? "done" : "TIMEOUT",
      (unsigned long long)(sys_now() - t0), (fail >> 16) & 1, fail & 0xff);
  return done;
}

/* ======================================================================== */
/* MAC init (init_mac_cfg_88xx: trx, protocol, edca, wmac) + USB + misc      */
/* ======================================================================== */
#define TX_FIFO_PG       512           // 64 KB / 128-byte pages
#define RSVD_DRV_PG      16            // rtw_hal_get_rsvd_page_num -> HALMAC_RSVD_PG_NUM16 (assumed)
#define RSVD_PG_TOTAL    (RSVD_DRV_PG + 24 + 8 + 8 + 0 + 4 + 0)  // drv, h2c extra/static, h2cq, cpu, fw txbuf, csi
#define RSVD_BOUNDARY    (TX_FIFO_PG - RSVD_PG_TOTAL)
#define H2CQ_ADDR_PG     (TX_FIFO_PG - 0 - 4 - 0 - 8)     // after csi, fw txbuf, cpu instr
#define RX_FIFO_SZ       16384
#define C2H_PKT_BUF      256

static bool rtl_init_trx(wchip_rtl8821cu_t* rtl){
  // txdma_queue_mapping_8821c: 3 bulk-out, NORMAL mode: VO/VI->NQ(2) BE/BK->LQ(1) MG/HI->HQ(3)
  enum { MAP_LQ = 1, MAP_NQ = 2, MAP_HQ = 3 };
  uint8_t vo = MAP_NQ, vi = MAP_NQ, be = MAP_LQ, bk = MAP_LQ, mg = MAP_HQ, hi = MAP_HQ;
  if(rtl->n_out == 2){ vo = vi = be = MAP_NQ; bk = mg = hi = MAP_HQ; }
  uint16_t pq = (hi << 14) | (mg << 12) | (bk << 10) | (be << 8) | (vi << 6) | (vo << 4);
  w16(rtl, 0x010C, (r16(rtl, 0x010C) & 0x000f) | pq);

  bool fwff = (r8(rtl, 0x0601) & BIT(7)) != 0;    // WMAC_FWPKT_CR.FWEN
  if(fwff) w8_clr(rtl, 0x0601, BIT(7));
  w8(rtl, 0x0100, 0);
  w16(rtl, 0x029C /*FWFF_CTRL*/, r16(rtl, 0x02A0 /*FWFF_PKT_INFO*/));
  w8(rtl, 0x0100, 0xff);                          // MAC_TRX_ENABLE (all 8 DMA/protocol/MAC enables)
  if(fwff) w8_set(rtl, 0x0601, BIT(7));
  w32(rtl, 0x1330, BIT(31));

  // priority_queue_cfg_8821c + pg_num_parser_88xx: 3 bulk-out NORMAL = hq 16 nq 16 lq 16 exq 0 gap 1
  uint16_t acq = RSVD_BOUNDARY;
  uint16_t hq = 16, nq = 16, lq = rtl->n_out == 2 ? 0 : 16, eq = 0, gap = 1;
  uint16_t pub = acq - hq - lq - nq - eq - gap;
  w16(rtl, 0x0230, hq); w16(rtl, 0x0234, lq); w16(rtl, 0x0238, nq); w16(rtl, 0x023C, eq); w16(rtl, 0x0240, pub);
  w32_set(rtl, 0x022C, BIT(31));
  rtl->rsvd_bndy = RSVD_BOUNDARY;
  w16(rtl, 0x0204, RSVD_BOUNDARY);
  w8_set(rtl, 0x0422, BIT(4));
  w16(rtl, 0x0424 /*BCNQ_BDNY_V1*/, RSVD_BOUNDARY);
  w16(rtl, 0x0206 /*FIFOPAGE_CTRL_2+2*/, RSVD_BOUNDARY);
  w16(rtl, 0x0456 /*BCNQ1_BDNY_V1*/, RSVD_BOUNDARY);
  w32(rtl, 0x011C /*RXFF_BNDY*/, RX_FIFO_SZ - C2H_PKT_BUF - 1);
  uint8_t llt = r8(rtl, 0x0208);
  w8(rtl, 0x0208, (llt & ~(0xf << 4)) | (3 << 4));   // BLK_DESC_NUM
  w8(rtl, 0x020B, 3);
  w8_set(rtl, 0x020D, BIT(1));
  w8_set(rtl, 0x0208, BIT(0));                        // AUTO_INIT_LLT
  if(!poll8(rtl, 0x0208, BIT(0), 0, 200)){ LOG("rtl8821cu: LLT init timeout"); return false; }
  w8(rtl, 0x0103 /*CR+3*/, 0);                        // TRNSFER_NORMAL
  LOG("rtl8821cu: tx fifo: rsvd boundary %u, pub %u, hq/nq/lq %u/%u/%u", RSVD_BOUNDARY, pub, hq, nq, lq);

  // init_h2c_8821c
  uint32_t h2cq = (uint32_t)H2CQ_ADDR_PG << 7, h2cq_sz = 8 << 7;
  w32(rtl, 0x0244, (r32(rtl, 0x0244) & 0xFFFC0000) | h2cq);
  w32(rtl, 0x024C, (r32(rtl, 0x024C) & 0xFFFC0000) | h2cq);
  w32(rtl, 0x0248, (r32(rtl, 0x0248) & 0xFFFC0000) | (h2cq + h2cq_sz));
  w8(rtl, 0x0254, (r8(rtl, 0x0254) & 0xFC) | 0x01);
  w8(rtl, 0x0254, (r8(rtl, 0x0254) & 0xFB) | 0x04);
  w8(rtl, 0x020D, (r8(rtl, 0x020D) & 0x7f) | 0x80);
  return true;
}

static void rtl_init_protocol(wchip_rtl8821cu_t* rtl){       // init_protocol_cfg_8821c
  w8(rtl, 0x0455, 0x70);                                     // AMPDU_MAX_TIME
  w8_set(rtl, 0x045E, BIT(2));                               // TX_HANG_CTRL.EN_EOF
  uint16_t pre = 0x1E4 | BIT(11);
  w8(rtl, 0x04E5, pre & 0xff); w8(rtl, 0x04E6, pre >> 8);
  w32(rtl, 0x04C8, 0xFF | (0x08 << 8) | (0x10 << 16) | (0x10 << 24));   // PROT_MODE_CTRL
  w16(rtl, 0x04CE, 0x01 | (0x08 << 8));                      // BAR retry / RA try rate agg
  w8(rtl, 0x1448, 0x06); w8(rtl, 0x144A, 0x06); w8(rtl, 0x144C, 0x06); w8(rtl, 0x144E, 0x06);
  w8_set(rtl, 0x0480, BIT(5));
}

static void rtl_init_edca(wchip_rtl8821cu_t* rtl){           // init_edca_cfg_8821c
  w8_clr(rtl, 0x05B4, BIT(4) | BIT(5) | BIT(6));
  w16(rtl, 0x0522, 0x0000);                                  // TXPAUSE clear
  w8(rtl, 0x051B, 0x09);                                     // slot
  w8(rtl, 0x0512, 0x19);                                     // PIFS
  w32(rtl, 0x0514, 0x0A | (0x0E << 8) | (0x10 << 16) | (0x10 << 24));   // SIFS
  w16(rtl, 0x0502, 0x186);                                   // VO TXOP
  w16(rtl, 0x0506, 0x3BC);                                   // VI TXOP
  w32(rtl, 0x0544, 0x05 | (0x1B << 16));                     // RD_NAV_NXT
  w16(rtl, 0x055E, 0x30 | (0x30 << 8));                      // RXTSF_OFFSET
  w8(rtl, 0x0550, r8(rtl, 0x0550) | BIT(3));                 // BCN_CTRL.EN_BCN_FUNCTION
  w32(rtl, 0x0540, 0x04 | (0x064 << 8));                     // TBTT_PROHIBIT
  w8(rtl, 0x0558, 0x04);                                     // DRVERLYINT
  w8(rtl, 0x0559, 0x02);                                     // BCNDMATIM
  w8_clr(rtl, 0x0521, BIT(4));                               // TX_PTCL_CTRL+1
}

static void rtl_init_wmac(wchip_rtl8821cu_t* rtl){           // init_wmac_cfg_8821c
  w32(rtl, 0x06A0, 0x0FFFFFFF);                              // RXFLTMAP0 (+1)
  w16(rtl, 0x06A4, 0xFFFF);                                  // RXFLTMAP2
  w32(rtl, 0x0608, 0xE400220E);                              // RCR
  w8(rtl, 0x060C, 12288 >> 9);                               // RX_PKT_LIMIT
  w8(rtl, 0x0606, 0x30);                                     // TCR+2
  w8(rtl, 0x0605, 0x30);                                     // TCR+1
  w8(rtl, 0x0639, 0x40);                                     // ACKTO_CCK
  w8_set(rtl, 0x066C, BIT(1));
  w8_set(rtl, 0x0718, BIT(6));
  w32(rtl, 0x07D8, 0x30810041);
  w8(rtl, 0x07D4, 0x98);                                     // WMAC_OPTION_FUNCTION+4 (normal)
}

static void rtl_init_usb(wchip_rtl8821cu_t* rtl){
  // cfg_usb_rx_agg_88xx: USB aggregation, size 5, timeout 0x20 (USB2), size limit on
  uint8_t dma_agg = r8(rtl, 0x0283) & ~BIT(7);
  uint8_t pq = r8(rtl, 0x010C) | BIT(2);                     // RXDMA_AGG_EN
  w32(rtl, 0x0280, r32(rtl, 0x0280) | BIT(29));              // EN_PRE_CALC
  w8(rtl, 0x010C, pq);
  w8(rtl, 0x0283, dma_agg);
  w16(rtl, 0x0280, 0x5 | (0x20 << 8));
  // init_usb_cfg_88xx
  uint8_t mode = BIT(1) | (0x3 << 2);
  if(r8(rtl, 0x00FF) == 0x20) mode |= 0 << 4;                // USB3 burst
  else mode |= ((r8(rtl, 0xFE11) & 0x3) == 0x1 ? 1 : 2) << 4;  // HS : FS
  w8(rtl, 0x0290 /*RXDMA_MODE*/, mode);
  w16_set(rtl, 0x020C, BIT(9));                              // DROP_DATA_EN
}

static void rtl_cfg_rx_drvinfo(wchip_rtl8821cu_t* rtl){       // cfg_drv_info_8821c(PHY_STATUS)
  w8(rtl, 0x060F, 4);                                        // 4 x 8 bytes of phy status
  w8(rtl, 0x0115, (r8(rtl, 0x0115) & 0xF0) | 0xF);           // TRXFF_BNDY+1: rxdesc len=0 issue
  w32(rtl, 0x0608, r32(rtl, 0x0608) | BIT(28));              // RCR.APP_PHYSTS
  w32(rtl, 0x07D4, r32(rtl, 0x07D4) & ~(BIT(8) | BIT(9)));
}

static void rtl_hal_init_misc(wchip_rtl8821cu_t* rtl){       // rtl8821c_hal_init_misc
  w32(rtl, 0x0670, BIT(31) | BIT(30));                       // CAMCMD: clear all security CAM
  w16(rtl, 0x06A2, 0x0400);                                  // RXFLTMAP1: PS-poll only (ctrl)
  w16(rtl, 0x06A4, 0xFFFF);                                  // all data
  w16(rtl, 0x06A0, 0xFFFF);                                  // all mgmt
  uint32_t rcr = r32(rtl, 0x0608);
  rcr &= ~(BIT(9) | BIT(8) | BIT(31) | BIT(5));              // AICV ACRC32 APP_FCS APWRMGT
  rcr |= BIT(28);                                            // APP_PHYSTS
  rcr &= ~(BIT(6) | BIT(7));                                 // CBSSID_DATA / CBSSID_BCN off: scan sees all BSSes
  w32(rtl, 0x0608, rcr);
  w32_set(rtl, 0x0420, BIT(12));                             // set_mgnt_xmit_ack
  w32(rtl, 0x04CC, 0x01ffff | ((uint32_t)r8(rtl, 0x04CF) << 24));   // disable BAR
  w8(rtl, 0x0577, 0x03);                                     // MISC_CTRL
  w16(rtl, 0x0100, r16(rtl, 0x0100) | BIT(9));               // MAC_SEC_EN
  w8(rtl, 0x0691, BIT(0) | BIT(1));                          // RX TSF addr filter
  if(rtl->rfe_expand == 2) w8(rtl, 0x0067, 0x36);            // 1212 module 5G RX issue
  w16(rtl, 0x042A /*RETRY_LIMIT*/, (0x30 << 8) | 0x30);  // hw_var_set_mlme_join: SRL/LRL = RL_VAL_STA
}

/* program STA identity (cfg_mac_addr_88xx port 0) */
static void rtl_set_macaddr(wchip_rtl8821cu_t* rtl, const uint8_t* a){
  w32(rtl, 0x0610, a[0] | (a[1] << 8) | (a[2] << 16) | ((uint32_t)a[3] << 24));
  w16(rtl, 0x0614, a[4] | (a[5] << 8));
}

/* ======================================================================== */
/* PHY: tables, BB/RF init, RFE/antenna, channel switch, TX power            */
/* ======================================================================== */
/* check_positive (halhwimg8821c_*.c): cut [27:24], pkg [15:12], intf [11:8] are
 * value-checked when nonzero in the condition; RFE [7:0] must match exactly. */
static bool tbl_cond_ok(wchip_rtl8821cu_t* rtl, uint32_t c1){
  uint32_t cut = rtl->cut == 0 ? 15 : rtl->cut;
  uint32_t pkg = rtl->pkg_type == 0 ? 15 : rtl->pkg_type;
  uint32_t drv = (cut << 24) | (pkg << 12) | (0x2 /*ODM_ITRF_USB*/ << 8) | rtl->rfe_type;
  if((c1 & 0x0F000000) && (c1 & 0x0F000000) != (drv & 0x0F000000)) return false;
  if((c1 & 0x0000F000) && (c1 & 0x0000F000) != (drv & 0x0000F000)) return false;
  if((c1 & 0x00000F00) && (c1 & 0x00000F00) != (drv & 0x00000F00)) return false;
  return (c1 & 0xFF) == (drv & 0xFF);
}

enum { TBL_MAC, TBL_BB, TBL_AGC, TBL_RF };
static int rtl_load_table(wchip_rtl8821cu_t* rtl, const uint32_t* t, size_t n, int kind){
  bool matched = true, skipped = false;
  uint32_t pre_v1 = 0;
  int writes = 0;
  for(size_t i = 0; i + 1 < n; i += 2){
    uint32_t v1 = t[i], v2 = t[i + 1];
    if(v1 & (BIT(31) | BIT(30))){
      if(v1 & BIT(31)){
        uint8_t c = (v1 >> 28) & 0x3;
        if(c == 3 /*ENDIF*/){ matched = true; skipped = false; }
        else if(c == 2 /*ELSE*/){ matched = !skipped; }
        else { pre_v1 = v1; }                    // IF / ELSE IF: condition words follow
      } else {                                   // BIT30: second half of the condition
        if(!skipped){
          if(tbl_cond_ok(rtl, pre_v1)){ matched = true; skipped = true; }
          else { matched = false; skipped = false; }
        } else matched = false;
      }
      continue;
    }
    if(!matched) continue;
    switch(kind){
      case TBL_MAC: w8(rtl, v1, (uint8_t)v2); break;
      case TBL_BB:
        if(v1 == 0xfe) usleep(50000); else if(v1 == 0xfd) usleep(5000); else if(v1 == 0xfc) usleep(1000);
        else if(v1 == 0xfb) usleep(50); else if(v1 == 0xfa) usleep(5); else if(v1 == 0xf9) usleep(1);
        else w32(rtl, v1, v2);
        break;
      case TBL_AGC: w32(rtl, v1, v2); break;
      case TBL_RF:
        if(v1 == 0xffe) usleep(50000); else if(v1 == 0xfe) usleep(100);
        else rf_w(rtl, (uint8_t)v1, RF_MASK, v2);
        break;
    }
    writes++;
  }
  return writes;
}

/* phydm_init_hw_info_by_rfe_type_8821c */
static void rtl_rfe_init(wchip_rtl8821cu_t* rtl){
  uint8_t e = rtl->rfe_expand;
  if(rtl->pkg_type == 1 && e >= 0x28 && e <= 0x2f) w32(rtl, 0xcb4, 0x00000073);
  else if(e == 4) w32(rtl, 0xcb4, 0x20000077);
  else w32(rtl, 0xcb4, 0x10000077);
}

enum { RF_SET_BTG, RF_SET_WLG, RF_SET_WLA };
/* config_phydm_switch_rf_set_8821c */
static void rtl_switch_rf_set(wchip_rtl8821cu_t* rtl, int set){
  bb_w(rtl, 0x1080, BIT(16), 1);
  bb_w(rtl, 0x0000, BIT(26), 1);
  uint32_t v = r32(rtl, 0xcb8);
  if(set == RF_SET_BTG){
    v |= BIT(16); v &= ~(BIT(18) | BIT(20) | BIT(21) | BIT(22) | BIT(23));
    bb_w(rtl, 0xa84, 0x00ff0000, 0xe);
    bb_w(rtl, 0xa80, 0x0000ffff, 0xfc84);
  } else if(set == RF_SET_WLG){
    v |= BIT(20) | BIT(21) | BIT(22); v &= ~(BIT(16) | BIT(18) | BIT(23));
    bb_w(rtl, 0xa84, 0x00ff0000, 0x12);
    bb_w(rtl, 0xa80, 0x0000ffff, 0x7532);
  } else {
    v |= BIT(20) | BIT(22) | BIT(23); v &= ~(BIT(16) | BIT(18) | BIT(21));
  }
  w32(rtl, 0xcb8, v);
}

/* phydm_stop_ic_trx (11AC series): wait for BB idle, pause TX, gate CCA/CCK */
typedef struct{ uint8_t txpause; uint32_t cck_tx; uint32_t cck_blk; bool ok; } trx_stop_t;
static trx_stop_t rtl_stop_trx(wchip_rtl8821cu_t* rtl){
  trx_stop_t s = {0};
  bb_w(rtl, 0x198c, 0x7, 0x7);                   // debug port clock on
  w32(rtl, 0x8fc, 0x0);                          // debug port 0
  for(int i = 0; i < 100; i++){
    if((r32(rtl, 0xfa0) & (BIT(17) | BIT(3))) == 0){ s.ok = true; break; }   // !PHYTXON && !CCA
    usleep(1000);
  }
  bb_w(rtl, 0x198c, 0x7, 0); bb_w(rtl, 0x8f8, 0x3c00000, 0);
  if(!s.ok){ LOG("rtl8821cu: stop_trx: BB not idle"); return s; }
  s.txpause = r8(rtl, 0x0522);
  bb_w(rtl, 0x0520, 0xff0000, 0xff);             // pause all TX queues
  bb_w(rtl, 0x838, BIT(1), 1);                   // disable OFDM RX CCA
  s.cck_tx = bb_r(rtl, 0xa04, 0xf0000000);
  s.cck_blk = bb_r(rtl, 0x808, BIT(28));
  bb_w(rtl, 0x808, BIT(28), 0);
  bb_w(rtl, 0xa04, 0xf0000000, 0);
  return s;
}
static void rtl_resume_trx(wchip_rtl8821cu_t* rtl, trx_stop_t s){
  if(!s.ok) return;
  w8(rtl, 0x0522, s.txpause);
  bb_w(rtl, 0x838, BIT(1), 0);
  bb_w(rtl, 0x808, BIT(28), s.cck_blk);          // (vendor forces 1; keep the band's CCK state)
  bb_w(rtl, 0xa04, 0xf0000000, s.cck_tx);
}
static void rtl_write_rf18(wchip_rtl8821cu_t* rtl, uint32_t v){
  trx_stop_t s = rtl_stop_trx(rtl);
  rf_w(rtl, 0x18, RF_MASK, v);
  rtl_resume_trx(rtl, s);
}

/* phy_get_tx_bbswing_8821c (efuse 0xC6/0xC7 path A bits) -> 0xc1c[31:21] */
static void rtl_set_bb_swing(wchip_rtl8821cu_t* rtl, bool is5g){
  uint8_t sw = rtl->efuse[is5g ? 0xC7 : 0xC6];
  if(sw == 0xff) sw = 0;
  static const uint16_t tbl[4] = {0x200, 0x16A, 0x101, 0x0B6};   // 0 / -3 / -6 / -9 dB
  bb_w(rtl, 0xc1c, 0xFFE00000, tbl[sw & 0x3]);
}

/* config_phydm_switch_band_8821c */
static bool rtl_switch_band(wchip_rtl8821cu_t* rtl, uint8_t ch){
  uint32_t rf18 = rf_r(rtl, 0x18, RF_MASK);
  bool is5g = ch > 14;
  if(!is5g){
    bb_w(rtl, 0x808, BIT(28), 1);                // CCK block on
    bb_w(rtl, 0x454, BIT(7), 0);                 // MAC CCK check off
    bb_w(rtl, 0xa80, BIT(18), 0);                // BB CCK check off
    bb_w(rtl, 0x814, 0x0000FC00, 15);            // CCA mask
    rf18 &= ~(BIT(16) | BIT(9) | BIT(8) | 0xff); rf18 |= ch;
    rtl_switch_rf_set(rtl, rtl->btg ? RF_SET_BTG : RF_SET_WLG);
    rf_w(rtl, 0xdf, BIT(6), 1);                  // TXA_TANK LUT mode
    rf_w(rtl, 0x64, 0x0000f, 0xf);               // TXA_PA_TANK
  } else {
    bb_w(rtl, 0xa80, BIT(18), 1);
    bb_w(rtl, 0x454, BIT(7), 1);
    bb_w(rtl, 0x808, BIT(28), 0);                // CCK block off
    bb_w(rtl, 0x814, 0x0000FC00, 15);
    rf18 &= ~(BIT(16) | BIT(9) | BIT(8)); rf18 |= BIT(8) | BIT(16);
    rf18 &= ~0xff; rf18 |= ch;
    rtl_switch_rf_set(rtl, RF_SET_WLA);
    rf_w(rtl, 0xdf, BIT(6), 0);
  }
  rtl_write_rf18(rtl, rf18);
  rtl_set_bb_swing(rtl, is5g);
  rtl->band = is5g;
  return true;
}

/* config_phydm_switch_channel_8821c (E-cut: no 0xb8 A-cut fix) */
static bool rtl_switch_channel(wchip_rtl8821cu_t* rtl, uint8_t ch){
  uint32_t rf18 = rf_r(rtl, 0x18, RF_MASK);
  rf18 &= ~(BIT(18) | BIT(17) | 0xff); rf18 |= ch;
  if(ch <= 14){
    bb_w(rtl, 0xc1c, 0x00000F00, 0x0);           // AGC table 0
    bb_w(rtl, 0x860, 0x1ffe0000, 0x96a);         // CFO tracking center freq
    if(ch == 14){
      w32(rtl, 0xa24, 0x0000b81c); bb_w(rtl, 0xa28, 0x0000ffff, 0x0000); w32(rtl, 0xaac, 0x00003667);
    } else {
      w32(rtl, 0xa24, rtl->rega24); bb_w(rtl, 0xa28, 0x0000ffff, rtl->rega28 & 0xffff); w32(rtl, 0xaac, rtl->regaac);
    }
  } else {
    if(ch >= 100 && ch <= 140) rf18 |= BIT(17);
    else if(ch > 140) rf18 |= BIT(18);
    else if(!(ch >= 36 && ch <= 64)) return false;
    uint32_t agc = (ch <= 64) ? 1 : (ch <= 144) ? 2 : 3;
    bb_w(rtl, 0xc1c, 0x00000F00, agc);
    uint32_t fc = ch <= 48 ? 0x494 : ch <= 64 ? 0x453 : ch <= 116 ? 0x452 : 0x412;
    bb_w(rtl, 0x860, 0x1ffe0000, fc);
  }
  rtl_write_rf18(rtl, rf18);
  return true;
}

/* config_phydm_switch_bandwidth_8821c(20 MHz) + phydm_rxdfirpar_by_bw_8821c + MAC cfg_ch_bw */
static void rtl_set_bw20(wchip_rtl8821cu_t* rtl){
  uint32_t v = r32(rtl, 0x8ac);
  v &= 0xffcffc00; v |= 0x10010000;
  w32(rtl, 0x8ac, v);
  bb_w(rtl, 0x8c4, BIT(30), 1);
  uint32_t rf18 = rf_r(rtl, 0x18, RF_MASK) | BIT(11) | BIT(10);
  rtl_write_rf18(rtl, rf18);
  bb_w(rtl, 0x948, BIT(29) | BIT(28), 0x2);
  bb_w(rtl, 0x94c, BIT(29) | BIT(28), 0x2);
  bb_w(rtl, 0xc20, BIT(31), 0x1);
  bb_w(rtl, 0x8f0, BIT(31), 0x0);
  // MAC: primary 20 idx 0, BW20
  w8(rtl, 0x0483 /*DATA_SC*/, 0 | (10 << 4));
  w32(rtl, 0x0668, r32(rtl, 0x0668) & ~(BIT(7) | BIT(8)));
}

/* TX power: efuse base index per channel group (Hal_EfuseParseTxPowerInfo,
 * PG with power index), no by-rate/limit tables. 0x1d00 + rate (4 rates/dword). */
static int ch_group_2g(uint8_t ch, bool cck){
  if(ch <= 2) return 0; if(ch <= 5) return 1; if(ch <= 8) return 2; if(ch <= 11) return 3;
  if(ch <= 13 || !cck) return 4; return 5;
}
static int ch_group_5g(uint8_t ch){
  static const uint8_t ub[14] = {42, 48, 58, 64, 106, 114, 122, 130, 138, 144, 155, 161, 171, 177};
  for(int i = 0; i < 14; i++) if(ch <= ub[i]) return i;
  return 13;
}
static int8_t s4(uint8_t v){ return (v & 0x8) ? (int8_t)(v | 0xf0) : (int8_t)v; }

static void rtl_set_tx_power(wchip_rtl8821cu_t* rtl, uint8_t ch){
  const uint8_t* m = rtl->efuse + 0x10;          // path A txpwr_idx_table (42 bytes per path)
  uint8_t cck, ofdm, ht;
  if(ch <= 14){
    /* BTG modules (RFE 2/4) calibrate 2.4G on the BT-shared path: its indexes live in the path-B
     * block (rtw88 rtw8821c_read_efuse; vendor sets 2.4G power on RF_PATH_B for BTG). */
    if(rtl->btg) m += 42;
    uint8_t cb = m[ch_group_2g(ch, true)], b40 = m[6 + ch_group_2g(ch, false)];
    if(cb == 0xff) cb = 0x2d; if(b40 == 0xff) b40 = 0x2d;
    uint8_t d = m[11] == 0xff ? 0x02 : m[11];     // ht_1s_diff: ofdm[3:0] bw20[7:4]
    cck = cb; ofdm = (uint8_t)(b40 + s4(d & 0xf)); ht = (uint8_t)(b40 + s4(d >> 4));
  } else {
    uint8_t b40 = m[18 + ch_group_5g(ch)];
    if(b40 == 0xff) b40 = 0x2a;
    uint8_t d = m[18 + 14] == 0xff ? 0x02 : m[18 + 14];
    cck = 0; ofdm = (uint8_t)(b40 + s4(d & 0xf)); ht = (uint8_t)(b40 + s4(d >> 4));
  }
  if(cck > 0x3f) cck = 0x3f; if(ofdm > 0x3f) ofdm = 0x3f; if(ht > 0x3f) ht = 0x3f;
  if(rtl_debug()) LOG("rtl8821cu: txpwr ch%u idx cck 0x%02x ofdm 0x%02x ht 0x%02x", ch, cck, ofdm, ht);
  uint32_t c4 = cck * 0x01010101u, o4 = ofdm * 0x01010101u, h4 = ht * 0x01010101u;
  w32(rtl, 0x1d00 + 0x00, c4);                   // CCK 1/2/5.5/11
  w32(rtl, 0x1d00 + 0x04, o4);                   // OFDM 6..18
  w32(rtl, 0x1d00 + 0x08, o4);                   // OFDM 24..54
  for(int r = 0x0c; r <= 0x10; r += 4) w32(rtl, 0x1d00 + r, h4);   // MCS0-7
  for(int r = 0x2c; r <= 0x34; r += 4) w32(rtl, 0x1d00 + r, h4);   // VHT1SS MCS0-9
}

static bool rtl_debug(void){
  static int dbg = -1;
  if(dbg < 0) dbg = getenv("RTL8821CU_DEBUG") != NULL;
  return dbg;
}

static bool rtl_set_channel(wchip_rtl8821cu_t* rtl, uint8_t ch){
  if(!ch || (ch > 14 && ch < 36) || ch > 177) return false;
  if(rtl_debug()){
    static uint32_t last_rx, last_crc;
    LOG("rtl8821cu: ch%u -> ch%u (rx %u crc_err %u since last tune)", rtl->channel, ch,
        rtl->rx_pkts - last_rx, rtl->rx_crc_err - last_crc);
    last_rx = rtl->rx_pkts; last_crc = rtl->rx_crc_err;
  }
  bool is5g = ch > 14;
  if(rtl->band != is5g){ rtl_switch_band(rtl, ch); rtl_coex_band(rtl, is5g); }
  if(!rtl_switch_channel(rtl, ch)) return false;
  bb_w(rtl, 0x0454, BIT(7), is5g ? 1 : 0);       // cfg_ch_88xx: MAC CCK check on 5G
  rtl_set_tx_power(rtl, ch);
  rtl->channel = ch;
  return true;
}

/* init_bb_rf + config_phydm_parameter_init_8821c + tables + crystal cap */
static bool rtl_phy_init(wchip_rtl8821cu_t* rtl){
  uint8_t v = r8(rtl, 0x0002) | BIT(2);          // FEN_USBA
  w8(rtl, 0x0002, v);
  v |= BIT(1) | BIT(0); w8(rtl, 0x0002, v);      // BB_GLB_RSTN | BBRSTB: 1 -> 0 -> 1
  v &= ~(BIT(1) | BIT(0)); w8(rtl, 0x0002, v);
  v |= BIT(1) | BIT(0); w8(rtl, 0x0002, v);
  w8(rtl, 0x001F, BIT(0) | BIT(1) | BIT(2));     // RF_EN | RF_RSTB | RF_SDMRSTB
  usleep(10);
  w8(rtl, 0x00EF /*WLRF1+3*/, BIT(0) | BIT(1) | BIT(2));
  usleep(10);

  bb_w(rtl, 0x808, BIT(28) | BIT(29), 0x0);      // ODM_PRE_SETTING
  int nb = rtl_load_table(rtl, rtl8821c_phy_reg, countof(rtl8821c_phy_reg), TBL_BB);
  int na = rtl_load_table(rtl, rtl8821c_agc_tab, countof(rtl8821c_agc_tab), TBL_AGC);
  /* phydm_hwconfig.c CONFIG_BB_AGC_TAB: "According to RFEtype, choosing correct AGC table" -
   * a module whose 2.4G front end sits on the BT-shared port gets the BTG gain diff on top. */
  if(rtl->btg) na += rtl_load_table(rtl, rtl8821c_agc_tab_diff_btg, countof(rtl8821c_agc_tab_diff_btg), TBL_AGC);
  // crystal cap: 0x24[30:25] = 0x28[6:1] = xtal
  bb_w(rtl, 0x0024, 0x7e000000, rtl->xtal_cap & 0x3f);
  bb_w(rtl, 0x0028, 0x7e, rtl->xtal_cap & 0x3f);
  bb_w(rtl, 0xa2c, BIT(18) | BIT(22), 0);        // rCCK0_FalseAlarmReport
  int nr = rtl_load_table(rtl, rtl8821c_radioa, countof(rtl8821c_radioa), TBL_RF);
  bb_w(rtl, 0x808, BIT(28) | BIT(29), 0x3);      // ODM_POST_SETTING
  rtl->rega24 = r32(rtl, 0xa24); rtl->rega28 = r32(rtl, 0xa28); rtl->regaac = r32(rtl, 0xaac);
  LOG("rtl8821cu: phy tables: bb %d agc %d rf %d writes; rf18=0x%05x rf00=0x%05x", nb, na, nr,
      rf_r(rtl, 0x18, RF_MASK), rf_r(rtl, 0x00, RF_MASK));
  return nb > 0 && na > 0 && nr > 0;
}

/* ---- BT/WiFi coexistence (halbtc8821c1ant, 1-antenna) ----------------------
 * The WL 2.4G front end shares the BT antenna/path, so arbitration matters both ways:
 * forcing GNT_WL by software (phydm's WL test mode) blocks BT TX entirely. Instead, like
 * the vendor coex at init + PHASE_2G/5G with "wifi not connected" settings: the packet
 * traffic arbiter (PTA) grants WL/BT per packet using coex table 0, firmware TDMA off.
 * That keeps BT scanning/advertising working while WL scans and connects; it does not
 * implement the vendor's runtime TDMA schemes for busy BT links (A2DP/SCO/HID). */

/* halbtc8821c1ant_write_indirect_reg: masked RMW of an LTE-coex indirect register */
static void ltecoex_mask(wchip_rtl8821cu_t* rtl, uint16_t off, uint32_t mask, uint32_t v){
  ltecoex_w(rtl, off, (ltecoex_r(rtl, off) & ~mask) | ((v << mask_shift(mask)) & mask));
}
enum { GNT_HW_PTA = 0, GNT_SW_LOW = 1, GNT_SW_HIGH = 3 };
static void coex_gnt(wchip_rtl8821cu_t* rtl, int bt, int wl){   // set_gnt_bt / set_gnt_wl (LTE 0x38)
  ltecoex_mask(rtl, 0x38, 0xc000, bt); ltecoex_mask(rtl, 0x38, 0x0c00, bt);
  ltecoex_mask(rtl, 0x38, 0x3000, wl); ltecoex_mask(rtl, 0x38, 0x0300, wl);
}
static void coex_scoreboard(wchip_rtl8821cu_t* rtl, bool wl_on){ // write_scbd: tell BT whether WL is active
  w16(rtl, 0x00AA, 0x8002 | (wl_on ? 0x0003 /*ACTIVE|ONOFF*/ : 0));
}

/* halbtc8821c1ant_init_hw_config (not wifi-only) + set_rfe_type/set_ant_switch for this module */
static void rtl_coex_init(wchip_rtl8821cu_t* rtl){
  rtl_rfe_init(rtl);
  bb_w(rtl, 0x0550, BIT(3), 1);                  // TBTT interrupt
  w8(rtl, 0x0790, 0x5);                          // BT report sample rate
  w8(rtl, 0x0778, 0x1);                          // 1-antenna
  bb_w(rtl, 0x0040, BIT(5), 1);                 // PTA 3-wire from BT side (0x40[5])
  bb_w(rtl, 0x0040, BIT(9), 1);                  // ... and its pins (0x41[1])
  bb_w(rtl, 0x04C4, BIT(20) | BIT(21), 0x1);     // 0x4c6[5:4]=1: PTA tx/rx signal from WL side
  bb_w(rtl, 0x0760, BIT(28), 1);                 // 0x763[4]: GNT_BT=1 when both selected
  bb_w(rtl, 0x06CC, BIT(27), 1);                 // 0x6cf[3]: beacon queue always hi-pri
  rf_w(rtl, 0x01, BIT(1), 0);                    // concurrent RX mode
  // table 0 with concurrent_rx_mode_on: WL hi-pri can break BT, TX response (ACK/BA/CTS) hi-pri
  w32(rtl, 0x06C0, 0x55555555); w32(rtl, 0x06C4, 0x55555555);
  w32(rtl, 0x06C8, 0xf0ffffff); w8(rtl, 0x06CC, 0x1b);
  ltecoex_mask(rtl, 0x38, BIT(7), 0);            // LTE coex function off
  ltecoex_mask(rtl, 0xa0, 0xffff, 0xffff);       // GNT_WL_LTE always 1
  ltecoex_mask(rtl, 0xa4, 0xffff, 0xffff);       // GNT_BT_LTE always 1
  bb_w(rtl, 0x0070, BIT(26), 1);                 // path control owner = WL
  /* external antenna switch (set_ant_switch, CTRL_BY_BBSW): RFE_ctrl8/9 by BB software,
   * polarity per RFE type; 1-ant BTG/WLG on Main/Aux (halbtc8821c1ant_set_rfe_type) */
  uint8_t mod = rtl->rfe_expand & 0x1f;
  bool exists = !(mod == 5 || mod == 13 || mod == 6 || mod == 14);
  if(exists){
    bool at_main = !(mod == 3 || mod == 11 || mod == 4 || mod == 12);
    bool inverse = !at_main;                     // swap polarity if the 1-ant sits on Aux
    if(!rtl->btg) inverse = !inverse;            // pos WLG with WLG not at BTG
    bb_w(rtl, 0x004C, BIT(23), 0);               // 0x4c[23]=0
    bb_w(rtl, 0x004C, BIT(24), 1);               // 0x4c[24]=1
    bb_w(rtl, 0x0CB4, 0x000000ff, 0x77);         // DPDT on RFE_ctrl8/9
    bb_w(rtl, 0x0CB4, 0x30000000, inverse ? 0x2 : 0x1);
  }
  bb_w(rtl, 0x0064, BIT(29) | BIT(28), 0x3);     // PAPE / LNA_ON driven by WL
  coex_scoreboard(rtl, true);
}

/* set_ant_path PHASE_2G / PHASE_5G */
static void rtl_coex_band(wchip_rtl8821cu_t* rtl, bool is5g){
  bb_w(rtl, 0x0070, BIT(26), 1);
  if(is5g) coex_gnt(rtl, GNT_HW_PTA, GNT_SW_HIGH);   // 5G WL has its own front end
  else     coex_gnt(rtl, GNT_HW_PTA, GNT_HW_PTA);    // 2.4G shared with BT: arbitrate
}

/* set_ant_path PHASE_WOFF: hand the antenna and front-end control back to BT */
static void rtl_coex_wl_off(wchip_rtl8821cu_t* rtl){
  coex_scoreboard(rtl, false);
  ltecoex_mask(rtl, 0x38, BIT(7), 0);
  bb_w(rtl, 0x0070, BIT(26), 0);                 // path control owner = BT
  bb_w(rtl, 0x004C, BIT(23), 0);                 // CTRL_BY_BT: 0x4c[24:23] = 0
  bb_w(rtl, 0x004C, BIT(24), 0);
  bb_w(rtl, 0x0064, BIT(29) | BIT(28), 0x0);     // PAPE / LNA_ON by BT (leakage)
}

/* ======================================================================== */
/* TX / RX                                                                   */
/* ======================================================================== */
#define DESC_RATE_1M   0x00
#define DESC_RATE_6M   0x04
#define DESC_RATE_24M  0x08

static void LIBUSB_CALL on_tx_done(struct libusb_transfer* x){
  rtl_tx_slot_t* s = x->user_data;
  if(x->status == LIBUSB_TRANSFER_COMPLETED) s->rtl->tx_ok++;
  else s->rtl->tx_err++;
  s->busy = false;
}

/* fill_default_txdesc (MGNT: fixed rate, retry limit; DATA: fixed rate w/ fallback) */
static bool rtl_tx(void* ctx, const uint8_t* f, size_t len){
  wchip_rtl8821cu_t* rtl = ctx;
  if(!rtl->up || len < 10 || len > RTL_TX_BUF_SZ - TXDESC_SZ - 8) return false;
  rtl_tx_slot_t* s = NULL;
  for(int i = 0; i < RTL_TX_XFERS; i++) if(!rtl->tx[i].busy){ s = &rtl->tx[i]; break; }
  if(!s){ rtl->tx_drop++; return false; }
  bool mgmt = (f[0] & 0x0c) == 0x00, bmc = (f[4] & 1) != 0, is5g = rtl->channel > 14;
  uint8_t qsel = mgmt ? QSEL_MGT : QSEL_BE;
  uint32_t total = TXDESC_SZ + len, off = TXDESC_SZ;
  bool pad = (total % 512) == 0;
  if(pad){ total += 8; off += 8; }
  uint8_t* d = s->buf;
  memset(d, 0, off);
  desc_set(d, 0, 0, 16, len);                    // TXPKTSIZE
  desc_set(d, 0, 16, 8, off);                    // OFFSET
  desc_set(d, 0, 26, 1, 1);                      // LS
  desc_set(d, 0, 24, 1, bmc);                    // BMC
  desc_set(d, 0, 31, 1, 1);                      // DISQSELSEQ
  if(pad) desc_set(d, 1, 24, 5, 1);              // PKT_OFFSET
  desc_set(d, 1, 8, 5, qsel);                    // QSEL
  desc_set(d, 3, 8, 1, 1);                       // USE_RATE
  desc_set(d, 6, 0, 12, 1);                      // SW_DEFINE: driver fixed rate
  desc_set(d, 8, 15, 1, 1);                      // EN_HWSEQ
  if(mgmt){
    desc_set(d, 4, 0, 7, is5g ? DESC_RATE_6M : DESC_RATE_1M);
    desc_set(d, 3, 10, 1, 1);                    // DISDATAFB
    desc_set(d, 4, 17, 1, 1);                    // RTY_LMT_EN
    desc_set(d, 4, 18, 6, bmc ? 0 : 6);          // RTS_DATA_RTY_LMT
    if(!bmc) desc_set(d, 2, 19, 1, 1);           // SPE_RPT: firmware CCX report (ACKed or dropped)
  } else {
    desc_set(d, 2, 16, 1, 1);                    // BK (no aggregation)
    desc_set(d, 4, 0, 7, bmc ? DESC_RATE_6M : DESC_RATE_24M);
    desc_set(d, 4, 8, 5, DESC_RATE_6M);          // DATA_RTY_LOWEST_RATE (fallback floor)
    desc_set(d, 4, 17, 1, 1);
    desc_set(d, 4, 18, 6, bmc ? 0 : 12);
    if(!bmc && rtl_debug()) desc_set(d, 2, 19, 1, 1);   // SPE_RPT for data too (debug: per-frame C2H)
  }
  desc_chksum(d);
  memcpy(d + off, f, len);
  libusb_fill_bulk_transfer(s->xfer, rtl->usb_device.devh, qsel_ep(rtl, qsel), d, (int)total, on_tx_done, s, 1000);
  s->busy = true;
  int r = libusb_submit_transfer(s->xfer);
  if(r){ s->busy = false; rtl->tx_err++; LOG("rtl8821cu: tx submit: %s", libusb_strerror(r)); return false; }
  return true;
}

static int8_t rtl_rssi(wchip_rtl8821cu_t* rtl, const uint8_t* ps, uint8_t rate){
  if((ps[0] & 0xf) == 0){                        // page 0: CCK
    static const int8_t lna0[8] = {22, 8, -6, -22, -31, -40, -46, -52};
    static const int8_t lna1[16] = {10, 6, 2, -2, -6, -10, -14, -17, -20, -24, -28, -31, -34, -37, -40, -44};
    uint32_t w3 = le32(ps + 12);
    uint8_t vga = (w3 >> 8) & 0x1f, lna = ((w3 >> 13) & 0x7) | (((w3 >> 23) & 1) << 3);
    int8_t g = rtl->rfe_expand == 0 ? lna0[lna & 7] : lna1[lna & 15];
    return (int8_t)(g - 2 * vga);
  }
  return (int8_t)(ps[1] - 110);                  // page 1: OFDM/HT/VHT PWDB_A
}

static void rtl_rx_buf(wchip_rtl8821cu_t* rtl, uint8_t* p, int len){
  while(len >= 24){
    uint32_t dw0 = le32(p), dw2 = le32(p + 8), dw3 = le32(p + 12);
    uint32_t pkt_len = dw0 & 0x3fff, drv = ((dw0 >> 16) & 0xf) * 8, shift = (dw0 >> 24) & 0x3;
    bool crc = (dw0 >> 14) & 1, icv = (dw0 >> 15) & 1, physt = (dw0 >> 26) & 1, c2h = (dw2 >> 28) & 1;
    uint32_t pkt_off = 24 + drv + shift + pkt_len;
    if(!pkt_len || pkt_off > (uint32_t)len) break;
    if(c2h){
      // C2H event from firmware: [cmd id][seq][payload]; 0xFF = offload format [id][seq][sub id][len]
      const uint8_t* c = p + 24 + drv + shift;
      if(pkt_len < 3){ /* runt */ }
      else if(c[0] == 0xFF && c[2] == 0x0F && pkt_len >= 16){     // CCX TX report
        uint32_t w2 = le32(c + 8), w3 = le32(c + 12);
        uint8_t state = (w2 >> 30) & 3;                          // 0 = ACKed, 1 = retry limit hit
        if(state == 0) rtl->tx_acked++; else rtl->tx_noack++;
        if(rtl_debug()) LOG("rtl8821cu: TX report: %s (data retries %u, qsel 0x%02x, ch%u)", state ? "NO ACK" : "acked",
                            w3 & 0x3f, (le32(c + 4) >> 8) & 0x1f, rtl->channel);
      } else {
        static int nc;
        if(nc++ < 8) LOG("rtl8821cu: C2H id 0x%02x sub 0x%02x len %u", c[0], c[2], pkt_len);
      }
    } else if(!crc && !icv){
      uint8_t* f = p + 24 + drv + shift;
      int8_t rssi = (physt && drv >= 32) ? rtl_rssi(rtl, p + 24, dw3 & 0x7f) : -60;
      rtl->rx_pkts++;
      if(rtl->mode == WIFI_MODE_MONITOR)          // SoftMAC/monitor: every frame on the RAW80211 plane
        wifi_dev_on_rx_80211(rtl->base.dev, f, pkt_len, rtl->channel, rssi, dw3 & 0x7f);
      else wifi_softmac_rx(&rtl->sm, f, pkt_len, rssi, rtl->channel);
    } else rtl->rx_crc_err++;
    pkt_off = (pkt_off + 7) & ~7u;               // USB RX aggregation: 8-byte aligned
    p += pkt_off; len -= (int)pkt_off;
  }
}

#define RTL_RX_MAX_ERR_STREAK 50   // ~5 s of back-to-back failures: the device is gone or wedged

/* Resubmit RX slots parked by a transient error (ERROR/STALL/OVERFLOW). Runs from the io
 * loop because clearing a halt is a synchronous control transfer. Repeats (100 ms) until
 * every slot is back in flight; armed once per error burst by on_rx_done. */
static void rtl_rx_retry(io_timer_t* t){
  wchip_rtl8821cu_t* rtl = t->usr_data;
  bool all = true;
  for(int i = 0; rtl->up && i < RTL_RX_XFERS; i++){
    rtl_rx_slot_t* s = &rtl->rx[i];
    if(s->active || !s->xfer) continue;
    if(s->stalled){ libusb_clear_halt(rtl->usb_device.devh, rtl->ep_in); s->stalled = false; }
    if(rtl->rx_err_streak > RTL_RX_MAX_ERR_STREAK) continue;     // gave up meanwhile
    if(libusb_submit_transfer(s->xfer) == 0) s->active = true; else all = false;
  }
  if(all || !rtl->up || rtl->rx_err_streak > RTL_RX_MAX_ERR_STREAK) io_rem_timer(t);
}

static void LIBUSB_CALL on_rx_done(struct libusb_transfer* x){
  rtl_rx_slot_t* s = x->user_data;
  wchip_rtl8821cu_t* rtl = s->rtl;
  if(x->status == LIBUSB_TRANSFER_COMPLETED){
    rtl->rx_err_streak = 0;
    rtl->in_usb_cb = true;
    rtl_rx_buf(rtl, x->buffer, x->actual_length);
    rtl->in_usb_cb = false;
  }
  if(rtl->up && (x->status == LIBUSB_TRANSFER_COMPLETED || x->status == LIBUSB_TRANSFER_TIMED_OUT)){
    if(libusb_submit_transfer(x) == 0) return;
  }
  s->active = false;
  if(x->status == LIBUSB_TRANSFER_CANCELLED || x->status == LIBUSB_TRANSFER_NO_DEVICE || !rtl->up) return;
  bool retry = ++rtl->rx_err_streak <= RTL_RX_MAX_ERR_STREAK;
  if(rtl->rx_err_streak <= 3 || !retry) LOG("rtl8821cu: rx error (status %d)%s", x->status, retry ? ", retrying" : ", giving up");
  if(!retry) return;
  s->stalled = x->status == LIBUSB_TRANSFER_STALL;
  io_add_timer(&rtl->rx_retry_timer);                         // no-op if already pending
}

/* ======================================================================== */
/* softmac radio hooks                                                       */
/* ======================================================================== */
static bool rtl_radio_tune(void* ctx, uint8_t ch){ return rtl_set_channel(ctx, ch); }

/* cfg_bssid_88xx + cfg_net_type_88xx (port 0: 2 = infrastructure STA, 0 = no link) */
static void rtl_apply_bssid(wchip_rtl8821cu_t* rtl){
  const uint8_t* b = rtl->bssid;
  bool link = (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) != 0;
  w32(rtl, 0x0618, b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24));
  w16(rtl, 0x061C, b[4] | (b[5] << 8));
  w8(rtl, 0x0102, (r8(rtl, 0x0102) & 0xFC) | (link ? 2 : 0));
  if(rtl_debug()) LOG("rtl8821cu: bssid %02x:%02x:%02x:%02x:%02x:%02x (%s)", b[0], b[1], b[2], b[3], b[4], b[5], link ? "STA link" : "no link");
}
static void rtl_bssid_tick(io_timer_t* t){ rtl_apply_bssid(t->usr_data); }

static void rtl_radio_bssid(void* ctx, const uint8_t* b){
  wchip_rtl8821cu_t* rtl = ctx;
  if(b) memcpy(rtl->bssid, b, 6); else memset(rtl->bssid, 0, 6);
  /* The MLME clears the link from RX completions (deauth, failed assoc). Register I/O is a
   * sync control transfer, which libusb rejects (-6 BUSY) inside its own event handling,
   * so defer to the io loop when called from a USB callback. */
  if(rtl->in_usb_cb){
    io_rem_timer(&rtl->bssid_timer);
    rtl->bssid_timer = (io_timer_t){ .repeat = false, .interval_ms = 0, .usr_data = rtl, .cb = rtl_bssid_tick };
    io_add_timer(&rtl->bssid_timer);
  } else rtl_apply_bssid(rtl);
}
static void rtl_radio_prepare(void* ctx, uint8_t ch){ rtl_iqk(ctx); }   // rtw_chip_prepare_tx: RF calibration before connecting
static const wifi_softmac_radio_t rtl_radio = { .tx = rtl_tx, .set_channel = rtl_radio_tune, .set_bssid = rtl_radio_bssid,
                                                .prepare_link = rtl_radio_prepare };

static const uint8_t rtl_scan_chans[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165,
};

/* ======================================================================== */
/* lifecycle                                                                 */
/* ======================================================================== */
/* Completion handler for a transfer abandoned by rtl_stop_xfers: it frees itself. */
static void LIBUSB_CALL on_orphan_done(struct libusb_transfer* x){ libusb_free_transfer(x); }

static void rtl_stop_xfers(wchip_rtl8821cu_t* rtl){
  rtl->up = false;
  io_rem_timer(&rtl->rx_retry_timer);
  for(int i = 0; i < RTL_RX_XFERS; i++) if(rtl->rx[i].active && rtl->rx[i].xfer) libusb_cancel_transfer(rtl->rx[i].xfer);
  for(int i = 0; i < RTL_TX_XFERS; i++) if(rtl->tx[i].busy && rtl->tx[i].xfer) libusb_cancel_transfer(rtl->tx[i].xfer);
  libusb_context* ctx = rtl->usb_device.host ? rtl->usb_device.host->context : NULL;
  for(uint64_t end = sys_now() + 1000; ctx && sys_now() < end; ){
    bool busy = false;
    for(int i = 0; i < RTL_RX_XFERS; i++) busy |= rtl->rx[i].active;
    for(int i = 0; i < RTL_TX_XFERS; i++) busy |= rtl->tx[i].busy;
    if(!busy) break;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};
    libusb_handle_events_timeout(ctx, &tv);
  }
  int orphans = 0;
  for(int i = 0; i < RTL_RX_XFERS; i++){
    rtl_rx_slot_t* s = &rtl->rx[i];
    if(!s->xfer) continue;
    if(s->active){ s->xfer->callback = on_orphan_done; s->active = false; orphans++; }   // still in flight
    else libusb_free_transfer(s->xfer);
    s->xfer = NULL; s->stalled = false;
  }
  for(int i = 0; i < RTL_TX_XFERS; i++){
    rtl_tx_slot_t* s = &rtl->tx[i];
    if(!s->xfer) continue;
    if(s->busy){ s->xfer->callback = on_orphan_done; s->busy = false; orphans++; }
    else libusb_free_transfer(s->xfer);
    s->xfer = NULL;
  }
  if(orphans) LOG("rtl8821cu: %d transfer(s) did not finish cancelling; left to free themselves", orphans);
}

static void rtl_stats_tick(io_timer_t* t){
  wchip_rtl8821cu_t* rtl = t->usr_data;
  static uint32_t lrx, lcrc, ltx;
  LOG("rtl8821cu: [stats] ch%u rx %u (+%u) crc_err +%u tx_ok %u (+%u) tx_err %u acked %u noack %u", rtl->channel, rtl->rx_pkts, rtl->rx_pkts - lrx,
      rtl->rx_crc_err - lcrc, rtl->tx_ok, rtl->tx_ok - ltx, rtl->tx_err, rtl->tx_acked, rtl->tx_noack);
  lrx = rtl->rx_pkts; lcrc = rtl->rx_crc_err; ltx = rtl->tx_ok;
}

static bool rtl_start_rx(wchip_rtl8821cu_t* rtl){
  for(int i = 0; i < RTL_TX_XFERS; i++){
    rtl->tx[i].rtl = rtl; rtl->tx[i].busy = false;
    if(!(rtl->tx[i].xfer = libusb_alloc_transfer(0))) return false;
  }
  for(int i = 0; i < RTL_RX_XFERS; i++){
    rtl_rx_slot_t* s = &rtl->rx[i];
    s->rtl = rtl;
    if(!(s->xfer = libusb_alloc_transfer(0))) return false;
    libusb_fill_bulk_transfer(s->xfer, rtl->usb_device.devh, rtl->ep_in, s->buf, RTL_RX_BUF_SZ, on_rx_done, s, 0);
    if(libusb_submit_transfer(s->xfer)){ LOG("rtl8821cu: rx submit failed"); return false; }
    s->active = true;
  }
  return true;
}

static bool rtl_bringup(wchip_rtl8821cu_t* rtl){
  uint32_t cfg1 = r32(rtl, 0x00F0);
  uint8_t chip_id = r8(rtl, 0x00FC);
  rtl->cut = (cfg1 >> 12) & 0xf;
  LOG("rtl8821cu: chip id 0x%02x cut %c sys_cfg1 0x%08x", chip_id, 'A' + rtl->cut, cfg1);
  if(chip_id != 0x09){ LOG("rtl8821cu: not an RTL8821C (id 0x%02x)", chip_id); return false; }

  uint64_t t0 = sys_now();
  if(!rtl_power_on(rtl)){ LOG("rtl8821cu: power on failed"); return false; }
  LOG("rtl8821cu: powered on");
  if(!rtl_download_fw(rtl)){ LOG("rtl8821cu: firmware download failed"); return false; }
  if(!rtl_efuse_read(rtl)) return false;
  rtl_efuse_parse(rtl);
  if(rtl_debug()) HDUMP(rtl->efuse + 0x10, 0x60, "rtl8821cu: efuse txpwr (0x10..0x6f)");

  if(!rtl_init_trx(rtl)) return false;
  rtl_init_protocol(rtl);
  rtl_init_edca(rtl);
  rtl_init_wmac(rtl);
  bb_w(rtl, 0x0668 /*WMAC_TRXPTCL_CTL*/, BIT(13), 0);        // RTS full BW off
  rtl_init_usb(rtl);
  LOG("rtl8821cu: MAC initialised");
  rtl_send_general_info(rtl);

  int nm = rtl_load_table(rtl, rtl8821c_mac_reg, countof(rtl8821c_mac_reg), TBL_MAC);
  rtl_cfg_rx_drvinfo(rtl);
  if(!rtl_phy_init(rtl)) return false;
  LOG("rtl8821cu: mac table %d writes, BB/RF initialised", nm);
  rtl_hal_init_misc(rtl);
  rtl_set_macaddr(rtl, WIFI_SOFTMAC_STA_MAC);
  rtl_coex_init(rtl);

  rtl->band = 0xff;
  if(!rtl_set_channel(rtl, 1)) return false;
  rtl_set_bw20(rtl);
  LOG("rtl8821cu: up in %llums (ch1 BW20)", (unsigned long long)(sys_now() - t0));
  return true;
}

static bool rtl_op_init(wifi_chip_t* chip, usb_dev_info_t* info){
  wchip_rtl8821cu_t* rtl = RTL(chip);
  bool rc = false;
  if(rtl->usb_device.usr_data) goto end;
  void* host = rtl->usb_device.host;
  rtl->usb_device = (usb_device_t){0};
  rtl->usb_device.host = host;
  rtl->if_no = -1; rtl->up = false;

  FUN_CHK(usb_device_init, &rtl->usb_device, info);
  FUN_CHK(usb_device_open, &rtl->usb_device);
  usb_device_endpoint_t eps[8];
  int n = usb_device_find_eps(&rtl->usb_device, RTL_IFACE, eps, countof(eps));
  FUN_CHK(usb_device_claim_iface, &rtl->usb_device, RTL_IFACE);
  rtl->if_no = RTL_IFACE;
  rtl->ep_in = rtl->ep_int = 0; rtl->n_out = 0;
  for(int i = 0; i < n; i++){
    if(eps[i].is_input){ if(!rtl->ep_in) rtl->ep_in = eps[i].num; else if(!rtl->ep_int) rtl->ep_int = eps[i].num; }
    else if(rtl->n_out < countof(rtl->ep_out)) rtl->ep_out[rtl->n_out++] = eps[i].num;
  }
  LOG("rtl8821cu: if%d eps: in 0x%02x int 0x%02x out %d [0x%02x 0x%02x 0x%02x]", RTL_IFACE, rtl->ep_in, rtl->ep_int,
      rtl->n_out, rtl->ep_out[0], rtl->ep_out[1], rtl->ep_out[2]);
  if(!rtl->ep_in || rtl->n_out < 2) goto end;

  rtl->h2c_box = 0; rtl->h2c_seq = 0;                  // fresh firmware starts at mailbox 0 (rtw88 does the same)
  memset(rtl->bssid, 0, sizeof rtl->bssid);
  rtl->rx_err_streak = 0;
  rtl->rx_retry_timer = (io_timer_t){ .repeat = true, .run_now = false, .interval_ms = 100, .usr_data = rtl, .cb = rtl_rx_retry };
  rtl->sm.scan_chans = rtl_scan_chans; rtl->sm.n_scan_chans = countof(rtl_scan_chans); rtl->sm.scan_dwell_ms = 120;
  wifi_softmac_init(&rtl->sm, chip, &rtl_radio, rtl, "rtl8821cu");

  // NOTE: like mt7601u this runs synchronously inside the hotplug callback (~2-3 s).
  if(!rtl_bringup(rtl)){ LOG("rtl8821cu: bring-up failed"); goto end; }
  rtl->sm.channel = rtl->channel;
  rtl->up = true;
  if(!rtl_start_rx(rtl)){ rtl_stop_xfers(rtl); goto end; }
  if(rtl_debug()){
    rtl->stats_timer = (io_timer_t){ .repeat = true, .interval_ms = 1000, .usr_data = rtl, .cb = rtl_stats_tick };
    io_add_timer(&rtl->stats_timer);
  }
  rtl->usb_device.usr_data = rtl;
  rc = true;
end:
  if(!rc && rtl->usb_device.devh){
    if(rtl->if_no >= 0) usb_device_release_iface(&rtl->usb_device, rtl->if_no);
    usb_device_close(&rtl->usb_device);
    usb_device_deinit(&rtl->usb_device);
  }
  return rc;
}

static bool rtl_op_deinit(wifi_chip_t* chip){
  wchip_rtl8821cu_t* rtl = RTL(chip);
  if(!rtl->usb_device.usr_data) return false;
  wifi_softmac_stop(&rtl->sm);
  io_rem_timer(&rtl->stats_timer);
  io_rem_timer(&rtl->bssid_timer);
  rtl_stop_xfers(rtl);
  if(r8(rtl, 0x00FC) == 0x09) rtl_coex_wl_off(rtl);   // still attached: give the antenna back to BT
  LOG("rtl8821cu: down (rx %u crc_err %u tx_ok %u tx_err %u tx_drop %u)", rtl->rx_pkts, rtl->rx_crc_err, rtl->tx_ok, rtl->tx_err, rtl->tx_drop);
  if(rtl->if_no >= 0) usb_device_release_iface(&rtl->usb_device, rtl->if_no);
  usb_device_close(&rtl->usb_device);
  usb_device_deinit(&rtl->usb_device);
  rtl->usb_device.usr_data = NULL;
  return true;
}

/* ---- wifi_chip_ops -------------------------------------------------------- */
#define UP_OR_FALSE(rtl) if(!(rtl)->usb_device.usr_data || !(rtl)->up) return false
static bool rtl_op_reset(wifi_chip_t* c){ UP_OR_FALSE(RTL(c)); return wifi_softmac_reset(&RTL(c)->sm, 1); }
static bool rtl_op_set_mode(wifi_chip_t* c, uint8_t mode){ RTL(c)->mode = mode; LOG("rtl8821cu set_mode %u", mode); return true; }
static bool rtl_op_set_channel(wifi_chip_t* c, uint8_t ch){ UP_OR_FALSE(RTL(c)); bool ok = rtl_set_channel(RTL(c), ch); if(ok) RTL(c)->sm.channel = ch; return ok; }
static bool rtl_op_scan(wifi_chip_t* c, wifi_scan_req_t* r){ UP_OR_FALSE(RTL(c)); return wifi_softmac_scan(&RTL(c)->sm, r); }
static bool rtl_op_connect(wifi_chip_t* c, wifi_connect_req_t* r){ UP_OR_FALSE(RTL(c)); return wifi_softmac_connect(&RTL(c)->sm, r); }
static bool rtl_op_disconnect(wifi_chip_t* c, uint8_t reason){ UP_OR_FALSE(RTL(c)); return wifi_softmac_disconnect(&RTL(c)->sm, reason); }
static bool rtl_op_set_appie(wifi_chip_t* c, uint8_t t, uint8_t* ie, size_t l){ return wifi_softmac_set_appie(&RTL(c)->sm, t, ie, l); }
static bool rtl_op_set_key(wifi_chip_t* c, wifi_key_t* k, uint8_t* key){ return wifi_softmac_set_key(&RTL(c)->sm, k, key); }
static bool rtl_op_set_igtk(wifi_chip_t* c, wifi_igtk_t* g){ return wifi_softmac_set_igtk(&RTL(c)->sm, g); }
static bool rtl_op_send_mgmt(wifi_chip_t* c, wifi_mgmt_tx_t* m, uint8_t* f, size_t l){ UP_OR_FALSE(RTL(c)); return wifi_softmac_send_mgmt(&RTL(c)->sm, m, f, l); }
static bool rtl_op_sae_msg(wifi_chip_t* c, wifi_sae_t* s, uint8_t* b, size_t l){ UP_OR_FALSE(RTL(c)); return wifi_softmac_sae_msg(&RTL(c)->sm, s, b, l); }
static bool rtl_op_config_done(wifi_chip_t* c){ return true; }
static bool rtl_op_tx_data(wifi_chip_t* c, uint8_t* eth, size_t l){ UP_OR_FALSE(RTL(c)); return wifi_softmac_tx_data(&RTL(c)->sm, eth, l); }
static bool rtl_op_tx_raw(wifi_chip_t* c, uint8_t* f, size_t l){ UP_OR_FALSE(RTL(c)); return rtl_tx(RTL(c), f, l); }

const wifi_chip_ops_t wchip_rtl8821cu_ops = {
  .init        = rtl_op_init,
  .deinit      = rtl_op_deinit,
  .reset       = rtl_op_reset,
  .set_mode    = rtl_op_set_mode,
  .set_channel = rtl_op_set_channel,
  .scan        = rtl_op_scan,
  .connect     = rtl_op_connect,
  .disconnect  = rtl_op_disconnect,
  .set_appie   = rtl_op_set_appie,
  .set_key     = rtl_op_set_key,
  .set_igtk    = rtl_op_set_igtk,
  .send_mgmt   = rtl_op_send_mgmt,
  .sae_msg     = rtl_op_sae_msg,
  .config_done = rtl_op_config_done,
  .tx_data     = rtl_op_tx_data,
  .tx_raw80211 = rtl_op_tx_raw,
};
