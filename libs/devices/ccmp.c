/* Software CCMP (AES-128-CCM) — see ccmp.h. Self-contained AES-128 + CCM (RFC 3610). */
#include "ccmp.h"
#include <string.h>

/* ---- AES-128 (encrypt only; CCM uses the forward cipher for CTR + CBC-MAC) -- */
static const uint8_t SB[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t xt(uint8_t x){ return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void aes128_expand(const uint8_t key[16], uint8_t rk[176]){
  memcpy(rk, key, 16);
  uint8_t rcon = 1;
  for(int i = 16; i < 176; i += 4){
    uint8_t t[4]; memcpy(t, rk + i - 4, 4);
    if(i % 16 == 0){
      uint8_t tmp = t[0]; t[0] = SB[t[1]] ^ rcon; t[1] = SB[t[2]]; t[2] = SB[t[3]]; t[3] = SB[tmp];
      rcon = xt(rcon);
    }
    for(int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
  }
}

static void aes128_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]){
  uint8_t s[16]; memcpy(s, in, 16);
  for(int i = 0; i < 16; i++) s[i] ^= rk[i];
  for(int r = 1; r <= 10; r++){
    uint8_t t[16];
    for(int i = 0; i < 16; i++) t[i] = SB[s[i]];
    // ShiftRows
    uint8_t a[16] = {
      t[0],  t[5],  t[10], t[15],
      t[4],  t[9],  t[14], t[3],
      t[8],  t[13], t[2],  t[7],
      t[12], t[1],  t[6],  t[11] };
    if(r < 10){
      for(int c = 0; c < 4; c++){
        uint8_t* col = a + c * 4;
        uint8_t s0 = col[0], s1 = col[1], s2 = col[2], s3 = col[3];
        col[0] = xt(s0) ^ (xt(s1) ^ s1) ^ s2 ^ s3;
        col[1] = s0 ^ xt(s1) ^ (xt(s2) ^ s2) ^ s3;
        col[2] = s0 ^ s1 ^ xt(s2) ^ (xt(s3) ^ s3);
        col[3] = (xt(s0) ^ s0) ^ s1 ^ s2 ^ xt(s3);
      }
    }
    for(int i = 0; i < 16; i++) s[i] = a[i] ^ rk[r * 16 + i];
  }
  memcpy(out, s, 16);
}

/* ---- CCM (M=8, L=2) ----------------------------------------------------- */
static void xor16(uint8_t* a, const uint8_t* b){ for(int i = 0; i < 16; i++) a[i] ^= b[i]; }

static void ccm_cbc_mac(const uint8_t rk[176], const uint8_t nonce[13],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* msg, size_t msg_len, uint8_t t[16]){
  uint8_t b[16];
  b[0] = 0x59;                                   /* Adata=1, M=8 →24, L=2 →1 */
  memcpy(b + 1, nonce, 13);
  b[14] = (uint8_t)(msg_len >> 8); b[15] = (uint8_t)msg_len;
  aes128_encrypt(rk, b, t);                       /* X1 = E(B0) */
  memset(b, 0, 16);
  b[0] = (uint8_t)(aad_len >> 8); b[1] = (uint8_t)aad_len;
  size_t n = aad_len < 14 ? aad_len : 14;
  memcpy(b + 2, aad, n);
  xor16(t, b); aes128_encrypt(rk, t, t);
  for(size_t off = n; off < aad_len; ){
    memset(b, 0, 16); size_t m = aad_len - off < 16 ? aad_len - off : 16;
    memcpy(b, aad + off, m); xor16(t, b); aes128_encrypt(rk, t, t); off += m;
  }
  for(size_t off = 0; off < msg_len; ){
    memset(b, 0, 16); size_t m = msg_len - off < 16 ? msg_len - off : 16;
    memcpy(b, msg + off, m); xor16(t, b); aes128_encrypt(rk, t, t); off += m;
  }
}

/* CTR: encrypt/decrypt data in place; also returns S0 (for the MIC). */
static void ccm_ctr(const uint8_t rk[176], const uint8_t nonce[13],
                    uint8_t* data, size_t len, uint8_t s0[16]){
  uint8_t a[16], s[16];
  a[0] = 0x01;                                    /* L-1 = 1 */
  memcpy(a + 1, nonce, 13);
  a[14] = 0; a[15] = 0;
  aes128_encrypt(rk, a, s0);
  uint16_t ctr = 1;
  for(size_t off = 0; off < len; ctr++){
    a[14] = (uint8_t)(ctr >> 8); a[15] = (uint8_t)ctr;
    aes128_encrypt(rk, a, s);
    size_t m = len - off < 16 ? len - off : 16;
    for(size_t k = 0; k < m; k++) data[off + k] ^= s[k];
    off += m;
  }
}

static void ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad,
                        size_t aad_len, const uint8_t* msg, size_t msg_len,
                        uint8_t* out, uint8_t mic[8]){
  uint8_t rk[176]; aes128_expand(key, rk);
  uint8_t t[16], s0[16];
  ccm_cbc_mac(rk, nonce, aad, aad_len, msg, msg_len, t);
  memcpy(out, msg, msg_len);
  ccm_ctr(rk, nonce, out, msg_len, s0);
  for(int i = 0; i < 8; i++) mic[i] = t[i] ^ s0[i];
}

