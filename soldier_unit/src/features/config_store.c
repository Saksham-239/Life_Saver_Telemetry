#include "config_store.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if SOLDIER_MULTICORE_ENABLED
#error "Multicore flash write not implemented. Add cross-core lockout before enabling."
#endif

#define FLASH_TOTAL_SIZE      (2u * 1024u * 1024u)
#define CONFIG_SECTOR_A_OFF   (FLASH_TOTAL_SIZE - 2u * CONFIG_FLASH_SECTOR_SZ)
#define CONFIG_SECTOR_B_OFF   (FLASH_TOTAL_SIZE - 1u * CONFIG_FLASH_SECTOR_SZ)
#define CONFIG_XIP_BASE       0x10000000u

#define CFG_OFF_MAGIC         0u
#define CFG_OFF_SOLDIER_ID    1u
#define CFG_OFF_KEY           2u    /* 16 bytes */
#define CFG_OFF_COUNTER       18u   /* 8 bytes uint64_t */
#define CFG_OFF_SEQ           26u   /* 4 bytes uint32_t */
#define CFG_OFF_CRC16         30u   /* 2 bytes uint16_t */
#define CFG_USED_BYTES        32u

static uint64_t s_session_base     = 0;
static uint64_t s_session_current  = 0;
static uint64_t s_session_ceiling  = 0;

static uint32_t s_active_sector_offset = 0;
static uint32_t s_active_seq = 0;
static bool     s_is_provisioned = false;

static uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void write_u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static const uint8_t *flash_sector_ptr(uint32_t offset) {
    return (const uint8_t *)((uintptr_t)CONFIG_XIP_BASE + offset);
}

static bool parse_sector(uint32_t offset, soldier_config_t *cfg, uint32_t *seq_out) {
    const uint8_t *p = flash_sector_ptr(offset);
    if (p[CFG_OFF_MAGIC] != CONFIG_MAGIC) {
        return false;
    }
    uint16_t stored_crc = read_u16_le(&p[CFG_OFF_CRC16]);
    uint16_t computed   = crc16(p, CFG_OFF_CRC16);
    if (stored_crc != computed) {
        return false;
    }
    if (p[CFG_OFF_SOLDIER_ID] < 1 || p[CFG_OFF_SOLDIER_ID] > SOLDIER_ID_MAX) {
        return false;
    }
    cfg->soldier_id = p[CFG_OFF_SOLDIER_ID];
    memcpy(cfg->aes_key, &p[CFG_OFF_KEY], AES_KEY_LEN);
    cfg->counter_ceiling = read_u64_le(&p[CFG_OFF_COUNTER]);
    *seq_out = read_u32_le(&p[CFG_OFF_SEQ]);
    return true;
}

bool config_store_is_provisioned(void) {
    return s_is_provisioned;
}

uint32_t config_store_get_active_seq(void) {
    return s_active_seq;
}

uint32_t config_store_get_active_offset(void) {
    return s_active_sector_offset;
}

