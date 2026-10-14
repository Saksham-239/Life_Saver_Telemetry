/**
 * aes_wrap.h — AES-128-CCM interface for the RF Soldier Safety System.
 *
 * Implementations:
 *   RP2040:    soldier_unit/src/crypto/aes_wrap.c  — uses tinycrypt
 *   ESP32-S3:  base_station/main/crypto/aes_wrap.c — uses mbedTLS
 *
 * Both implement this identical header.  The wire output is byte-for-byte
 * compatible because AES-CCM is a standard algorithm and both libraries
 * use the same nonce (from packet_make_nonce()), the same AAD (raw header
 * bytes), the same tag length (CCM_TAG_LEN = 8), and L=2.
 *
 * CCM library known-answer test (KAT) references:
 *   tinycrypt:  tests/test_ccm_mode.c in the tinycrypt repository
 *   mbedTLS:    tests/suites/test_suite_ccm.function in the mbedTLS tree
 * Run these platform-shipped KATs during bring-up to confirm the library
 * is wired correctly before testing cross-node decryption.
 */

#ifndef AES_WRAP_H
#define AES_WRAP_H

#include <stdbool.h>
#include "../../shared/include/packet_protocol.h"
#include "../../shared/src/packet_codec.h"

/**
 * Encrypt a packet_t and produce a ready-to-transmit rf_frame_t.
 *
 * Steps performed internally:
 *   1. packet_header_encode(pkt->hdr)  → 10-byte AAD buffer
 *   2. packet_body_encode(pkt->body)   → 10-byte plaintext buffer
 *   3. packet_make_nonce(pkt->hdr, CCM_DIR_SOLDIER_TO_BASE) → 13-byte nonce
 *   4. AES-128-CCM: encrypt plaintext with AAD and nonce, using key
 *      Output: 10-byte ciphertext + 8-byte tag
 *   5. Assemble rf_frame_t: copy header bytes, ciphertext, tag.
 *
 * @param pkt       Source packet (hdr + body fields must be valid).
 * @param key       16-byte AES-128 pre-shared key for pkt->hdr.soldier_id.
 * @param frame_out Output rf_frame_t (28 bytes ready for nRF24 TX FIFO).
 * @return true on success; false if the underlying CCM call fails.
 */
bool aes_ccm_encrypt(const packet_t *pkt,
                     const uint8_t   key[AES_KEY_LEN],
                     rf_frame_t     *frame_out);

/**
 * Authenticate and decrypt an rf_frame_t received from the nRF24 RX FIFO.
 *
 * Steps performed internally:
 *   1. packet_header_decode(frame->header) — validate version, soldier_id.
 *      Returns false if header is invalid.
 *   2. packet_make_nonce(decoded_hdr, CCM_DIR_SOLDIER_TO_BASE) → nonce.
 *   3. AES-128-CCM auth+decrypt: verify tag against header (AAD) and ciphertext.
 *      Returns false if the authentication tag does not match (tamper / wrong key).
 *   4. packet_body_decode(plaintext) — validate msg_type, battery_pct.
 *      Returns false if body fields are out of range.
 *   5. Populate pkt_out->hdr and pkt_out->body.
 *
 * @param frame     Received rf_frame_t (28 bytes from nRF24 RX FIFO).
 * @param key       16-byte AES-128 key for the soldier_id in frame->header.
 *                  Caller must look up the correct key from the NVS/flash table
 *                  using frame->header.soldier_id BEFORE calling this function.
 * @param pkt_out   Output packet on success; undefined on failure.
 * @return true on authenticated, successfully decoded packet.
 * @return false on any failure (header invalid / tag mismatch / body invalid).
 */
bool aes_ccm_decrypt(const rf_frame_t *frame,
                     const uint8_t     key[AES_KEY_LEN],
                     packet_t         *pkt_out);

#endif /* AES_WRAP_H */
