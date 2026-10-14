/**
 * mpu6050.h — MPU-6050 I2C driver for the RP2040 soldier unit.
 *
 * I2C0 on GP4 (SDA) / GP5 (SCL) / GP6 (INT wake source).
 * I2C address: 0x68 (AD0 pin tied to GND on the breakout board).
 */

#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>
#include <stdbool.h>

#define MPU6050_AFS_16G         3u
#define MPU6050_ACCEL_SCALE_16G 2048.0f

/** Raw sensor reading straight from the ADC registers. */
typedef struct {
    int16_t ax, ay, az;   /**< Accelerometer raw (±16g range: 1g = 2048 LSB) */
    int16_t gx, gy, gz;   /**< Gyroscope raw (±250°/s default: 131 LSB/°/s)    */
} mpu6050_raw_t;

/** Processed values for fall detection. */
typedef struct {
    float accel_magnitude;    /**< √(ax² + ay² + az²) in multiples of g         */
    float tilt_deg;           /**< Angle from vertical (0° = upright)            */
    bool  stationary;         /**< true if magnitude variance < STILL_THRESHOLD  */
} mpu6050_processed_t;

/**
 * Initialise I2C0 and configure the MPU-6050.
 * Checks WHO_AM_I register (must read 0x68).
 * @return true on success; false if the device does not respond correctly.
 */
bool mpu6050_init(void);

/**
 * Read raw accelerometer and gyroscope values.
 * @param out  Output struct (must not be NULL).
 */
void mpu6050_read_raw(mpu6050_raw_t *out);

/**
 * Compute processed values from a raw reading.
 * Updates an internal variance window for the stationary flag.
 * @param raw  Raw reading from mpu6050_read_raw().
 * @param out  Processed output.
 */
void mpu6050_process(const mpu6050_raw_t *raw, mpu6050_processed_t *out);

/**
 * Configure the MPU-6050 motion interrupt on INT pin (GP6).
 * Used as a dormant-mode wake source (§6.5 / §10.2).
 * Threshold is approximate — exact calibration is [O].
 */
void mpu6050_configure_motion_interrupt(void);

/** Reset the internal variance window (call after a confirmed fall event). */
void mpu6050_reset_variance(void);

/** Clear the latched motion interrupt by reading INT_STATUS. */
void mpu6050_clear_interrupt(void);

#endif /* MPU6050_H */
