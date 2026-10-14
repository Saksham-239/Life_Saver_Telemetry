/**
 * rf_config.h — Shared RF layer configuration (nRF24L01+)
 *
 * Defines the shared channel, physical address, and retry parameters
 * to ensure RP2040 and ESP32-S3 maintain radio alignment.
 */
#pragma once

/* RF Channel (0-125) */
#define RF_CONFIG_CHANNEL       76

/* 5-byte physical pipe addresses for units (Pipes 1–5 for soldiers 1–5, Pipe 0 for TX auto-ACK).
 * Per Nordic nRF24L01+ Specification §7.6 & §8.3.1:
 * Address bytes are written and transmitted LSByte first. Pipes 2-5 store only the LSByte (byte 0)
 * and share bytes 1-4 with Pipe 1. Therefore, the variable byte (soldier ID) MUST reside at byte 0. */
#define RF_CONFIG_ADDRESS       ((const uint8_t *)"\x00\xDE\xAD\xBE\xEF")
#define RF_PIPE0_ADDR           ((const uint8_t *)"\x00\xDE\xAD\xBE\xEF")
#define RF_PIPE1_ADDR           ((const uint8_t *)"\x01\xDE\xAD\xBE\xEF")
#define RF_PIPE2_ADDR           ((const uint8_t *)"\x02\xDE\xAD\xBE\xEF")
#define RF_PIPE3_ADDR           ((const uint8_t *)"\x03\xDE\xAD\xBE\xEF")
#define RF_PIPE4_ADDR           ((const uint8_t *)"\x04\xDE\xAD\xBE\xEF")
#define RF_PIPE5_ADDR           ((const uint8_t *)"\x05\xDE\xAD\xBE\xEF")

#include "packet_protocol.h"

/* Payload length (must match packet_codec fixed length or max) */
#define RF_CONFIG_PAYLOAD_SIZE  28

_Static_assert(RF_CONFIG_PAYLOAD_SIZE == RF_FRAME_LEN, "RF payload size must match frame length");

/* Hardware Retry Limits */
#define RF_CONFIG_RETRY_DELAY   15  /* 15 = 4000us */
#define RF_CONFIG_RETRY_COUNT   15

/**
 * Cross-checks that an incoming RF frame arrived on the correct physical pipe
 * corresponding to the claimed soldier ID (DEF-12).
 * Physical Pipes 1..5 correspond strictly to Soldier IDs 1..5.
 */
static inline bool rf_pipe_matches_soldier_id(uint8_t pipe_num, uint8_t soldier_id) {
    if (soldier_id < 1 || soldier_id > SOLDIER_ID_MAX) {
        return false;
    }
    return pipe_num == soldier_id;
}
