/**
 * nvs_config.h/c — ESP32-S3 NVS storage for per-soldier AES-128 keys
 * and the received-counter ceiling for replay detection.
 *
 * NVS namespace: "sol_cfg"
 * Keys per soldier (soldier_id = 1..SOLDIER_ID_MAX):
 *   "key_N"     — 16-byte AES key (blob)
 *   "rxctr_N"   — uint64 last accepted RX counter (replay guard)
 *
 * Provisioning:
 *   nvs_config_set_key(id, key) — called from serial provisioning menu
 *   nvs_config_save_rx_counter(id, ctr) — called after each valid RX
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../../../shared/include/packet_protocol.h"

/**
 * Coarse high-water mark stride for NVS persistence.
 * Persisting every 100 packets reduces flash wear by 99% and bounds
 * potential replay exposure on ungraceful reboot to < 100 packets.
 */
#define NVS_HWM_STRIDE 100ULL

/**
 * Initialise NVS flash partition and launch background NVS writer task.
 * Call once from app_main() before any other nvs_config_* call.
 */
void nvs_config_init(void);

/**
 * Retrieve the AES-128 key for the given soldier_id.
 * @param soldier_id  1–SOLDIER_ID_MAX
 * @param key_out     Output: 16-byte key buffer.
 * @return true if a key is stored for this soldier; false if unconfigured.
 */
bool nvs_config_get_key(uint8_t soldier_id, uint8_t key_out[AES_KEY_LEN]);

/**
 * Store or overwrite the AES-128 key for the given soldier_id.
 * Called from the serial provisioning menu.
 */
bool nvs_config_set_key(uint8_t soldier_id, const uint8_t key[AES_KEY_LEN]);

/**
 * Get the last accepted RX counter for a soldier (replay guard).
 * Returns 0 if no counter has been saved yet.
 */
uint64_t nvs_config_get_rx_counter(uint8_t soldier_id);

/**
 * Persist an updated RX counter ceiling immediately (synchronous).
 * Used during provisioning or shutdown.
 */
bool nvs_config_save_rx_counter(uint8_t soldier_id, uint64_t counter);

/**
 * Enqueue an updated RX counter to background worker task (non-blocking).
 * Coarse high-water mark stride filters flash writes to avoid flash wear
 * and completely decouples flash I/O latency from task_radio_rx.
 */
bool nvs_config_enqueue_rx_counter(uint8_t soldier_id, uint64_t counter, bool force_immediate);
