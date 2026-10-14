/**
 * mpu6050.c — MPU-6050 I2C driver implementation (RP2040 / Pico SDK).
 *
 * Reference: InvenSense MPU-6000/6050 Product Specification Revision 3.4
 *   Register map: RM-MPU-6000A-00 Revision 4.2
 *
 * I2C pins (§11):
 *   SDA → GP4 (I2C0)
 *   SCL → GP5 (I2C0)
 *   INT → GP6 (GPIO input, active-high on motion interrupt)
 *
 * Default sensor configuration applied at init:
 *   Accel range: ±2 g   → 1 g = 16384 LSB
 *   Gyro  range: ±250°/s → 131 LSB/°/s
 *   DLPF:  44 Hz bandwidth (register CONFIG = 0x03)
 *   Sample rate: 200 Hz (SMPLRT_DIV = 4 for 1 kHz / (1+4) = 200 Hz)
 *
 * Stationary detection:
 *   Maintains a sliding window of STILL_WINDOW_SAMPLES magnitude samples (200 ms window).
 *   Variance below STILL_VARIANCE_THRESHOLD marks the unit as stationary.
 *   Window size and threshold are placeholders — tuning is [O].
 */

#include "mpu6050.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ── I2C configuration ───────────────────────────────────────────────────── */
#define MPU_I2C_PORT    i2c0
#define MPU_SDA_PIN     4u
#define MPU_SCL_PIN     5u
#define MPU_INT_PIN     6u
#define MPU_I2C_ADDR    0x68u
#define MPU_I2C_BAUD    400000u   /* 400 kHz fast mode */

/* ── MPU-6050 register addresses (RM-MPU-6000A-00 Table 1) ─────────────── */
#define REG_SMPLRT_DIV      0x19u
#define REG_CONFIG          0x1Au  /* DLPF configuration                     */
#define REG_GYRO_CONFIG     0x1Bu  /* Gyro full-scale select                 */
#define REG_ACCEL_CONFIG    0x1Cu  /* Accel full-scale select                */
#define REG_MOT_THR         0x1Fu  /* Motion detection threshold             */
#define REG_MOT_DUR         0x20u  /* Motion detection duration              */
#define REG_INT_PIN_CFG     0x37u  /* INT pin / bypass enable config         */
#define REG_INT_ENABLE      0x38u  /* Interrupt enable                       */
#define REG_ACCEL_XOUT_H    0x3Bu  /* First of 14 output bytes (accel+temp+gyro) */
#define REG_PWR_MGMT_1      0x6Bu  /* Power management 1                     */
#define REG_WHO_AM_I        0x75u  /* Device identity — should read 0x68     */

/* ── Accel scale factor (±16 g range, 16-bit signed: 2048 LSB/g) ─────────── */
#define ACCEL_SCALE         MPU6050_ACCEL_SCALE_16G
_Static_assert((int)ACCEL_SCALE == 2048, "ACCEL_SCALE must match +-16g full scale (2048 LSB/g)");
#define G_TO_MS2            9.80665f

/* ── Stationary detection parameters ─────────────────────────────────────── */
/* 400 ms sliding window: 80 samples at 200 Hz (restoring 400 ms physical duration) */
#define STILL_WINDOW_SAMPLES    80u
#define STILL_VARIANCE_THRESHOLD 0.01f   /* g² — varies by environment */

/* ── Internal state ──────────────────────────────────────────────────────── */
static float s_mag_window[STILL_WINDOW_SAMPLES];
static uint8_t s_win_idx = 0;
static bool    s_win_full = false;

/* ── I2C helpers ─────────────────────────────────────────────────────────── */

static bool i2c_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_write_blocking(MPU_I2C_PORT, MPU_I2C_ADDR, buf, 2, false) == 2;
}

static bool i2c_read_regs(uint8_t reg, uint8_t *out, size_t len) {
    if (i2c_write_blocking(MPU_I2C_PORT, MPU_I2C_ADDR, &reg, 1, true) != 1) return false;
    return i2c_read_blocking(MPU_I2C_PORT, MPU_I2C_ADDR, out, len, false) == (int)len;
}

