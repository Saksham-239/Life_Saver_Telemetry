/**
 * nvs_config.c — ESP-IDF NVS implementation for soldier key table
 * and asynchronous replay counter persistence with coarse high-water mark stride.
 */

#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG       = "nvs_cfg";
static const char *NAMESPACE = "sol_cfg";

typedef struct {
    uint8_t soldier_id;
    uint64_t counter;
    bool force_immediate;
} nvs_rx_req_t;

static QueueHandle_t s_nvs_queue = NULL;
static uint64_t s_persisted_hwm[SOLDIER_ID_MAX] = {0};

/* ── Key-name helpers ────────────────────────────────────────────────────── */
static void key_name(uint8_t soldier_id, char *buf, size_t len) {
    snprintf(buf, len, "key_%d", soldier_id);
}
static void ctr_name(uint8_t soldier_id, char *buf, size_t len) {
    snprintf(buf, len, "rxctr_%d", soldier_id);
}

/* ── Asynchronous NVS Writer Task ────────────────────────────────────────── */
static void task_nvs_writer(void *arg) {
    (void)arg;
    /* Pre-load persisted high-water marks */
    for (uint8_t i = 1; i <= SOLDIER_ID_MAX; i++) {
        s_persisted_hwm[i - 1] = nvs_config_get_rx_counter(i);
    }

    nvs_rx_req_t req;
    while (1) {
        if (xQueueReceive(s_nvs_queue, &req, portMAX_DELAY) == pdTRUE) {
            if (req.soldier_id < 1 || req.soldier_id > SOLDIER_ID_MAX) continue;
            uint8_t idx = req.soldier_id - 1;
            uint64_t last = s_persisted_hwm[idx];

            /* Coarse high-water mark stride: persist if counter exceeds last persisted by NVS_HWM_STRIDE,
             * or on the very first received counter (last == 0), or if force_immediate is requested (alarm packets). */
            if (req.force_immediate || req.counter >= last + NVS_HWM_STRIDE || last == 0) {
                if (nvs_config_save_rx_counter(req.soldier_id, req.counter)) {
                    s_persisted_hwm[idx] = req.counter;
                    ESP_LOGI(TAG, "Persisted NVS HWM for Soldier %d: %llu (last %llu, immediate=%d)",
                             req.soldier_id, (unsigned long long)req.counter, (unsigned long long)last, req.force_immediate);
                }
            }
        }
    }
}

void nvs_config_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated (firmware update) — erase and reinit */
        ESP_LOGW(TAG, "NVS truncated/corrupt — erasing partition");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: 0x%x", err);
            return;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x", err);
        return;
    }

    /* Start async NVS worker task and queue */
    if (!s_nvs_queue) {
        s_nvs_queue = xQueueCreate(16, sizeof(nvs_rx_req_t));
        if (s_nvs_queue) {
            xTaskCreate(task_nvs_writer, "NVS_Writer", 3072, NULL, 2, NULL);
        } else {
            ESP_LOGE(TAG, "Failed to create NVS request queue");
        }
    }
}

bool nvs_config_get_key(uint8_t soldier_id, uint8_t key_out[AES_KEY_LEN]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    char name[16];
    key_name(soldier_id, name, sizeof(name));
    size_t len = AES_KEY_LEN;
    err = nvs_get_blob(h, name, key_out, &len);
    nvs_close(h);
    if (err != ESP_OK || len != AES_KEY_LEN) {
        ESP_LOGW(TAG, "No key for soldier %d", soldier_id);
        return false;
    }
    return true;
}

bool nvs_config_set_key(uint8_t soldier_id, const uint8_t key[AES_KEY_LEN]) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed for set_key: 0x%x", err);
        return false;
    }
    char name[16];
    key_name(soldier_id, name, sizeof(name));
    err = nvs_set_blob(h, name, key, AES_KEY_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed for key_%d: 0x%x", soldier_id, err);
        return false;
    }
    return true;
}

uint64_t nvs_config_get_rx_counter(uint8_t soldier_id) {
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    char name[16];
    ctr_name(soldier_id, name, sizeof(name));
    uint64_t val = 0;
    nvs_get_u64(h, name, &val);
    nvs_close(h);
    return val;
}

bool nvs_config_save_rx_counter(uint8_t soldier_id, uint64_t counter) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed for save_rx_counter: 0x%x", err);
        return false;
    }
    char name[16];
    ctr_name(soldier_id, name, sizeof(name));
    err = nvs_set_u64(h, name, counter);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist rx counter for soldier %d: 0x%x", soldier_id, err);
        return false;
    }
    return true;
}

bool nvs_config_enqueue_rx_counter(uint8_t soldier_id, uint64_t counter, bool force_immediate) {
    if (soldier_id < 1 || soldier_id > SOLDIER_ID_MAX || !s_nvs_queue) return false;
    nvs_rx_req_t req = { .soldier_id = soldier_id, .counter = counter, .force_immediate = force_immediate };
    return xQueueSend(s_nvs_queue, &req, 0) == pdTRUE;
}
