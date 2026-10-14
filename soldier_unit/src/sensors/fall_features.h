#ifndef FALL_FEATURES_H
#define FALL_FEATURES_H

#include <stdint.h>
#include <stdbool.h>
#include "mpu6050.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fall Feature Pipeline Configuration
 * Sample rate: 200 Hz (5 ms per sample)
 *
 * Ring Buffer Memory Accounting:
 * - Pre-impact circular buffer: 128 samples (640 ms)
 *   Storage: 128 * 6 bytes (int16_t ax, ay, az) = 768 bytes
 * - State variables & accumulators: 80 bytes
 * - Total RAM Footprint: 848 bytes
 * - Flash Footprint: ~650 bytes
 */
#define FALL_RING_BUF_SIZE      128u    /* 128 samples = 640 ms @ 200 Hz */
#define FALL_PRE_GRAV_START     120u    /* 600 ms before spike */
#define FALL_PRE_GRAV_END        20u    /* 100 ms before spike */
#define FALL_PRE_MIN_SAMPLES    100u    /* 500 ms before spike */
#define FALL_IMPACT_TRACK_MS    100u    /* 100 ms impact cluster window */
#define FALL_POST_START_MS     1000u    /* Post-impact window starts at +1000 ms */
#define FALL_POST_END_MS       2000u    /* Post-impact window ends at +2000 ms */
#define FALL_LOCKOUT_MS        1500u    /* 1.5 s refractory lockout to prevent double-fire */

#define FALL_SPIKE_TRIGGER_G    2.5f    /* SVM spike trigger threshold (g) */

typedef struct {
    float svm_peak;       /* Maximum SVM during impact cluster (g) */
    float svm_min_pre;    /* Minimum SVM during 500 ms pre-impact window (g) */
    float jerk_peak;      /* Maximum rate of change of SVM (g/s) */
    float tilt_dynamic;   /* Angle between pre-impact gravity and post-impact gravity (degrees) */
    float post_std;       /* Standard deviation of SVM during 1.0s post-impact stillness window (g) */
    float gyro_peak;      /* Maximum angular velocity during impact cluster (deg/s) */
} causal_fall_features_t;

typedef enum {
    CAUSAL_STATE_IDLE = 0,
    CAUSAL_STATE_IMPACT_TRACK,
    CAUSAL_STATE_POST_WAIT,
    CAUSAL_STATE_POST_ACCUM,
    CAUSAL_STATE_LOCKOUT
} causal_detector_state_t;

typedef struct {
    /* 128-sample ring buffer for pre-impact raw accelerometer data: 768 bytes RAM */
    int16_t ring_ax[FALL_RING_BUF_SIZE];
    int16_t ring_ay[FALL_RING_BUF_SIZE];
    int16_t ring_az[FALL_RING_BUF_SIZE];
    uint8_t ring_head;
    uint32_t samples_received;

    causal_detector_state_t state;
    uint32_t spike_time_ms;
    uint32_t lockout_until_ms;

    /* Pre-impact captured features */
    float pre_svm_min;
    float g_pre_x;
    float g_pre_y;
    float g_pre_z;

    /* Impact cluster tracking */
    float peak_svm;
    float peak_jerk;
    float peak_gyro;
    float last_svm;

    /* Post-impact zero-array accumulators (running sum & sum of squares) */
    uint32_t post_sample_count;
    double sum_ax;
    double sum_ay;
    double sum_az;
    double sum_svm;
    double sum_svm_sq;

    causal_fall_features_t last_features;
    bool features_ready;
} causal_fall_detector_t;

/**
 * Initialise causal streaming fall detector.
 */
void causal_fall_detector_init(causal_fall_detector_t *det);

/**
 * Feed one sample in causal streaming time order.
 * @param det        Detector state
 * @param raw        Raw 6-axis MPU6050 reading (ax, ay, az, gx, gy, gz)
 * @param now_ms     Monotonic timestamp in milliseconds
 * @param out_feats  Output pointer to receive feature vector when window completes
 * @return true if post-impact observation window completed and feature vector is ready
 */
bool causal_fall_detector_feed(causal_fall_detector_t *det,
                               const mpu6050_raw_t *raw,
                               uint32_t now_ms,
                               causal_fall_features_t *out_feats);

#ifdef __cplusplus
}
#endif

#endif /* FALL_FEATURES_H */