bool config_store_save_raw(uint32_t target_offset, const soldier_config_t *cfg, uint32_t seq) {
    uint8_t buf[FLASH_PAGE_SIZE];
    memset(buf, 0xFF, sizeof(buf));

    buf[CFG_OFF_MAGIC] = CONFIG_MAGIC;
    buf[CFG_OFF_SOLDIER_ID] = cfg->soldier_id;
    memcpy(&buf[CFG_OFF_KEY], cfg->aes_key, AES_KEY_LEN);
    write_u64_le(&buf[CFG_OFF_COUNTER], cfg->counter_ceiling);
    write_u32_le(&buf[CFG_OFF_SEQ], seq);

    uint16_t computed = crc16(buf, CFG_OFF_CRC16);
    buf[CFG_OFF_CRC16]     = (uint8_t)(computed & 0xFF);
    buf[CFG_OFF_CRC16 + 1] = (uint8_t)(computed >> 8);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(target_offset, CONFIG_FLASH_SECTOR_SZ);
    flash_range_program(target_offset, buf, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    const uint8_t *readback = flash_sector_ptr(target_offset);
    uint16_t rb_crc = crc16(readback, CFG_OFF_CRC16);
    uint16_t rb_stored = read_u16_le(&readback[CFG_OFF_CRC16]);
    if (rb_crc != rb_stored || rb_crc != computed) {
        printf("[ERROR] Flash read-back CRC mismatch at offset 0x%08lX\n", (unsigned long)target_offset);
        return false;
    }

    s_active_sector_offset = target_offset;
    s_active_seq = seq;
    s_is_provisioned = true;
    return true;
}

bool config_store_save(const soldier_config_t *cfg) {
    uint32_t target_offset = (s_active_sector_offset == CONFIG_SECTOR_A_OFF) ?
                             CONFIG_SECTOR_B_OFF : CONFIG_SECTOR_A_OFF;
    uint32_t next_seq = s_is_provisioned ? (s_active_seq + 1) : 1;
    return config_store_save_raw(target_offset, cfg, next_seq);
}

bool config_store_load(soldier_config_t *cfg) {
    soldier_config_t cfg_a, cfg_b;
    uint32_t seq_a = 0, seq_b = 0;
    bool valid_a = parse_sector(CONFIG_SECTOR_A_OFF, &cfg_a, &seq_a);
    bool valid_b = parse_sector(CONFIG_SECTOR_B_OFF, &cfg_b, &seq_b);

    bool use_a = false;
    bool found_valid = false;

    if (valid_a && valid_b) {
        use_a = (seq_a >= seq_b);
        found_valid = true;
    } else if (valid_a) {
        use_a = true;
        found_valid = true;
    } else if (valid_b) {
        use_a = false;
        found_valid = true;
    }

    if (!found_valid) {
        s_session_base    = 0;
        s_session_current = 0;
        s_session_ceiling = 0;
        s_is_provisioned  = false;
        memset(cfg, 0, sizeof(*cfg));
        printf("[FATAL] No valid provisioned config in flash! System fails closed to prevent nonce reuse.\n");
        return false;
    }

    soldier_config_t selected = use_a ? cfg_a : cfg_b;
    uint32_t selected_seq     = use_a ? seq_a : seq_b;
    s_active_sector_offset    = use_a ? CONFIG_SECTOR_A_OFF : CONFIG_SECTOR_B_OFF;
    s_active_seq              = selected_seq;
    s_is_provisioned          = true;

    /* Reserve counter block for this session */
    s_session_base    = selected.counter_ceiling;
    s_session_current = s_session_base;
    s_session_ceiling = s_session_base + COUNTER_BLOCK_SIZE;

    soldier_config_t updated = selected;
    updated.counter_ceiling  = s_session_ceiling;

    /* Save reservation to standby sector */
    uint32_t standby_offset = use_a ? CONFIG_SECTOR_B_OFF : CONFIG_SECTOR_A_OFF;
    if (!config_store_save_raw(standby_offset, &updated, selected_seq + 1)) {
        printf("[FATAL] Counter reservation flash write failed! Halting.\n");
        s_session_base    = 0;
        s_session_current = 0;
        s_session_ceiling = 0;
        while (true) {
            watchdog_update();
            sleep_ms(100);
        }
        return false;
    }

    *cfg = updated;
    return true;
}

uint64_t config_store_session_base(void) {
    return s_session_base;
}

bool config_store_next_counter(uint64_t *counter_out) {
    if (!s_is_provisioned || s_session_ceiling == 0 || s_session_current >= s_session_ceiling) {
        return false;
    }
    *counter_out = s_session_current++;
    return true;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        out[i] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
}

static void print_key_masked(const uint8_t *key, bool first_set) {
    if (!first_set) {
        printf("(not set)\n");
    } else {
        printf("%02X...%02X (set — full key hidden after entry)\n",
               key[0], key[AES_KEY_LEN - 1]);
    }
}

void config_provisioning_run(soldier_config_t *cfg) {
    bool key_was_set = false;
    char line[128];

    printf("\n");
    printf("========================================\n");
    printf("  RP2040 Soldier Unit — Provisioning\n");
    printf("========================================\n");
    printf("Commands:\n");
    printf("  id <1-5>         Set soldier ID\n");
    printf("  key <32 hex>     Set AES-128 key (32 hex chars, no spaces)\n");
    printf("  counter <n>      Set counter ceiling (initial provisioning)\n");
    printf("  show             Display current config\n");
    printf("  save             Persist to flash and exit\n");
    printf("  exit             Discard changes and exit\n");
    printf("----------------------------------------\n");

    while (true) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (strncmp(line, "id ", 3) == 0) {
            long v = strtol(line + 3, NULL, 10);
            if (v < 1 || v > (long)SOLDIER_ID_MAX) {
                printf("Error: id must be 1–%d\n", SOLDIER_ID_MAX);
            } else {
                cfg->soldier_id = (uint8_t)v;
                printf("soldier_id = %d\n", cfg->soldier_id);
            }
        } else if (strncmp(line, "key ", 4) == 0) {
            const char *hex = line + 4;
            if (strlen(hex) != AES_KEY_LEN * 2) {
                printf("Error: key must be exactly %d hex chars (got %zu)\n",
                       AES_KEY_LEN * 2, strlen(hex));
            } else {
                hex_to_bytes(hex, cfg->aes_key, AES_KEY_LEN);
                key_was_set = true;
                printf("Key set.\n");
            }
        } else if (strncmp(line, "counter ", 8) == 0) {
            unsigned long long v = strtoull(line + 8, NULL, 10);
            cfg->counter_ceiling = (uint64_t)v;
            printf("counter_ceiling = %llu\n", (unsigned long long)cfg->counter_ceiling);
        } else if (strcmp(line, "show") == 0) {
            printf("soldier_id      : %d\n", cfg->soldier_id);
            printf("key             : ");
            print_key_masked(cfg->aes_key, key_was_set);
            printf("counter_ceiling : %llu\n", (unsigned long long)cfg->counter_ceiling);
        } else if (strcmp(line, "save") == 0) {
            bool ok = config_store_save(cfg);
            if (ok) {
                printf("Config saved to flash (verified).\n");
            } else {
                printf("[ERROR] Flash write verification failed!\n");
            }
            printf("Exiting provisioning mode.\n\n");
            return;
        } else if (strcmp(line, "exit") == 0) {
            printf("Discarding changes. Exiting provisioning mode.\n\n");
            return;
        } else if (strlen(line) > 0) {
            printf("Unknown command: '%s'\n", line);
        }
    }
}
