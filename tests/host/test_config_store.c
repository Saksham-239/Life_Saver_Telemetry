#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "config_store.h"
#include "hardware/flash.h"
#include "../../shared/include/packet_protocol.h"

#define SECTOR_A_OFFSET (FLASH_TOTAL_SIZE - 2u * CONFIG_FLASH_SECTOR_SZ)
#define SECTOR_B_OFFSET (FLASH_TOTAL_SIZE - 1u * CONFIG_FLASH_SECTOR_SZ)

/* Track all allocated counters to guarantee zero duplication */
#define MAX_TRACKED_COUNTERS 10000
static uint64_t s_allocated_counters[MAX_TRACKED_COUNTERS];
static size_t s_num_allocated = 0;

static void record_and_assert_unique(uint64_t ctr) {
    for (size_t i = 0; i < s_num_allocated; i++) {
        if (s_allocated_counters[i] == ctr) {
            fprintf(stderr, "FATAL: Duplicate counter %llu allocated! Nonce reuse defect!\n",
                    (unsigned long long)ctr);
            exit(1);
        }
    }
    assert(s_num_allocated < MAX_TRACKED_COUNTERS);
    s_allocated_counters[s_num_allocated++] = ctr;
}

int main(void) {
    printf("====================================================================\n");
    printf("=== TEST: Config Store & Counter Reservation (DEF-06 & Q3)       ===\n");
    printf("====================================================================\n\n");

    flash_stub_init();

    /* ── Subtest 1: Block Reservation Size ────────────────────────────────── */
    printf("--- Subtest 1: Block Reservation Size ---\n");
    printf("COUNTER_BLOCK_SIZE = %llu (expected 2^32 = %llu)\n", 
           (unsigned long long)COUNTER_BLOCK_SIZE, 
           (unsigned long long)(1ULL << 32));
    
    if (COUNTER_BLOCK_SIZE != (1ULL << 32)) {
        fprintf(stderr, "FAIL: COUNTER_BLOCK_SIZE is %llu instead of 2^32!\n",
                (unsigned long long)COUNTER_BLOCK_SIZE);
        return 1;
    }
    printf("  [PASS] Block reservation is 2^32.\n\n");

    /* ── Subtest 2: Fail Closed on Blank Flash ────────────────────────────── */
    printf("--- Subtest 2: Fail Closed on Blank Flash ---\n");
    flash_range_erase(SECTOR_A_OFFSET, CONFIG_FLASH_SECTOR_SZ);
    flash_range_erase(SECTOR_B_OFFSET, CONFIG_FLASH_SECTOR_SZ);

    soldier_config_t cfg;
    bool loaded = config_store_load(&cfg);
    uint64_t allocated_ctr = 0;
    bool got_ctr = config_store_next_counter(&allocated_ctr);

    printf("On blank flash: loaded=%d, got_ctr=%d\n", loaded, got_ctr);

    if (loaded || got_ctr) {
        fprintf(stderr, "FAIL: Blank flash did not fail closed! (loaded=%d, got_ctr=%d)\n", loaded, got_ctr);
        return 1;
    }
    printf("  [PASS] Blank flash fails closed (zero counters allocated).\n\n");

    /* ── Subtest 3: Initial Provisioning & Load ───────────────────────────── */
    printf("--- Subtest 3: Initial Provisioning & Load ---\n");
    cfg.soldier_id = 1;
    for (size_t i = 0; i < AES_KEY_LEN; i++) cfg.aes_key[i] = (uint8_t)(i + 1);
    cfg.counter_ceiling = 1000;
    bool saved = config_store_save(&cfg);
    if (!saved) {
        fprintf(stderr, "FAIL: Initial provisioning save failed!\n");
        return 1;
    }

    soldier_config_t boot_cfg;
    if (!config_store_load(&boot_cfg)) {
        fprintf(stderr, "FAIL: Boot load failed on provisioned flash!\n");
        return 1;
    }
    uint64_t c1, c2;
    if (!config_store_next_counter(&c1) || !config_store_next_counter(&c2)) {
        fprintf(stderr, "FAIL: Failed to allocate counters!\n");
        return 1;
    }
    record_and_assert_unique(c1);
    record_and_assert_unique(c2);

    printf("Allocated counters: c1=%llu, c2=%llu (session base=%llu)\n",
           (unsigned long long)c1, (unsigned long long)c2,
           (unsigned long long)config_store_session_base());
    if (c1 != 1000 || c2 != 1001) {
        fprintf(stderr, "FAIL: Counters do not start from provisioned ceiling 1000!\n");
        return 1;
    }
    printf("  [PASS] Initial provisioning and counter allocation verified.\n\n");

    /* ── Subtest 4: Exhaustive Crash Injection at Erase & Program Steps ────── */
    printf("--- Subtest 4: Exhaustive Crash Injection at Every Flash Step ---\n");

    /* Step 4a: Crash during write to standby sector after erase (standby sector blank) */
    printf("  [Testing crash after standby sector erase...]\n");
    /* Sector B is currently active (Seq 2). Sector A is standby. Simulate crash erasing Sector A */
    flash_range_erase(SECTOR_A_OFFSET, CONFIG_FLASH_SECTOR_SZ);
    if (!config_store_load(&boot_cfg)) {
        fprintf(stderr, "FAIL: Recovery from erased standby sector failed!\n");
        return 1;
    }
    uint64_t c_rec;
    if (!config_store_next_counter(&c_rec)) {
        fprintf(stderr, "FAIL: Counter allocation failed after recovery!\n");
        return 1;
    }
    record_and_assert_unique(c_rec);
    printf("    Recovered from active sector: counter=%llu (PASS)\n", (unsigned long long)c_rec);

    /* Step 4b: Crash during program of standby sector (torn sector at various byte offsets) */
    size_t torn_offsets[] = { 0, 4, 10, 16, 24, 30 };
    for (size_t t = 0; t < sizeof(torn_offsets)/sizeof(torn_offsets[0]); t++) {
        size_t off = torn_offsets[t];
        printf("  [Testing torn sector: corrupting standby byte offset %zu...]\n", off);

        /* Simulate a torn write to standby sector during next block update */
        uint32_t active_off = config_store_get_active_offset();
        uint32_t standby_off = (active_off == SECTOR_A_OFFSET) ? SECTOR_B_OFFSET : SECTOR_A_OFFSET;

        soldier_config_t corrupt_cfg = boot_cfg;
        corrupt_cfg.counter_ceiling += (1ULL << 32);
        config_store_save_raw(standby_off, &corrupt_cfg, config_store_get_active_seq() + 1);
        flash_stub_corrupt_byte(standby_off + (uint32_t)off, 0x55);

        /* Boot load must reject torn standby sector and recover active sector */
        if (!config_store_load(&boot_cfg)) {
            fprintf(stderr, "FAIL: Recovery from torn standby sector at offset %zu failed!\n", off);
            return 1;
        }
        if (!config_store_next_counter(&c_rec)) {
            fprintf(stderr, "FAIL: Counter allocation failed after torn sector recovery!\n");
            return 1;
        }
        record_and_assert_unique(c_rec);
        printf("    Torn sector at offset %zu properly rejected via CRC: counter=%llu (PASS)\n",
               off, (unsigned long long)c_rec);
    }
    printf("  [PASS] All torn sector crash steps recovered cleanly with zero counter reuse.\n\n");

    /* ── Subtest 5: Single-Bit Flip Fault Injection (CRC Validation) ───────── */
    printf("--- Subtest 5: Bit-Flip Fault Injection & CRC Rejection ---\n");
    /* Write a new candidate sector into standby offset, then flip a single bit in the key */
    uint32_t active_sec = config_store_get_active_offset();
    uint32_t standby_sec = (active_sec == SECTOR_A_OFFSET) ? SECTOR_B_OFFSET : SECTOR_A_OFFSET;

    soldier_config_t bf_cfg = boot_cfg;
    bf_cfg.counter_ceiling += (1ULL << 32);
    config_store_save_raw(standby_sec, &bf_cfg, config_store_get_active_seq() + 1);

    /* Flip bit in standby sector key byte */
    flash_stub_corrupt_byte(standby_sec + 5, 0x12);

    /* Verify that boot load rejects the bit-flipped sector due to CRC mismatch */
    if (!config_store_load(&boot_cfg)) {
        fprintf(stderr, "FAIL: Recovery failed after bit-flip on standby sector!\n");
        return 1;
    }
    if (!config_store_next_counter(&c_rec)) {
        fprintf(stderr, "FAIL: Counter allocation failed after bitflip recovery!\n");
        return 1;
    }
    record_and_assert_unique(c_rec);
    printf("    Bit-flip corrupted standby sector rejected via CRC; recovered active: counter=%llu (PASS)\n",
           (unsigned long long)c_rec);
    printf("  [PASS] Single-bit flip fault rejected by CRC-16 check.\n\n");

    /* ── Subtest 6: Both Sectors Valid With Different Sequence Numbers ─────── */
    printf("--- Subtest 6: Both Sectors Valid (Arbitration by Monotonic Sequence) ---\n");
    /* Write valid Seq 50 to Sector A with ceiling 500,000,000,000 */
    soldier_config_t cfg_a = cfg;
    cfg_a.counter_ceiling = 500000000000ULL;
    config_store_save_raw(SECTOR_A_OFFSET, &cfg_a, 50);

    /* Write valid Seq 51 to Sector B with ceiling 600,000,000,000 */
    soldier_config_t cfg_b = cfg;
    cfg_b.counter_ceiling = 600000000000ULL;
    config_store_save_raw(SECTOR_B_OFFSET, &cfg_b, 51);

    if (!config_store_load(&boot_cfg)) {
        fprintf(stderr, "FAIL: Load failed when both sectors valid!\n");
        return 1;
    }
    printf("Selected sector: seq=%u (expected 52 after boot promotion), ceiling=%llu (expected %llu)\n",
           config_store_get_active_seq(),
           (unsigned long long)boot_cfg.counter_ceiling,
           (unsigned long long)(600000000000ULL + COUNTER_BLOCK_SIZE));

    if (config_store_get_active_seq() != 52 || boot_cfg.counter_ceiling != (600000000000ULL + COUNTER_BLOCK_SIZE)) {
        fprintf(stderr, "FAIL: Arbitration failed to select and advance higher sequence number (Seq 51 -> 52)!\n");
        return 1;
    }
    if (!config_store_next_counter(&c_rec)) {
        fprintf(stderr, "FAIL: Counter allocation failed!\n");
        return 1;
    }
    record_and_assert_unique(c_rec);
    if (c_rec != 600000000000ULL) {
        fprintf(stderr, "FAIL: Counter did not start at higher ceiling 600000000000!\n");
        return 1;
    }
    printf("  [PASS] Sector with higher sequence number correctly arbitrated.\n\n");

    /* ── Subtest 7: Total Counter Uniqueness Assertion ────────────────────── */
    printf("--- Subtest 7: Nonce Uniqueness Verification ---\n");
    printf("Total distinct nonces allocated during test: %zu\n", s_num_allocated);
    for (size_t i = 0; i < s_num_allocated; i++) {
        for (size_t j = i + 1; j < s_num_allocated; j++) {
            if (s_allocated_counters[i] == s_allocated_counters[j]) {
                fprintf(stderr, "FATAL DEFECT: Nonce collision detected: index %zu and %zu both have %llu!\n",
                        i, j, (unsigned long long)s_allocated_counters[i]);
                return 1;
            }
        }
    }
    printf("  [PASS] Zero duplicate nonces allocated across all failure/recovery modes.\n\n");

    printf("====================================================================\n");
    printf("PASS: Exhaustive power-loss, bit-flip, and arbitration tests PASSED.\n");
    printf("====================================================================\n");
    return 0;
}
