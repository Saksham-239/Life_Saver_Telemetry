/**
 * fall_dt_model.h - Depth-Limited Fixed-Point Decision Tree (Depth <= 4)
 * Fitted via sklearn DecisionTreeClassifier(max_depth=4, class_weight='balanced')
 * on SisFall 200 Hz dataset.
 *
 * NOTE: PENDING OPERATING POINT APPROVAL.
 * DO NOT integrate into fall_detect.c without approved operating point.
 *
 * Fixed-point units:
 *   svm_peak_mg:   g * 1000
 *   tilt_deg_x10:  deg * 10
 *   post_std_mg:   g * 1000
 *   gyro_peak_x10: deg/s * 10
 */

#ifndef FALL_DT_MODEL_H
#define FALL_DT_MODEL_H

#include <stdint.h>
#include <stdbool.h>

static inline bool fall_dt_classify_fixed(int32_t svm_peak_mg,
                                          int32_t tilt_deg_x10,
                                          int32_t post_std_mg,
                                          int32_t gyro_peak_x10) {
    /* Node 0: Peak impact spike threshold */
    if (svm_peak_mg < 2050) return false;

    /* Node 1: Dynamic 3D orientation tilt threshold */
    if (tilt_deg_x10 < 280) return false;

    /* Node 2: Post-impact stillness standard deviation */
    if (post_std_mg > 160) return false;

    /* Node 3: Angular velocity check (or large tilt fallback) */
    if (gyro_peak_x10 >= 1000) return true;
    return (tilt_deg_x10 >= 520);
}

#endif /* FALL_DT_MODEL_H */
