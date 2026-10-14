#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include "fall_detect.h"
#include "mpu6050.h"

int test_def02_saturation(void) {
    printf("--- Subtest DEF-02: MPU6050 +/-16g Range Saturation vs 2.5g Threshold ---\n");
    fall_detector_t fd;
    fall_detect_init(&fd);

    /* Max reading on Z-axis with 16-bit signed max (32767) in +/-16g mode */
    mpu6050_raw_t raw = { .ax = 0, .ay = 0, .az = 32767, .gx = 0, .gy = 0, .gz = 0 };
    mpu6050_processed_t p;
    mpu6050_process(&raw, &p);

    printf("Raw az=32767 -> processed mag=%.3fg (SPIKE_THRESHOLD_G=%.2fg)\n", 
           p.accel_magnitude, SPIKE_THRESHOLD_G);

    fall_event_t ev = fall_detect_update(&fd, &raw, &p, 1000);
    (void)ev;

    if (fd.state != FD_SPIKE_SEEN) {
        fprintf(stderr, "FAIL: Saturated reading (%.3fg) did not exceed spike threshold! (state=%d)\n",
                p.accel_magnitude, fd.state);
        return 1;
    }

    printf("PASS: Spike triggered.\n");
    return 0;
}

int test_def09_sequence_timer(void) {
    printf("--- Subtest DEF-09: Sequence Timeout Decoupled from State Timer ---\n");
    fall_detector_t fd;
    fall_detect_init(&fd);

    mpu6050_raw_t raw = {0};
    mpu6050_processed_t p = { .accel_magnitude = 3.0f, .tilt_deg = 10.0f, .stationary = false };

    /* t=1000ms: Spike detected -> state becomes FD_SPIKE_SEEN */
    fall_detect_update(&fd, &raw, &p, 1000);
    if (fd.state != FD_SPIKE_SEEN) {
        fprintf(stderr, "Precondition failed: not in FD_SPIKE_SEEN\n");
        return 2;
    }

    /* t=1800ms: Tilt occurs -> state becomes FD_TILTED */
    p.accel_magnitude = 1.0f;
    p.tilt_deg = 65.0f; /* delta = 55 deg > 45 deg */
    fall_detect_update(&fd, &raw, &p, 1800);
    if (fd.state != FD_TILTED) {
        fprintf(stderr, "Precondition failed: not in FD_TILTED\n");
        return 2;
    }

    /* t=2700ms: becomes stationary -> enters FD_STILL */
    p.stationary = true;
    fall_detect_update(&fd, &raw, &p, 2700);
    if (fd.state != FD_STILL) {
        fprintf(stderr, "Precondition failed: not in FD_STILL\n");
        return 2;
    }

    /* t=4250ms: elapsed since spike is 3250ms (> 3000ms FALL_SEQ_TIMEOUT_MS).
     * Must timeout and reset to FD_IDLE. */
    fall_event_t ev = fall_detect_update(&fd, &raw, &p, 4250);
    printf("At t=4250ms (3250ms after spike): event=%d, state=%d\n", ev, fd.state);

    if (ev == FALL_EVENT_CANDIDATE || fd.state != FD_IDLE) {
        fprintf(stderr, "FAIL: Sequence timeout did not reset detector to IDLE after 3250ms! (state=%d)\n", fd.state);
        return 1;
    }

    printf("PASS: Sequence timeout properly enforced.\n");
    return 0;
}

