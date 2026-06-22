#ifndef DEVICE_CCMP_H
#define DEVICE_CCMP_H

/* Software CCMP (AES-128-CCM, M=8 L=2) for the mt7601u harness, which runs the radio in
 * monitor/injection with no HW cipher. Encrypts/decrypts 802.11 data MPDU bodies so a real
 * WPA2 link carries data. KAT-anchored on RFC 3610 Packet Vector #1. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Encapsulate: out = [802.11 hdr][CCMP hdr 8][ciphertext body_len][MIC 8]. Sets the Protected
 * bit in the copied header. keyid: 0 for pairwise (PTK), GTK index for group. mgmt=true for a
 * robust management frame (802.11w) — sets the nonce Management bit + keeps the subtype in the AAD.
 * Returns out length. */
size_t ccmp_encrypt(const uint8_t tk[16], const uint8_t* hdr, size_t hdr_len, bool qos, bool mgmt,
                    uint64_t pn, uint8_t keyid, const uint8_t* body, size_t body_len, uint8_t* out);

/* Decapsulate: in = [CCMP hdr 8][ciphertext][MIC 8] (the MPDU body after the 802.11 header).
 * Recovers the plaintext body into out. Returns body length, or -1 on MIC failure. */
int ccmp_decrypt(const uint8_t key[16], const uint8_t* hdr, size_t hdr_len, bool qos, bool mgmt,
                 const uint8_t* in, size_t in_len, uint8_t* out);

/* AES-CMAC (RFC 4493) over the same AES-128 core, for 802.11w BIP-CMAC-128 (IGTK) verification. */
void aes_cmac128(const uint8_t key[16], const uint8_t* msg, size_t msg_len, uint8_t mac[16]);

/* RFC 3610 Packet Vector #1 known-answer test. 0 on pass. */
int ccmp_selftest(void);

#endif