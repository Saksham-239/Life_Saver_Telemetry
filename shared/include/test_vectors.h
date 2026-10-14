#pragma once
#include <stdint.h>

/* Golden test vectors for AES-CCM cross-platform compatibility test */

static const uint8_t GOLDEN_KEY[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

/* Unencrypted packet components */
static const uint8_t GOLDEN_VERSION = 1;
static const uint8_t GOLDEN_SOLDIER_ID = 2;
static const uint64_t GOLDEN_TX_COUNTER = 1;

static const uint8_t GOLDEN_MSG_TYPE = 1; /* ALARM_FALL */
static const uint8_t GOLDEN_FLAGS = 0x04; /* FLAG_BATTERY_LOW */
static const uint8_t GOLDEN_BATTERY = 15; /* 15% */
static const uint8_t GOLDEN_RETRIES = 2;
static const uint32_t GOLDEN_UPTIME = 0x0001E240; /* 123456 ms */
static const uint16_t GOLDEN_EVENT_ID = 42;

/* Expected 28-byte RF frame
 * This must be manually verified ONCE from a trusted crypto library, then
 * used as the golden standard for both TinyCrypt (RP2040) and mbedTLS (ESP32).
 */
static const uint8_t GOLDEN_FRAME[28] = {
    0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xc8, 0xf3, 0x4c, 0xc4, 0x49, 0xd4,
    0xc3, 0x60, 0x10, 0x13, 0x15, 0x18, 0x6f, 0x97,
    0xc4, 0x4f, 0x4f, 0x2b
};
