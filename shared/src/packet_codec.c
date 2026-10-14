/**
 * packet_codec.c — field-by-field serialisation for the RF Soldier Safety protocol.
 *
 * All fields are written/read at explicit byte offsets so that the canonical
 * wire representation is identical regardless of which toolchain compiled this
 * translation unit (arm-none-eabi-gcc vs xtensa-esp32s3-elf-gcc).
 */

#include "packet_codec.h"
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static inline void write_u16_le(uint8_t *dst, uint16_t val) {
    dst[0] = (uint8_t)(val & 0xFFu);
    dst[1] = (uint8_t)((val >> 8) & 0xFFu);
}

static inline uint16_t read_u16_le(const uint8_t *src) {
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static inline void write_u32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val & 0xFFu);
    dst[1] = (uint8_t)((val >>  8) & 0xFFu);
    dst[2] = (uint8_t)((val >> 16) & 0xFFu);
    dst[3] = (uint8_t)((val >> 24) & 0xFFu);
}

static inline uint32_t read_u32_le(const uint8_t *src) {
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

static inline void write_u64_le(uint8_t *dst, uint64_t val) {
    dst[0] = (uint8_t)(val & 0xFFu);
    dst[1] = (uint8_t)((val >>  8) & 0xFFu);
    dst[2] = (uint8_t)((val >> 16) & 0xFFu);
    dst[3] = (uint8_t)((val >> 24) & 0xFFu);
    dst[4] = (uint8_t)((val >> 32) & 0xFFu);
    dst[5] = (uint8_t)((val >> 40) & 0xFFu);
    dst[6] = (uint8_t)((val >> 48) & 0xFFu);
    dst[7] = (uint8_t)((val >> 56) & 0xFFu);
}

static inline uint64_t read_u64_le(const uint8_t *src) {
    return (uint64_t)src[0]
         | ((uint64_t)src[1] <<  8)
         | ((uint64_t)src[2] << 16)
         | ((uint64_t)src[3] << 24)
         | ((uint64_t)src[4] << 32)
         | ((uint64_t)src[5] << 40)
         | ((uint64_t)src[6] << 48)
         | ((uint64_t)src[7] << 56);
}

/* ── Header byte offsets ─────────────────────────────────────────────────── */
#define HDR_OFF_VERSION     0u
#define HDR_OFF_SOLDIER_ID  1u
#define HDR_OFF_COUNTER     2u   /* 8 bytes, little-endian uint64 */

/* ── Body byte offsets ────────────────────────────────────────────────────── */
#define BODY_OFF_MSG_TYPE       0u
#define BODY_OFF_SENSOR_FLAGS   1u
#define BODY_OFF_BATTERY_PCT    2u
#define BODY_OFF_PREV_RETRIES   3u
#define BODY_OFF_UPTIME_MS      4u   /* 4 bytes, little-endian uint32 */
#define BODY_OFF_EVENT_ID       8u   /* 2 bytes, little-endian uint16 */

/* ── Header codec ─────────────────────────────────────────────────────────── */

void packet_header_encode(const packet_header_t *hdr, uint8_t buf[PACKET_HEADER_LEN]) {
    buf[HDR_OFF_VERSION]    = hdr->version;
    buf[HDR_OFF_SOLDIER_ID] = hdr->soldier_id;
    write_u64_le(&buf[HDR_OFF_COUNTER], hdr->persistent_tx_counter);
}

bool packet_header_decode(const uint8_t buf[PACKET_HEADER_LEN], packet_header_t *hdr) {
    if (buf[HDR_OFF_VERSION] != PROTO_VERSION) {
        return false;
    }
    uint8_t sid = buf[HDR_OFF_SOLDIER_ID];
    if (sid < 1u || sid > SOLDIER_ID_MAX) {
        return false;
    }
    hdr->version                = buf[HDR_OFF_VERSION];
    hdr->soldier_id             = sid;
    hdr->persistent_tx_counter  = read_u64_le(&buf[HDR_OFF_COUNTER]);
    return true;
}

/* ── Body codec ───────────────────────────────────────────────────────────── */

void packet_body_encode(const packet_body_t *body, uint8_t buf[PACKET_BODY_LEN]) {
    buf[BODY_OFF_MSG_TYPE]     = body->msg_type;
    buf[BODY_OFF_SENSOR_FLAGS] = body->sensor_flags;
    buf[BODY_OFF_BATTERY_PCT]  = body->battery_pct;
    buf[BODY_OFF_PREV_RETRIES] = body->prev_tx_retries;
    write_u32_le(&buf[BODY_OFF_UPTIME_MS], body->sender_uptime_ms);
    write_u16_le(&buf[BODY_OFF_EVENT_ID],  body->event_id);
}

bool packet_body_decode(const uint8_t buf[PACKET_BODY_LEN], packet_body_t *body) {
    uint8_t mt = buf[BODY_OFF_MSG_TYPE];
    if (mt > (uint8_t)MSG_TYPE_MAX_VALID) {
        return false;
    }
    if (buf[BODY_OFF_BATTERY_PCT] > 100u) {
        return false;
    }
    body->msg_type         = mt;
    body->sensor_flags     = buf[BODY_OFF_SENSOR_FLAGS];
    body->battery_pct      = buf[BODY_OFF_BATTERY_PCT];
    body->prev_tx_retries  = buf[BODY_OFF_PREV_RETRIES];
    body->sender_uptime_ms = read_u32_le(&buf[BODY_OFF_UPTIME_MS]);
    body->event_id         = read_u16_le(&buf[BODY_OFF_EVENT_ID]);
    return true;
}

/* ── Nonce derivation ─────────────────────────────────────────────────────── */

void packet_make_nonce(const packet_header_t *hdr, uint8_t dir,
                       uint8_t nonce[CCM_NONCE_LEN]) {
    /*
     * Nonce byte map (13 bytes, L=2):
     *   [0]    version
     *   [1]    direction byte (CCM_DIR_*)
     *   [2]    soldier_id
     *   [3–10] persistent_tx_counter (little-endian uint64)
     *   [11]   0x00 pad
     *   [12]   0x00 pad
     *
     * This layout is deterministic across both compilers because every
     * field is written explicitly — no struct layout assumptions.
     */
    nonce[0]  = hdr->version;
    nonce[1]  = dir;
    nonce[2]  = hdr->soldier_id;
    write_u64_le(&nonce[3], hdr->persistent_tx_counter);
    nonce[11] = 0x00u;
    nonce[12] = 0x00u;
}
