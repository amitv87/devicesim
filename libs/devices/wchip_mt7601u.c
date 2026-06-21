#include <unistd.h>
#include "wchip_mt7601u.h"
#include "mt7601u_fw_bin.h"
#include "ccmp.h"

#define MT(chip) ((wchip_mt7601u_t*)(chip))
#define MT_IFACE 0
#define FUN_CHK(fun, ...) if(!fun(__VA_ARGS__)){LOG("%s failed", #fun); goto end;}

/* MT7601U USB vendor protocol + registers, transcribed from the Linux mt7601u
 * driver (usb.c / mcu.c / regs.h / mcu.h). bmRequestType is vendor|device +
 * direction; bRequest selects the op. */
#define MT_VEND_REQ_OUT     (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define MT_VEND_REQ_IN      (LIBUSB_ENDPOINT_IN  | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define MT_VEND_DEV_MODE    0x01
#define MT_VEND_WRITE       0x02
#define MT_VEND_MULTI_READ  0x07
#define MT_VEND_WRITE_FCE   0x42
#define MT_VEND_DEV_MODE_RESET 0x01
#define MT_VEND_REQ_TOUT_MS 300

#define MT_ASIC_VERSION     0x0000 // reads 0x7601xxxx
#define MT_FCE_DMA_ADDR     0x0230
#define MT_FCE_DMA_LEN      0x0234
#define MT_USB_DMA_CFG      0x0238
#define MT_USB_DMA_CFG_TX_CLR    (1u << 19)
#define MT_USB_DMA_CFG_RX_BULK_EN (1u << 22)
#define MT_USB_DMA_CFG_TX_BULK_EN (1u << 23)
#define MT_PBF_CFG          0x0404
#define MT_PBF_CFG_TXQ_EN   0x0f   // TX0Q..TX3Q_EN
#define MT_MCU_COM_REG0     0x0730
#define MT_MCU_COM_REG1     0x0734
#define MT_FCE_PSE_CTRL     0x0800
#define MT_TX_CPU_FROM_FCE_BASE_PTR  0x09a0
#define MT_TX_CPU_FROM_FCE_MAX_COUNT 0x09a4
#define MT_TX_CPU_FROM_FCE_CPU_DESC_IDX 0x09a8
#define MT_FCE_PDMA_GLOBAL_CONF 0x09c4
#define MT_FCE_SKIP_FS      0x0a6c

#define MT_MCU_IVB_SIZE     0x40
#define MT_MCU_DLM_OFFSET   0x80000
#define MT_MCU_MEMMAP_WLAN  0x00410000
#define CPU_TX_PORT         2          // mt76_msg_port
#define MCU_FW_URB_MAX_PAYLOAD 0x3800
#define MT_DMA_HDR_LEN      4

