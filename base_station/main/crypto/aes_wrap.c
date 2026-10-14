/**
 * aes_wrap.c — AES-128-CCM implementation for the ESP32-S3 base station.
 * Uses mbedTLS (bundled with ESP-IDF; hardware AES acceleration available).
 *
 * mbedTLS CCM known-answer tests:
 *   Run mbedtls/tests/test_suite_ccm before first cross-node decryption test.
 *   These are part of the mbedTLS test suite bundled with ESP-IDF.
 *
 * CCM parameters (must match soldier_unit/src/crypto/aes_wrap.c exactly):
 *   Key size:   AES_KEY_LEN = 16 bytes (AES-128)
 *   Tag length: CCM_TAG_LEN = 8 bytes
 *   Nonce:      CCM_NONCE_LEN = 13 bytes, L = 2
 *   AAD:        10-byte encoded packet_header_t
 *   Plaintext:  10-byte encoded packet_body_t
 */

#include "../../shared/include/aes_wrap.h"
#include "../../shared/src/packet_codec.h"
#include <mbedtls/ccm.h>
#include <string.h>

bool aes_ccm_encrypt(const packet_t  *pkt,
                     const uint8_t    key[AES_KEY_LEN],
                     rf_frame_t      *frame_out) {
    /* 1. Encode header (AAD) and body (plaintext) */
    uint8_t hdr_buf[PACKET_HEADER_LEN];
    packet_header_encode(&pkt->hdr, hdr_buf);

    uint8_t body_buf[PACKET_BODY_LEN];
    packet_body_encode(&pkt->body, body_buf);

    /* 2. Derive 13-byte nonce */
    uint8_t nonce[CCM_NONCE_LEN];
    packet_make_nonce(&pkt->hdr, CCM_DIR_SOLDIER_TO_BASE, nonce);

    /* 3. Set up mbedTLS CCM context */
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);

    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES,
                                  key, AES_KEY_LEN * 8u);  /* key length in bits */
    if (rc != 0) {
        mbedtls_ccm_free(&ctx);
        return false;
    }

    /*
     * mbedtls_ccm_encrypt_and_tag:
     *   ctx       — CCM context
     *   length    — plaintext length
     *   iv        — nonce
     *   iv_len    — nonce length (13)
     *   aad       — additional authenticated data
     *   aad_len   — AAD length
     *   input     — plaintext
     *   output    — ciphertext (same length as plaintext)
     *   tag       — output tag buffer
     *   tag_len   — CCM_TAG_LEN (8)
     */
    rc = mbedtls_ccm_encrypt_and_tag(
        &ctx,
        PACKET_BODY_LEN,
        nonce,      CCM_NONCE_LEN,
        hdr_buf,    PACKET_HEADER_LEN,
        body_buf,
        frame_out->ciphertext,
        frame_out->tag, CCM_TAG_LEN);

    mbedtls_ccm_free(&ctx);

    if (rc != 0) {
        return false;
    }

    /* 4. Copy the clear header into the frame */
    memcpy(&frame_out->header, hdr_buf, PACKET_HEADER_LEN);

    return true;
}

bool aes_ccm_decrypt(const rf_frame_t *frame,
                     const uint8_t     key[AES_KEY_LEN],
                     packet_t         *pkt_out) {
    /* 1. Decode and validate the clear header */
    uint8_t hdr_buf[PACKET_HEADER_LEN];
    memcpy(hdr_buf, &frame->header, PACKET_HEADER_LEN);

    if (!packet_header_decode(hdr_buf, &pkt_out->hdr)) {
        return false;
    }

    /* 2. Derive nonce */
    uint8_t nonce[CCM_NONCE_LEN];
    packet_make_nonce(&pkt_out->hdr, CCM_DIR_SOLDIER_TO_BASE, nonce);

    /* 3. mbedTLS CCM context */
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);

    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES,
                                  key, AES_KEY_LEN * 8u);
    if (rc != 0) {
        mbedtls_ccm_free(&ctx);
        return false;
    }

    /*
     * mbedtls_ccm_auth_decrypt:
     *   Decrypts and verifies the authentication tag in one call.
     *   Returns 0 on success; MBEDTLS_ERR_CCM_AUTH_FAILED on tag mismatch.
     */
    uint8_t body_buf[PACKET_BODY_LEN];
    rc = mbedtls_ccm_auth_decrypt(
        &ctx,
        PACKET_BODY_LEN,
        nonce,               CCM_NONCE_LEN,
        hdr_buf,             PACKET_HEADER_LEN,
        frame->ciphertext,
        body_buf,
        frame->tag,          CCM_TAG_LEN);

    mbedtls_ccm_free(&ctx);

    if (rc != 0) {
        /* MBEDTLS_ERR_CCM_AUTH_FAILED or parameter error */
        return false;
    }

    /* 4. Decode and validate plaintext body */
    return packet_body_decode(body_buf, &pkt_out->body);
}
