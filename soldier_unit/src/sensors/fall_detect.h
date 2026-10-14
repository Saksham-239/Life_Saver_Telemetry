/**
 * fall_detect.h — Fall detection state machine for the RP2040 soldier unit.
 *
 * Architecture (Terra correction #8):
 *   The detector is a separate sensor-layer state machine that emits a
 *   FALL_CANDIDATE event.  The application layer (state_machine.c) starts
 *   the 10-second confirmation window on that event.
 *   This separates sensor logic from alarm lifecycle — neither layer knows
 *   about the other's internal state.
 *
 * Internal state sequence:
 *   IDLE → (accel spike) → SPIKE_SEEN → (orientation change) → TILTED
 *        → (sustained stillness) → FALL_CANDIDATE
 *   Any state → IDLE if the sub-sequence does not complete within FALL_SEQ_TIMEOUT_MS.
 *
 * Threshold values are [O] — see §9.  The defines below are starting points
 * based on published MPU-6050 fall-detection literature, not validated against
 * real data from this specific physical configuration.
 */

#ifndef FALL_DETECT_H
#define FALL_DETECT_H

#include <stdbool.h>
#include <stdint.h>
#include "mpu6050.h"

/* ── Threshold constants — [O] tuning required with real sensor data ──────── */

/** Accel magnitude spike that triggers SPIKE_SEEN (in g).
 *  Must exceed normal walking (~1.2 g) and sitting down (~1.5 g).
 *  Placeholder: 2.5 g — revisit with logged traces.                      */
#define SPIKE_THRESHOLD_G       2.5f

/** Orientation change required after a spike to trigger TILTED (in degrees).
 *  Normal walking tilt < 15°; a fall changes orientation substantially.
 *  Placeholder: 45°.                                                      */
#define TILT_THRESHOLD_DEG      45.0f

/** How long the unit must remain still after tilting (ms).
 *  Distinguishes a completed fall (stays down) from a stumble (recovers).
 *  Placeholder: 1500 ms.                                                  */
#define STILL_WINDOW_MS         1500u

/** Maximum time from first spike to FALL_CANDIDATE before the sequence resets.
 *  If spike → tilt → still takes longer than this, it wasn't a fall.     */
#define FALL_SEQ_TIMEOUT_MS     3000u

/* ── Calibration mode ────────────────────────────────────────────────────── */
/**
 * Define FALL_CALIBRATION_MODE at build time (-DFALL_CALIBRATION_MODE) to
 * enable raw sensor logging without triggering alarms.
 *
 * Calibration procedure:
 *   1. Build with -DFALL_CALIBRATION_MODE -DDEBUG_FALL
 *   2. Flash the soldier unit and connect serial monitor (115200)
 *   3. Walk normally for 2 minutes — record max accel magnitude seen
 *      → set SPIKE_THRESHOLD_G above this maximum (e.g. max + 0.5g)
 *   4. Sit down / kneel repeatedly — record max tilt change seen
 *      → set TILT_THRESHOLD_DEG above this maximum (e.g. max + 10°)
 *   5. Simulate a fall (drop unit onto a cushion at body-fall angle)
 *      → verify spike, tilt, and still phases trigger in sequence
 *      → adjust STILL_WINDOW_MS so the detector fires within 2s of impact
 *   6. Remove FALL_CALIBRATION_MODE and rebuild for deployment
 *
 * In calibration mode, fall_detect_update() logs raw values every call
 * but always returns FALL_EVENT_NONE (no alarm state changes).
 */

/* ── Event emitted to application layer ─────────────────────────────────── */

typedef enum {
    FALL_EVENT_NONE      = 0,
    FALL_EVENT_CANDIDATE = 1,   /**< Spike + tilt + stillness confirmed — start 10s window */
} fall_event_t;

/* ── Detector internal state (opaque to caller) ──────────────────────────── */

typedef enum {
    FD_IDLE         = 0,
    FD_SPIKE_SEEN   = 1,
    FD_TILTED       = 2,
    FD_STILL        = 3,
} fall_detector_state_t;

typedef struct {
    fall_detector_state_t state;
    uint32_t              spike_timestamp;   /**< Timestamp of initial impact spike */
    uint32_t              state_entered_ms;  /**< Time current state was entered    */
    float                 tilt_at_spike;     /**< Tilt angle when spike detected    */
} fall_detector_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/** Initialise detector to IDLE state. */
void fall_detect_init(fall_detector_t *fd);

/**
 * Feed one processed MPU-6050 sample into the detector.
 * Call at the sensor polling rate (e.g. every 20 ms for 50 Hz).
 *
 * @param fd        Detector instance.
 * @param raw       Raw reading from mpu6050_read_raw().
 * @param processed Output of mpu6050_process().
 * @param now_ms    Current time in ms (to_ms_since_boot(get_absolute_time())).
 * @return FALL_EVENT_CANDIDATE if the spike→tilt→still sequence completed.
 *         FALL_EVENT_NONE otherwise.
 */
fall_event_t fall_detect_update(fall_detector_t *fd,
                                const mpu6050_raw_t *raw,
                                const mpu6050_processed_t *processed,
                                uint32_t now_ms);

/** Reset detector to IDLE (call after a FALL_CANDIDATE is consumed). */
void fall_detect_reset(fall_detector_t *fd);

/** Return a human-readable state name (for debug logging). */
const char *fall_detect_state_name(fall_detector_state_t s);

#endif /* FALL_DETECT_H */