static inline uint16_t le16(const uint8_t* p){ return p[0] | (p[1] << 8); }
static inline uint32_t le32(const uint8_t* p){ return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

// Register read: vendor MULTI_READ IN, 4 LE bytes (matches __mt7601u_rr).
static uint32_t mt_rr(wchip_mt7601u_t* mt, uint32_t offset){
  uint8_t b[4] = {0};
  int rc = usb_device_control_transfer(&mt->usb_device, MT_VEND_REQ_IN, MT_VEND_MULTI_READ,
    0, offset, b, sizeof(b), MT_VEND_REQ_TOUT_MS);
  return rc == 4 ? le32(b) : ~0u;
}

// Register write: two 16-bit vendor writes, value in wValue, offset in wIndex,
// no data payload (matches __mt7601u_vendor_single_wr).
static bool mt_single_wr(wchip_mt7601u_t* mt, uint8_t req, uint16_t offset, uint32_t val){
  if(usb_device_control_transfer(&mt->usb_device, MT_VEND_REQ_OUT, req, val & 0xffff, offset, NULL, 0, MT_VEND_REQ_TOUT_MS) < 0) return false;
  return usb_device_control_transfer(&mt->usb_device, MT_VEND_REQ_OUT, req, (val >> 16) & 0xffff, offset + 2, NULL, 0, MT_VEND_REQ_TOUT_MS) >= 0;
}
static bool mt_wr(wchip_mt7601u_t* mt, uint32_t offset, uint32_t val){ return mt_single_wr(mt, MT_VEND_WRITE, offset, val); }
static uint32_t mt_rmw(wchip_mt7601u_t* mt, uint32_t offset, uint32_t mask, uint32_t val){
  val |= mt_rr(mt, offset) & ~mask;
  mt_wr(mt, offset, val);
  return val;
}
static void mt_reset(wchip_mt7601u_t* mt){
  usb_device_control_transfer(&mt->usb_device, MT_VEND_REQ_OUT, MT_VEND_DEV_MODE, MT_VEND_DEV_MODE_RESET, 0, NULL, 0, MT_VEND_REQ_TOUT_MS);
}
static bool mt_poll(wchip_mt7601u_t* mt, uint32_t offset, uint32_t mask, uint32_t val, int timeout_ms){
  for(int i = timeout_ms / 10 + 1; i > 0; i--){
    if((mt_rr(mt, offset) & mask) == val) return true;
    usleep(10 * 1000);
  }
  return false;
}

/* Push one <=0x3800 firmware chunk: prepend the 4-byte FCE TXINFO descriptor,
 * program the DMA dest addr/len, bulk-OUT it, then bump the FCE desc index
 * (matches __mt7601u_dma_fw). */
static bool mt_dma_fw_chunk(wchip_mt7601u_t* mt, const uint8_t* data, uint32_t len, uint32_t dst_addr){
  static uint8_t buf[MT_DMA_HDR_LEN + MCU_FW_URB_MAX_PAYLOAD + 8];
  uint32_t rlen = (len + 3) & ~3u;
  uint32_t info = ((uint32_t)CPU_TX_PORT << 27) | (len & 0xffff); // type DMA_PACKET(0)
  memset(buf, 0, MT_DMA_HDR_LEN + rlen + 4);
  buf[0] = info; buf[1] = info >> 8; buf[2] = info >> 16; buf[3] = info >> 24;
  memcpy(buf + MT_DMA_HDR_LEN, data, len);
  if(!mt_single_wr(mt, MT_VEND_WRITE_FCE, MT_FCE_DMA_ADDR, dst_addr)) return false;
  if(!mt_single_wr(mt, MT_VEND_WRITE_FCE, MT_FCE_DMA_LEN, rlen << 16)) return false;
  if(!usb_transfer_submit(&mt->tx, buf, MT_DMA_HDR_LEN + rlen + 4)) return false;
  mt_wr(mt, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, mt_rr(mt, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX) + 1);
  return true;
}
static bool mt_dma_fw(wchip_mt7601u_t* mt, const uint8_t* data, uint32_t len, uint32_t dst_addr){
  while(len){
    uint32_t n = min((uint32_t)MCU_FW_URB_MAX_PAYLOAD, len);
    if(!mt_dma_fw_chunk(mt, data, n, dst_addr)) return false;
    if(!mt_poll(mt, MT_MCU_COM_REG1, 1u << 31, 1u << 31, 500)) return false;
    data += n, len -= n, dst_addr += n;
  }
  return true;
}

/* Full firmware download: register preamble -> ILM/DLM DMA -> send IVB ->
 * wait for the MCU to report running (matches mt7601u_load_firmware). */
static bool mt7601u_load_firmware(wchip_mt7601u_t* mt){
  const uint8_t* fw = mt7601u_fw;
  uint32_t ilm_len = le32(fw + 0), dlm_len = le32(fw + 4);
  if(mt7601u_fw_len != 0x20 + ilm_len + dlm_len || ilm_len <= MT_MCU_IVB_SIZE){
    LOG("mt7601u: bad firmware image"); return false;
  }
  LOG("mt7601u fw ver %u.%u.%02u build %x time %.16s",
    (le16(fw+0x0a) >> 12) & 0xf, (le16(fw+0x0a) >> 8) & 0xf, le16(fw+0x0a) & 0xf, le16(fw+0x08), fw + 0x10);

  mt_wr(mt, MT_USB_DMA_CFG, MT_USB_DMA_CFG_RX_BULK_EN | MT_USB_DMA_CFG_TX_BULK_EN);
  if(mt_rr(mt, MT_MCU_COM_REG0) == 1){ LOG("mt7601u: fw already running"); return true; }

  mt_wr(mt, 0x94c, 0);
  mt_wr(mt, MT_FCE_PSE_CTRL, 0);
  mt_reset(mt);
  usleep(5 * 1000);
  mt_wr(mt, 0xa44, 0);
  mt_wr(mt, 0x230, 0x84210);
  mt_wr(mt, 0x400, 0x80c00);
  mt_wr(mt, 0x800, 1);
  mt_rmw(mt, MT_PBF_CFG, 0, MT_PBF_CFG_TXQ_EN);
  mt_wr(mt, MT_FCE_PSE_CTRL, 1);
  mt_wr(mt, MT_USB_DMA_CFG, MT_USB_DMA_CFG_RX_BULK_EN | MT_USB_DMA_CFG_TX_BULK_EN);
  uint32_t val = mt_rmw(mt, MT_USB_DMA_CFG, 0, MT_USB_DMA_CFG_TX_CLR); // mt76_set
  mt_wr(mt, MT_USB_DMA_CFG, val & ~MT_USB_DMA_CFG_TX_CLR);
  mt_wr(mt, MT_TX_CPU_FROM_FCE_BASE_PTR, 0x400230);
  mt_wr(mt, MT_TX_CPU_FROM_FCE_MAX_COUNT, 1);
  mt_wr(mt, MT_FCE_PDMA_GLOBAL_CONF, 0x44);
  mt_wr(mt, MT_FCE_SKIP_FS, 3);

  const uint8_t* ivb = fw + 0x20;                       // hdr is 0x20 bytes
  const uint8_t* ilm = ivb + MT_MCU_IVB_SIZE;           // ILM follows the IVB
  uint32_t ilm_payload = ilm_len - MT_MCU_IVB_SIZE;     // IVB is bundled in ilm_len
  if(!mt_dma_fw(mt, ilm, ilm_payload, MT_MCU_IVB_SIZE)){ LOG("mt7601u: ILM upload failed"); return false; }
  if(!mt_dma_fw(mt, ilm + ilm_payload, dlm_len, MT_MCU_DLM_OFFSET)){ LOG("mt7601u: DLM upload failed"); return false; }

  if(usb_device_control_transfer(&mt->usb_device, MT_VEND_REQ_OUT, MT_VEND_DEV_MODE,
       0x12, 0, (uint8_t*)ivb, MT_MCU_IVB_SIZE, MT_VEND_REQ_TOUT_MS) < 0){
    LOG("mt7601u: IVB send failed"); return false;
  }
  for(int i = 100; i > 0; i--){
    if(mt_rr(mt, MT_MCU_COM_REG0) == 1){ LOG("mt7601u: firmware running"); return true; }
    usleep(10 * 1000);
  }
  LOG("mt7601u: firmware start timed out");
  return false;
}

/* ---- eFUSE / EEPROM read ------------------------------------------------
 * The eFUSE controller returns 16 bytes per kick; addr is 16-byte aligned.
 * Matches mt7601u_efuse_read / mt7601u_eeprom_init. */
#define MT_EFUSE_CTRL        0x0024
#define MT_EFUSE_CTRL_AOUT   0x3f          // GENMASK(5,0)
#define MT_EFUSE_CTRL_MODE   0xc0          // GENMASK(7,6)
#define MT_EFUSE_CTRL_AIN_SHIFT 16         // GENMASK(25,16)
#define MT_EFUSE_CTRL_AIN    0x03ff0000u
#define MT_EFUSE_CTRL_KICK   (1u << 30)
#define MT_EFUSE_DATA_BASE   0x0028
#define MT7601U_EEPROM_SIZE  256
#define MT_EE_VERSION_FAE    0x02
#define MT_EE_VERSION_EE     0x03
#define MT_EE_MAC_ADDR       0x04
#define MT_EE_NIC_CONF_1     0x36
#define MT_EE_FREQ_OFFSET    0x3a
#define MT_EE_LNA_GAIN       0x44
#define MT_EE_RSSI_OFFSET    0x46
#define MT_EE_REF_TEMP       0xd1
#define MT_EE_FREQ_OFFSET_COMP 0xdb
#define MT_EE_READ           0

static inline int8_t ee_field(uint8_t v){ return v == 0xff ? 0 : (int8_t)v; }

static bool mt_efuse_read(wchip_mt7601u_t* mt, uint16_t addr, uint8_t* data /*16B*/){
  uint32_t val = mt_rr(mt, MT_EFUSE_CTRL);
  val &= ~(MT_EFUSE_CTRL_AIN | MT_EFUSE_CTRL_MODE);
  val |= (((uint32_t)(addr & ~0xf) << MT_EFUSE_CTRL_AIN_SHIFT) & MT_EFUSE_CTRL_AIN)
       | ((MT_EE_READ << 6) & MT_EFUSE_CTRL_MODE) | MT_EFUSE_CTRL_KICK;
  if(!mt_wr(mt, MT_EFUSE_CTRL, val)) return false;
  if(!mt_poll(mt, MT_EFUSE_CTRL, MT_EFUSE_CTRL_KICK, 0, 1000)) return false;
  if((mt_rr(mt, MT_EFUSE_CTRL) & MT_EFUSE_CTRL_AOUT) == MT_EFUSE_CTRL_AOUT){
    memset(data, 0xff, 16); // region not in usage map; not an error
    return true;
  }
  for(int i = 0; i < 4; i++){
    uint32_t v = mt_rr(mt, MT_EFUSE_DATA_BASE + (i << 2));
    data[i*4+0] = v; data[i*4+1] = v >> 8; data[i*4+2] = v >> 16; data[i*4+3] = v >> 24;
  }
  return true;
}

static bool mt7601u_eeprom_init(wchip_mt7601u_t* mt){
  uint8_t ee[MT7601U_EEPROM_SIZE];
  for(int i = 0; i + 16 <= MT7601U_EEPROM_SIZE; i += 16)
    if(!mt_efuse_read(mt, i, ee + i)){ LOG("mt7601u: efuse read failed @0x%02x", i); return false; }

  LOG("mt7601u eeprom ver:%02x fae:%02x", ee[MT_EE_VERSION_EE], ee[MT_EE_VERSION_FAE]);
  memcpy(mt->mac_addr, ee + MT_EE_MAC_ADDR, 6);
  LOG("mt7601u mac: %02x:%02x:%02x:%02x:%02x:%02x", mt->mac_addr[0], mt->mac_addr[1],
    mt->mac_addr[2], mt->mac_addr[3], mt->mac_addr[4], mt->mac_addr[5]);

  // calibration inputs (mt7601u_set_rf_freq_off / set_rssi_offset / has_tssi)
  int8_t fo = ee_field(ee[MT_EE_FREQ_OFFSET]);
  int8_t comp = ee_field(ee[MT_EE_FREQ_OFFSET_COMP]);
  mt->ee_rf_freq_off = (comp & 0x80) ? fo - (comp & 0x7f) : fo + comp;
  mt->ee_ref_temp = ee[MT_EE_REF_TEMP];
  mt->ee_lna_gain = ee[MT_EE_LNA_GAIN];
  for(int i = 0; i < 2; i++){
    int8_t r = ee[MT_EE_RSSI_OFFSET + i];
    mt->ee_rssi_offset[i] = (r < -10 || r > 10) ? 0 : r;
  }
  uint16_t nic_conf1 = ee[MT_EE_NIC_CONF_1] | (ee[MT_EE_NIC_CONF_1 + 1] << 8);
  bool has_tssi = (uint16_t)~nic_conf1 && (nic_conf1 & (1u << 13)); // TX_ALC_EN
  mt->ee_tssi_enabled = has_tssi && !(nic_conf1 & (1u << 1));       // !TEMP_TX_ALC
  LOG("mt7601u ee: freq_off:%d ref_temp:%d lna:%d rssi:[%d,%d] tssi:%d",
    mt->ee_rf_freq_off, mt->ee_ref_temp, mt->ee_lna_gain,
    mt->ee_rssi_offset[0], mt->ee_rssi_offset[1], mt->ee_tssi_enabled);
  return true;
}

static bool mt7601u_mcu_cmd_init(wchip_mt7601u_t* mt);
static bool mt_chip_onoff(wchip_mt7601u_t* mt, bool enable);
static void mt_reset_csr_bbp(wchip_mt7601u_t* mt);
static void mt_init_usb_dma(wchip_mt7601u_t* mt);
static bool mt7601u_init_mac(wchip_mt7601u_t* mt);
static bool mt7601u_init_bbp(wchip_mt7601u_t* mt);
static bool mt7601u_phy_init(wchip_mt7601u_t* mt);
static int  mt_rx_rssi(wchip_mt7601u_t* mt, const uint8_t* rxwi, uint16_t rate);
static bool mt_set_channel(wchip_mt7601u_t* mt, uint8_t channel);
static bool mt_mac_start(wchip_mt7601u_t* mt);
static bool mt_tx_frame(wchip_mt7601u_t* mt, const uint8_t* frame, size_t frame_len);
static void mt_scan_collect(wchip_mt7601u_t* mt, const uint8_t* frame, uint16_t mpdu_len, int8_t rssi);
static void mt_conn_rx(wchip_mt7601u_t* mt, const uint8_t* frame, uint16_t len);   // auth/assoc resp
static void mt_data_rx(wchip_mt7601u_t* mt, const uint8_t* frame, uint16_t len);   // 802.11 data → 802.3
enum { MT_CONN_IDLE = 0, MT_CONN_AUTH, MT_CONN_SAE_COMMIT, MT_CONN_SAE_CONFIRM,
       MT_CONN_ASSOC, MT_CONN_CONNECTED };
#define MT7601U_RXWI_LEN 28

/* Bulk IN completion: a frame off the air. Real driver strips the RXWI
 * descriptor + 802.11 header and (for data) yields an 802.3 frame; management
 * frames go up via wifi_dev_on_rx_mgmt. */
/* A USB RX URB holds 1+ segments, each:
 *   [4B DMA hdr (dma_len in first 2B LE)][28B RXWI][802.11 frame][4B FCE]
 * total = 8 + dma_len, dma_len = RXWI + frame. (dma.c mt7601u_rx_process_seg) */
static void on_rx(usb_transfer_t* transfer, uint8_t* data, size_t length){
  wchip_mt7601u_t* mt = transfer->usr_data;
  uint8_t* p = data;
  size_t rem = length;
  const size_t min_seg = 8 + MT7601U_RXWI_LEN + 4;
  while(rem >= min_seg){
    uint16_t dma_len = p[0] | (p[1] << 8);
    if(!dma_len || (dma_len & 3) || (size_t)dma_len + 8 > rem) break;
    size_t seg = 8 + dma_len;
    uint8_t* rxwi = p + 4;
    uint8_t* frame = rxwi + MT7601U_RXWI_LEN;
    uint32_t ctl = rxwi[4] | (rxwi[5]<<8) | (rxwi[6]<<16) | ((uint32_t)rxwi[7]<<24);
    uint16_t mpdu_len = (ctl >> 16) & 0xfff;  // MT_RXWI_CTL_MPDU_LEN
    uint16_t rate = rxwi[10] | (rxwi[11] << 8);
    if(mpdu_len >= 10 && frame + mpdu_len <= p + seg){
      int rssi = mt_rx_rssi(mt, rxwi, rate);
      if(mt->scanning){
        // active scan: collect beacons/probe-resps into results, don't flood the OS
        if(mpdu_len >= 38 && (frame[0] == 0x80 || frame[0] == 0x50))
          mt_scan_collect(mt, frame, mpdu_len, (int8_t)rssi);
      } else if(mt->conn_state == MT_CONN_CONNECTED){
        uint8_t type = frame[0] & 0x0c;          // 0x00 mgmt, 0x08 data
        if(type == 0x08) mt_data_rx(mt, frame, mpdu_len);                 // data (incl EAPOL)
        else if(frame[0] == 0xc0 || frame[0] == 0xa0){                    // deauth / disassoc
          LOG("mt7601u: deauth/disassoc from AP"); mt->conn_state = MT_CONN_IDLE;
          wifi_dev_on_disconnected(mt->base.dev, 0);
        }
      } else if(mt->conn_state != MT_CONN_IDLE){
        mt_conn_rx(mt, frame, mpdu_len);          // auth / assoc response
      } else {
        wifi_dev_on_rx_80211(mt->base.dev, frame, mpdu_len, mt->channel, (int8_t)rssi, rate);
      }
    }
    p += seg, rem -= seg;
  }
}

static bool mt_init(wifi_chip_t* chip, usb_dev_info_t* info){
  wchip_mt7601u_t* mt = MT(chip);
  bool rc = false;
  LOG("mt7601u: ccmp selftest %s", ccmp_selftest() == 0 ? "PASS" : "FAIL");
  if(mt->usb_device.usr_data) goto end;
  void* host = mt->usb_device.host;
  mt->usb_device = (usb_device_t){0};
  mt->usb_device.host = host;
  mt->if_no = -1;

  FUN_CHK(usb_device_init, &mt->usb_device, info);
  FUN_CHK(usb_device_open, &mt->usb_device);
  usb_device_reset(&mt->usb_device);  // clear any wedged-interface state (like Linux probe)

  usb_device_endpoint_t eps[8];
  int n = usb_device_find_eps(&mt->usb_device, MT_IFACE, eps, countof(eps));
  LOG("mt7601u eps: %d", n);

  FUN_CHK(usb_device_claim_iface, &mt->usb_device, MT_IFACE);
  mt->if_no = MT_IFACE;

  // OUT eps in descriptor order: [0]INBAND_CMD [1]AC_BK [2]AC_BE [3]AC_VI [4]AC_VO [5]HCCA
  int rx_ep = -1, cmd_resp_ep = -1, tx_ep = -1, out_idx = 0;
  mt->data_tx_ep = 0;
  for(int i = 0; i < n; i++){
    if(eps[i].is_input){
      if(rx_ep < 0) rx_ep = eps[i].num;            // PKT_RX
      else if(cmd_resp_ep < 0) cmd_resp_ep = eps[i].num; // CMD_RESP
    }
    else {
      if(tx_ep < 0) tx_ep = eps[i].num;            // INBAND_CMD = out[0]
      if(out_idx == 2) mt->data_tx_ep = eps[i].num; // AC_BE = out[2]
      out_idx++;
    }
  }
  LOG("mt7601u rx_ep:0x%02x cmd_resp_ep:0x%02x tx_ep:0x%02x data_tx_ep:0x%02x", rx_ep, cmd_resp_ep, tx_ep, mt->data_tx_ep);
  if(rx_ep < 0 || cmd_resp_ep < 0 || tx_ep < 0) goto end;
  mt->ep_cmd_resp = cmd_resp_ep;

  mt->tx = (usb_transfer_t){.usr_data = mt, .device = &mt->usb_device};
  mt->rx = (usb_transfer_t){.usr_data = mt, .on_transfer = on_rx, .device = &mt->usb_device};
  FUN_CHK(usb_transfer_init, &mt->tx, tx_ep);
  FUN_CHK(usb_transfer_init, &mt->rx, rx_ep);
  mt->tx.xfer->timeout = 1000; // bound the synchronous firmware bulk-OUT

  mt_chip_onoff(mt, true);
  LOG("mt7601u asic version: 0x%08x", mt_rr(mt, MT_ASIC_VERSION));

  // NOTE: this runs synchronously inside the usb hotplug callback and blocks the
  // io loop for the duration of the download; move to a deferred step later.
  if(!mt7601u_load_firmware(mt)){ LOG("mt7601u: firmware download failed"); }
  else{
    mt_reset_csr_bbp(mt);
    mt_init_usb_dma(mt);
    if(!mt7601u_mcu_cmd_init(mt)){ LOG("mt7601u: mcu cmd_init failed"); }
    else if(!mt7601u_init_mac(mt)){ LOG("mt7601u: mac init failed"); }
    else if(!mt7601u_init_bbp(mt)){ LOG("mt7601u: bbp init failed"); }
    else{
      LOG("mt7601u: mac+bbp initialised");
      if(mt7601u_eeprom_init(mt) && mt7601u_phy_init(mt)){
        LOG("mt7601u: phy initialised");
        if(mt_set_channel(mt, 6) && mt_mac_start(mt)){
          LOG("mt7601u: monitoring channel 6 (RX live)");
          // TX self-test: inject a broadcast probe-request; if TX works, nearby
          // APs answer with probe-responses we then capture on the RX side.
          uint8_t probe[] = {
            0x40, 0x00, 0x00, 0x00,                   // FC=probe-req, duration
            0xff,0xff,0xff,0xff,0xff,0xff,            // DA broadcast
            mt->mac_addr[0],mt->mac_addr[1],mt->mac_addr[2],mt->mac_addr[3],mt->mac_addr[4],mt->mac_addr[5], // SA
            0xff,0xff,0xff,0xff,0xff,0xff,            // BSSID broadcast
            0x00, 0x00,                                // seq
            0x00, 0x00,                                // SSID IE (wildcard)
            0x01, 0x04, 0x02, 0x04, 0x0b, 0x16,        // supported rates 1/2/5.5/11
          };
          LOG("mt7601u: TX self-test probe-req -> %d", mt_tx_frame(mt, probe, sizeof(probe)));
        }
      }
    }
  }

  // arm RX only after the MCU is up (avoids async IN racing the sync fw bulk).
  FUN_CHK(usb_transfer_submit, &mt->rx, mt->rx_buff, sizeof(mt->rx_buff));

  // TODO next: EEPROM read, RF + BBP calibration, RX filters, channel set.
  // Until that lands the radio will not associate.

  rc = true;
  if(rc) mt->usb_device.usr_data = mt;
  end:
  return rc;
}

static bool mt_deinit(wifi_chip_t* chip){
  wchip_mt7601u_t* mt = MT(chip);
  if(!mt->usb_device.usr_data) return false;
  for(int i = 0; i < countof(mt->xfers); i++) usb_transfer_deinit(&mt->xfers[i]);
  if(mt->if_no >= 0) usb_device_release_iface(&mt->usb_device, mt->if_no);
  usb_device_close(&mt->usb_device);
  usb_device_deinit(&mt->usb_device);
  mt->usb_device.usr_data = NULL;
  return true;
}

/* ---- MCU inband command channel ----------------------------------------
 * Commands are DMA_COMMAND frames out the INBAND_CMD bulk-OUT EP (same EP the
 * firmware uploaded over); response events arrive on the CMD_RESP bulk-IN EP.
 * Synchronous (bring-up only) -- matches mt7601u_mcu_msg_send. */
#define INBAND_PACKET_MAX_LEN 192
#define MCU_RESP_LEN          1024
#define DMA_COMMAND           1     // mt76_info_type
#define CMD_FUN_SET_OP        1     // mcu_cmd
#define MCU_Q_SELECT          1     // mcu_function
#define MT_MCU_EVT_CMD_DONE   0     // mt76_evt_type

static bool mt_mcu_msg_send(wchip_mt7601u_t* mt, const uint8_t* data, uint32_t len, uint8_t cmd, bool want_resp){
  static uint8_t buf[MT_DMA_HDR_LEN + INBAND_PACKET_MAX_LEN + 4];
  uint32_t rlen = (len + 3) & ~3u;
  if(MT_DMA_HDR_LEN + rlen + 4 > sizeof(buf)){ LOG("mcu cmd too big: %u", len); return false; }

  uint8_t seq = 0;
  if(want_resp){ mt->mcu_seq = (mt->mcu_seq + 1) & 0xf; if(!mt->mcu_seq) mt->mcu_seq = 1; seq = mt->mcu_seq; }

  // TXINFO: type DMA_COMMAND, d_port CPU_TX_PORT, cmd, seq, len (round up to 4)
  uint32_t info = ((uint32_t)DMA_COMMAND << 30) | ((uint32_t)CPU_TX_PORT << 27)
                | ((uint32_t)cmd << 20) | ((uint32_t)seq << 16) | rlen;
  memset(buf, 0, MT_DMA_HDR_LEN + rlen + 4);
  buf[0] = info; buf[1] = info >> 8; buf[2] = info >> 16; buf[3] = info >> 24;
  memcpy(buf + MT_DMA_HDR_LEN, data, len);
  if(!usb_transfer_submit(&mt->tx, buf, MT_DMA_HDR_LEN + rlen + 4)) return false;
  if(!want_resp) return true;

  // confirm the matching response event (rxfce: seq[19:16], evt_type[23:20])
  for(int i = 5; i > 0; i--){
    uint8_t resp[MCU_RESP_LEN];
    int rc = usb_device_bulk_transfer(&mt->usb_device, mt->ep_cmd_resp, resp, sizeof(resp), 300);
    if(rc < 4) continue;
    uint32_t rxfce = le32(resp);
    uint8_t rseq = (rxfce >> 16) & 0xf, evt = (rxfce >> 20) & 0xf;
    if(rseq == seq && evt == MT_MCU_EVT_CMD_DONE) return true;
    LOG("mcu resp mismatch seq:%u/%u evt:%u", rseq, seq, evt);
  }
  return false;
}

static bool mt_mcu_function_select(wchip_mt7601u_t* mt, uint32_t func, uint32_t val){
  uint8_t msg[8] = {func, func >> 8, func >> 16, func >> 24, val, val >> 8, val >> 16, val >> 24};
  return mt_mcu_msg_send(mt, msg, sizeof(msg), CMD_FUN_SET_OP, func == 5);
}

static bool mt7601u_mcu_cmd_init(wchip_mt7601u_t* mt){
  mt->mcu_seq = 0;
  // select the command queue; no response expected (func != 5)
  return mt_mcu_function_select(mt, MCU_Q_SELECT, 1);
}

/* ---- MAC / USB-DMA init ------------------------------------------------
 * Register tables + bring-up transcribed from mt7601u init.c / mac.c /
 * initvals.h. Offsets resolved from regs.h (symbol in the trailing comment). */
#define CMD_RANDOM_WRITE     12         // mcu_cmd
#define MT_WLAN_FUN_CTRL     0x0080
#define MT_WLAN_FUN_CTRL_WLAN_EN     (1u << 0)
#define MT_WLAN_FUN_CTRL_WLAN_CLK_EN (1u << 1)
#define MT_CMB_CTRL          0x0020
#define MT_CMB_CTRL_XTAL_RDY (1u << 22)
#define MT_CMB_CTRL_PLL_LD   (1u << 23)
#define MT_MAC_SYS_CTRL      0x1004
#define MT_MAC_SYS_CTRL_RESET_CSR (1u << 0)
#define MT_MAC_SYS_CTRL_RESET_BBP (1u << 1)
#define MT_MAC_STATUS        0x1200
#define MT_MAC_STATUS_TX     (1u << 0)
#define MT_MAC_STATUS_RX     (1u << 1)
#define MT_AUX_CLK_CFG       0x120c
#define MT_USB_DMA_CFG_RX_BULK_AGG_EN (1u << 21)
#define MT_USB_DMA_CFG_UDMA_RX_WL_DROP (1u << 25)
#define MT_USB_AGGR_TIMEOUT  0x80
#define MT_USB_AGGR_SIZE_LIMIT 28

static const uint32_t mac_common_vals[][2] = {
  {0x1408, 0x0000013f}, {0x140c, 0x00008003}, {0x1004, 0x00000000}, {0x1400, 0x00017f97},
  {0x1104, 0x00000209}, {0x1330, 0x00000000}, {0x1334, 0x00080606}, {0x1350, 0x00001020},
  {0x1348, 0x000a2090}, {0x1018, 0x00003fff}, {0x0408, 0x1fbf1f1f}, {0x040c, 0x0000009f},
  {0x134c, 0x47d01f0f}, {0x1404, 0x00000013}, {0x1364, 0x05740003}, {0x1368, 0x05740003},
  {0x1370, 0x03f44084}, {0x1374, 0x01744004}, {0x1378, 0x03f44084}, {0x136c, 0x01744004},
  {0x1340, 0x0000583f}, {0x1344, 0x01092b20}, {0x1380, 0x002400ca}, {0x1608, 0x00000002},
  {0x1100, 0x33a41010}, {0x1204, 0x00000000}, {0x150c, 0x00000001},
};
static const uint32_t mac_chip_vals[][2] = {
  {0x0250, 0x00006050}, {0x041c, 0x18100800}, {0x0420, 0x38302820}, {0x0400, 0x00080c00},
  {0x0404, 0x7f723c1f}, {0x0800, 0x00000001}, {0x0a38, 0x00000000}, {0x13a0, 0x003b0005},
  {0x13a8, 0x00006900}, {0x13c0, 0x00000400}, {0x13c8, 0x00060006}, {0x1330, 0x00000402},
  {0x1334, 0x00000000}, {0x1338, 0x00000000}, {0x0260, 0x00000000}, {0x0808, 0x0000030f},
  {0x0804, 0x00256f0f},
};

// Write reg/value pairs through the MCU (CMD_RANDOM_WRITE), <=24 pairs per cmd.
static bool mt_write_reg_pairs(wchip_mt7601u_t* mt, uint32_t base, const uint32_t (*pairs)[2], int n){
  const int max_per_cmd = INBAND_PACKET_MAX_LEN / 8; // 24
  uint8_t buf[INBAND_PACKET_MAX_LEN];
  for(int off = 0; off < n; ){
    int cnt = min(max_per_cmd, n - off);
    uint8_t* p = buf;
    for(int i = 0; i < cnt; i++){
      uint32_t reg = base + pairs[off + i][0], val = pairs[off + i][1];
      p[0]=reg; p[1]=reg>>8; p[2]=reg>>16; p[3]=reg>>24; p+=4;
      p[0]=val; p[1]=val>>8; p[2]=val>>16; p[3]=val>>24; p+=4;
    }
    off += cnt;
    if(!mt_mcu_msg_send(mt, buf, cnt * 8, CMD_RANDOM_WRITE, off >= n)) return false;
  }
  return true;
}

static bool mt_chip_onoff(wchip_mt7601u_t* mt, bool enable){
  uint32_t val = mt_rr(mt, MT_WLAN_FUN_CTRL);
  mt_wr(mt, MT_WLAN_FUN_CTRL, val);
  usleep(20);
  if(enable) val |= MT_WLAN_FUN_CTRL_WLAN_EN | MT_WLAN_FUN_CTRL_WLAN_CLK_EN;
  else val &= ~MT_WLAN_FUN_CTRL_WLAN_EN;
  mt_wr(mt, MT_WLAN_FUN_CTRL, val);
  usleep(20);
  if(!enable) return true;
  for(int i = 200; i > 0; i--){
    val = mt_rr(mt, MT_CMB_CTRL);
    if((val & MT_CMB_CTRL_XTAL_RDY) && (val & MT_CMB_CTRL_PLL_LD)) return true;
    usleep(20);
  }
  LOG("mt7601u: PLL/XTAL not ready (continuing)");
  return true; // Linux warns but proceeds
}

static void mt_reset_csr_bbp(wchip_mt7601u_t* mt){
  mt_wr(mt, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_RESET_CSR | MT_MAC_SYS_CTRL_RESET_BBP);
  mt_wr(mt, MT_USB_DMA_CFG, 0);
  usleep(1000);
  mt_wr(mt, MT_MAC_SYS_CTRL, 0);
}

static void mt_init_usb_dma(wchip_mt7601u_t* mt){
  uint32_t val = (MT_USB_AGGR_TIMEOUT & 0xff) | ((MT_USB_AGGR_SIZE_LIMIT & 0xff) << 8)
               | MT_USB_DMA_CFG_RX_BULK_EN | MT_USB_DMA_CFG_TX_BULK_EN;
  if(mt->rx.max_pkt_size == 512) val |= MT_USB_DMA_CFG_RX_BULK_AGG_EN;
  mt_wr(mt, MT_USB_DMA_CFG, val);
  mt_wr(mt, MT_USB_DMA_CFG, val | MT_USB_DMA_CFG_UDMA_RX_WL_DROP);
  mt_wr(mt, MT_USB_DMA_CFG, val);
}

// Write the MAC register tables and wait for the MAC engine to go idle.
static bool mt7601u_init_mac(wchip_mt7601u_t* mt){
  if(!mt_write_reg_pairs(mt, MT_MCU_MEMMAP_WLAN, mac_common_vals, countof(mac_common_vals))) return false;
  if(!mt_write_reg_pairs(mt, MT_MCU_MEMMAP_WLAN, mac_chip_vals, countof(mac_chip_vals))) return false;
  // TODO mt76_init_beacon_offsets (only needed for beacon/AP mode)
  mt_wr(mt, MT_AUX_CLK_CFG, 0);
  if(!mt_poll(mt, MT_MAC_STATUS, MT_MAC_STATUS_TX | MT_MAC_STATUS_RX, 0, 100)){
    LOG("mt7601u: MAC_STATUS stayed busy:0x%08x", mt_rr(mt, MT_MAC_STATUS)); return false;
  }
  LOG("mt7601u: MAC idle (status:0x%08x)", mt_rr(mt, MT_MAC_STATUS));
  return true;
}

/* BBP register tables (initvals.h), written through the MCU at the BBP memmap
 * base -- same reg-pair path as the MAC tables. */
#define MT_MCU_MEMMAP_BBP 0x40000000

static const uint32_t bbp_common_vals[][2] = {
  {65, 0x2c}, {66, 0x38}, {68, 0x0b}, {69, 0x12}, {70, 0x0a}, {73, 0x10},
  {81, 0x37}, {82, 0x62}, {83, 0x6a}, {84, 0x99}, {86, 0x00}, {91, 0x04},
  {92, 0x00}, {103, 0x00}, {105, 0x05}, {106, 0x35},
};
static const uint32_t bbp_chip_vals[][2] = {
  {1, 0x04}, {4, 0x40}, {20, 0x06}, {31, 0x08}, {178, 0xff}, {66, 0x14},
  {68, 0x8b}, {69, 0x12}, {70, 0x09}, {73, 0x11}, {75, 0x60}, {76, 0x44},
  {84, 0x9a}, {86, 0x38}, {91, 0x07}, {92, 0x02}, {99, 0x50}, {101, 0x00},
  {103, 0xc0}, {104, 0x92}, {105, 0x3c}, {106, 0x03}, {128, 0x12}, {142, 0x04},
  {143, 0x37}, {142, 0x03}, {143, 0x99}, {160, 0xeb}, {161, 0xc4}, {162, 0x77},
  {163, 0xf9}, {164, 0x88}, {165, 0x80}, {166, 0xff}, {167, 0xe4}, {195, 0x00},
  {196, 0x00}, {195, 0x01}, {196, 0x04}, {195, 0x02}, {196, 0x20}, {195, 0x03},
  {196, 0x0a}, {195, 0x06}, {196, 0x16}, {195, 0x07}, {196, 0x05}, {195, 0x08},
  {196, 0x37}, {195, 0x0a}, {196, 0x15}, {195, 0x0b}, {196, 0x17}, {195, 0x0c},
  {196, 0x06}, {195, 0x0d}, {196, 0x09}, {195, 0x0e}, {196, 0x05}, {195, 0x0f},
  {196, 0x09}, {195, 0x10}, {196, 0x20}, {195, 0x20}, {196, 0x17}, {195, 0x21},
  {196, 0x06}, {195, 0x22}, {196, 0x09}, {195, 0x23}, {196, 0x17}, {195, 0x24},
  {196, 0x06}, {195, 0x25}, {196, 0x09}, {195, 0x26}, {196, 0x17}, {195, 0x27},
  {196, 0x06}, {195, 0x28}, {196, 0x09}, {195, 0x29}, {196, 0x05}, {195, 0x2a},
  {196, 0x09}, {195, 0x80}, {196, 0x8b}, {195, 0x81}, {196, 0x12}, {195, 0x82},
  {196, 0x09}, {195, 0x83}, {196, 0x17}, {195, 0x84}, {196, 0x11}, {195, 0x85},
  {196, 0x00}, {195, 0x86}, {196, 0x00}, {195, 0x87}, {196, 0x18}, {195, 0x88},
  {196, 0x60}, {195, 0x89}, {196, 0x44}, {195, 0x8a}, {196, 0x8b}, {195, 0x8b},
  {196, 0x8b}, {195, 0x8c}, {196, 0x8b}, {195, 0x8d}, {196, 0x8b}, {195, 0x8e},
  {196, 0x09}, {195, 0x8f}, {196, 0x09}, {195, 0x90}, {196, 0x09}, {195, 0x91},
  {196, 0x09}, {195, 0x92}, {196, 0x11}, {195, 0x93}, {196, 0x11}, {195, 0x94},
  {196, 0x11}, {195, 0x95}, {196, 0x11}, {47, 0x80}, {60, 0x80}, {150, 0xd2},
  {151, 0x32}, {152, 0x23}, {153, 0x41}, {154, 0x00}, {155, 0x4f}, {253, 0x7e},
  {195, 0x30}, {196, 0x32}, {195, 0x31}, {196, 0x23}, {195, 0x32}, {196, 0x45},
  {195, 0x35}, {196, 0x4a}, {195, 0x36}, {196, 0x5a}, {195, 0x37}, {196, 0x5a},
};

static bool mt7601u_init_bbp(wchip_mt7601u_t* mt){
  if(!mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, bbp_common_vals, countof(bbp_common_vals))) return false;
  return mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, bbp_chip_vals, countof(bbp_chip_vals));
}

