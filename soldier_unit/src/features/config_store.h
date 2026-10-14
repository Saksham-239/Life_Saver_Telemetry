#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../shared/include/packet_protocol.h"

#define SOLDIER_MULTICORE_ENABLED  0

#define CONFIG_MAGIC            0xA5u
#define CONFIG_HOLD_ENTER_MS    3000u
#define CONFIG_FLASH_SECTOR_SZ  4096u

typedef struct {
    uint8_t  soldier_id;
    uint8_t  aes_key[AES_KEY_LEN];
    uint64_t counter_ceiling;
} soldier_config_t;

bool config_store_load(soldier_config_t *cfg);
uint64_t config_store_session_base(void);
bool config_store_next_counter(uint64_t *counter_out);
bool config_store_save(const soldier_config_t *cfg);
bool config_store_save_raw(uint32_t target_offset, const soldier_config_t *cfg, uint32_t seq);
uint32_t config_store_get_active_seq(void);
uint32_t config_store_get_active_offset(void);
void config_provisioning_run(soldier_config_t *cfg);
bool config_store_is_provisioned(void);

#endif /* CONFIG_STORE_H */