static bool ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* aad,
                        size_t aad_len, const uint8_t* ct, size_t ct_len,
                        uint8_t* out, const uint8_t mic[8]){
  uint8_t rk[176]; aes128_expand(key, rk);
  uint8_t t[16], s0[16];
  memcpy(out, ct, ct_len);
  ccm_ctr(rk, nonce, out, ct_len, s0);            /* CTR is symmetric → plaintext */
  ccm_cbc_mac(rk, nonce, aad, aad_len, out, ct_len, t);
  for(int i = 0; i < 8; i++) if((uint8_t)(t[i] ^ s0[i]) != mic[i]) return false;
  return true;
}

/* ---- CCMP framing (nonce + AAD from the 802.11 header) ------------------ */
static void ccmp_na(const uint8_t* hdr, bool qos, bool mgmt, const uint8_t pn6[6],
                    uint8_t nonce[13], uint8_t* aad, size_t* aad_len){
  uint16_t fc = (uint16_t)(hdr[0] | (hdr[1] << 8));
  uint8_t tid = qos ? (uint8_t)(hdr[24] & 0x0f) : 0;
  nonce[0] = (uint8_t)(tid | (mgmt ? 0x10 : 0));  /* priority + Management bit (802.11w) */
  memcpy(nonce + 1, hdr + 10, 6);                 /* A2 */
  nonce[7]=pn6[5]; nonce[8]=pn6[4]; nonce[9]=pn6[3]; nonce[10]=pn6[2]; nonce[11]=pn6[1]; nonce[12]=pn6[0];
  /* mask Retry/PwrMgt/MoreData + set Protected; mask the subtype only for DATA frames */
  uint16_t clr = (uint16_t)(0x3800 | (mgmt ? 0 : 0x0070));
  uint16_t mfc = (uint16_t)((fc & ~clr) | 0x4000);
  size_t p = 0;
  aad[p++] = (uint8_t)mfc; aad[p++] = (uint8_t)(mfc >> 8);
  memcpy(aad + p, hdr + 4, 18); p += 18;          /* A1 A2 A3 */
  uint16_t sc = (uint16_t)((hdr[22] | (hdr[23] << 8)) & 0x000f);   /* seq# masked, frag kept */
  aad[p++] = (uint8_t)sc; aad[p++] = (uint8_t)(sc >> 8);
  if(qos){ aad[p++] = tid; aad[p++] = 0; }
  *aad_len = p;
}

size_t ccmp_encrypt(const uint8_t tk[16], const uint8_t* hdr, size_t hdr_len, bool qos, bool mgmt,
                    uint64_t pn, uint8_t keyid, const uint8_t* body, size_t body_len, uint8_t* out){
  uint8_t pn6[6]; for(int i = 0; i < 6; i++) pn6[i] = (uint8_t)(pn >> (8 * i));
  uint8_t nonce[13], aad[32]; size_t aad_len;
  ccmp_na(hdr, qos, mgmt, pn6, nonce, aad, &aad_len);
  memcpy(out, hdr, hdr_len);
  out[1] |= 0x40;                                 /* Protected */
  uint8_t* cc = out + hdr_len;
  cc[0]=pn6[0]; cc[1]=pn6[1]; cc[2]=0; cc[3]=(uint8_t)((keyid << 6) | 0x20);   /* ExtIV */
  cc[4]=pn6[2]; cc[5]=pn6[3]; cc[6]=pn6[4]; cc[7]=pn6[5];
  ccm_encrypt(tk, nonce, aad, aad_len, body, body_len, cc + 8, cc + 8 + body_len);
  return hdr_len + 8 + body_len + 8;
}