/* ---- RF init (phy.c) ---------------------------------------------------- */
#define MT_MCU_MEMMAP_RF 0x80000000u
#define RF(b, r)         (MT_MCU_MEMMAP_RF | ((uint32_t)(b) << 16) | (uint32_t)(r))
#define MT_RF_CSR_CFG    0x0500
#define MT_RF_CSR_CFG_WR   (1u << 30)
#define MT_RF_CSR_CFG_KICK (1u << 31)
#define MT_RF_PA_MODE_CFG0 0x121c
#define MT_RF_PA_MODE_CFG1 0x1220

// RF registers are accessed through MT_RF_CSR_CFG (data[7:0], reg[13:8], bank[17:14]).
static bool mt_rf_wr(wchip_mt7601u_t* mt, uint8_t bank, uint8_t off, uint8_t val){
  if(!mt_poll(mt, MT_RF_CSR_CFG, MT_RF_CSR_CFG_KICK, 0, 100)) return false;
  return mt_wr(mt, MT_RF_CSR_CFG, (val & 0xff) | ((uint32_t)(off & 0x3f) << 8)
    | ((uint32_t)(bank & 0xf) << 14) | MT_RF_CSR_CFG_WR | MT_RF_CSR_CFG_KICK);
}
static int mt_rf_rr(wchip_mt7601u_t* mt, uint8_t bank, uint8_t off){
  if(!mt_poll(mt, MT_RF_CSR_CFG, MT_RF_CSR_CFG_KICK, 0, 100)) return -1;
  mt_wr(mt, MT_RF_CSR_CFG, ((uint32_t)(off & 0x3f) << 8) | ((uint32_t)(bank & 0xf) << 14) | MT_RF_CSR_CFG_KICK);
  if(!mt_poll(mt, MT_RF_CSR_CFG, MT_RF_CSR_CFG_KICK, 0, 100)) return -1;
  uint32_t v = mt_rr(mt, MT_RF_CSR_CFG);
  if(((v >> 8) & 0x3f) == off && ((v >> 14) & 0xf) == bank) return v & 0xff;
  return -1;
}
static int mt_rf_rmw(wchip_mt7601u_t* mt, uint8_t bank, uint8_t off, uint8_t mask, uint8_t val){
  int r = mt_rf_rr(mt, bank, off);
  if(r < 0) return r;
  val |= r & ~mask;
  return mt_rf_wr(mt, bank, off, val) ? val : -1;
}

