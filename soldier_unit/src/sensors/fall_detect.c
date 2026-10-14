/**
 * fall_detect.c — Fall detection state machine implementation.
 *
 * State transitions (time-sequenced AND-gate, per §9 / Terra correction #8):
 *
 *   FD_IDLE
 *     └─ accel_magnitude > SPIKE_THRESHOLD_G
 *        → FD_SPIKE_SEEN  (record spike time and tilt_at_spike)
 *
 *   FD_SPIKE_SEEN
 *     ├─ timeout (> FALL_SEQ_TIMEOUT_MS) → FD_IDLE (stumble/false spike)
 *     └─ tilt_deg - tilt_at_spike > TILT_THRESHOLD_DEG
 *        → FD_TILTED
 *
 *   FD_TILTED
 *     ├─ timeout (> FALL_SEQ_TIMEOUT_MS from spike) → FD_IDLE
 *     └─ stationary == true
 *        → FD_STILL (record still start time)
 *
 *   FD_STILL
 *     ├─ stationary becomes false → FD_IDLE (recovered, not a real fall)
 *     ├─ timeout (> FALL_SEQ_TIMEOUT_MS from spike) → FD_IDLE
 *     └─ time in FD_STILL > STILL_WINDOW_MS
 *        → emit FALL_EVENT_CANDIDATE, stay in FD_STILL until reset
 *
 * Note: The detector stays in FD_STILL after emitting the event so that
 * repeated calls don't re-emit.  fall_detect_reset() moves it back to
 * FD_IDLE after the application layer has consumed the event.
 */

#include "fall_detect.h"
#include <stdio.h>

/* A value > FD_STILL marks "event already emitted" to prevent double-fire */
#define FD_EMITTED 4u

void fall_detect_init(fall_detector_t *fd) {
    fd->state            = FD_IDLE;
    fd->spike_timestamp  = 0;
    fd->state_entered_ms = 0;
    fd->tilt_at_spike    = 0.0f;
}

fall_event_t fall_detect_update(fall_detector_t *fd,
                                const mpu6050_raw_t *raw,
                                const mpu6050_processed_t *p,
                                uint32_t now_ms) {
    (void)raw;
#ifdef FALL_CALIBRATION_MODE
    /* Calibration: log raw values without triggering any state changes.
     * Connect serial monitor and record traces for threshold tuning. 
     * Format: timestamp, ax, ay, az, gx, gy, gz, magnitude, tilt, stat, state
     */
    printf("%lu,%d,%d,%d,%d,%d,%d,%.3f,%.1f,%d,%d\n",
           (unsigned long)now_ms, 
           raw->ax, raw->ay, raw->az, 
           raw->gx, raw->gy, raw->gz,
           p->accel_magnitude, p->tilt_deg, 
           p->stationary, fd->state);
    return FALL_EVENT_NONE;
#endif

    /* Already emitted — wait for reset */
    if ((uint8_t)fd->state == FD_EMITTED) {
        return FALL_EVENT_NONE;
    }

    /* Global sequence timeout (measured from initial spike) */
    if (fd->state != FD_IDLE) {
        uint32_t elapsed = now_ms - fd->spike_timestamp;
        if (elapsed > FALL_SEQ_TIMEOUT_MS) {
#ifdef DEBUG_FALL
            printf("[fall] Timeout in state %s — reset to IDLE\n",
                   fall_detect_state_name(fd->state));
#endif
            fall_detect_reset(fd);
            return FALL_EVENT_NONE;
        }
    }

    switch (fd->state) {
    case FD_IDLE:
        if (p->accel_magnitude > SPIKE_THRESHOLD_G) {
            fd->state            = FD_SPIKE_SEEN;
            fd->spike_timestamp  = now_ms;
            fd->state_entered_ms = now_ms;
            fd->tilt_at_spike    = p->tilt_deg;
#ifdef DEBUG_FALL
            printf("[fall] SPIKE detected (%.2fg) → FD_SPIKE_SEEN\n", p->accel_magnitude);
#endif
        }
        break;

    case FD_SPIKE_SEEN:
        /* Check if orientation has changed significantly since spike */
        if (p->tilt_deg - fd->tilt_at_spike > TILT_THRESHOLD_DEG ||
            fd->tilt_at_spike - p->tilt_deg > TILT_THRESHOLD_DEG) {
            fd->state = FD_TILTED;
#ifdef DEBUG_FALL
            printf("[fall] TILT confirmed (Δ%.1f°) → FD_TILTED\n",
                   p->tilt_deg - fd->tilt_at_spike);
#endif
        }
        break;

    case FD_TILTED:
        if (p->stationary) {
            fd->state            = FD_STILL;
            fd->state_entered_ms = now_ms; /* Reset for STILL_WINDOW_MS counter */
#ifdef DEBUG_FALL
            printf("[fall] STATIONARY detected → FD_STILL\n");
#endif
        }
        break;

    case FD_STILL: {
        /* If unit starts moving again, it was a stumble, not a fall */
        if (!p->stationary) {
#ifdef DEBUG_FALL
            printf("[fall] Movement resumed — reset to IDLE\n");
#endif
            fd->state = FD_IDLE;
            break;
        }
        uint32_t still_ms = now_ms - fd->state_entered_ms;
        if (still_ms >= STILL_WINDOW_MS) {
            fd->state = (fall_detector_state_t)FD_EMITTED; /* Prevent re-emit */
#ifdef DEBUG_FALL
            printf("[fall] FALL_CANDIDATE emitted after %u ms still\n", still_ms);
#endif
            return FALL_EVENT_CANDIDATE;
        }
        break;
    }

    default:
        break;
    }

    return FALL_EVENT_NONE;
}

void fall_detect_reset(fall_detector_t *fd) {
    fd->state            = FD_IDLE;
    fd->spike_timestamp  = 0;
    fd->state_entered_ms = 0;
    fd->tilt_at_spike    = 0.0f;
}

const char *fall_detect_state_name(fall_detector_state_t s) {
    switch (s) {
    case FD_IDLE:       return "IDLE";
    case FD_SPIKE_SEEN: return "SPIKE_SEEN";
    case FD_TILTED:     return "TILTED";
    case FD_STILL:      return "STILL";
    default:            return "EMITTED";
    }
}
