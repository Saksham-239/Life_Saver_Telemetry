#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "../../../shared/include/packet_protocol.h"
#include "../../../shared/include/test_vectors.h"
#include "../../../shared/include/aes_wrap.h"

static const char *TAG = "test_ccm_esp32";

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 mbedTLS CCM Test ===");

    packet_t pkt = {
        .hdr = {
            .version = GOLDEN_VERSION,
            .soldier_id = GOLDEN_SOLDIER_ID,
            .persistent_tx_counter = GOLDEN_TX_COUNTER
        },
        .body = {
            .msg_type = GOLDEN_MSG_TYPE,
            .sensor_flags = GOLDEN_FLAGS,
            .battery_pct = GOLDEN_BATTERY,
            .prev_tx_retries = GOLDEN_RETRIES,
            .sender_uptime_ms = GOLDEN_UPTIME,
            .event_id = GOLDEN_EVENT_ID
        }
    };

    rf_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    if (!aes_ccm_encrypt(&pkt, GOLDEN_KEY, &frame)) {
        ESP_LOGE(TAG, "FAIL: aes_ccm_encrypt returned false");
        return;
    }

    ESP_LOGI(TAG, "ENCRYPTED FRAME (28 bytes):");
    uint8_t *f_ptr = (uint8_t*)&frame;
    for (int i = 0; i < RF_FRAME_LEN; i++) {
        printf("%02X ", f_ptr[i]);
    }
    printf("\n");

    if (memcmp(&frame, GOLDEN_FRAME, RF_FRAME_LEN) != 0) {
        ESP_LOGE(TAG, "FAIL: Encrypted frame does not match GOLDEN_FRAME!");
        return;
    }
    ESP_LOGI(TAG, "PASS: Encrypted frame matches GOLDEN_FRAME.");

    /* Decrypt back */
    packet_t dec_pkt;
    memset(&dec_pkt, 0, sizeof(dec_pkt));
    
    if (!aes_ccm_decrypt(&frame, GOLDEN_KEY, &dec_pkt)) {
        ESP_LOGE(TAG, "FAIL: aes_ccm_decrypt returned false");
        return;
    }

    if (dec_pkt.body.event_id != GOLDEN_EVENT_ID) {
        ESP_LOGE(TAG, "FAIL: Decrypted payload mismatch");
        return;
    }

    /* Negative test 1: wrong key */
    uint8_t bad_key[16];
    memcpy(bad_key, GOLDEN_KEY, 16);
    bad_key[0] ^= 0x01;
    if (aes_ccm_decrypt(&frame, bad_key, &dec_pkt)) {
        ESP_LOGE(TAG, "FAIL: Negative test 1 (wrong key) succeeded inappropriately");
        return;
    }

    /* Negative test 2: flipped ciphertext bit */
    rf_frame_t bad_frame;
    memcpy(&bad_frame, &frame, sizeof(rf_frame_t));
    bad_frame.ciphertext[0] ^= 0x01;
    if (aes_ccm_decrypt(&bad_frame, GOLDEN_KEY, &dec_pkt)) {
        ESP_LOGE(TAG, "FAIL: Negative test 2 (corrupted ciphertext) succeeded inappropriately");
        return;
    }

    /* Negative test 3: corrupted AAD (header) */
    memcpy(&bad_frame, &frame, sizeof(rf_frame_t));
    bad_frame.header.version ^= 0x01;
    if (aes_ccm_decrypt(&bad_frame, GOLDEN_KEY, &dec_pkt)) {
        ESP_LOGE(TAG, "FAIL: Negative test 3 (corrupted AAD) succeeded inappropriately");
        return;
    }

    /* Negative test 4: corrupted tag */
    memcpy(&bad_frame, &frame, sizeof(rf_frame_t));
    bad_frame.tag[0] ^= 0x01;
    if (aes_ccm_decrypt(&bad_frame, GOLDEN_KEY, &dec_pkt)) {
        ESP_LOGE(TAG, "FAIL: Negative test 4 (corrupted tag) succeeded inappropriately");
        return;
    }

    /* Negative test 5: replay-counter modification (also AAD corruption) */
    memcpy(&bad_frame, &frame, sizeof(rf_frame_t));
    bad_frame.header.persistent_tx_counter++;
    if (aes_ccm_decrypt(&bad_frame, GOLDEN_KEY, &dec_pkt)) {
        ESP_LOGE(TAG, "FAIL: Negative test 5 (replay-counter modification) succeeded inappropriately");
        return;
    }

    ESP_LOGI(TAG, "ALL TESTS PASSED");

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