int test_synthetic_traces(void) {
    printf("--- Subtest: Synthetic Motion Traces (Fall vs ADL) ---\n");
    fall_detector_t fd;
    mpu6050_raw_t raw = {0};
    mpu6050_processed_t p = {0};

    /* Trace 1: Verified Fall Trace */
    fall_detect_init(&fd);
    /* t=100ms: Freefall */
    p.accel_magnitude = 0.3f; p.tilt_deg = 5.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 100);
    /* t=200ms: Impact shock */
    p.accel_magnitude = 4.8f; p.tilt_deg = 15.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 200);
    /* t=300ms: Lying down / tilted */
    p.accel_magnitude = 1.0f; p.tilt_deg = 75.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 300);
    /* t=400ms: Becomes still */
    p.accel_magnitude = 1.0f; p.tilt_deg = 75.0f; p.stationary = true;
    fall_detect_update(&fd, &raw, &p, 400);
    /* t=1950ms: Still for 1550ms (total from spike = 1750ms < 3000ms) */
    fall_event_t ev = fall_detect_update(&fd, &raw, &p, 1950);
    if (ev != FALL_EVENT_CANDIDATE) {
        fprintf(stderr, "FAIL: Legitimate fall trace failed to emit FALL_EVENT_CANDIDATE!\n");
        return 1;
    }
    printf("  [PASS] Verified fall trace emitted candidate.\n");

    /* Trace 2: ADL Walking */
    fall_detect_init(&fd);
    for (uint32_t t = 100; t <= 2000; t += 50) {
        p.accel_magnitude = 1.0f + 0.3f * sinf((float)t * 0.01f);
        p.tilt_deg = 8.0f;
        p.stationary = false;
        ev = fall_detect_update(&fd, &raw, &p, t);
        if (ev != FALL_EVENT_NONE || fd.state != FD_IDLE) {
            fprintf(stderr, "FAIL: ADL walking triggered false positive!\n");
            return 1;
        }
    }
    printf("  [PASS] ADL walking rejected.\n");

    /* Trace 3: ADL Sitting down smoothly */
    fall_detect_init(&fd);
    p.accel_magnitude = 1.4f; p.tilt_deg = 18.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 500);
    p.accel_magnitude = 1.0f; p.tilt_deg = 20.0f; p.stationary = true;
    ev = fall_detect_update(&fd, &raw, &p, 2100);
    if (ev != FALL_EVENT_NONE || fd.state != FD_IDLE) {
        fprintf(stderr, "FAIL: ADL sitting triggered false positive!\n");
        return 1;
    }
    printf("  [PASS] ADL sitting down rejected.\n");

    /* Trace 4: ADL Jump and landing upright */
    fall_detect_init(&fd);
    p.accel_magnitude = 3.9f; p.tilt_deg = 10.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 500); /* Spike triggered */
    /* Lands upright, continuous movement */
    p.accel_magnitude = 1.2f; p.tilt_deg = 12.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 600);
    fall_detect_update(&fd, &raw, &p, 4000); /* Timeout */
    if (fd.state != FD_IDLE) {
        fprintf(stderr, "FAIL: ADL jump did not reset to IDLE!\n");
        return 1;
    }
    printf("  [PASS] ADL jump landing upright rejected.\n");

    /* Trace 5: Stumble with immediate recovery */
    fall_detect_init(&fd);
    p.accel_magnitude = 3.5f; p.tilt_deg = 10.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 500);
    p.accel_magnitude = 1.0f; p.tilt_deg = 60.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 700);
    /* Recovering and moving again */
    p.accel_magnitude = 1.3f; p.tilt_deg = 15.0f; p.stationary = false;
    fall_detect_update(&fd, &raw, &p, 1000);
    if (fd.state != FD_IDLE) {
        /* Once in FD_STILL movement resets to IDLE, or sequence timeout resets */
        fall_detect_update(&fd, &raw, &p, 3600);
    }
    if (fd.state != FD_IDLE) {
        fprintf(stderr, "FAIL: Stumble with recovery did not return to IDLE!\n");
        return 1;
    }
    printf("  [PASS] Stumble with recovery rejected.\n");

    return 0;
}

int main(void) {
    printf("=== TEST: MPU Range & Fall Detect Logic (DEF-02 & DEF-09 Verification) ===\n");
    int r1 = test_def02_saturation();
    int r2 = test_def09_sequence_timer();
    int r3 = test_synthetic_traces();
    return (r1 != 0 || r2 != 0 || r3 != 0) ? 1 : 0;
}
