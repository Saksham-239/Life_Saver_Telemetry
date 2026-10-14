/**
 * aes_wrap.c — AES-128-CCM implementation for the RP2040 soldier unit.
 * Uses tinycrypt (Intel/Zephyr, MIT license).
 *
 * Dependency: soldier_unit/lib/tinycrypt/
 *   Source: https://github.com/intel/tinycrypt
 *   Files needed: lib/source/aes.c, lib/source/ccm_mode.c, lib/source/utils.c
 *                 lib/include/tinycrypt/aes.h, ccm_mode.h, constants.h
 *
 * tinycrypt CCM known-answer tests:
 *   Run tests/test_ccm_mode.c from the tinycrypt tree on the RP2040 during
 *   bring-up (before any cross-node decryption test) to confirm the library
 *   is correctly wired.  tinycrypt ships these KATs; don't skip them.
 *
 * CCM parameters (must match base_station/main/crypto/aes_wrap.c exactly):
 *   Key size:   16 bytes (AES-128)
 *   Tag length: CCM_TAG_LEN = 8 bytes
 *   Nonce:      CCM_NONCE_LEN = 13 bytes, L = 2 (max plaintext 65535 bytes)
 *   AAD:        10-byte encoded packet_header_t
 *   Plaintext:  10-byte encoded packet_body_t
 */

#include "../../shared/include/aes_wrap.h"
#include "../../shared/src/packet_codec.h"
#include <tinycrypt/ccm_mode.h>
#include <tinycrypt/aes.h>
#include <tinycrypt/constants.h>
#include <string.h>

/* tinycrypt uses TC_CCM_SECURITY_PARAMETER = (tag_len / 2). For 8-byte tag: M=4 */
#define TC_M_PARAMETER  4   /* (CCM_TAG_LEN / 2) = 4 */

bool aes_ccm_encrypt(const packet_t  *pkt,
                     const uint8_t    key[AES_KEY_LEN],
                     rf_frame_t      *frame_out) {
    /* 1. Encode header into AAD buffer (also the clear portion of the frame) */
    uint8_t hdr_buf[PACKET_HEADER_LEN];
    packet_header_encode(&pkt->hdr, hdr_buf);

    /* 2. Encode body into plaintext buffer */
    uint8_t body_buf[PACKET_BODY_LEN];
    packet_body_encode(&pkt->body, body_buf);

    /* 3. Derive 13-byte nonce */
    uint8_t nonce[CCM_NONCE_LEN];
    packet_make_nonce(&pkt->hdr, CCM_DIR_SOLDIER_TO_BASE, nonce);

    /* 4. Set up tinycrypt AES key schedule */
    struct tc_aes_key_sched_struct sched;
    if (tc_aes128_set_encrypt_key(&sched, key) != TC_CRYPTO_SUCCESS) {
        return false;
    }

    /* 5. Set up tinycrypt CCM context */
    struct tc_ccm_mode_struct ccm;
    /*
     * tc_ccm_config parameters:
     *   c        — CCM context
     *   sched    — AES key schedule
     *   nonce    — nonce buffer (must be exactly 13 bytes for L=2)
     *   nlen     — nonce length (13)
     *   mlen     — tag length in bytes (CCM_TAG_LEN = 8)
     *
     * tinycrypt derives L from nlen: L = 15 - nlen = 15 - 13 = 2.
     */
    if (tc_ccm_config(&ccm, &sched, nonce, CCM_NONCE_LEN, CCM_TAG_LEN)
            != TC_CRYPTO_SUCCESS) {
        return false;
    }

    /*
     * 6. Output buffer layout for tc_ccm_generation_encryption:
     *    The function writes: encrypted_body || tag
     *    into an output buffer of size PACKET_BODY_LEN + CCM_TAG_LEN = 18 bytes.
     *
     *    We write directly into frame_out->ciphertext and the immediately
     *    following frame_out->tag (they are contiguous in the packed struct).
     */
    uint8_t ccm_out[PACKET_BODY_LEN + CCM_TAG_LEN];
    int rc = tc_ccm_generation_encryption(
        ccm_out,                /* output: ciphertext || tag                */
        sizeof(ccm_out),        /* output buffer length                     */
        hdr_buf,                /* aad: authenticated but not encrypted      */
        PACKET_HEADER_LEN,      /* aad length                               */
        body_buf,               /* plaintext                                */
        PACKET_BODY_LEN,        /* plaintext length                         */
        &ccm);

    if (rc != TC_CRYPTO_SUCCESS) {
        return false;
    }

    /* 7. Assemble rf_frame_t */
    memcpy(&frame_out->header,    hdr_buf,              PACKET_HEADER_LEN);
    memcpy(frame_out->ciphertext, ccm_out,              PACKET_BODY_LEN);
    memcpy(frame_out->tag,        ccm_out + PACKET_BODY_LEN, CCM_TAG_LEN);

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

    /* 2. Derive nonce from the decoded header */
    uint8_t nonce[CCM_NONCE_LEN];
    packet_make_nonce(&pkt_out->hdr, CCM_DIR_SOLDIER_TO_BASE, nonce);

    /* 3. AES key schedule */
    struct tc_aes_key_sched_struct sched;
    if (tc_aes128_set_encrypt_key(&sched, key) != TC_CRYPTO_SUCCESS) {
        return false;
    }

    /* 4. CCM context */
    struct tc_ccm_mode_struct ccm;
    if (tc_ccm_config(&ccm, &sched, nonce, CCM_NONCE_LEN, CCM_TAG_LEN)
            != TC_CRYPTO_SUCCESS) {
        return false;
    }

    /*
     * 5. Reassemble ciphertext || tag into a contiguous buffer for tinycrypt.
     *    tc_ccm_decryption_verification expects: ciphertext || tag as input.
     */
    uint8_t ccm_in[PACKET_BODY_LEN + CCM_TAG_LEN];
    memcpy(ccm_in,                   frame->ciphertext, PACKET_BODY_LEN);
    memcpy(ccm_in + PACKET_BODY_LEN, frame->tag,        CCM_TAG_LEN);

    uint8_t body_buf[PACKET_BODY_LEN];
    int rc = tc_ccm_decryption_verification(
        body_buf,            /* output: decrypted plaintext           */
        PACKET_BODY_LEN,     /* output buffer length                  */
        hdr_buf,             /* aad                                   */
        PACKET_HEADER_LEN,   /* aad length                            */
        ccm_in,              /* input: ciphertext || tag              */
        sizeof(ccm_in),      /* input length                          */
        &ccm);

    if (rc != TC_CRYPTO_SUCCESS) {
        /* Authentication failed — tag mismatch, wrong key, or replay */
        return false;
    }

    /* 6. Decode and validate plaintext body */
    return packet_body_decode(body_buf, &pkt_out->body);
}
