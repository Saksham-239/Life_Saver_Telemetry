/**
 * protocol_vectors.c — Golden byte vectors for §16 cross-compiler wire-format test.
 *
 * HOW TO USE:
 *   1. Compile and run on the RP2040 (arm-none-eabi-gcc). Output over UART0.
 *   2. Compile and run the identical sources on the ESP32-S3 (xtensa-esp32s3-elf-gcc).
 *   3. Both must print "ALL TESTS PASSED" — any failure indicates a codec bug
 *      or toolchain-specific struct layout issue that will corrupt the wire format.
 *
 * What is tested here:
 *   - packet_header_encode / packet_header_decode (golden bytes)
 *   - packet_body_encode / packet_body_decode (golden bytes)
 *   - packet_make_nonce (golden 13-byte nonce)
 *   - Encode → decode roundtrips for all three
 *   - Reject paths: invalid version, out-of-range soldier_id, reserved msg_type
 *
 * What is NOT tested here (out of scope for a pure-codec test):
 *   - AES-CCM encryption/decryption — those are covered by each platform's
 *     own crypto library known-answer tests (mbedTLS / tinycrypt ship these).
 *   - Radio driver behaviour — tested in bring-up phase (§14 step 1–2).
 *
 * This file has no platform-specific dependencies beyond packet_codec.h and
 * a printf-capable serial output.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "packet_protocol.h"
#include "packet_codec.h"

static int s_failures = 0;

static void check(const char *label, bool cond) {
    if (!cond) {
        printf("FAIL: %s\n", label);
        s_failures++;
    }
}

static void check_bytes(const char *label,
                        const uint8_t *actual, const uint8_t *expected, size_t len) {
    if (memcmp(actual, expected, len) != 0) {
        printf("FAIL: %s\n  got:     ", label);
        for (size_t i = 0; i < len; i++) printf("%02X ", actual[i]);
        printf("\n  expected:");
        for (size_t i = 0; i < len; i++) printf("%02X ", expected[i]);
        printf("\n");
        s_failures++;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * VECTOR 1 — packet_header_t: soldier 2, counter = 0x0000000000000001
 *
 * Input:
 *   version              = 0x01
 *   soldier_id           = 0x02
 *   persistent_tx_counter= 1  (0x0000000000000001)
 *
 * Expected PACKET_HEADER_LEN (10) byte encoding:
 *   [0]  01  version
 *   [1]  02  soldier_id
 *   [2]  01  counter byte 0 (LSB)
 *   [3]  00
 *   [4]  00
 *   [5]  00
 *   [6]  00
 *   [7]  00
 *   [8]  00
 *   [9]  00  counter byte 7 (MSB)
 * ══════════════════════════════════════════════════════════════════════════ */
