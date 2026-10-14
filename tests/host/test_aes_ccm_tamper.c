/**
 * test_aes_ccm_tamper.c - AES-128-CCM Cryptographic Authentication & Tamper Rejection Test
 *
 * Verifies Gate G5 Item 5:
 * 1. Valid packet encrypts and decrypts cleanly with genuine AES key.
 * 2. Wrong AES key fails decryption and tag verification.
 * 3. Header tampering (every byte 0..9) is detected and rejected.
 * 4. Ciphertext tampering (every byte 0..9) is detected and rejected.
 * 5. Authentication tag tampering (every byte 0..7) is detected and rejected.
 * 6. Multi-bit randomized corruption test (100 random corruptions rejected 100%).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "packet_protocol.h"
#include "packet_codec.h"
#include "aes_wrap.h"

static const uint8_t TEST_KEY_A[AES_KEY_LEN] = {
    0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
    0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C
};

static const uint8_t TEST_KEY_B[AES_KEY_LEN] = {
    0x8E, 0x73, 0xB0, 0xF7, 0xDA, 0x0E, 0x64, 0x52,
    0xC8, 0x10, 0xF3, 0x2B, 0x80, 0x90, 0x79, 0xE5
};

int main(void) {
    printf("========================================================================\n");
    printf("  AES-128-CCM Tag Verification & Tamper Rejection Test (TinyCrypt)\n");
    printf("========================================================================\n\n");

    /* 1. Create a representative emergency alert packet */
    packet_t orig_pkt;
    memset(&orig_pkt, 0, sizeof(orig_pkt));
    orig_pkt.hdr.version = PROTO_VERSION;
    orig_pkt.hdr.soldier_id = 3;
    orig_pkt.hdr.persistent_tx_counter = 42105ULL;

    orig_pkt.body.msg_type = MSG_DOWN;
    orig_pkt.body.sensor_flags = SFLAG_FALL_DETECTED | SFLAG_STATIONARY;
    orig_pkt.body.battery_pct = 87;
    orig_pkt.body.prev_tx_retries = 2;
    orig_pkt.body.sender_uptime_ms = 3600000;
    orig_pkt.body.event_id = 19;

    rf_frame_t frame;
    bool enc_ok = aes_ccm_encrypt(&orig_pkt, TEST_KEY_A, &frame);
    assert(enc_ok && "aes_ccm_encrypt failed on valid input");
    printf("1. ENCRYPTION & DECRYPTION ROUNDTRIP:\n");
    printf("   Encrypted 28-byte frame:\n");
    printf("     Header (10B):     ");
    for (size_t i = 0; i < PACKET_HEADER_LEN; i++) printf("%02X ", ((const uint8_t *)&frame.header)[i]);
    printf("\n     Ciphertext (10B): ");
    for (size_t i = 0; i < PACKET_BODY_LEN; i++) printf("%02X ", frame.ciphertext[i]);
    printf("\n     CCM Tag (8B):     ");
    for (size_t i = 0; i < CCM_TAG_LEN; i++) printf("%02X ", frame.tag[i]);
    printf("\n");

    packet_t dec_pkt;
    memset(&dec_pkt, 0, sizeof(dec_pkt));
    bool dec_ok = aes_ccm_decrypt(&frame, TEST_KEY_A, &dec_pkt);
    assert(dec_ok && "aes_ccm_decrypt failed on valid frame");
    assert(dec_pkt.hdr.soldier_id == orig_pkt.hdr.soldier_id);
    assert(dec_pkt.hdr.persistent_tx_counter == orig_pkt.hdr.persistent_tx_counter);
    assert(dec_pkt.body.msg_type == orig_pkt.body.msg_type);
    assert(dec_pkt.body.sensor_flags == orig_pkt.body.sensor_flags);
    assert(dec_pkt.body.event_id == orig_pkt.body.event_id);
    printf("   -> Roundtrip decrypted packet matches original perfectly [PASS]\n\n");

    /* 2. Wrong Key Rejection */
    printf("2. WRONG KEY AUTHENTICATION REJECTION:\n");
    bool wrong_key_ok = aes_ccm_decrypt(&frame, TEST_KEY_B, &dec_pkt);
    assert(!wrong_key_ok && "aes_ccm_decrypt must fail when using wrong key");
    printf("   -> Decryption with mismatched Key B strictly rejected [PASS]\n\n");

    /* 3. Header / AAD Tampering (Every single byte) */
    printf("3. HEADER (AAD) TAMPERING REJECTION (10 Bytes Checked):\n");
    uint8_t *hdr_bytes = (uint8_t *)&frame.header;
    for (size_t i = 0; i < PACKET_HEADER_LEN; i++) {
        uint8_t orig_byte = hdr_bytes[i];
        hdr_bytes[i] ^= 0x01; /* Single-bit tamper */

        bool tamper_ok = aes_ccm_decrypt(&frame, TEST_KEY_A, &dec_pkt);
        assert(!tamper_ok && "aes_ccm_decrypt MUST reject tampered header byte");

        hdr_bytes[i] = orig_byte; /* Restore */
    }
    printf("   -> All 10/10 single-byte header corruptions rejected [PASS]\n\n");

    /* 4. Ciphertext Tampering (Every single byte) */
    printf("4. CIPHERTEXT TAMPERING REJECTION (10 Bytes Checked):\n");
    for (size_t i = 0; i < PACKET_BODY_LEN; i++) {
        uint8_t orig_byte = frame.ciphertext[i];
        frame.ciphertext[i] ^= 0x01; /* Single-bit tamper */

        bool tamper_ok = aes_ccm_decrypt(&frame, TEST_KEY_A, &dec_pkt);
        assert(!tamper_ok && "aes_ccm_decrypt MUST reject tampered ciphertext byte");

        frame.ciphertext[i] = orig_byte; /* Restore */
    }
    printf("   -> All 10/10 single-byte ciphertext corruptions rejected [PASS]\n\n");

    /* 5. Authentication Tag Tampering (Every single byte) */
    printf("5. AUTHENTICATION TAG TAMPERING REJECTION (8 Bytes Checked):\n");
    for (size_t i = 0; i < CCM_TAG_LEN; i++) {
        uint8_t orig_byte = frame.tag[i];
        frame.tag[i] ^= 0x01; /* Single-bit tamper */

        bool tamper_ok = aes_ccm_decrypt(&frame, TEST_KEY_A, &dec_pkt);
        assert(!tamper_ok && "aes_ccm_decrypt MUST reject tampered tag byte");

        frame.tag[i] = orig_byte; /* Restore */
    }
    printf("   -> All 8/8 single-byte authentication tag corruptions rejected [PASS]\n\n");

    /* 6. Randomized Multi-Bit Tamper Test (100 Iterations) */
    printf("6. RANDOMIZED MULTI-BIT TAMPER TEST (100 Corrupted Frames):\n");
    int rejected_count = 0;
    for (int trial = 0; trial < 100; trial++) {
        rf_frame_t corrupted_frame;
        uint8_t *corrupt_ptr = (uint8_t *)&corrupted_frame;
        do {
            corrupted_frame = frame;
            int corrupt_bytes = 1 + (rand() % 5);
            for (int c = 0; c < corrupt_bytes; c++) {
                size_t pos = (size_t)(rand() % (int)RF_FRAME_LEN);
                uint8_t mask = (uint8_t)(1u << (rand() % 8));
                corrupt_ptr[pos] ^= mask;
            }
        } while (memcmp(&corrupted_frame, &frame, sizeof(rf_frame_t)) == 0);

        if (!aes_ccm_decrypt(&corrupted_frame, TEST_KEY_A, &dec_pkt)) {
            rejected_count++;
        }
    }
    printf("   -> %d / 100 corrupted frames rejected by AES-CCM authentication.\n", rejected_count);
    assert(rejected_count == 100 && "All corrupted frames must be rejected");
    printf("   -> 100%% rejection rate verified [PASS]\n\n");

    printf("========================================================================\n");
    printf("  ALL AES-128-CCM TAMPER & AUTHENTICATION TESTS PASSED [GREEN]\n");
    printf("========================================================================\n");
    return 0;
}