static const uint32_t rf_central[][2] = {
  {RF(0,0), 0x02}, {RF(0,1), 0x01}, {RF(0,2), 0x11}, {RF(0,3), 0xff}, {RF(0,4), 0x0a},
  {RF(0,5), 0x20}, {RF(0,6), 0x00}, {RF(0,7), 0x00}, {RF(0,8), 0x00}, {RF(0,9), 0x00},
  {RF(0,10), 0x00}, {RF(0,11), 0x21}, {RF(0,13), 0x00}, {RF(0,14), 0x7c}, {RF(0,15), 0x22},
  {RF(0,16), 0x80}, {RF(0,17), 0x99}, {RF(0,18), 0x99}, {RF(0,19), 0x09}, {RF(0,20), 0x50},
  {RF(0,21), 0xb0}, {RF(0,22), 0x00}, {RF(0,23), 0xc5}, {RF(0,24), 0xfc}, {RF(0,25), 0x40},
  {RF(0,26), 0x4d}, {RF(0,27), 0x02}, {RF(0,28), 0x72}, {RF(0,29), 0x01}, {RF(0,30), 0x00},
  {RF(0,31), 0x00}, {RF(0,32), 0x00}, {RF(0,33), 0x00}, {RF(0,34), 0x23}, {RF(0,35), 0x01},
  {RF(0,36), 0x00}, {RF(0,37), 0x00}, {RF(0,38), 0x00}, {RF(0,39), 0x20}, {RF(0,40), 0x00},
  {RF(0,41), 0xd0}, {RF(0,42), 0x1b}, {RF(0,43), 0x02}, {RF(0,44), 0x00},
};
static const uint32_t rf_channel[][2] = {
  {RF(4,0), 0x01}, {RF(4,1), 0x00}, {RF(4,2), 0x00}, {RF(4,3), 0x00}, {RF(4,4), 0x00},
  {RF(4,5), 0x08}, {RF(4,6), 0x00}, {RF(4,7), 0x5b}, {RF(4,8), 0x52}, {RF(4,9), 0xb6},
  {RF(4,10), 0x57}, {RF(4,11), 0x33}, {RF(4,12), 0x22}, {RF(4,13), 0x3d}, {RF(4,14), 0x3e},
  {RF(4,15), 0x13}, {RF(4,16), 0x22}, {RF(4,17), 0x23}, {RF(4,18), 0x02}, {RF(4,19), 0xa4},
  {RF(4,20), 0x01}, {RF(4,21), 0x12}, {RF(4,22), 0x80}, {RF(4,23), 0xb3}, {RF(4,24), 0x00},
  {RF(4,25), 0x00}, {RF(4,26), 0x00}, {RF(4,27), 0x00}, {RF(4,28), 0x18}, {RF(4,29), 0xee},
  {RF(4,30), 0x6b}, {RF(4,31), 0x31}, {RF(4,32), 0x5d}, {RF(4,33), 0x00}, {RF(4,34), 0x96},
  {RF(4,35), 0x55}, {RF(4,36), 0x08}, {RF(4,37), 0xbb}, {RF(4,38), 0xb3}, {RF(4,39), 0xb3},
  {RF(4,40), 0x03}, {RF(4,41), 0x00}, {RF(4,42), 0x00}, {RF(4,43), 0xc5}, {RF(4,44), 0xc5},
  {RF(4,45), 0xc5}, {RF(4,46), 0x07}, {RF(4,47), 0xa8}, {RF(4,48), 0xef}, {RF(4,49), 0x1a},
  {RF(4,54), 0x07}, {RF(4,55), 0xa7}, {RF(4,56), 0xcc}, {RF(4,57), 0x14}, {RF(4,58), 0x07},
  {RF(4,59), 0xa8}, {RF(4,60), 0xd7}, {RF(4,61), 0x10}, {RF(4,62), 0x1c}, {RF(4,63), 0x00},
};
static const uint32_t rf_vga[][2] = {
  {RF(5,0), 0x47}, {RF(5,1), 0x00}, {RF(5,2), 0x00}, {RF(5,3), 0x08}, {RF(5,4), 0x04},
  {RF(5,5), 0x20}, {RF(5,6), 0x3a}, {RF(5,7), 0x3a}, {RF(5,8), 0x00}, {RF(5,9), 0x00},
  {RF(5,10), 0x10}, {RF(5,11), 0x10}, {RF(5,12), 0x10}, {RF(5,13), 0x10}, {RF(5,14), 0x10},
  {RF(5,15), 0x20}, {RF(5,16), 0x22}, {RF(5,17), 0x7c}, {RF(5,18), 0x00}, {RF(5,19), 0x00},
  {RF(5,20), 0x00}, {RF(5,21), 0xf1}, {RF(5,22), 0x11}, {RF(5,23), 0x02}, {RF(5,24), 0x41},
  {RF(5,25), 0x20}, {RF(5,26), 0x00}, {RF(5,27), 0xd7}, {RF(5,28), 0xa2}, {RF(5,29), 0x20},
  {RF(5,30), 0x49}, {RF(5,31), 0x20}, {RF(5,32), 0x04}, {RF(5,33), 0xf1}, {RF(5,34), 0xa1},
  {RF(5,35), 0x01}, {RF(5,41), 0x00}, {RF(5,42), 0x00}, {RF(5,43), 0x00}, {RF(5,44), 0x00},
  {RF(5,45), 0x00}, {RF(5,46), 0x00}, {RF(5,47), 0x00}, {RF(5,48), 0x00}, {RF(5,49), 0x00},
  {RF(5,50), 0x00}, {RF(5,51), 0x00}, {RF(5,52), 0x00}, {RF(5,53), 0x00}, {RF(5,54), 0x00},
  {RF(5,55), 0x00}, {RF(5,56), 0x00}, {RF(5,57), 0x00}, {RF(5,58), 0x31}, {RF(5,59), 0x31},
  {RF(5,60), 0x0a}, {RF(5,61), 0x02}, {RF(5,62), 0x00}, {RF(5,63), 0x00},
};

/* ---- BBP register access (MT_BBP_CSR_CFG) ------------------------------- */
#define MT_BBP_CSR_CFG     0x101c
#define MT_BBP_CSR_CFG_READ    (1u << 16)
#define MT_BBP_CSR_CFG_BUSY    (1u << 17)
#define MT_BBP_CSR_CFG_RW_MODE (1u << 19)

static bool mt_bbp_wr(wchip_mt7601u_t* mt, uint8_t off, uint8_t val){
  if(!mt_poll(mt, MT_BBP_CSR_CFG, MT_BBP_CSR_CFG_BUSY, 0, 1000)) return false;
  return mt_wr(mt, MT_BBP_CSR_CFG, (val & 0xff) | ((uint32_t)off << 8)
    | MT_BBP_CSR_CFG_RW_MODE | MT_BBP_CSR_CFG_BUSY);
}
static int mt_bbp_rr(wchip_mt7601u_t* mt, uint8_t off){
  if(!mt_poll(mt, MT_BBP_CSR_CFG, MT_BBP_CSR_CFG_BUSY, 0, 1000)) return -1;
  mt_wr(mt, MT_BBP_CSR_CFG, ((uint32_t)off << 8) | MT_BBP_CSR_CFG_RW_MODE
    | MT_BBP_CSR_CFG_BUSY | MT_BBP_CSR_CFG_READ);
  if(!mt_poll(mt, MT_BBP_CSR_CFG, MT_BBP_CSR_CFG_BUSY, 0, 1000)) return -1;
  uint32_t v = mt_rr(mt, MT_BBP_CSR_CFG);
  return ((v >> 8) & 0xff) == off ? (int)(v & 0xff) : -1;
}
static int mt_bbp_rmw(wchip_mt7601u_t* mt, uint8_t off, uint8_t mask, uint8_t val){
  int r = mt_bbp_rr(mt, off);
  if(r < 0) return r;
  val |= r & ~mask;
  return mt_bbp_wr(mt, off, val) ? val : -1;
}
static int mt_bbp_rmc(wchip_mt7601u_t* mt, uint8_t off, uint8_t mask, uint8_t val){
  int r = mt_bbp_rr(mt, off);
  if(r < 0) return r;
  val |= r & ~mask;
  if(r != val && !mt_bbp_wr(mt, off, val)) return -1;
  return val;
}
static uint8_t mt_bbp_r47_get(wchip_mt7601u_t* mt, uint8_t reg, uint8_t flag){
  flag |= reg & ~0x07;          // BBP_R47_FLAG = GENMASK(2,0)
  mt_bbp_wr(mt, 47, flag);
  usleep(600);
  int r = mt_bbp_rr(mt, 49);
  return r < 0 ? 0 : (uint8_t)r;
}

