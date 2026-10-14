#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "packet_protocol.h"
#include "rf_config.h"

int main(void) {
    printf("=== TEST DEF-12: MultiCeiver Per-Soldier Pipes 1-5 & Header Cross-Check ===\n");

    /* ── Subtest 1: MultiCeiver Pipe Address Constraints ─────────────────────── */
    printf("--- Subtest 1: MultiCeiver Physical Pipe Address Formatting ---\n");
    const uint8_t *p1 = RF_PIPE1_ADDR;
    const uint8_t *p2 = RF_PIPE2_ADDR;
    const uint8_t *p3 = RF_PIPE3_ADDR;
    const uint8_t *p4 = RF_PIPE4_ADDR;
    const uint8_t *p5 = RF_PIPE5_ADDR;

    /* Verify common 4 bytes (bytes 1-4) across pipes 1-5 required by nRF24 hardware */
    assert(memcmp(p1 + 1, p2 + 1, 4) == 0);
    assert(memcmp(p1 + 1, p3 + 1, 4) == 0);
    assert(memcmp(p1 + 1, p4 + 1, 4) == 0);
    assert(memcmp(p1 + 1, p5 + 1, 4) == 0);

    /* Verify distinct LSByte (byte 0, first SPI byte written) matching Soldier IDs 1-5 */
    assert(p1[0] == 1);
    assert(p2[0] == 2);
    assert(p3[0] == 3);
    assert(p4[0] == 4);
    assert(p5[0] == 5);
    printf("PASS: MultiCeiver physical addresses adhere to nRF24 LSByte and shared bytes 1-4 constraints.\n");

    /* ── Subtest 2: Valid Pipe-to-Soldier ID Matches ─────────────────────────── */
    printf("--- Subtest 2: Valid Pipe-to-Soldier ID Matches ---\n");
    for (uint8_t id = 1; id <= SOLDIER_ID_MAX; id++) {
        if (!rf_pipe_matches_soldier_id(id, id)) {
            fprintf(stderr, "FAIL: Legitimate pipe %u for Soldier %u was rejected!\n", id, id);
            return 1;
        }
    }
    printf("PASS: All 5 legitimate soldier pipes 1-5 accepted.\n");

    /* ── Subtest 3: Cross-Talk & Aliased Auto-ACK Rejection ───────────────────── */
    printf("--- Subtest 3: Cross-Talk & Mismatch Rejection ---\n");
    /* Frame intended for Soldier 2 arriving on Pipe 1 */
    if (rf_pipe_matches_soldier_id(1, 2)) {
        fprintf(stderr, "FAIL: Mismatched frame (pipe 1, soldier 2) was accepted!\n");
        return 1;
    }

    /* Frame intended for Soldier 1 arriving on Pipe 2 */
    if (rf_pipe_matches_soldier_id(2, 1)) {
        fprintf(stderr, "FAIL: Mismatched frame (pipe 2, soldier 1) was accepted!\n");
        return 1;
    }

    /* Frame claiming Soldier 1 arriving on Pipe 0 (TX / broadcast pipe) */
    if (rf_pipe_matches_soldier_id(0, 1)) {
        fprintf(stderr, "FAIL: Frame on Pipe 0 was accepted for Soldier 1!\n");
        return 1;
    }

    /* Invalid out-of-range soldier IDs */
    if (rf_pipe_matches_soldier_id(1, 0) || rf_pipe_matches_soldier_id(1, 6) || rf_pipe_matches_soldier_id(1, 255)) {
        fprintf(stderr, "FAIL: Out-of-range soldier ID was accepted!\n");
        return 1;
    }

    printf("PASS: All cross-talk, pipe mismatch, and out-of-range packets correctly rejected.\n");
    printf("====================================================================\n");
    printf("PASS: DEF-12 RX multi-pipe cross-check verification completed.\n");
    return 0;
}
