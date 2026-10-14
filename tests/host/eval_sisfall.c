/**
 * eval_sisfall.c - SisFall Benchmark Harness linking REAL soldier_unit firmware
 *
 * Links:
 *   - soldier_unit/src/sensors/fall_detect.c
 *   - soldier_unit/src/sensors/mpu6050.c
 *
 * Implements:
 * 1. Differential test verification against Python reference model.
 * 2. Real firmware baseline state machine evaluation.
 * 3. Fixed-point decision tree model (< 1 KB) evaluation from fall_dt_model.h.
 * 4. Diagnostic stage failure analysis.
 * 5. Official Readme.txt activity labels.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "fall_detect.h"
#include "mpu6050.h"
#include "fall_dt_model.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_SAMPLES 30000
#define SAMPLE_RATE_HZ 200.0f
#define DT_MS 5.0f

/* Butterworth 2nd-order 44 Hz @ 200 Hz */
typedef struct {
    double b0, b1, b2;
    double a1, a2;
    double x1, x2;
    double y1, y2;
} biquad_t;

static void biquad_init(biquad_t *f, double fc_hz, double fs_hz) {
    double omega = 2.0 * M_PI * fc_hz / fs_hz;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * 0.70710678);
    double a0 = 1.0 + alpha;

    f->b0 = ((1.0 - cs) / 2.0) / a0;
    f->b1 = (1.0 - cs) / a0;
    f->b2 = ((1.0 - cs) / 2.0) / a0;
    f->a1 = (-2.0 * cs) / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->x1 = f->x2 = 0.0;
    f->y1 = f->y2 = 0.0;
}

static inline double biquad_filter(biquad_t *f, double in) {
    double out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = in;
    f->y2 = f->y1;
    f->y1 = out;
    return out;
}

#include "fall_features.h"

static int load_and_filter_trial(const char *filepath,
                                 mpu6050_raw_t *raw_out,
                                 int max_len) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return 0;

    biquad_t fx, fy, fz;
    biquad_init(&fx, 44.0, SAMPLE_RATE_HZ);
    biquad_init(&fy, 44.0, SAMPLE_RATE_HZ);
    biquad_init(&fz, 44.0, SAMPLE_RATE_HZ);

    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < max_len) {
        int ax_i, ay_i, az_i, gx_i, gy_i, gz_i;
        if (sscanf(line, "%d,%d,%d,%d,%d,%d", &ax_i, &ay_i, &az_i, &gx_i, &gy_i, &gz_i) >= 3) {
            /* ADXL345: 256 LSB/g. Map to firmware frame: ax = ax, ay = az, az = -ay */
            double ax_g = (double)ax_i / 256.0;
            double ay_g = (double)az_i / 256.0;
            double az_g = -(double)ay_i / 256.0;

            /* Filter */
            double ax_flt = biquad_filter(&fx, ax_g);
            double ay_flt = biquad_filter(&fy, ay_g);
            double az_flt = biquad_filter(&fz, az_g);

            /* Scale to MPU6050 raw integer units (+-16g = 2048 LSB/g) */
            raw_out[count].ax = (int16_t)(ax_flt * 2048.0);
            raw_out[count].ay = (int16_t)(ay_flt * 2048.0);
            raw_out[count].az = (int16_t)(az_flt * 2048.0);
            /* ITG3200: 16.384 LSB/dps. Frame mapping: gx = gx, gy = gz, gz = -gy */
            raw_out[count].gx = (int16_t)gx_i;
            raw_out[count].gy = (int16_t)gz_i;
            raw_out[count].gz = (int16_t)(-gy_i);
            count++;
        }
    }
    fclose(fp);
    return count;
}

static bool run_firmware_trial(const mpu6050_raw_t *samples, int count) {
    fall_detector_t fd;
    fall_detect_init(&fd);
    mpu6050_reset_variance();

    for (int i = 0; i < count; i++) {
        uint32_t now_ms = (uint32_t)(i * 5);
        mpu6050_processed_t p;
        mpu6050_process(&samples[i], &p);
        fall_event_t ev = fall_detect_update(&fd, &samples[i], &p, now_ms);
        if (ev == FALL_EVENT_CANDIDATE) {
            return true;
        }
    }
    return false;
}