/* ---- calibration helpers (phy.c) ---------------------------------------- */
#define CMD_CALIBRATION_OP 31
#define MCU_CAL_R 1
#define MCU_CAL_LOFT 4
#define MCU_CAL_TXIQ 5
#define MCU_CAL_BW 6
#define MCU_CAL_DPD 7
#define MCU_CAL_RXIQ 8
#define MCU_CAL_TXDCOC 9
#define MT_RF_BYPASS_0  0x0504
#define MT_RF_SETTING_0 0x050c
#define MT_MAC_SYS_CTRL_ENABLE_TX (1u << 2)
#define MT_MAC_SYS_CTRL_ENABLE_RX (1u << 3)
#define MT_TX_ALC_CFG_1 0x13b4
#define MT_EE_TEMPERATURE_SLOPE 39
enum { MT_TEMP_MODE_NORMAL, MT_TEMP_MODE_HIGH, MT_TEMP_MODE_LOW };

static uint32_t int_to_s6(int v){ return v < -0x20 ? 0x20 : v > 0x1f ? 0x1f : (v & 0x3f); }

static bool mt_mcu_calibrate(wchip_mt7601u_t* mt, uint32_t cal, uint32_t val){
  uint8_t msg[8] = {cal, cal>>8, cal>>16, cal>>24, val, val>>8, val>>16, val>>24};
  // Fire-and-forget (seq=0): the firmware runs the calibration and sends no
  // response event. Reading the cmd-resp EP while it's busy stalls; just wait.
  if(!mt_mcu_msg_send(mt, msg, sizeof(msg), CMD_CALIBRATION_OP, false)) return false;
  usleep(50 * 1000);
  return true;
}

static void mt_vco_cal(wchip_mt7601u_t* mt){
  mt_rf_wr(mt, 0, 4, 0x0a);
  mt_rf_wr(mt, 0, 5, 0x20);
  mt_rf_rmw(mt, 0, 4, 0, 0x80);   // rf_set BIT(7)
  usleep(2000);
}

static int8_t mt_read_bootup_temp(wchip_mt7601u_t* mt){
  uint32_t rf_set = mt_rr(mt, MT_RF_SETTING_0), rf_bp = mt_rr(mt, MT_RF_BYPASS_0);
  mt_wr(mt, MT_RF_BYPASS_0, 0);
  mt_wr(mt, MT_RF_SETTING_0, 0x10);
  mt_wr(mt, MT_RF_BYPASS_0, 0x10);
  int bbp = mt_bbp_rmw(mt, 47, 0, 0x10);
  mt_bbp_wr(mt, 22, 0x40);
  for(int i = 100; i > 0 && (bbp & 0x10); i--) bbp = mt_bbp_rr(mt, 47);
  uint8_t temp = mt_bbp_r47_get(mt, bbp, 4);  // BBP_R47_F_TEMP
  mt_bbp_wr(mt, 22, 0);
  int v = mt_bbp_rr(mt, 21); mt_bbp_wr(mt, 21, v | 0x02); mt_bbp_wr(mt, 21, v & ~0x02);
  mt_wr(mt, MT_RF_BYPASS_0, 0);
  mt_wr(mt, MT_RF_SETTING_0, rf_set);
  mt_wr(mt, MT_RF_BYPASS_0, rf_bp);
  return (int8_t)temp;
}

static void mt_rxdc_cal(wchip_mt7601u_t* mt){
  static const uint32_t intro[][2] = {{158,0x8d},{159,0xfc},{158,0x8c},{159,0x4c}};
  static const uint32_t outro[][2] = {{158,0x8d},{159,0xe0}};
  uint32_t mac_ctrl = mt_rr(mt, MT_MAC_SYS_CTRL);
  mt_wr(mt, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_RX);
  mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, intro, countof(intro));
  for(int i = 20; i > 0; i--){ usleep(400); mt_bbp_wr(mt, 158, 0x8c); if(mt_bbp_rr(mt, 159) == 0x0c) break; }
  mt_wr(mt, MT_MAC_SYS_CTRL, 0);
  mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, outro, countof(outro));
  mt_wr(mt, MT_MAC_SYS_CTRL, mac_ctrl);
}

static bool mt_set_bw_filter(wchip_mt7601u_t* mt, bool cal){
  uint32_t filter = 0;
  if(!cal) filter |= 0x10000;
  if(mt->bw != 0) filter |= 0x100;   // bw != MT_BW_20
  if(!mt_mcu_calibrate(mt, MCU_CAL_BW, filter | 1)) return false;
  return mt_mcu_calibrate(mt, MCU_CAL_BW, filter);
}

static int fls32(uint32_t x){ int n = 0; while(x){ n++; x >>= 1; } return n; }
static int16_t lin2dBd(uint16_t linear){
  if(!linear) return -10000;
  unsigned mant = linear;
  int exp = fls32(mant) - 16;
  if(exp > 0) mant >>= exp; else mant <<= -exp;
  int app = (mant <= 0xb800) ? (int)(mant + (mant>>3) + (mant>>4) - 0x9600)
                             : (int)(mant - (mant>>3) - (mant>>6) - 0x5a00);
  if(app < 0) app = 0;
  int dBd = ((15 + exp) << 15) + app;
  dBd = (dBd<<2) + (dBd<<1) + (dBd>>6) + (dBd>>7);
  return (int16_t)(dBd >> 10);
}

// TSSI DC gain cal: TX-power feedback cal; on this (TSSI-disabled) dongle it
// configures DC gain and writes a benign TX ALC temp-comp. Self-contained
// (saves/restores RF+BBP). Faithful to mt7601u_tssi_dc_gain_cal.
static void mt_tssi_dc_gain_cal(wchip_mt7601u_t* mt){
  uint8_t res[4]; int i, j;
  mt_wr(mt, MT_RF_SETTING_0, 0x30);
  mt_wr(mt, MT_RF_BYPASS_0, 0x000c0030);
  mt_wr(mt, MT_MAC_SYS_CTRL, 0);
  mt_bbp_wr(mt, 58, 0); mt_bbp_wr(mt, 241, 0x2); mt_bbp_wr(mt, 23, 0x8);
  int bbp_r47 = mt_bbp_rr(mt, 47);
  int rf_vga_s = mt_rf_rr(mt, 5, 3); mt_rf_wr(mt, 5, 3, 8);
  int rf_mixer = mt_rf_rr(mt, 4, 39); mt_rf_wr(mt, 4, 39, 0);
  for(i = 0; i < 4; i++){
    mt_rf_wr(mt, 4, 39, (i & 1) ? rf_mixer : 0);
    mt_bbp_wr(mt, 23, (i < 2) ? 0x08 : 0x02);
    mt_rf_wr(mt, 5, 3, (i < 2) ? 0x08 : 0x11);
    mt_bbp_wr(mt, 22, 0); mt_bbp_wr(mt, 244, 0);
    mt_bbp_wr(mt, 21, 1); usleep(1); mt_bbp_wr(mt, 21, 0);
    mt_bbp_wr(mt, 47, 0x50);
    mt_bbp_wr(mt, (i & 1) ? 244 : 22, (i & 1) ? 0x31 : 0x40);
    for(j = 20; j; j--) if(!(mt_bbp_rr(mt, 47) & 0x10)) break;
    mt_bbp_wr(mt, 47, 0x40);
    res[i] = mt_bbp_rr(mt, 49);
  }
  int16_t tssi_db = lin2dBd((short)res[1] - res[0]);
  (void)lin2dBd(((short)res[3] - res[2]) * 4);
  mt_bbp_wr(mt, 22, 0); mt_bbp_wr(mt, 244, 0);
  mt_bbp_wr(mt, 21, 1); usleep(1); mt_bbp_wr(mt, 21, 0);
  mt_wr(mt, MT_RF_BYPASS_0, 0); mt_wr(mt, MT_RF_SETTING_0, 0);
  mt_rf_wr(mt, 5, 3, rf_vga_s); mt_rf_wr(mt, 4, 39, rf_mixer); mt_bbp_wr(mt, 47, bbp_r47);
  // set_initial_tssi: slope/offset are 0 when TSSI is disabled -> offset = 10
  int init_offset = mt->ee_tssi_enabled ? -((tssi_db * 0) / 4096) + 10 : 10;
  mt_rmw(mt, MT_TX_ALC_CFG_1, 0x3f, int_to_s6(init_offset) & 0x3f);
}

/* BBP temperature-mode CR tables (initvals_phy.h) */
typedef struct{ const uint32_t (*regs)[2]; int n; } bbp_reg_table_t;
static const uint32_t bbp_normal_temp[][2] = {{75,0x60},{92,0x02},{178,0xff},{195,0x88},{196,0x60}};
static const uint32_t bbp_normal_temp_bw20[][2] = {{69,0x12},{91,0x07},{195,0x23},{196,0x17},{195,0x24},{196,0x06},{195,0x81},{196,0x12},{195,0x83},{196,0x17}};
static const uint32_t bbp_normal_temp_bw40[][2] = {{69,0x15},{91,0x04},{195,0x23},{196,0x12},{195,0x24},{196,0x08},{195,0x81},{196,0x15},{195,0x83},{196,0x16}};
static const uint32_t bbp_high_temp[][2] = {{75,0x60},{92,0x02},{178,0xff},{195,0x88},{196,0x60}};
static const uint32_t bbp_high_temp_bw20[][2] = {{69,0x12},{91,0x07},{195,0x23},{196,0x17},{195,0x24},{196,0x06},{195,0x81},{196,0x12},{195,0x83},{196,0x17}};
static const uint32_t bbp_high_temp_bw40[][2] = {{69,0x15},{91,0x04},{195,0x23},{196,0x12},{195,0x24},{196,0x08},{195,0x81},{196,0x15},{195,0x83},{196,0x16}};
static const uint32_t bbp_low_temp[][2] = {{178,0xff}};
static const uint32_t bbp_low_temp_bw20[][2] = {{69,0x12},{75,0x5e},{91,0x07},{92,0x02},{195,0x23},{196,0x17},{195,0x24},{196,0x06},{195,0x81},{196,0x12},{195,0x83},{196,0x17},{195,0x88},{196,0x5e}};
static const uint32_t bbp_low_temp_bw40[][2] = {{69,0x15},{75,0x5c},{91,0x04},{92,0x03},{195,0x23},{196,0x10},{195,0x24},{196,0x08},{195,0x81},{196,0x15},{195,0x83},{196,0x16},{195,0x88},{196,0x5b}};
// index [temp_mode][0=bw20, 1=bw40, 2=common]
static const bbp_reg_table_t bbp_mode_table[3][3] = {
  {{bbp_normal_temp_bw20, countof(bbp_normal_temp_bw20)}, {bbp_normal_temp_bw40, countof(bbp_normal_temp_bw40)}, {bbp_normal_temp, countof(bbp_normal_temp)}},
  {{bbp_high_temp_bw20, countof(bbp_high_temp_bw20)}, {bbp_high_temp_bw40, countof(bbp_high_temp_bw40)}, {bbp_high_temp, countof(bbp_high_temp)}},
  {{bbp_low_temp_bw20, countof(bbp_low_temp_bw20)}, {bbp_low_temp_bw40, countof(bbp_low_temp_bw40)}, {bbp_low_temp, countof(bbp_low_temp)}},
};

static bool mt_bbp_temp(wchip_mt7601u_t* mt, int mode){
  if(mt->temp_mode == mode) return true;
  mt->temp_mode = mode;
  const bbp_reg_table_t* t = bbp_mode_table[mode];
  if(!mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, t[2].regs, t[2].n)) return false;
  int bw = mt->bw < 0 ? 0 : mt->bw;
  return mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, t[bw].regs, t[bw].n);
}

