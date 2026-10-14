#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "FreeRTOS.h"

typedef enum {
    I2C_NUM_0 = 0,
    I2C_NUM_1 = 1,
} i2c_port_t;

typedef enum {
    I2C_MODE_SLAVE = 0,
    I2C_MODE_MASTER = 1,
} i2c_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE = 1,
} gpio_pullup_t;

typedef int esp_err_t;
#define ESP_OK 0

typedef struct {
    i2c_mode_t mode;
    int sda_io_num;
    int scl_io_num;
    gpio_pullup_t sda_pullup_en;
    gpio_pullup_t scl_pullup_en;
    struct {
        uint32_t clk_speed;
    } master;
} i2c_config_t;

static inline esp_err_t i2c_param_config(i2c_port_t port, const i2c_config_t *conf) {
    (void)port; (void)conf; return ESP_OK;
}
static inline esp_err_t i2c_driver_install(i2c_port_t port, i2c_mode_t mode, size_t slv_rx_buf_len, size_t slv_tx_buf_len, int intr_alloc_flags) {
    (void)port; (void)mode; (void)slv_rx_buf_len; (void)slv_tx_buf_len; (void)intr_alloc_flags; return ESP_OK;
}

esp_err_t i2c_master_write_to_device(i2c_port_t port, uint16_t device_address,
                                     const uint8_t *write_buffer, size_t write_size,
                                     uint32_t ticks_to_wait);