static const packet_header_t VEC1_HDR = {
    .version               = PROTO_VERSION,
    .soldier_id            = 2,
    .persistent_tx_counter = 1ULL,
};
static const uint8_t VEC1_HDR_BYTES[PACKET_HEADER_LEN] = {
    0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ══════════════════════════════════════════════════════════════════════════
 * VECTOR 2 — packet_header_t: soldier 5, counter = 0xDEADBEEFCAFEBABE
 *
 * Expected PACKET_HEADER_LEN (10) byte encoding:
 *   [0]  01  version
 *   [1]  05  soldier_id
 *   [2]  BE  counter[0] LSB of 0xDEADBEEFCAFEBABE
 *   [3]  BA
 *   [4]  FE
 *   [5]  CA
 *   [6]  EF
 *   [7]  BE
 *   [8]  AD
 *   [9]  DE  counter[7] MSB
 * ══════════════════════════════════════════════════════════════════════════ */
static const packet_header_t VEC2_HDR = {
    .version               = PROTO_VERSION,
    .soldier_id            = 5,
    .persistent_tx_counter = 0xDEADBEEFCAFEBABEULL,
};
static const uint8_t VEC2_HDR_BYTES[PACKET_HEADER_LEN] = {
    0x01, 0x05, 0xBE, 0xBA, 0xFE, 0xCA, 0xEF, 0xBE, 0xAD, 0xDE
};

/* ══════════════════════════════════════════════════════════════════════════
 * VECTOR 3 — packet_body_t: MSG_DOWN, fall_detected, 30% batt, 10 retries
 *
 * Input:
 *   msg_type         = MSG_DOWN (0x05)
 *   sensor_flags     = SFLAG_FALL_DETECTED | SFLAG_STATIONARY = 0x03
 *   battery_pct      = 30  (0x1E)
 *   prev_tx_retries  = 10  (0x0A)
 *   sender_uptime_ms = 300000 = 0x000493E0 → LE: E0 93 04 00
 *   event_id         = 7   (0x0007)      → LE: 07 00
 *
 * Expected PACKET_BODY_LEN (10) byte encoding:
 *   [0]  05  msg_type
 *   [1]  03  sensor_flags
 *   [2]  1E  battery_pct
 *   [3]  0A  prev_tx_retries
 *   [4]  E0  uptime[0]
 *   [5]  93  uptime[1]
 *   [6]  04  uptime[2]
 *   [7]  00  uptime[3]
 *   [8]  07  event_id[0]
 *   [9]  00  event_id[1]
 * ══════════════════════════════════════════════════════════════════════════ */
static const packet_body_t VEC3_BODY = {
    .msg_type         = MSG_DOWN,
    .sensor_flags     = SFLAG_FALL_DETECTED | SFLAG_STATIONARY,
    .battery_pct      = 30,
    .prev_tx_retries  = 10,
    .sender_uptime_ms = 300000UL,
    .event_id         = 7,
};
static const uint8_t VEC3_BODY_BYTES[PACKET_BODY_LEN] = {
    0x05, 0x03, 0x1E, 0x0A, 0xE0, 0x93, 0x04, 0x00, 0x07, 0x00
};

/* ══════════════════════════════════════════════════════════════════════════
 * VECTOR 4 — packet_body_t: MSG_EMERGENCY_CLEAR, max battery, event_id=0xABCD
 *
 * Input:
 *   msg_type         = MSG_EMERGENCY_CLEAR (0x08)
 *   sensor_flags     = 0x00
 *   battery_pct      = 100 (0x64)
 *   prev_tx_retries  = 0
 *   sender_uptime_ms = 60000 = 0x0000EA60 → LE: 60 EA 00 00
 *   event_id         = 0xABCD             → LE: CD AB
 *
 * Expected:
 *   [0]  08  MSG_EMERGENCY_CLEAR
 *   [1]  00
 *   [2]  64
 *   [3]  00
 *   [4]  60
 *   [5]  EA
 *   [6]  00
 *   [7]  00
 *   [8]  CD
 *   [9]  AB
 * ══════════════════════════════════════════════════════════════════════════ */
static const packet_body_t VEC4_BODY = {
    .msg_type         = MSG_EMERGENCY_CLEAR,
    .sensor_flags     = 0x00,
    .battery_pct      = 100,
    .prev_tx_retries  = 0,
    .sender_uptime_ms = 60000UL,
    .event_id         = 0xABCD,
};
static const uint8_t VEC4_BODY_BYTES[PACKET_BODY_LEN] = {
    0x08, 0x00, 0x64, 0x00, 0x60, 0xEA, 0x00, 0x00, 0xCD, 0xAB
};

/* ══════════════════════════════════════════════════════════════════════════
 * VECTOR 5 — nonce from VEC1_HDR, direction SOLDIER_TO_BASE
 *
 * Nonce layout:
 *   [0]  version = 0x01
 *   [1]  dir     = 0x00 (CCM_DIR_SOLDIER_TO_BASE)
 *   [2]  sid     = 0x02
 *   [3–10] counter = 1 (LE uint64) = 01 00 00 00 00 00 00 00
 *   [11] 0x00
 *   [12] 0x00
 * ══════════════════════════════════════════════════════════════════════════ */
static const uint8_t VEC5_NONCE[CCM_NONCE_LEN] = {
    0x01, 0x00, 0x02,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
};

/* ── Test runner ─────────────────────────────────────────────────────────── */

static void test_header_encode(void) {
    uint8_t buf[PACKET_HEADER_LEN];
    packet_header_encode(&VEC1_HDR, buf);
    check_bytes("hdr_encode_vec1", buf, VEC1_HDR_BYTES, PACKET_HEADER_LEN);

    packet_header_encode(&VEC2_HDR, buf);
    check_bytes("hdr_encode_vec2", buf, VEC2_HDR_BYTES, PACKET_HEADER_LEN);
}

static void test_header_decode(void) {
    packet_header_t hdr;

    check("hdr_decode_vec1_ok", packet_header_decode(VEC1_HDR_BYTES, &hdr));
    check("hdr_decode_vec1_version",    hdr.version    == VEC1_HDR.version);
    check("hdr_decode_vec1_soldier_id", hdr.soldier_id == VEC1_HDR.soldier_id);
    check("hdr_decode_vec1_counter",    hdr.persistent_tx_counter == VEC1_HDR.persistent_tx_counter);

    check("hdr_decode_vec2_ok", packet_header_decode(VEC2_HDR_BYTES, &hdr));
    check("hdr_decode_vec2_counter",    hdr.persistent_tx_counter == VEC2_HDR.persistent_tx_counter);
}

static void test_header_decode_reject(void) {
    uint8_t bad_buf[PACKET_HEADER_LEN];
    packet_header_t hdr;

    /* Bad version */
    packet_header_encode(&VEC1_HDR, bad_buf);
    bad_buf[0] = 0x00;
    check("hdr_decode_bad_version_rejects", !packet_header_decode(bad_buf, &hdr));

    /* soldier_id = 0 */
    packet_header_encode(&VEC1_HDR, bad_buf);
    bad_buf[1] = 0x00;
    check("hdr_decode_sid_zero_rejects", !packet_header_decode(bad_buf, &hdr));

    /* soldier_id = SOLDIER_ID_MAX + 1 */
    packet_header_encode(&VEC1_HDR, bad_buf);
    bad_buf[1] = SOLDIER_ID_MAX + 1;
    check("hdr_decode_sid_overflow_rejects", !packet_header_decode(bad_buf, &hdr));
}

static void test_body_encode(void) {
    uint8_t buf[PACKET_BODY_LEN];
    packet_body_encode(&VEC3_BODY, buf);
    check_bytes("body_encode_vec3", buf, VEC3_BODY_BYTES, PACKET_BODY_LEN);

    packet_body_encode(&VEC4_BODY, buf);
    check_bytes("body_encode_vec4", buf, VEC4_BODY_BYTES, PACKET_BODY_LEN);
}

static void test_body_decode(void) {
    packet_body_t body;

    check("body_decode_vec3_ok", packet_body_decode(VEC3_BODY_BYTES, &body));
    check("body_decode_vec3_msgtype",  body.msg_type         == VEC3_BODY.msg_type);
    check("body_decode_vec3_flags",    body.sensor_flags     == VEC3_BODY.sensor_flags);
    check("body_decode_vec3_battery",  body.battery_pct      == VEC3_BODY.battery_pct);
    check("body_decode_vec3_retries",  body.prev_tx_retries  == VEC3_BODY.prev_tx_retries);
    check("body_decode_vec3_uptime",   body.sender_uptime_ms == VEC3_BODY.sender_uptime_ms);
    check("body_decode_vec3_eventid",  body.event_id         == VEC3_BODY.event_id);

    check("body_decode_vec4_ok", packet_body_decode(VEC4_BODY_BYTES, &body));
    check("body_decode_vec4_eventid",  body.event_id == VEC4_BODY.event_id);
}

static void test_body_decode_reject(void) {
    uint8_t bad_buf[PACKET_BODY_LEN];
    packet_body_t body;

    /* Reserved msg_type (0x09) */
    packet_body_encode(&VEC3_BODY, bad_buf);
    bad_buf[0] = 0x09;
    check("body_decode_reserved_msgtype_rejects", !packet_body_decode(bad_buf, &body));

    /* battery_pct > 100 */
    packet_body_encode(&VEC3_BODY, bad_buf);
    bad_buf[2] = 101;
    check("body_decode_battery_overflow_rejects", !packet_body_decode(bad_buf, &body));
}

static void test_nonce(void) {
    uint8_t nonce[CCM_NONCE_LEN];
    packet_make_nonce(&VEC1_HDR, CCM_DIR_SOLDIER_TO_BASE, nonce);
    check_bytes("nonce_vec1", nonce, VEC5_NONCE, CCM_NONCE_LEN);
}

static void test_roundtrip_header(void) {
    uint8_t buf[PACKET_HEADER_LEN];
    packet_header_t decoded;
    packet_header_encode(&VEC2_HDR, buf);
    check("rt_hdr_decode", packet_header_decode(buf, &decoded));
    check("rt_hdr_counter", decoded.persistent_tx_counter == VEC2_HDR.persistent_tx_counter);
    check("rt_hdr_sid",     decoded.soldier_id             == VEC2_HDR.soldier_id);
}

static void test_roundtrip_body(void) {
    uint8_t buf[PACKET_BODY_LEN];
    packet_body_t decoded;
    packet_body_encode(&VEC4_BODY, buf);
    check("rt_body_decode", packet_body_decode(buf, &decoded));
    check("rt_body_eventid",  decoded.event_id         == VEC4_BODY.event_id);
    check("rt_body_uptime",   decoded.sender_uptime_ms == VEC4_BODY.sender_uptime_ms);
    check("rt_body_msgtype",  decoded.msg_type         == VEC4_BODY.msg_type);
}

static void test_size_asserts(void) {
    /* These are compile-time in packet_protocol.h; print runtime confirmation too */
    printf("sizeof(packet_header_t) = %u (expected %u)\n",
           (unsigned)sizeof(packet_header_t), (unsigned)PACKET_HEADER_LEN);
    printf("sizeof(packet_body_t)   = %u (expected %u)\n",
           (unsigned)sizeof(packet_body_t),   (unsigned)PACKET_BODY_LEN);
    printf("sizeof(rf_frame_t)      = %u (expected %u)\n",
           (unsigned)sizeof(rf_frame_t),      (unsigned)RF_FRAME_LEN);
    check("sizeof_header", sizeof(packet_header_t) == PACKET_HEADER_LEN);
    check("sizeof_body",   sizeof(packet_body_t)   == PACKET_BODY_LEN);
    check("sizeof_frame",  sizeof(rf_frame_t)       == RF_FRAME_LEN);
}

/**
 * Run all codec tests.  Call from main() or a test task.
 * @return 0 if all passed, >0 count of failures.
 */
int protocol_vectors_run_all(void) {
    s_failures = 0;
    printf("=== packet_protocol codec test vectors ===\n");

    test_size_asserts();
    test_header_encode();
    test_header_decode();
    test_header_decode_reject();
    test_body_encode();
    test_body_decode();
    test_body_decode_reject();
    test_nonce();
    test_roundtrip_header();
    test_roundtrip_body();

    if (s_failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%d TEST(S) FAILED\n", s_failures);
    }
    return s_failures;
}
