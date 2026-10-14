/**
 * board_pins.h — Base Station ESP32-S3 Hardware Pin Definitions
 *
 * Designed to avoid GPIO 26-32 and 33-37 which are typically reserved for
 * standard flash/PSRAM and Octal SPI flash/PSRAM respectively.
 */
#pragma once

/* ── I2C OLED Display ────────────────────────────────────────────────────── */
#define OLED_SDA_PIN    8
#define OLED_SCL_PIN    9

/* ── SPI nRF24L01+ ───────────────────────────────────────────────────────── */
#define NRF_SCK_PIN     12
#define NRF_MOSI_PIN    11
#define NRF_MISO_PIN    13
#define NRF_CSN_PIN     10
#define NRF_CE_PIN      14

/* ── Active Buzzer ───────────────────────────────────────────────────────── */
#define BUZZER_GPIO     4