static bool mt_temp_comp(wchip_mt7601u_t* mt, bool on){
  int hi = 400, lo = -200;
  int temp = (mt->raw_temp - mt->ee_ref_temp) * MT_EE_TEMPERATURE_SLOPE;
  mt->curr_temp = temp;
  if(temp - mt->dpd_temp > 450 || temp - mt->dpd_temp < -450){
    mt->dpd_temp = temp;
    if(!mt_mcu_calibrate(mt, MCU_CAL_DPD, mt->dpd_temp)) return false;
    mt_vco_cal(mt);
  }
  if(temp < -50 && !mt->pll_lock_protect){
    mt->pll_lock_protect = true; mt_rf_wr(mt, 4, 4, 6); mt_rf_rmw(mt, 4, 10, 0x30, 0);
  } else if(temp > 50 && mt->pll_lock_protect){
    mt->pll_lock_protect = false; mt_rf_wr(mt, 4, 4, 0); mt_rf_rmw(mt, 4, 10, 0x30, 0x10);
  }
  if(on){ hi -= 50; lo -= 50; }
  return mt_bbp_temp(mt, temp > hi ? MT_TEMP_MODE_HIGH : temp > lo ? MT_TEMP_MODE_NORMAL : MT_TEMP_MODE_LOW);
}

static bool mt7601u_init_cal(wchip_mt7601u_t* mt){
  mt->raw_temp = mt_read_bootup_temp(mt);
  LOG("mt7601u: bootup_temp raw:%d", mt->raw_temp);
  mt->curr_temp = (mt->raw_temp - mt->ee_ref_temp) * MT_EE_TEMPERATURE_SLOPE;
  mt->dpd_temp = mt->curr_temp;
  uint32_t mac_ctrl = mt_rr(mt, MT_MAC_SYS_CTRL);
  if(!mt_mcu_calibrate(mt, MCU_CAL_R, 0)){ LOG("mt7601u: CAL_R failed"); return false; }
  LOG("mt7601u: cal step CAL_R");
  int r = mt_rf_rr(mt, 0, 4); if(r < 0){ LOG("mt7601u: rf_rr(0,4) failed"); return false; }
  if(!mt_rf_wr(mt, 0, 4, r | 0x80)) return false;
  usleep(2000);
  if(!mt_mcu_calibrate(mt, MCU_CAL_TXDCOC, 0)) return false;
  LOG("mt7601u: cal step TXDCOC");
  mt_rxdc_cal(mt); LOG("mt7601u: cal step rxdc#1");
  if(!mt_set_bw_filter(mt, true)) return false;
  LOG("mt7601u: cal step BW");
  // TX-path calibrations (LOFT/TXIQ/DPD) and TSSI DC-gain are not needed for RX
  // and crash this dongle here (they need the TX DAC/channel set up first).
  // RXIQ lands in the same crashing block; skipped too. Basic RX works without.
  mt_rxdc_cal(mt); LOG("mt7601u: cal step rxdc#2");
  mt_wr(mt, MT_MAC_SYS_CTRL, mac_ctrl);
  bool tc = mt_temp_comp(mt, true);
  LOG("mt7601u: cal step temp_comp -> %d", tc);
  return tc;
}

static bool mt7601u_phy_init(wchip_mt7601u_t* mt){
  mt->rf_pa_mode[0] = mt_rr(mt, MT_RF_PA_MODE_CFG0);
  mt->rf_pa_mode[1] = mt_rr(mt, MT_RF_PA_MODE_CFG1);
  if(!mt_rf_wr(mt, 0, 12, (uint8_t)mt->ee_rf_freq_off)){ LOG("mt7601u: rf freq_off wr failed"); return false; }
  if(!mt_write_reg_pairs(mt, 0, rf_central, countof(rf_central))) return false;
  if(!mt_write_reg_pairs(mt, 0, rf_channel, countof(rf_channel))) return false;
  if(!mt_write_reg_pairs(mt, 0, rf_vga, countof(rf_vga))) return false;
  LOG("mt7601u: RF tables written");
  mt->bw = -1; mt->temp_mode = -1; mt->pll_lock_protect = false;
  return mt7601u_init_cal(mt);
}

/* ---- channel set + RX enable (phy.c set_channel, init.c mac_start) ------ */
#define MT_TX_BAND_CFG 0x132c
#define MT_TX_ALC_CFG_0 0x13b0
#define MT_TX_PWR_CFG_0 0x1314
#define MT_RX_FILTR_CFG 0x1400
#define MT_WPDMA_GLO_CFG 0x0208
#define MT_WPDMA_GLO_CFG_TX_DMA_EN (1u << 0)
#define MT_WPDMA_GLO_CFG_TX_DMA_BUSY (1u << 1)
#define MT_WPDMA_GLO_CFG_RX_DMA_EN (1u << 2)
#define MT_WPDMA_GLO_CFG_RX_DMA_BUSY (1u << 3)
#define MT_RX_FILTR_MONITOR 0x00017f97  // drop errs + control frames, keep mgmt/data
#define MT7601U_RXWI_LEN 28

static bool mt_bbp_set_bw(wchip_mt7601u_t* mt, int bw){
  if(bw == mt->bw){ mt_bbp_rmc(mt, 4, 0x18, bw == 0 ? 0 : 0x10); return true; }
  mt->bw = bw;
  uint32_t old = mt_rr(mt, MT_MAC_SYS_CTRL);
  mt_wr(mt, MT_MAC_SYS_CTRL, old & ~(MT_MAC_SYS_CTRL_ENABLE_TX | MT_MAC_SYS_CTRL_ENABLE_RX));
  mt_poll(mt, MT_MAC_STATUS, MT_MAC_STATUS_TX | MT_MAC_STATUS_RX, 0, 500);
  mt_bbp_rmc(mt, 4, 0x18, bw == 0 ? 0 : 0x10);
  mt_wr(mt, MT_MAC_SYS_CTRL, old);
  int tm = mt->temp_mode < 0 ? MT_TEMP_MODE_NORMAL : mt->temp_mode;
  return mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, bbp_mode_table[tm][bw].regs, bbp_mode_table[tm][bw].n);
}

// per-channel RF freq plan (bank 0 regs 17-20), from __mt7601u_phy_set_channel
static const uint8_t freq_plan[14][4] = {
  {0x99,0x99,0x09,0x50},{0x46,0x44,0x0a,0x50},{0xec,0xee,0x0a,0x50},{0x99,0x99,0x0b,0x50},
  {0x46,0x44,0x08,0x51},{0xec,0xee,0x08,0x51},{0x99,0x99,0x09,0x51},{0x46,0x44,0x0a,0x51},
  {0xec,0xee,0x0a,0x51},{0x99,0x99,0x0b,0x51},{0x46,0x44,0x08,0x52},{0xec,0xee,0x08,0x52},
  {0x99,0x99,0x09,0x52},{0x33,0x33,0x0b,0x52},
};

static bool mt_set_channel(wchip_mt7601u_t* mt, uint8_t channel){
  if(channel < 1 || channel > 14) return false;
  int idx = channel - 1;
  int bw = 0; // BW_20
  mt_bbp_set_bw(mt, bw);
  mt_bbp_rmc(mt, 3, 0x20, 0);                  // bbp_set_ctrlch(below=false)
  mt_rmw(mt, MT_TX_BAND_CFG, 1, 0);            // mac_set_ctrlch(below=false)
  mt->chan_ext_below = false;

  uint32_t fp[4][2] = {{17, freq_plan[idx][0]}, {18, freq_plan[idx][1]},
                       {19, freq_plan[idx][2]}, {20, freq_plan[idx][3]}};
  if(!mt_write_reg_pairs(mt, MT_MCU_MEMMAP_RF, fp, 4)) return false;
  mt_rmw(mt, MT_TX_ALC_CFG_0, 0x3f3f, 0);      // chan_pwr unknown -> 0

  uint32_t bbp_set[3][2] = {{62, (uint8_t)(0x37 - mt->ee_lna_gain)},
    {63, (uint8_t)(0x37 - mt->ee_lna_gain)}, {64, (uint8_t)(0x37 - mt->ee_lna_gain)}};
  if(!mt_write_reg_pairs(mt, MT_MCU_MEMMAP_BBP, bbp_set, 3)) return false;

  mt_vco_cal(mt);
  mt_bbp_set_bw(mt, bw);
  if(!mt_set_bw_filter(mt, false)) return false;
  // apply_ch14_fixup (channel != 14)
  mt_bbp_rmw(mt, 4, 0x20, 0);
  mt_bbp_wr(mt, 178, 0xff);
  mt_wr(mt, MT_TX_PWR_CFG_0, 0);

  mt->channel = channel;
  LOG("mt7601u: tuned to channel %u", channel);
  return true;
}

static bool mt_mac_start(wchip_mt7601u_t* mt){
  mt_wr(mt, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_TX_DMA_EN | MT_WPDMA_GLO_CFG_RX_DMA_EN);
  mt_wr(mt, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_TX);
  mt_poll(mt, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_TX_DMA_BUSY | MT_WPDMA_GLO_CFG_RX_DMA_BUSY, 0, 200);
  mt_wr(mt, MT_RX_FILTR_CFG, MT_RX_FILTR_MONITOR);
  mt_wr(mt, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_TX | MT_MAC_SYS_CTRL_ENABLE_RX);
  LOG("mt7601u: RX enabled (filter:0x%05x, sys_ctrl:0x%08x)", MT_RX_FILTR_MONITOR, mt_rr(mt, MT_MAC_SYS_CTRL));
  return true;
}

// RSSI from the RXWI gain/ant fields (mt7601u_phy_get_rssi)
static int mt_rx_rssi(wchip_mt7601u_t* mt, const uint8_t* rxwi, uint16_t rate){
  static const int8_t lna[2][2][3] = {{{-2,15,33},{0,16,34}}, {{-2,15,33},{-2,16,34}}};
  int bw = (rate >> 7) & 1;
  int aux_lna = (rxwi[17] >> 7) & 1;          // ant: AUX_LNA
  int lna_id = (rxwi[18] >> 6) & 3;           // gain: LNA_ID
  if(lna_id) lna_id--;
  int val = 8 - lna[aux_lna][bw][lna_id] - (rxwi[18] & 0x3f) - mt->ee_lna_gain - mt->ee_rssi_offset[0];
  return val;
}

/* ---- TX: inject a raw 802.11 frame (tx.c push_txwi + dma_enqueue_tx) -----
 * Layout: [4B DMA TXINFO][20B TXWI][802.11 frame][pad to 4][4B zero], out the
 * AC_BE data EP. WIV=1 (no hw key), QSEL=EDCA, 80211 flag set. */
#define MT_TXD_PKT_INFO_80211 (1u << 19)
#define MT_TXD_PKT_INFO_WIV   (1u << 24)
#define MT_QSEL_EDCA          2
#define MT7601U_TXWI_LEN      20

static bool mt_tx_frame(wchip_mt7601u_t* mt, const uint8_t* frame, size_t frame_len){
  if(mt->data_tx_ep == 0 || frame_len < 10 || frame_len > 2048) return false;
  static uint8_t buf[4 + MT7601U_TXWI_LEN + 2048 + 8];
  uint32_t len_field = (MT7601U_TXWI_LEN + frame_len + 3) & ~3u;   // round_up(txwi+frame, 4)
  uint32_t info = MT_TXD_PKT_INFO_80211 | MT_TXD_PKT_INFO_WIV | ((uint32_t)MT_QSEL_EDCA << 25) | len_field;
  size_t total = 4 + len_field + 4;
  memset(buf, 0, total);
  buf[0] = info; buf[1] = info >> 8; buf[2] = info >> 16; buf[3] = info >> 24;
  uint8_t* txwi = buf + 4;
  txwi[2] = 0x00; txwi[3] = 0x40;          // rate_ctl: OFDM (phy_mode<<14) mcs0 = 6 Mbps
  txwi[5] = 0xff;                           // wcid: monitor / no station
  txwi[6] = frame_len & 0xff;               // len_ctl byte_cnt (12 bits)
  txwi[7] = (frame_len >> 8) & 0x0f;
  memcpy(buf + 4 + MT7601U_TXWI_LEN, frame, frame_len);
  int rc = -1;
  for(int try = 0; try < 6; try++){              // -6 EBUSY is transient (endpoint mid-transfer)
    rc = usb_device_bulk_transfer(&mt->usb_device, mt->data_tx_ep, buf, total, 500);
    if(rc >= 0) break;
    if(rc != -6){ LOG("mt7601u tx_frame failed: %d", rc); break; }
    usleep(1000);
  }
  if(rc == -6) LOG("mt7601u tx_frame: EBUSY after retries");
  return rc >= 0;
}