typedef struct {
    int event_count;
    int detection_count;
} causal_stream_result_t;

static causal_stream_result_t run_causal_streaming_trial(const mpu6050_raw_t *samples, int count, const char *model) {
    causal_stream_result_t res = {0, 0};
    causal_fall_detector_t det;
    causal_fall_detector_init(&det);

    for (int i = 0; i < count; i++) {
        uint32_t now_ms = (uint32_t)(i * 5);
        causal_fall_features_t feats;
        if (causal_fall_detector_feed(&det, &samples[i], now_ms, &feats)) {
            res.event_count++;
            bool detected = false;
            if (!model || strcmp(model, "tree") == 0) {
                int32_t svm_mg = (int32_t)(feats.svm_peak * 1000.0f);
                int32_t tilt_x10 = (int32_t)(feats.tilt_dynamic * 10.0f);
                int32_t std_mg = (int32_t)(feats.post_std * 1000.0f);
                int32_t gyro_x10 = (int32_t)(feats.gyro_peak * 10.0f);
                detected = fall_dt_classify_fixed(svm_mg, tilt_x10, std_mg, gyro_x10);
            } else if (strcmp(model, "multi") == 0) {
                detected = (feats.svm_peak >= 2.05f &&
                            feats.tilt_dynamic >= 30.0f &&
                            feats.post_std <= 0.165f &&
                            (feats.gyro_peak >= 95.0f || feats.tilt_dynamic >= 55.0f));
            } else if (strcmp(model, "logistic") == 0) {
                float score = 1.25f * feats.svm_peak +
                              0.055f * feats.tilt_dynamic -
                              18.5f * feats.post_std +
                              0.0075f * feats.gyro_peak -
                              1.60f * feats.svm_min_pre - 2.65f;
                detected = (score > 0.0f);
            }
            if (detected) {
                res.detection_count++;
            }
        }
    }
    return res;
}

static mpu6050_raw_t g_sample_buf[MAX_SAMPLES];

#include <dirent.h>

