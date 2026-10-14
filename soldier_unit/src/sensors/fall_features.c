#include "fall_features.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void causal_fall_detector_init(causal_fall_detector_t *det) {
    if (!det) return;
    memset(det, 0, sizeof(*det));
    det->state = CAUSAL_STATE_IDLE;
}

bool causal_fall_detector_feed(causal_fall_detector_t *det,
                               const mpu6050_raw_t *raw,
                               uint32_t now_ms,
                               causal_fall_features_t *out_feats) {
    if (!det || !raw) return false;

    /* 1. Physical engineering conversions:
     * Accelerometer: +-16g full scale -> 2048 LSB/g */
    float ax = (float)raw->ax / 2048.0f;
    float ay = (float)raw->ay / 2048.0f;
    float az = (float)raw->az / 2048.0f;
    float svm = sqrtf(ax * ax + ay * ay + az * az);

    /* Gyroscope: 16.384 LSB/(deg/s) */
    float gx = (float)raw->gx / 16.384f;
    float gy = (float)raw->gy / 16.384f;
    float gz = (float)raw->gz / 16.384f;
    float omega = sqrtf(gx * gx + gy * gy + gz * gz);

    /* Rate of change of acceleration (jerk @ 200 Hz = 5 ms step) */
    float jerk = fabsf(svm - det->last_svm) * 200.0f;
    det->last_svm = svm;

    /* 2. Push current sample into 128-sample circular buffer */
    det->ring_ax[det->ring_head] = raw->ax;
    det->ring_ay[det->ring_head] = raw->ay;
    det->ring_az[det->ring_head] = raw->az;
    det->ring_head = (uint8_t)((det->ring_head + 1u) % FALL_RING_BUF_SIZE);
    det->samples_received++;

    /* 3. Streaming Causal FSM Progression */
    switch (det->state) {
    case CAUSAL_STATE_IDLE: {
        /* Check if refractory lockout has elapsed */
        if (now_ms < det->lockout_until_ms) {
            break;
        }

        /* Check for impact spike trigger */
        if (svm >= FALL_SPIKE_TRIGGER_G && det->samples_received >= FALL_PRE_GRAV_START) {
            det->spike_time_ms = now_ms;
            det->state = CAUSAL_STATE_IMPACT_TRACK;

            /* Compute pre-impact gravity vector g_pre from [-120 .. -20] samples */
            double sum_pre_x = 0.0, sum_pre_y = 0.0, sum_pre_z = 0.0;
            uint32_t count_pre = 0;
            for (uint32_t offset = FALL_PRE_GRAV_START; offset >= FALL_PRE_GRAV_END; offset--) {
                uint8_t idx = (uint8_t)((det->ring_head + FALL_RING_BUF_SIZE - offset) % FALL_RING_BUF_SIZE);
                sum_pre_x += (double)det->ring_ax[idx] / 2048.0;
                sum_pre_y += (double)det->ring_ay[idx] / 2048.0;
                sum_pre_z += (double)det->ring_az[idx] / 2048.0;
                count_pre++;
            }
            if (count_pre > 0) {
                det->g_pre_x = (float)(sum_pre_x / count_pre);
                det->g_pre_y = (float)(sum_pre_y / count_pre);
                det->g_pre_z = (float)(sum_pre_z / count_pre);
            } else {
                det->g_pre_x = ax; det->g_pre_y = ay; det->g_pre_z = az;
            }

            /* Compute minimum SVM in 500 ms pre-impact window [-100 .. 0] samples */
            float min_pre = svm;
            for (uint32_t offset = FALL_PRE_MIN_SAMPLES; offset > 0; offset--) {
                uint8_t idx = (uint8_t)((det->ring_head + FALL_RING_BUF_SIZE - offset) % FALL_RING_BUF_SIZE);
                float sax = (float)det->ring_ax[idx] / 2048.0f;
                float say = (float)det->ring_ay[idx] / 2048.0f;
                float saz = (float)det->ring_az[idx] / 2048.0f;
                float s_mag = sqrtf(sax * sax + say * say + saz * saz);
                if (s_mag < min_pre) {
                    min_pre = s_mag;
                }
            }
            det->pre_svm_min = min_pre;

            /* Initialize impact cluster tracking */
            det->peak_svm = svm;
            det->peak_jerk = jerk;
            det->peak_gyro = omega;
        }
        break;
    }

    case CAUSAL_STATE_IMPACT_TRACK: {
        /* Track maximum peaks during impact cluster (+100 ms) */
        if (svm > det->peak_svm) det->peak_svm = svm;
        if (jerk > det->peak_jerk) det->peak_jerk = jerk;
        if (omega > det->peak_gyro) det->peak_gyro = omega;

        if (now_ms >= det->spike_time_ms + FALL_IMPACT_TRACK_MS) {
            det->state = CAUSAL_STATE_POST_WAIT;
        }
        break;
    }

    case CAUSAL_STATE_POST_WAIT: {
        if (now_ms >= det->spike_time_ms + FALL_POST_START_MS) {
            /* Enter post-impact stillness observation window [+1000 ms .. +2000 ms] */
            det->state = CAUSAL_STATE_POST_ACCUM;
            det->post_sample_count = 0;
            det->sum_ax = 0.0;
            det->sum_ay = 0.0;
            det->sum_az = 0.0;
            det->sum_svm = 0.0;
            det->sum_svm_sq = 0.0;
        }
        break;
    }

    case CAUSAL_STATE_POST_ACCUM: {
        /* Accumulate post-impact samples */
        det->sum_ax += ax;
        det->sum_ay += ay;
        det->sum_az += az;
        det->sum_svm += svm;
        det->sum_svm_sq += ((double)svm * (double)svm);
        det->post_sample_count++;

        if (now_ms >= det->spike_time_ms + FALL_POST_END_MS) {
            /* Observation window complete: extract final features */
            uint32_t N = det->post_sample_count;
            if (N > 0) {
                float g_post_x = (float)(det->sum_ax / N);
                float g_post_y = (float)(det->sum_ay / N);
                float g_post_z = (float)(det->sum_az / N);

                float norm_pre = sqrtf(det->g_pre_x * det->g_pre_x +
                                       det->g_pre_y * det->g_pre_y +
                                       det->g_pre_z * det->g_pre_z);
                float norm_post = sqrtf(g_post_x * g_post_x +
                                        g_post_y * g_post_y +
                                        g_post_z * g_post_z);

                float tilt_dyn = 0.0f;
                if (norm_pre > 0.1f && norm_post > 0.1f) {
                    float dot = (det->g_pre_x * g_post_x +
                                 det->g_pre_y * g_post_y +
                                 det->g_pre_z * g_post_z) / (norm_pre * norm_post);
                    if (dot > 1.0f) dot = 1.0f;
                    if (dot < -1.0f) dot = -1.0f;
                    tilt_dyn = acosf(dot) * (180.0f / (float)M_PI);
                }

                double mean_svm = det->sum_svm / N;
                double var_svm = (det->sum_svm_sq / N) - (mean_svm * mean_svm);
                float post_std = (var_svm > 0.0) ? (float)sqrt(var_svm) : 0.0f;

                causal_fall_features_t feats;
                feats.svm_peak     = det->peak_svm;
                feats.svm_min_pre  = det->pre_svm_min;
                feats.jerk_peak    = det->peak_jerk;
                feats.tilt_dynamic = tilt_dyn;
                feats.post_std     = post_std;
                feats.gyro_peak    = det->peak_gyro;

                det->last_features = feats;
                det->features_ready = true;

                if (out_feats) {
                    *out_feats = feats;
                }

                /* Enter lockout refractory period to avoid duplicate triggering */
                det->lockout_until_ms = now_ms + FALL_LOCKOUT_MS;
                det->state = CAUSAL_STATE_IDLE;
                return true;
            } else {
                det->state = CAUSAL_STATE_IDLE;
            }
        }
        break;
    }

    default:
        det->state = CAUSAL_STATE_IDLE;
        break;
    }

    return false;
}
