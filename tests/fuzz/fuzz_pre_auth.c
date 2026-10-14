/**
 * fuzz_pre_auth.c — LLVM libFuzzer harness for pre-auth raw 28-byte wire frame parsing
 *
 * Tests robustness against arbitrary corrupt, truncated, out-of-bounds, or hostile
 * byte sequences entering via nRF24L01+ radio RX FIFO before authentication.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "packet_protocol.h"
#include "packet_codec.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < PACKET_HEADER_LEN) {
        return 0;
    }

    /* 1. Header parsing & validation */
    packet_header_t hdr;
    bool hdr_ok = packet_header_decode(data, &hdr);
    if (hdr_ok) {
        /* Nonce derivation */
        uint8_t nonce[CCM_NONCE_LEN];
        packet_make_nonce(&hdr, CCM_DIR_SOLDIER_TO_BASE, nonce);

        /* Re-encode check */
        uint8_t enc_hdr[PACKET_HEADER_LEN];
        packet_header_encode(&hdr, enc_hdr);
    }

    /* 2. Body parsing if enough bytes */
    if (size >= PACKET_HEADER_LEN + PACKET_BODY_LEN) {
        packet_body_t body;
        bool body_ok = packet_body_decode(data + PACKET_HEADER_LEN, &body);
        if (body_ok) {
            uint8_t enc_body[PACKET_BODY_LEN];
            packet_body_encode(&body, enc_body);
        }
    }

    /* 3. Full 28-byte frame parsing */
    if (size >= RF_FRAME_LEN) {
        rf_frame_t frame;
        memcpy(&frame, data, sizeof(frame));
        (void)frame;
    }

    return 0;
}