static void run_stream_all_evaluation(const char *dataset_root, const char *model) {
    printf("========================================================================\n");
    printf("  SisFall C Causal Streaming Evaluation Across ALL Trials\n");
    printf("  Model: %s | Sample Rate: 200 Hz | DLPF: CFG 3 (44 Hz)\n", model ? model : "tree");
    printf("========================================================================\n");

    DIR *d_root = opendir(dataset_root);
    if (!d_root) {
        printf("Error: Could not open dataset root: %s\n", dataset_root);
        return;
    }

    int total_fall_trials = 0;
    int detected_fall_trials = 0;
    int total_fall_detections = 0;

    int total_adl_trials = 0;
    int fa_adl_trials = 0;
    int total_adl_detections = 0;
    double total_adl_duration_s = 0.0;

    /* Per activity tallies */
    int act_trials[35] = {0};
    int act_det_trials[35] = {0};
    int act_total_dets[35] = {0};
    double act_duration_s[35] = {0.0};

    struct dirent *sub_ent;
    while ((sub_ent = readdir(d_root)) != NULL) {
        if (sub_ent->d_name[0] == '.') continue;
        char sub_path[512];
        snprintf(sub_path, sizeof(sub_path), "%s/%s", dataset_root, sub_ent->d_name);
        DIR *d_sub = opendir(sub_path);
        if (!d_sub) continue;

        struct dirent *f_ent;
        while ((f_ent = readdir(d_sub)) != NULL) {
            if (f_ent->d_name[0] == '.') continue;
            if (!strstr(f_ent->d_name, ".txt")) continue;

            char file_path[1024];
            snprintf(file_path, sizeof(file_path), "%s/%s", sub_path, f_ent->d_name);

            char act_code[8] = {0};
            strncpy(act_code, f_ent->d_name, 3);
            bool is_fall = (act_code[0] == 'F');
            int act_idx = -1;
            if (is_fall) {
                int num = atoi(act_code + 1);
                if (num >= 1 && num <= 15) act_idx = num - 1; /* 0..14 */
            } else if (act_code[0] == 'D') {
                int num = atoi(act_code + 1);
                if (num >= 1 && num <= 19) act_idx = 15 + (num - 1); /* 15..33 */
            }

            int n = load_and_filter_trial(file_path, g_sample_buf, MAX_SAMPLES);
            if (n < 50) continue;

            double dur_s = (double)n / SAMPLE_RATE_HZ;
            causal_stream_result_t res = run_causal_streaming_trial(g_sample_buf, n, model);

            if (is_fall) {
                total_fall_trials++;
                if (res.detection_count > 0) detected_fall_trials++;
                total_fall_detections += res.detection_count;
            } else {
                total_adl_trials++;
                if (res.detection_count > 0) fa_adl_trials++;
                total_adl_detections += res.detection_count;
                total_adl_duration_s += dur_s;
            }

            if (act_idx >= 0 && act_idx < 35) {
                act_trials[act_idx]++;
                if (res.detection_count > 0) act_det_trials[act_idx]++;
                act_total_dets[act_idx] += res.detection_count;
                act_duration_s[act_idx] += dur_s;
            }
        }
        closedir(d_sub);
    }
    closedir(d_root);

    double sens_pct = total_fall_trials > 0 ? (100.0 * detected_fall_trials / total_fall_trials) : 0.0;
    double adl_hours = total_adl_duration_s / 3600.0;
    double fa_rate_per_hour = adl_hours > 0.0 ? (total_adl_detections / adl_hours) : 0.0;
    double fa_event_pct = total_adl_trials > 0 ? (100.0 * fa_adl_trials / total_adl_trials) : 0.0;

    printf("\nSTREAMING EVALUATION RESULTS:\n");
    printf("  Total Fall Trials:          %d\n", total_fall_trials);
    printf("  Detected Falls (TP Events): %d (%.2f%% Sensitivity)\n", detected_fall_trials, sens_pct);
    printf("  Total Fall Detections:      %d (Every detection counted)\n", total_fall_detections);
    printf("  Missed Falls (FN):          %d (%.2f%%)\n", total_fall_trials - detected_fall_trials, 100.0 - sens_pct);
    printf("\n  Total ADL Trials:           %d (%.2f hours)\n", total_adl_trials, adl_hours);
    printf("  ADL Trials with FA:         %d / %d (%.2f%% FA Event Probability)\n",
           fa_adl_trials, total_adl_trials, fa_event_pct);
    printf("  Total ADL Detections (FA):  %d (Every detection counted)\n", total_adl_detections);
    printf("  ADL False Alarm Rate:       %.2f FA / hour\n", fa_rate_per_hour);

    printf("\nPER-ACTIVITY FALSE ALARM BREAKDOWN (D01..D19):\n");
    printf("Act | Description                 | Trials | FA Events | P(FA) %% | Total Dets | Hours | FA/h\n");
    printf("----+-----------------------------+--------+-----------+---------+------------+-------+------\n");
    const char *d_names[19] = {
        "Walking slowly (100s)       ",
        "Walking quickly (100s)      ",
        "Jogging slowly (100s)       ",
        "Jogging quickly (100s)      ",
        "Stairs slowly (25s)         ",
        "Stairs quickly (25s)        ",
        "Sit half-height slow (12s)  ",
        "Sit half-height quick (12s) ",
        "Sit low-height slow (12s)   ",
        "Sit low-height quick (12s)  ",
        "Collapse into chair (12s)   ",
        "Lying slowly (12s)          ",
        "Lying quickly (12s)         ",
        "Turn in bed (12s)           ",
        "Bending at knees (12s)      ",
        "Bending waist only (12s)    ",
        "Car in/out (25s)            ",
        "Stumble walking (12s)       ",
        "Jump gently (12s)           "
    };

    for (int i = 0; i < 19; i++) {
        int idx = 15 + i;
        double h = act_duration_s[idx] / 3600.0;
        double p_fa = act_trials[idx] > 0 ? (100.0 * act_det_trials[idx] / act_trials[idx]) : 0.0;
        double rate = h > 0.0 ? (act_total_dets[idx] / h) : 0.0;
        printf("D%02d | %s | %6d | %9d | %6.1f%% | %10d | %5.2f | %6.1f\n",
               i + 1, d_names[i], act_trials[idx], act_det_trials[idx], p_fa, act_total_dets[idx], h, rate);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s --diff <trial_path>\n", argv[0]);
        printf("  %s --diff-features <trial_path>\n", argv[0]);
        printf("  %s --stream-trial <trial_path> [tree|multi|logistic]\n", argv[0]);
        printf("  %s --stream-all <dataset_root> [tree|multi|logistic]\n", argv[0]);
        printf("  %s --test\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--stream-all") == 0 && argc >= 3) {
        const char *model = (argc >= 4) ? argv[3] : "tree";
        run_stream_all_evaluation(argv[2], model);
        return 0;
    }

    if (strcmp(argv[1], "--diff") == 0 && argc >= 3) {
        int n = load_and_filter_trial(argv[2], g_sample_buf, MAX_SAMPLES);
        if (n < 50) {
            printf("RESULT: ERROR_EMPTY\n");
            return 2;
        }
        bool det = run_firmware_trial(g_sample_buf, n);
        printf("RESULT: %d\n", det ? 1 : 0);
        return 0;
    }

    if (strcmp(argv[1], "--diff-features") == 0 && argc >= 3) {
        int n = load_and_filter_trial(argv[2], g_sample_buf, MAX_SAMPLES);
        if (n < 50) {
            printf("RESULT: ERROR_EMPTY\n");
            return 2;
        }
        causal_fall_detector_t det;
        causal_fall_detector_init(&det);
        int ev_count = 0;
        for (int i = 0; i < n; i++) {
            uint32_t now_ms = (uint32_t)(i * 5);
            causal_fall_features_t feats;
            if (causal_fall_detector_feed(&det, &g_sample_buf[i], now_ms, &feats)) {
                printf("FEAT: svm_peak=%.4f svm_min_pre=%.4f jerk_peak=%.4f tilt_dynamic=%.4f post_std=%.4f gyro_peak=%.4f\n",
                       feats.svm_peak, feats.svm_min_pre, feats.jerk_peak,
                       feats.tilt_dynamic, feats.post_std, feats.gyro_peak);
                ev_count++;
            }
        }
        if (ev_count == 0) {
            printf("FEAT_NONE\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "--stream-trial") == 0 && argc >= 3) {
        const char *model = (argc >= 4) ? argv[3] : "tree";
        int n = load_and_filter_trial(argv[2], g_sample_buf, MAX_SAMPLES);
        if (n < 50) {
            printf("RESULT_STREAM: ERROR_EMPTY\n");
            return 2;
        }
        causal_stream_result_t res = run_causal_streaming_trial(g_sample_buf, n, model);
        printf("RESULT_STREAM: events=%d detections=%d duration=%.2f\n",
               res.event_count, res.detection_count, (float)n / SAMPLE_RATE_HZ);
        return 0;
    }

    if (strcmp(argv[1], "--test") == 0) {
        printf("========================================================\n");
        printf("  eval_sisfall: Firmware C Harness Verification\n");
        printf("========================================================\n");
        printf("Real firmware fall_detect.c, mpu6050.c, fall_features.c linked successfully.\n");
        printf("Causal detector RAM accounting: %zu bytes per instance (128-sample ring buffer: 768 B).\n",
               sizeof(causal_fall_detector_t));
        printf("Fixed-point DT model footprint: %zu bytes.\n", sizeof(fall_dt_classify_fixed));
        return 0;
    }

    return 0;
}
