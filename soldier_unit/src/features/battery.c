/**
 * battery.c — Battery percentage estimation for the RP2040 soldier unit.
 *
 * Hardware (§11):
 *   GP26 (ADC0) ← voltage divider output from 18650+ terminal.
 *   Voltage divider: R1 = 100 kΩ (top), R2 = 100 kΩ (bottom)
 *   → V_adc = V_batt / 2   (adjust R1/R2 if different values are used)
 *   RP2040 ADC reference: 3.3 V, 12-bit resolution (4096 counts).
 *
 * Li-ion discharge curve (piecewise linear, 5-point approximation):
 *   4.20 V → 100 %
 *   3.90 V →  75 %
 *   3.70 V →  50 %
 *   3.50 V →  25 %
 *   3.00 V →   0 %
 *
 * These breakpoints are approximate for a generic 18650 cell.
 * Accuracy: ±5–10 % — sufficient for a low-battery warning flag.
 */

#include "battery.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include <stdio.h>

#define BATT_ADC_PIN        26u   /* GP26 = ADC0                              */
#define BATT_ADC_CHANNEL    0u
#define BATT_SAMPLES        8u    /* Average over N readings to reduce noise  */
#define ADC_VREF            3.3f
#define ADC_COUNTS          4096.0f
#define DIVIDER_RATIO       2.0f  /* V_batt = V_adc × DIVIDER_RATIO           */
#define LOW_BATTERY_PCT     20u

/* ── Piecewise discharge curve ───────────────────────────────────────────── */
typedef struct { float volts; uint8_t pct; } curve_point_t;

static const curve_point_t CURVE[] = {
    { 4.20f, 100u },
    { 3.90f,  75u },
    { 3.70f,  50u },
    { 3.50f,  25u },
    { 3.00f,   0u },
};
#define CURVE_LEN (sizeof(CURVE) / sizeof(CURVE[0]))

/* ── Internal state ──────────────────────────────────────────────────────── */
static uint8_t s_battery_pct = 100u;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static float adc_to_voltage(uint16_t raw) {
    return ((float)raw / ADC_COUNTS) * ADC_VREF * DIVIDER_RATIO;
}

static uint8_t voltage_to_pct(float v) {
    if (v >= CURVE[0].volts) return 100u;
    if (v <= CURVE[CURVE_LEN - 1].volts) return 0u;

    for (size_t i = 0; i < CURVE_LEN - 1; i++) {
        if (v <= CURVE[i].volts && v > CURVE[i + 1].volts) {
            float range_v   = CURVE[i].volts - CURVE[i + 1].volts;
            float range_pct = (float)(CURVE[i].pct - CURVE[i + 1].pct);
            float t = (v - CURVE[i + 1].volts) / range_v;
            return (uint8_t)(CURVE[i + 1].pct + (uint8_t)(t * range_pct + 0.5f));
        }
    }
    return 0u;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void battery_init(void) {
    adc_init();
    adc_gpio_init(BATT_ADC_PIN);
    adc_select_input(BATT_ADC_CHANNEL);
}

uint8_t battery_read_pct(void) {
    /* Average BATT_SAMPLES readings */
    uint32_t sum = 0;
    for (uint8_t i = 0; i < BATT_SAMPLES; i++) {
        sum += adc_read();
    }
    float v = adc_to_voltage((uint16_t)(sum / BATT_SAMPLES));
    s_battery_pct = voltage_to_pct(v);
    return s_battery_pct;
}

uint8_t battery_get_cached_pct(void) {
    return s_battery_pct;
}

bool battery_is_low(void) {
    return s_battery_pct <= LOW_BATTERY_PCT;
}