static inline int16_t combine(uint8_t hi, uint8_t lo) {
    return (int16_t)(((uint16_t)hi << 8) | lo);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool mpu6050_init(void) {
    /* Initialise I2C0 */
    i2c_init(MPU_I2C_PORT, MPU_I2C_BAUD);
    gpio_set_function(MPU_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA_PIN);
    gpio_pull_up(MPU_SCL_PIN);

    /* INT pin as input (active-high) */
    gpio_init(MPU_INT_PIN);
    gpio_set_dir(MPU_INT_PIN, GPIO_IN);

    sleep_ms(10);  /* Sensor power-on time */

    /* Verify device identity */
    uint8_t who = 0;
    if (!i2c_read_regs(REG_WHO_AM_I, &who, 1) || who != MPU_I2C_ADDR) {
        printf("[MPU6050] WHO_AM_I = 0x%02X (expected 0x68) — check wiring\n", who);
        return false;
    }

    /* Wake from sleep (PWR_MGMT_1 reset state has SLEEP=1) */
    if (!i2c_write_reg(REG_PWR_MGMT_1, 0x00u)) return false;
    sleep_ms(10);

    /* Sample rate: 200 Hz.  SMPLRT_DIV = (gyro_rate / target) - 1
     * Gyro rate with DLPF enabled = 1 kHz.  1000/200 - 1 = 4.            */
    if (!i2c_write_reg(REG_SMPLRT_DIV, 4u)) return false;

    /* DLPF: 44 Hz bandwidth (CONFIG = 0x03).
     * Reduces high-frequency vibration noise in accel readings.           */
    if (!i2c_write_reg(REG_CONFIG, 0x03u)) return false;

    /* Gyro full-scale: ±250°/s (FS_SEL = 0b00) */
    if (!i2c_write_reg(REG_GYRO_CONFIG, 0x00u)) return false;

    /* Accel full-scale: ±16 g (AFS_SEL = 0b11 -> bits [4:3] = 0x18) */
    if (!i2c_write_reg(REG_ACCEL_CONFIG, (uint8_t)(MPU6050_AFS_16G << 3))) return false;

    memset(s_mag_window, 0, sizeof(s_mag_window));
    printf("[MPU6050] Initialised OK (200 Hz, ±16g, ±250°/s, DLPF 44Hz)\n");
    return true;
}

void mpu6050_read_raw(mpu6050_raw_t *out) {
    /* Burst-read 14 bytes starting at ACCEL_XOUT_H:
     * [0..1]=AX [2..3]=AY [4..5]=AZ [6..7]=TEMP [8..9]=GX [10..11]=GY [12..13]=GZ */
    uint8_t buf[14] = {0};
    if (!i2c_read_regs(REG_ACCEL_XOUT_H, buf, 14)) {
        memset(out, 0, sizeof(*out));
        return;
    }
    out->ax = combine(buf[0],  buf[1]);
    out->ay = combine(buf[2],  buf[3]);
    out->az = combine(buf[4],  buf[5]);
    /* buf[6..7] = temperature, discarded */
    out->gx = combine(buf[8],  buf[9]);
    out->gy = combine(buf[10], buf[11]);
    out->gz = combine(buf[12], buf[13]);
}

void mpu6050_process(const mpu6050_raw_t *raw, mpu6050_processed_t *out) {
    /* Convert to g */
    float ax = raw->ax / ACCEL_SCALE;
    float ay = raw->ay / ACCEL_SCALE;
    float az = raw->az / ACCEL_SCALE;

    /* Magnitude */
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    out->accel_magnitude = mag;

    /* Tilt from vertical: angle between accel vector and gravity axis (Z).
     * When flat and stationary, az ≈ 1g, ax ≈ ay ≈ 0 → tilt_deg ≈ 0.
     * After a fall, the orientation changes significantly.
     * Clamp acos argument to [-1, 1] to prevent NaN.                       */
    float cos_tilt = az / (mag > 0.01f ? mag : 0.01f);
    if (cos_tilt >  1.0f) cos_tilt =  1.0f;
    if (cos_tilt < -1.0f) cos_tilt = -1.0f;
    out->tilt_deg = acosf(cos_tilt) * (180.0f / 3.14159265f);

    /* Update variance window */
    s_mag_window[s_win_idx] = mag;
    s_win_idx = (uint8_t)((s_win_idx + 1) % STILL_WINDOW_SAMPLES);
    if (s_win_idx == 0) s_win_full = true;

    uint8_t count = s_win_full ? STILL_WINDOW_SAMPLES : s_win_idx;
    if (count < 2u) {
        out->stationary = false;
        return;
    }

    /* Compute variance of magnitude window */
    float sum = 0.0f, sum_sq = 0.0f;
    for (uint8_t i = 0; i < count; i++) {
        sum    += s_mag_window[i];
        sum_sq += s_mag_window[i] * s_mag_window[i];
    }
    float mean = sum / count;
    float variance = (sum_sq / count) - (mean * mean);
    out->stationary = (variance < STILL_VARIANCE_THRESHOLD);
}

#define REG_INT_STATUS      0x3Au  /* Interrupt status                       */

void mpu6050_configure_motion_interrupt(void) {
    /* Motion detection: threshold and duration (placeholder values, tuning is [O]) */
    i2c_write_reg(REG_MOT_THR, 20u);   /* Each LSB = 2 mg; 20 ≈ 40 mg threshold */
    i2c_write_reg(REG_MOT_DUR, 5u);    /* Duration in ms                          */

    /* INT pin: active-high, push-pull, latched until cleared by reading INT_STATUS */
    i2c_write_reg(REG_INT_PIN_CFG, 0x20u);  /* LATCH_INT_EN=1 (bit 5), INT_RD_CLEAR=0 */
    /* Enable motion interrupt only */
    i2c_write_reg(REG_INT_ENABLE, 0x40u);   /* MOT_EN = bit 6 */
}

void mpu6050_clear_interrupt(void) {
    uint8_t status;
    i2c_read_regs(REG_INT_STATUS, &status, 1);
}

void mpu6050_reset_variance(void) {
    memset(s_mag_window, 0, sizeof(s_mag_window));
    s_win_idx  = 0;
    s_win_full = false;
}
