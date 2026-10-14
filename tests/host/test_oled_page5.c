#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "FreeRTOS.h"
#include "packet_protocol.h"
#include "oled.h"
#include "driver/i2c.h"

static uint8_t s_captured_pages[8][128];
static int s_data_page_count = 0;

esp_err_t i2c_master_write_to_device(i2c_port_t port, uint16_t device_address,
                                     const uint8_t *write_buffer, size_t write_size,
                                     uint32_t ticks_to_wait) {
    (void)port;
    (void)device_address;
    (void)ticks_to_wait;

    if (write_size > 1 && write_buffer[0] == 0x40) {
        /* SSD1306 data stream write */
        if (s_data_page_count < 8 && write_size == 129) {
            memcpy(s_captured_pages[s_data_page_count], &write_buffer[1], 128);
            s_data_page_count++;
        }
    }
    return ESP_OK;
}

int main(void) {
    printf("=== TEST DEF-07: SSD1306 OLED Rendering for Soldier 5 on Page 5 ===\n");

    soldier_record_t soldiers[SOLDIER_ID_MAX];
    memset(soldiers, 0, sizeof(soldiers));

    for (size_t i = 0; i < SOLDIER_ID_MAX; i++) {
        soldiers[i].soldier_id = (uint8_t)(i + 1);
        soldiers[i].alarm = ALARM_NORMAL;
        soldiers[i].link = LINK_ONLINE;
        soldiers[i].battery_pct = 95;
        soldiers[i].base_rx_time_ms = 1000;
        soldiers[i].last_msg_type = MSG_HEARTBEAT;
    }

    s_data_page_count = 0;
    memset(s_captured_pages, 0, sizeof(s_captured_pages));

    oled_render_status(soldiers, 12000, false);

    /* Verify that 8 pages were flushed */
    printf("Flushed %d pages to display controller.\n", s_data_page_count);
    assert(s_data_page_count == 8);

    /* Check Page 0 (Header): must contain non-zero pixel data */
    int page0_nonzeros = 0;
    for (int x = 0; x < 128; x++) {
        if (s_captured_pages[0][x] != 0) page0_nonzeros++;
    }
    printf("Page 0 (Header) non-zero bytes: %d\n", page0_nonzeros);
    assert(page0_nonzeros > 10);

    /* Check Page 1 (Soldier 1): must contain non-zero pixel data */
    int page1_nonzeros = 0;
    for (int x = 0; x < 128; x++) {
        if (s_captured_pages[1][x] != 0) page1_nonzeros++;
    }
    printf("Page 1 (Soldier 1) non-zero bytes: %d\n", page1_nonzeros);
    assert(page1_nonzeros > 10);

    /* Check Page 5 (Soldier 5): must contain non-zero pixel data */
    int page5_nonzeros = 0;
    for (int x = 0; x < 128; x++) {
        if (s_captured_pages[5][x] != 0) page5_nonzeros++;
    }
    printf("Page 5 (Soldier 5) non-zero bytes: %d\n", page5_nonzeros);

    if (page5_nonzeros == 0) {
        fprintf(stderr, "FAIL: Page 5 is completely blank! Soldier 5 was truncated by max_display clamp.\n");
        return 1;
    }

    printf("PASS: Soldier 5 is successfully rendered on Page 5 (%d non-zero bytes).\n", page5_nonzeros);
    return 0;
}