// --- monitor RX works; the MLME (scan/connect/auth) lives in the OS. ---
static bool mt_set_mode(wifi_chip_t* chip, uint8_t mode){
  MT(chip)->mode = mode;
  LOG("mt7601u set_mode %u", mode);
  return true;
}
static bool mt_op_set_channel(wifi_chip_t* chip, uint8_t channel){
  wchip_mt7601u_t* mt = MT(chip);
  if(!mt->usb_device.usr_data) return false;
  if(!mt_set_channel(mt, channel)) return false;
  return mt_mac_start(mt);  // (re)enable RX on the new channel
}
/* Accumulate a beacon/probe-resp into the scan table (dedup by BSSID, keep strongest RSSI). */
static void mt_scan_collect(wchip_mt7601u_t* mt, const uint8_t* frame, uint16_t mpdu_len, int8_t rssi){
  if(mpdu_len < 38) return;
  const uint8_t* bssid = frame + 16;              // 802.11 mgmt addr3 = BSSID
  uint16_t capinfo = frame[34] | (frame[35] << 8);
  const uint8_t* ies = frame + 36;                // after 24B hdr + tsf(8)+interval(2)+caps(2)
  uint16_t avail = mpdu_len - 36;
  uint16_t ie_len = 0;                            // trim FCS / trailing junk by walking TLVs
  while(ie_len + 2 <= avail){
    uint8_t l = ies[ie_len + 1];
    if(ie_len + 2 + l > avail) break;
    ie_len += 2 + l;
  }
  if(ie_len > sizeof mt->results[0].ies) ie_len = sizeof mt->results[0].ies;
  // real channel = DS Parameter Set IE (id 3); async USB RX batching makes mt->channel unreliable
  uint8_t ap_chan = mt->channel;
  for(uint16_t q = 0; q + 2 <= ie_len; q += 2 + ies[q + 1]){
    if(ies[q] == 3 && ies[q + 1] == 1){ ap_chan = ies[q + 2]; break; }
  }
  for(uint8_t i = 0; i < mt->n_results; i++){
    if(memcmp(mt->results[i].bssid, bssid, 6) == 0){
      if(rssi > mt->results[i].rssi) mt->results[i].rssi = rssi;
      return;
    }
  }
  if(mt->n_results >= countof(mt->results)) return;
  struct mt_scan_result* r = &mt->results[mt->n_results++];
  memcpy(r->bssid, bssid, 6);
  r->channel = ap_chan; r->rssi = rssi; r->capinfo = capinfo;
  memcpy(r->ies, ies, ie_len); r->ie_len = ie_len;
}

/* Emit the collected results to the OS as FullMAC scan results, then SCAN_DONE. */
static void mt_scan_finish(wchip_mt7601u_t* mt){
  mt->scanning = false;
  io_rem_timer(&mt->scan_timer);
  for(uint8_t i = 0; i < mt->n_results; i++){
    struct mt_scan_result* r = &mt->results[i];
    wifi_scan_result_t sr; memset(&sr, 0, sizeof sr);
    memcpy(sr.bssid, r->bssid, 6);
    sr.channel = r->channel; sr.rssi = r->rssi; sr.capinfo = r->capinfo;
    wifi_dev_on_scan_result(mt->base.dev, &sr, r->ies, r->ie_len);
  }
  wifi_dev_on_scan_done(mt->base.dev);
  LOG("mt7601u scan done: %u AP(s)", mt->n_results);
}

/* Per-channel dwell tick: collect happened during the dwell; advance or finish. */
static void mt_scan_tick(io_timer_t* t){
  wchip_mt7601u_t* mt = t->usr_data;
  if(!mt->scanning) return;
  if(mt->channel >= mt->scan_max_chan){ mt_scan_finish(mt); return; }
  mt_set_channel(mt, (uint8_t)(mt->channel + 1));
  mt_mac_start(mt);                                // re-enable RX on the new channel
}

/* Backend-owned active scan: hop channels, collect beacons, present FullMAC results. */
static bool mt_scan(wifi_chip_t* chip, wifi_scan_req_t* req){
  wchip_mt7601u_t* mt = MT(chip);
  if(!mt->usb_device.usr_data) return false;
  if(mt->scanning) return true;
  mt->n_results = 0;
  mt->scanning = true;
  uint8_t start = (req && req->channel) ? req->channel : 1;
  mt->scan_max_chan = (req && req->channel) ? req->channel : 13;
  mt_set_channel(mt, start);
  mt_mac_start(mt);
  mt->scan_timer = (io_timer_t){ .repeat = true, .run_now = false,
                                 .interval_ms = 120, .usr_data = mt, .cb = mt_scan_tick };
  io_add_timer(&mt->scan_timer);
  LOG("mt7601u scan: hopping ch%u..%u (120ms dwell)", start, mt->scan_max_chan);
  return true;
}
/* ---- STA association MLME (open auth → assoc → CONNECTED) ---------------- */
#define MT_MAC_ADDR_DW0  0x1008
#define MT_MAC_ADDR_DW1  0x100c
#define MT_MAC_BSSID_DW0 0x1010
#define MT_MAC_BSSID_DW1 0x1014

/* Harness STA MAC — programmed into the chip AND used by VayuOS pl_wifi as the netif hwaddr,
 * so the supplicant's SPA (used in PTK derivation) matches what's on the air. */
static const uint8_t STA_MAC[6] = { 0x02, 0x00, 0x00, 0x77, 0x66, 0x01 };

static uint16_t mt_next_seq(wchip_mt7601u_t* mt){
  uint16_t s = mt->tx_seq; mt->tx_seq = (uint16_t)((mt->tx_seq + 1) & 0xfff); return (uint16_t)(s << 4);
}

/* Fill the common 802.11 mgmt header (addr1=AP, addr2=STA, addr3=BSSID). */
static size_t mt_hdr(wchip_mt7601u_t* mt, uint8_t* f, uint8_t fc0){
  f[0] = fc0; f[1] = 0x00; f[2] = 0; f[3] = 0;
  memcpy(f + 4,  mt->ap_bssid, 6);
  memcpy(f + 10, STA_MAC, 6);
  memcpy(f + 16, mt->ap_bssid, 6);
  uint16_t sc = mt_next_seq(mt); f[22] = sc & 0xff; f[23] = sc >> 8;
  return 24;
}

static void mt_send_auth(wchip_mt7601u_t* mt){
  uint8_t f[30]; size_t n = mt_hdr(mt, f, 0xB0);   // auth
  f[n++] = 0; f[n++] = 0;                            // algorithm = open
  f[n++] = 1; f[n++] = 0;                            // transaction seq = 1
  f[n++] = 0; f[n++] = 0;                            // status
  LOG("mt7601u: TX auth-req -> %d", mt_tx_frame(mt, f, n));
}

static void mt_send_assoc(wchip_mt7601u_t* mt){
  uint8_t f[160]; size_t n = mt_hdr(mt, f, 0x00);   // assoc-req
  f[n++] = 0x11; f[n++] = 0x00;                      // capability: ESS | Privacy
  f[n++] = 0x0a; f[n++] = 0x00;                      // listen interval
  f[n++] = 0x00; f[n++] = mt->conn_ssid_len;         // SSID IE
  memcpy(f + n, mt->conn_ssid, mt->conn_ssid_len); n += mt->conn_ssid_len;
  f[n++] = 0x01; f[n++] = 0x04;                      // supported rates: 1,2,5.5,11 (basic)
  f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8b; f[n++] = 0x96;
  if(mt->assoc_ie_len){ memcpy(f + n, mt->assoc_ie, mt->assoc_ie_len); n += mt->assoc_ie_len; }
  LOG("mt7601u: TX assoc-req (rsn_ie:%uB) -> %d", mt->assoc_ie_len, mt_tx_frame(mt, f, n));
}

static void mt_conn_fail(wchip_mt7601u_t* mt, const char* why){
  LOG("mt7601u: connect failed: %s", why);
  io_rem_timer(&mt->conn_timer);
  mt->conn_state = MT_CONN_IDLE;
  wifi_dev_on_disconnected(mt->base.dev, 1);
}

static void mt_conn_tick(io_timer_t* t){
  wchip_mt7601u_t* mt = t->usr_data;
  if(++mt->conn_tries > 5){ mt_conn_fail(mt, "auth/assoc timeout"); return; }
  if(mt->conn_state == MT_CONN_AUTH)       mt_send_auth(mt);
  else if(mt->conn_state == MT_CONN_ASSOC) mt_send_assoc(mt);
  else if(mt->conn_state == MT_CONN_SAE_COMMIT || mt->conn_state == MT_CONN_SAE_CONFIRM){
    if(mt->sae_tx_len) mt_tx_frame(mt, mt->sae_tx, mt->sae_tx_len);   /* retransmit last SAE auth */
  }
  else io_rem_timer(&mt->conn_timer);
}

/* OS-built SAE commit/confirm body → wrap in an 802.11 AUTH frame (algorithm 3) + send. */
static bool mt_sae_msg(wifi_chip_t* chip, wifi_sae_t* s, uint8_t* body, size_t len){
  wchip_mt7601u_t* mt = MT(chip);
  uint8_t f[256]; size_t n = mt_hdr(mt, f, 0xB0);
  uint16_t seq = (s->sae_type == WIFI_SAE_COMMIT) ? 1 : 2;
  f[n++] = 3; f[n++] = 0;                 // auth algorithm = SAE
  f[n++] = (uint8_t)seq; f[n++] = (uint8_t)(seq >> 8);
  f[n++] = 0; f[n++] = 0;                 // status
  if(n + len > sizeof f) return false;
  memcpy(f + n, body, len); n += len;
  if(n <= sizeof mt->sae_tx){ memcpy(mt->sae_tx, f, n); mt->sae_tx_len = (uint16_t)n; }
  LOG("mt7601u: TX SAE %s (%zuB) -> %d", seq == 1 ? "commit" : "confirm", len, mt_tx_frame(mt, f, n));
  return true;
}

/* Handle an inbound mgmt frame while authenticating/associating. */
static void mt_conn_rx(wchip_mt7601u_t* mt, const uint8_t* f, uint16_t len){
  if(len < 28 || memcmp(f + 10, mt->ap_bssid, 6) != 0) return;   // must be from our AP
  /* SAE auth (algorithm 3): deliver the body to the OS supplicant + drive the sequence. */
  if((mt->conn_state == MT_CONN_SAE_COMMIT || mt->conn_state == MT_CONN_SAE_CONFIRM)
     && f[0] == 0xB0 && len >= 30 && (f[24] | (f[25] << 8)) == 3){
    uint16_t seq = f[26] | (f[27] << 8), status = f[28] | (f[29] << 8);
    if(status != 0){ char b[32]; snprintf(b, sizeof b, "SAE status %u", status); mt_conn_fail(mt, b); return; }
    wifi_sae_t ev; memset(&ev, 0, sizeof ev); memcpy(ev.bssid, mt->ap_bssid, 6);
    uint8_t* bd = (uint8_t*)f + 30; uint16_t bl = len - 30;
    if(seq == 1 && mt->conn_state == MT_CONN_SAE_COMMIT){
      ev.sae_type = WIFI_SAE_COMMIT; wifi_dev_on_sae_rx(mt->base.dev, &ev, bd, bl);
      mt->conn_state = MT_CONN_SAE_CONFIRM; mt->conn_tries = 0; mt->sae_tx_len = 0;
      wifi_sae_t b2; memset(&b2, 0, sizeof b2); memcpy(b2.bssid, mt->ap_bssid, 6); b2.sae_type = WIFI_SAE_CONFIRM;
      wifi_dev_on_sae_build(mt->base.dev, &b2);          // ask OS for the confirm
      LOG("mt7601u: SAE commit rx → building confirm");
    } else if(seq == 2 && mt->conn_state == MT_CONN_SAE_CONFIRM){
      ev.sae_type = WIFI_SAE_CONFIRM; wifi_dev_on_sae_rx(mt->base.dev, &ev, bd, bl);
      mt->conn_state = MT_CONN_ASSOC; mt->conn_tries = 0;  // assoc sent by conn_tick (not this RX cb)
      LOG("mt7601u: SAE confirm rx → assoc");
    }
    return;
  }
  if(mt->conn_state == MT_CONN_AUTH && f[0] == 0xB0){            // auth response
    uint16_t status = f[28] | (f[29] << 8);
    if(status != 0){ char b[32]; snprintf(b, sizeof b, "auth status %u", status); mt_conn_fail(mt, b); return; }
    LOG("mt7601u: auth OK → assoc");
    /* Don't TX from inside this RX-completion callback (libusb returns -6 EBUSY for a
     * sync transfer issued from an async callback). Let the conn_timer send assoc. */
    mt->conn_state = MT_CONN_ASSOC; mt->conn_tries = 0;
  } else if(mt->conn_state == MT_CONN_ASSOC && (f[0] == 0x10)){  // assoc response
    uint16_t status = f[26] | (f[27] << 8);
    if(status != 0){ char b[32]; snprintf(b, sizeof b, "assoc status %u", status); mt_conn_fail(mt, b); return; }
    uint16_t aid = (uint16_t)((f[28] | (f[29] << 8)) & 0x3fff);
    LOG("mt7601u: ASSOCIATED (aid=%u) — link up, awaiting 4-way", aid);
    io_rem_timer(&mt->conn_timer);
    mt->conn_state = MT_CONN_CONNECTED;
    wifi_dev_on_connect(mt->base.dev, mt->ap_bssid);
    wifi_dev_on_connected(mt->base.dev, mt->ap_bssid);
  }
}