int ccmp_decrypt(const uint8_t key[16], const uint8_t* hdr, size_t hdr_len, bool qos, bool mgmt,
                 const uint8_t* in, size_t in_len, uint8_t* out){
  if(in_len < 8 + 8) return -1;
  const uint8_t* cc = in;
  uint8_t pn6[6] = { cc[0], cc[1], cc[4], cc[5], cc[6], cc[7] };
  uint8_t nonce[13], aad[32]; size_t aad_len;
  ccmp_na(hdr, qos, mgmt, pn6, nonce, aad, &aad_len);
  size_t ct_len = in_len - 8 - 8;
  const uint8_t* ct = in + 8;
  const uint8_t* mic = in + 8 + ct_len;
  if(!ccm_decrypt(key, nonce, aad, aad_len, ct, ct_len, out, mic)) return -1;
  return (int)ct_len;
}

/* ---- AES-CMAC (RFC 4493) for 802.11w BIP-CMAC-128 ----------------------- */
static void cmac_lshift(const uint8_t in[16], uint8_t out[16]){
  uint8_t ov = 0;
  for(int i = 15; i >= 0; i--){ out[i] = (uint8_t)((in[i] << 1) | ov); ov = in[i] >> 7; }
}

void aes_cmac128(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16]){
  uint8_t rk[176]; aes128_expand(key, rk);
  uint8_t L[16] = {0}; aes128_encrypt(rk, L, L);            /* L = E(0) */
  uint8_t K1[16], K2[16];
  cmac_lshift(L, K1);  if(L[0]  & 0x80) K1[15] ^= 0x87;     /* subkeys */
  cmac_lshift(K1, K2); if(K1[0] & 0x80) K2[15] ^= 0x87;
  size_t n = (len + 15) / 16;
  bool complete = (n != 0) && (len % 16 == 0);
  if(n == 0) n = 1;
  uint8_t X[16] = {0};
  for(size_t i = 0; i + 1 < n; i++){ xor16(X, msg + 16 * i); aes128_encrypt(rk, X, X); }
  uint8_t last[16];
  size_t rem = len - 16 * (n - 1);
  if(complete){ memcpy(last, msg + 16 * (n - 1), 16); xor16(last, K1); }
  else { memset(last, 0, 16); memcpy(last, msg + 16 * (n - 1), rem); last[rem] = 0x80; xor16(last, K2); }
  xor16(X, last); aes128_encrypt(rk, X, X);
  memcpy(mac, X, 16);
}

/* ---- KAT: RFC 3610 Packet Vector #1 ------------------------------------- */
int ccmp_selftest(void){
  uint8_t key[16], nonce[13], aad[8], msg[23];
  for(int i = 0; i < 16; i++) key[i] = 0xC0 + i;
  uint8_t nv[13] = { 0,0,0,3,2,1,0,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5 };
  memcpy(nonce, nv, 13);
  for(int i = 0; i < 8; i++) aad[i] = i;
  for(int i = 0; i < 23; i++) msg[i] = 8 + i;
  static const uint8_t want_ct[23] = {
    0x58,0x8c,0x97,0x9a,0x61,0xc6,0x63,0xd2,0xf0,0x66,0xd0,0xc2,0xc0,0xf9,0x89,0x80,
    0x6d,0x5f,0x6b,0x61,0xda,0xc3,0x84 };
  static const uint8_t want_mic[8] = { 0x17,0xe8,0xd1,0x2c,0xfd,0xf9,0x26,0xe0 };
  uint8_t ct[23], mic[8];
  ccm_encrypt(key, nonce, aad, 8, msg, 23, ct, mic);
  if(memcmp(ct, want_ct, 23) || memcmp(mic, want_mic, 8)) return 1;
  uint8_t dec[23];
  if(!ccm_decrypt(key, nonce, aad, 8, ct, 23, dec, mic) || memcmp(dec, msg, 23)) return 2;
  mic[0] ^= 1;                                     /* tamper → must fail */
  if(ccm_decrypt(key, nonce, aad, 8, ct, 23, dec, mic)) return 3;

  /* AES-CMAC RFC 4493 example 1 (empty message). */
  static const uint8_t ck[16] = {
    0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
  static const uint8_t want_mac[16] = {
    0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46 };
  uint8_t cm[16]; aes_cmac128(ck, NULL, 0, cm);
  if(memcmp(cm, want_mac, 16)) return 4;

  /* Robust-management CCMP round-trip (mgmt nonce/AAD path). */
  uint8_t hdr[24] = { 0xc0,0x00, 0,0, 1,2,3,4,5,6, 7,8,9,10,11,12, 1,2,3,4,5,6, 0x10,0x00 };
  uint8_t mbody[2] = { 0x03, 0x00 }, menc[64], mdec[8];
  size_t mn = ccmp_encrypt(key, hdr, 24, false, true, 5, 0, mbody, 2, menc);
  if(ccmp_decrypt(key, hdr, 24, false, true, menc + 24, mn - 24, mdec) != 2
     || memcmp(mdec, mbody, 2)) return 5;
  return 0;
}