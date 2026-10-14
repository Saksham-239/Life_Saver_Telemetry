/**
 * main.c — Base Station (ESP32-S3) top-level entry point.
 *
 * Boot sequence:
 *   1. NVS init (for AES keys & RX counters).
 *   2. Provisioning check (UART0).
 *   3. Peripheral init: OLED, Buzzer, nRF24.
 *   4. Base SM init (loads counters from NVS for known soldiers).
 *   5. Task spawn: RadioRX, UI (Display + Buzzer logic).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Protocol */
#include "../../shared/include/packet_protocol.h"
#include "../../shared/include/aes_wrap.h"
#include "../../shared/include/rf_config.h"
#include "../../shared/src/packet_codec.h"

/* Config & State */
#include "config/nvs_config.h"
#include "config/base_provision.h"
#include "state/base_sm.h"
#include "system/logger.h"
#include "esp_task_wdt.h"

/* Peripherals */
#include "ui/oled.h"
#include "ui/buzzer.h"
#include "radio/radio_bs.h"

static const char *TAG = "main";

/* ── Test vectors self-test (from shared) ────────────────────────────────── */
extern int protocol_vectors_run_all(void);

/* ── Tasks ───────────────────────────────────────────────────────────────── */

/**
 * Task_RadioRx: continuously polls the nRF24 for incoming frames.
 * Upon reception, decodes the clear header to extract soldier_id,
 * fetches the AES key, decrypts, and feeds the Base SM.
 *
 * The clear header is parsed via packet_header_decode() to avoid
 * direct access to packed struct fields and to validate version/ID
 * before key lookup.
 */
static void task_radio_rx(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);

    rf_frame_t frame;
    packet_header_t pre_hdr;   /* Parsed from clear header before decrypt */
    packet_t pkt;
    uint8_t key[AES_KEY_LEN];
    uint8_t rx_pipe = 0;

    while (1) {
        esp_task_wdt_reset();

        /* Drain all available frames from the 3-deep RX FIFO before sleeping */
        while (radio_recv_raw_pipe(&frame, &rx_pipe)) {
            /* Decode and validate clear header (version + soldier_id range) */
            if (!packet_header_decode((const uint8_t *)&frame.header, &pre_hdr)) {
                ESP_LOGW(TAG, "Invalid clear header (bad version or soldier_id) — drop");
                continue;
            }

            uint8_t sid = pre_hdr.soldier_id;

            /* DEF-12 RX Cross-Check: physical RF pipe MUST match claimed soldier_id */
            if (!rf_pipe_matches_soldier_id(rx_pipe, sid)) {
                ESP_LOGW(TAG, "Pipe-ID mismatch: frame arrived on pipe %u but claimed soldier_id %u — drop",
                         rx_pipe, sid);
                continue;
            }
            if (!nvs_config_get_key(sid, key)) {
                ESP_LOGW(TAG, "No key provisioned for Soldier %d — drop", sid);
                continue;
            }

            if (!aes_ccm_decrypt(&frame, key, &pkt)) {
                ESP_LOGW(TAG, "Decrypt failed for Soldier %d (bad tag/key/nonce)", sid);
                continue;
            }

            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            base_sm_rx_packet(&pkt, now_ms);

            /* Enqueue replay counter persistence asynchronously off task_radio_rx.
             * Coarse high-water mark stride (NVS_HWM_STRIDE=100) filters flash commits
             * in background task_nvs_writer to protect flash endurance and eliminate
             * flash write latency from the radio RX polling loop. */
            soldier_record_t rec;
            if (base_sm_get_soldier(sid, &rec) && rec.last_rx_counter == pkt.hdr.persistent_tx_counter) {
                bool is_alarm_msg = (pkt.body.msg_type == MSG_DOWN ||
                                     pkt.body.msg_type == MSG_BACKUP ||
                                     pkt.body.msg_type == MSG_ENEMY ||
                                     pkt.body.msg_type == MSG_MEDICAL ||
                                     pkt.body.msg_type == MSG_FALL_PENDING ||
                                     pkt.body.msg_type == MSG_COMPLETE ||
                                     pkt.body.msg_type == MSG_FALL_CANCEL ||
                                     pkt.body.msg_type == MSG_EMERGENCY_CLEAR ||
                                     rec.alarm == ALARM_EMERGENCY ||
                                     rec.alarm == ALARM_ALERT_LATCHED);
                nvs_config_enqueue_rx_counter(sid, pkt.hdr.persistent_tx_counter, is_alarm_msg);
            }
        }

        /* No frames available — sleep before next poll */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/**
 * Task_UI: Handles regular OLED updates, SM tick, and buzzer alerts.
 */
static void task_ui(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);

    soldier_record_t soldiers[SOLDIER_ID_MAX];
    bool blink = false;
    
    while (1) {
        esp_task_wdt_reset();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        
        /* Advance SM (handles timeouts/link-loss) */
        base_sm_tick(now_ms);
        
        /* Get state snapshot */
        base_sm_get_state(soldiers);
        
        /* Determine highest severity for buzzer */
        bool has_emergency = false;
        bool has_alert = false;
        bool has_link_lost = false;
        
        for (int i = 0; i < SOLDIER_ID_MAX; i++) {
            if (soldiers[i].alarm == ALARM_EMERGENCY) has_emergency = true;
            else if (soldiers[i].alarm == ALARM_ALERT_LATCHED) has_alert = true;
            
            if (soldiers[i].base_rx_time_ms > 0 && soldiers[i].link == LINK_LOST) {
                has_link_lost = true;
            }
        }
        
        /* Update display */
        oled_render_status(soldiers, now_ms, blink);
        blink = !blink;
        
        /* Play buzzer pattern asynchronously (non-blocking via queue) */
        if (has_emergency) {
            buzzer_play(BUZZ_EMERGENCY);
        } else if (has_alert) {
            buzzer_play(BUZZ_ALERT);
        } else if (has_link_lost && blink) {
            buzzer_play(BUZZ_LINK_LOST);
        }

        /* Strictly bounded UI loop period: constant 500ms refresh cycle */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


/* ── Application Entry ───────────────────────────────────────────────────── */

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3 Base Station Boot ===");
    
    /* 1. NVS Init */
    nvs_config_init();
    
    /* 2. Provisioning */
    base_provision_check_and_run();
    
    /* 3. Logger Init */
    logger_init();
    
    /* 3. Codec Self-Test */
    if (protocol_vectors_run_all() != 0) {
        ESP_LOGE(TAG, "Codec unit tests failed! Halting.");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Codec tests PASSED.");
    
    /* 4. Peripheral Init */
    buzzer_init();
    
    if (!oled_init()) {
        ESP_LOGE(TAG, "OLED init failed. Check I2C wiring (SDA=8, SCL=9).");
    } else {
        oled_clear();
        ESP_LOGI(TAG, "OLED ready.");
    }
    
    if (!radio_init()) {
        ESP_LOGE(TAG, "nRF24L01 init failed. Halting.");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Radio ready.");
    
    /* 5. Base SM Init */
    base_sm_init();
    
    /* Pre-load RX counters from NVS */
    for (uint8_t i = 1; i <= SOLDIER_ID_MAX; i++) {
        uint64_t ctr = nvs_config_get_rx_counter(i);
        if (ctr > 0) {
            base_sm_set_rx_counter(i, ctr);
        }
    }
    
    /* 6. Launch Tasks */
    xTaskCreate(task_radio_rx, "RadioRx", 4096, NULL, 5, NULL);
    xTaskCreate(task_ui,       "UI",      4096, NULL, 4, NULL);
    
    ESP_LOGI(TAG, "Base station running.");
}