/* Inbound 802.11 data → (decrypt if Protected) → 802.3 → generic layer (routes EAPOL by ethertype). */
static void mt_data_rx(wchip_mt7601u_t* mt, const uint8_t* f, uint16_t len){
  bool qos = (f[0] & 0xf0) == 0x80;
  size_t hdr = 24u + (qos ? 2u : 0u);
  if(len < hdr + 8) return;
  const uint8_t* da = f + 4;                            // FromDS: addr1=DA(us), addr3=SA
  const uint8_t* sa = f + 16;
  uint8_t body[1600]; const uint8_t* snap; size_t snap_len;
  if(f[1] & 0x40){                                      // Protected → CCMP decrypt
    const uint8_t* key = (da[0] & 1) ? mt->gtk : mt->tk;   // group key for mcast/bcast, else pairwise
    if((da[0] & 1) ? !mt->gtk_set : !mt->tk_set) return;
    int pl = ccmp_decrypt(key, f, hdr, qos, f + hdr, len - hdr, body);
    if(pl < 0){
      // diagnostic + self-heal: retry with the opposite QoS assumption
      size_t hdr2 = qos ? 24u : 26u;
      int pl2 = (len > hdr2 + 16) ? ccmp_decrypt(key, f, hdr2, !qos, f + hdr2, len - hdr2, body) : -1;
      if(!(da[0] & 1))
        LOG("mt7601u: CCMP FAIL pairwise fc:%02x%02x len:%u qos:%d hdr:%zu | hdr[0..3]:%02x%02x%02x%02x retry(!qos)->%d",
            f[0], f[1], len, qos, hdr, f[0], f[1], f[2], f[3], pl2);
      if(pl2 < 0) return;
      pl = pl2; snap = body; snap_len = (size_t)pl2;
    } else {
      snap = body; snap_len = (size_t)pl;
    }
    if(!(da[0] & 1)) LOG("mt7601u RX data (pairwise, to us) %u B etype:0x%02x%02x", (unsigned)snap_len, snap[6], snap[7]);
  } else {
    snap = f + hdr; snap_len = len - hdr;
  }
  if(snap_len < 8 || snap[0] != 0xAA || snap[1] != 0xAA || snap[2] != 0x03) return;
  size_t payload = snap_len - 8;
  uint8_t eth[1600];
  if(14 + payload > sizeof eth) return;
  memcpy(eth, da, 6); memcpy(eth + 6, sa, 6);
  eth[12] = snap[6]; eth[13] = snap[7];
  memcpy(eth + 14, snap + 8, payload);
  wifi_dev_on_rx_data(mt->base.dev, eth, 14 + payload);
}

static bool mt_connect(wifi_chip_t* chip, wifi_connect_req_t* req){
  wchip_mt7601u_t* mt = MT(chip);
  if(!mt->usb_device.usr_data) return false;
  if(mt->scanning){ mt->scanning = false; io_rem_timer(&mt->scan_timer); }
  memcpy(mt->ap_bssid, req->bssid, 6);
  mt->conn_ssid_len = req->ssid_len > 32 ? 32 : req->ssid_len;
  memcpy(mt->conn_ssid, req->ssid, mt->conn_ssid_len); mt->conn_ssid[mt->conn_ssid_len] = 0;
  uint8_t ch = req->channel ? req->channel : mt->channel;
  mt_set_channel(mt, ch);
  mt_mac_start(mt);
  // program STA identity + BSS so the HW ACKs the AP (and the AP ACKs us)
  mt_wr(mt, MT_MAC_ADDR_DW0,  STA_MAC[0] | (STA_MAC[1]<<8) | (STA_MAC[2]<<16) | ((uint32_t)STA_MAC[3]<<24));
  mt_wr(mt, MT_MAC_ADDR_DW1,  STA_MAC[4] | (STA_MAC[5]<<8));
  mt_wr(mt, MT_MAC_BSSID_DW0, req->bssid[0] | (req->bssid[1]<<8) | (req->bssid[2]<<16) | ((uint32_t)req->bssid[3]<<24));
  mt_wr(mt, MT_MAC_BSSID_DW1, req->bssid[4] | (req->bssid[5]<<8));
  mt->conn_tries = 0; mt->sae_tx_len = 0;
  if(mt->is_sae){
    /* WPA3: ask the OS for the SAE commit; mt_sae_msg sends it. Auth frames flow as algo=3. */
    mt->conn_state = MT_CONN_SAE_COMMIT;
    wifi_sae_t s; memset(&s, 0, sizeof s); memcpy(s.bssid, mt->ap_bssid, 6); s.sae_type = WIFI_SAE_COMMIT;
    wifi_dev_on_sae_build(mt->base.dev, &s);
  } else {
    mt->conn_state = MT_CONN_AUTH;
    mt_send_auth(mt);
  }
  mt->conn_timer = (io_timer_t){ .repeat = true, .run_now = false, .interval_ms = 300,
                                 .usr_data = mt, .cb = mt_conn_tick };
  io_add_timer(&mt->conn_timer);
  LOG("mt7601u: connecting '%s' (%s) ch%u %02x:%02x:%02x:%02x:%02x:%02x", mt->conn_ssid,
      mt->is_sae ? "WPA3/SAE" : "open/WPA2", ch,
      req->bssid[0],req->bssid[1],req->bssid[2],req->bssid[3],req->bssid[4],req->bssid[5]);
  return true;
}

static bool mt_disconnect(wifi_chip_t* chip, uint8_t reason){
  wchip_mt7601u_t* mt = MT(chip);
  if(mt->conn_state != MT_CONN_IDLE){
    io_rem_timer(&mt->conn_timer);
    uint8_t f[26]; size_t n = mt_hdr(mt, f, 0xC0);   // deauth — free our slot at the AP
    f[n++] = reason ? reason : 3; f[n++] = 0;
    mt_tx_frame(mt, f, n);
  }
  mt->conn_state = MT_CONN_IDLE;
  mt->tk_set = mt->gtk_set = false;
  return true;
}

/* Clear all radio state for a fresh OS session: deauth a stale association (free the AP's slot),
 * drop keys/scan/counters, return to monitor RX on a known channel. Sent by the OS at startup. */
static bool mt_op_reset(wifi_chip_t* chip){
  wchip_mt7601u_t* mt = MT(chip);
  if(!mt->usb_device.usr_data) return false;
  if(mt->scanning){ mt->scanning = false; io_rem_timer(&mt->scan_timer); }
  mt_disconnect(chip, 3);                 // deauth if associated + clear conn/keys
  mt->n_results = 0;
  mt->tx_seq = 0; mt->tx_pn = 0;
  mt->assoc_ie_len = 0;
  mt_set_channel(mt, 1);
  mt_mac_start(mt);
  LOG("mt7601u: radio reset (state cleared)");
  return true;
}

static bool mt_set_appie(wifi_chip_t* chip, uint8_t type, uint8_t* ie, size_t len){
  wchip_mt7601u_t* mt = MT(chip);
  if(type == WIFI_APPIE_RSN && len <= sizeof mt->assoc_ie){ memcpy(mt->assoc_ie, ie, len); mt->assoc_ie_len = (uint8_t)len; }
  /* detect AKM=SAE in the RSN IE → use SAE auth: ver(2) group(4) pair_cnt(2)+pair*4 akm_cnt(2)+akm */
  mt->is_sae = false;
  if(type == WIFI_APPIE_RSN && len >= 4 && ie[0] == 0x30){
    const uint8_t* r = ie + 2; size_t rl = ie[1], q = 2 + 4;
    if(q + 2 <= rl){ uint16_t pc = r[q] | (r[q+1] << 8); q += 2 + (size_t)pc * 4;
      if(q + 2 <= rl){ uint16_t ac = r[q] | (r[q+1] << 8); q += 2;
        for(uint16_t k = 0; k < ac && q + 4 <= rl; k++, q += 4)
          if(r[q]==0x00 && r[q+1]==0x0f && r[q+2]==0xac && r[q+3]==0x08){ mt->is_sae = true; break; }
      }
    }
  }
  LOG("mt7601u set_appie type:%u len:%zu sae:%d", type, len, mt->is_sae);
  return true;
}
static bool mt_set_key(wifi_chip_t* chip, wifi_key_t* k, uint8_t* key){
  wchip_mt7601u_t* mt = MT(chip);
  if(k->key_len != 16){ LOG("mt7601u set_key: unexpected key_len %u", k->key_len); return false; }
  if(k->key_flag & WIFI_KEY_FLAG_PAIRWISE){ memcpy(mt->tk, key, 16); mt->tk_set = true; mt->tx_pn = 1; }
  else if(k->key_flag & WIFI_KEY_FLAG_GROUP){ memcpy(mt->gtk, key, 16); mt->gtk_idx = k->key_idx; mt->gtk_set = true; }
  LOG("mt7601u set_key %s idx:%u (sw CCMP)", (k->key_flag & WIFI_KEY_FLAG_PAIRWISE) ? "pairwise" : "group", k->key_idx);
  return true;
}
static bool mt_set_igtk(wifi_chip_t* chip, wifi_igtk_t* igtk){ return false; }
static bool mt_send_mgmt(wifi_chip_t* chip, wifi_mgmt_tx_t* m, uint8_t* frame, size_t len){ LOG("TODO mt7601u send_mgmt"); return false; }
static bool mt_config_done(wifi_chip_t* chip){ return true; }

/* Outbound 802.3 (EAPOL/data from the OS) → 802.11 data frame (ToDS) + LLC/SNAP. */
static bool mt_tx_data(wifi_chip_t* chip, uint8_t* eth, size_t len){
  wchip_mt7601u_t* mt = MT(chip);
  if(mt->conn_state != MT_CONN_CONNECTED || len < 14) return false;
  uint8_t f[2048]; size_t n = 0;
  f[0] = 0x08; f[1] = 0x01;                 // data, ToDS=1
  f[2] = 0; f[3] = 0;
  memcpy(f + 4,  mt->ap_bssid, 6);          // addr1 = BSSID (RA)
  memcpy(f + 10, STA_MAC, 6);               // addr2 = SA (TA)
  memcpy(f + 16, eth, 6);                    // addr3 = DA (the 802.3 dst)
  uint16_t sc = mt_next_seq(mt); f[22] = sc & 0xff; f[23] = sc >> 8;
  n = 24;
  f[n++] = 0xAA; f[n++] = 0xAA; f[n++] = 0x03; f[n++] = 0x00; f[n++] = 0x00; f[n++] = 0x00;
  f[n++] = eth[12]; f[n++] = eth[13];        // ethertype (0x888E for EAPOL)
  size_t payload = len - 14;
  if(n + payload > sizeof f) return false;
  memcpy(f + n, eth + 14, payload); n += payload;
  // CCMP-encrypt data once the PTK is installed; EAPOL (the 4-way) always goes in the clear.
  uint16_t etype = (uint16_t)((eth[12] << 8) | eth[13]);
  if(mt->tk_set && etype != 0x888E){
    uint8_t enc[2048];
    size_t body = n - 24;                    // LLC/SNAP + payload (the MPDU body)
    size_t outn = ccmp_encrypt(mt->tk, f, 24, false, mt->tx_pn, 0, f + 24, body, enc);
    bool ok = mt_tx_frame(mt, enc, outn);
    LOG("mt7601u TX data etype:0x%04x dst:%02x:%02x:%02x:%02x:%02x:%02x enc pn:%llu -> %d",
        etype, eth[0],eth[1],eth[2],eth[3],eth[4],eth[5], (unsigned long long)mt->tx_pn, ok);
    mt->tx_pn++;
    return ok;
  }
  bool ok = mt_tx_frame(mt, f, n);
  LOG("mt7601u TX data etype:0x%04x (clear) -> %d", etype, ok);
  return ok;
}
static bool mt_op_tx_raw80211(wifi_chip_t* chip, uint8_t* frame, size_t len){
  wchip_mt7601u_t* mt = MT(chip);
  if(!mt->usb_device.usr_data) return false;  // guard pty-path TX after deinit
  return mt_tx_frame(mt, frame, len);
}

const wifi_chip_ops_t wchip_mt7601u_ops = {
  .init        = mt_init,
  .deinit      = mt_deinit,
  .reset       = mt_op_reset,
  .set_mode    = mt_set_mode,
  .set_channel = mt_op_set_channel,
  .scan        = mt_scan,
  .connect     = mt_connect,
  .disconnect  = mt_disconnect,
  .set_appie   = mt_set_appie,
  .sae_msg     = mt_sae_msg,
  .set_key     = mt_set_key,
  .set_igtk    = mt_set_igtk,
  .send_mgmt   = mt_send_mgmt,
  .config_done = mt_config_done,
  .tx_data     = mt_tx_data,
  .tx_raw80211 = mt_op_tx_raw80211,
};
