/**
 * packet_codec.h — explicit field-by-field serialisation for packet_header_t
 *                  and packet_body_t ↔ canonical byte arrays.
 *
 * Why explicit offsets rather than struct cast / memcpy:
 *   _Static_assert(sizeof(...)) only catches size regressions.
 *   It does NOT catch field-order drift or compiler padding differences that
 *   happen to produce the same total size.  Writing each field at a hard-coded
 *   byte offset makes the wire format deterministic across
 *   arm-none-eabi-gcc (RP2040) and xtensa-esp32s3-elf-gcc (ESP32-S3).
 *
 * These functions are used by:
 *   - aes_wrap.c: serialise before encryption / deserialise after decryption
 *   - protocol_vectors.c: golden-byte test (§16 cross-compiler check)
 *
 * The encrypted wire frame (rf_frame_t) is assembled and disassembled by
 * aes_ccm_encrypt() / aes_ccm_decrypt() in aes_wrap.h — not here.
 */

#ifndef PACKET_CODEC_H
#define PACKET_CODEC_H

#include <stdint.h>
#include <stdbool.h>
#include "packet_protocol.h"

/* ── Header codec ────────────────────────────────────────────────────────── */

/**
 * Serialise a packet_header_t into a PACKET_HEADER_LEN (10) byte buffer.
 *
 * Byte layout (matches packet_header_t field comment in packet_protocol.h):
 *   [0]    version
 *   [1]    soldier_id
 *   [2–9]  persistent_tx_counter (little-endian uint64)
 */
void packet_header_encode(const packet_header_t *hdr, uint8_t buf[PACKET_HEADER_LEN]);

/**
 * Deserialise PACKET_HEADER_LEN (10) bytes into a packet_header_t.
 * @return true  if version == PROTO_VERSION and soldier_id in [1, SOLDIER_ID_MAX].
 * @return false on sanity failure; *hdr contents are undefined.
 */
bool packet_header_decode(const uint8_t buf[PACKET_HEADER_LEN], packet_header_t *hdr);

/* ── Body codec ──────────────────────────────────────────────────────────── */

/**
 * Serialise a packet_body_t into a PACKET_BODY_LEN (10) byte buffer.
 * This buffer is the plaintext passed to AES-CCM encryption.
 *
 * Byte layout (matches packet_body_t field comment in packet_protocol.h):
 *   [0]    msg_type
 *   [1]    sensor_flags
 *   [2]    battery_pct
 *   [3]    prev_tx_retries
 *   [4–7]  sender_uptime_ms (little-endian uint32)
 *   [8–9]  event_id         (little-endian uint16)
 */
void packet_body_encode(const packet_body_t *body, uint8_t buf[PACKET_BODY_LEN]);

/**
 * Deserialise PACKET_BODY_LEN (10) bytes (decrypted plaintext) into a packet_body_t.
 * @return true  if msg_type <= MSG_TYPE_MAX_VALID and battery_pct <= 100.
 * @return false on sanity failure; *body contents are undefined.
 */
bool packet_body_decode(const uint8_t buf[PACKET_BODY_LEN], packet_body_t *body);

/* ── Nonce derivation ────────────────────────────────────────────────────── */

/**
 * Derive the 13-byte AES-CCM nonce from a packet header.
 *
 * Nonce layout (CCM_NONCE_LEN = 13, L = 2):
 *   [0]    version                (from hdr->version)
 *   [1]    direction              (CCM_DIR_SOLDIER_TO_BASE = 0x00 for v1 packets)
 *   [2]    soldier_id             (from hdr->soldier_id)
 *   [3–10] persistent_tx_counter  (from hdr->persistent_tx_counter, little-endian)
 *   [11]   0x00 (pad)
 *   [12]   0x00 (pad)
 *
 * The nonce is NOT transmitted — both sides reconstruct it from the clear header.
 * This function must produce identical output on both compilers for the same input.
 *
 * @param hdr   Source header (must not be NULL).
 * @param dir   Direction byte: CCM_DIR_SOLDIER_TO_BASE or CCM_DIR_BASE_TO_SOLDIER.
 * @param nonce Output buffer of CCM_NONCE_LEN (13) bytes.
 */
void packet_make_nonce(const packet_header_t *hdr, uint8_t dir,
                       uint8_t nonce[CCM_NONCE_LEN]);

#endif /* PACKET_CODEC_H */
