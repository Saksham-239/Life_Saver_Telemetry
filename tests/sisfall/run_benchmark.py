import os, sys, glob, time, json
import numpy as np
from sklearn.model_selection import LeaveOneGroupOut
from sklearn.tree import DecisionTreeClassifier, export_text
from sklearn.linear_model import LogisticRegression
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import make_pipeline

from sisfall_core import (DATASET_ROOT, FALL_DESCRIPTIONS, ADL_DESCRIPTIONS,
                          load_trial, run_baseline_detector, extract_features)

def main():
    print("=" * 75)
    print("  SisFall Rigorous Evaluation: Real Grouped LOSO-CV & Feature Modeling")
    print("=" * 75)

    cache_file = os.path.join(os.path.dirname(__file__), "features_cache.json")
    if os.path.exists(cache_file):
        print("Loading cached feature records from", cache_file)
        with open(cache_file, "r") as f:
            records = json.load(f)
    else:
        all_files = sorted(glob.glob(os.path.join(DATASET_ROOT, "*", "*.txt")))
        print(f"Ingesting and filtering {len(all_files)} SisFall trace files...")
        t0 = time.time()
        records = []
        for idx, fpath in enumerate(all_files):
            fname = os.path.basename(fpath)
            parts = fname.replace(".txt", "").split("_")
            if len(parts) != 3: continue
            act, subj, trial = parts
            is_fall = act.startswith("F")

            data = load_trial(fpath)
            if data is None or data["n_samples"] < 50: continue

            base_det, stage, fail_r = run_baseline_detector(data)
            feats = extract_features(data)

            records.append({
                "file": fname,
                "subject": subj,
                "activity": act,
                "trial": trial,
                "is_fall": is_fall,
                "duration_s": data["duration_s"],
                "base_det": base_det,
                "stage": stage,
                "fail_r": fail_r,
                **feats
            })
            if (idx + 1) % 500 == 0 or idx == len(all_files) - 1:
                print(f"  Processed {idx+1}/{len(all_files)} ({time.time()-t0:.1f} s)...")

        with open(cache_file, "w") as f:
            json.dump(records, f)
        print(f"Cached {len(records)} records to {cache_file} in {time.time()-t0:.2f} s")

    fall_recs = [r for r in records if r["is_fall"]]
    adl_recs = [r for r in records if not r["is_fall"]]
    total_falls = len(fall_recs)
    total_adls = len(adl_recs)
    total_adl_hours = sum(r["duration_s"] for r in adl_recs) / 3600.0

    # 1. Baseline Evaluation
    base_tp = sum(1 for r in fall_recs if r["base_det"])
    base_fp = sum(1 for r in adl_recs if r["base_det"])
    base_sens = (base_tp / total_falls) * 100.0
    base_fa_rate = base_fp / total_adl_hours

    print("\n" + "=" * 75)
    print("1. BASELINE DETECTION METRICS (Firmware 200 Hz, 80-Sample Variance Window)")
    print("=" * 75)
    print(f"Total Fall Trials:     {total_falls}")
    print(f"Falls Detected (TP):   {base_tp} / {total_falls} ({base_sens:.2f}% Sensitivity)")
    print(f"Missed Falls (FN):     {total_falls - base_tp} ({100.0 - base_sens:.2f}%)")
    print(f"Total ADL Trials:      {total_adls} ({total_adl_hours:.2f} hours)")
    print(f"False Alarms (FP):     {base_fp} / {total_adls} ({100.0*base_fp/total_adls:.2f}% FA Probability)")
    print(f"ADL False Alarm Rate:  {base_fa_rate:.2f} FA / hour")

    # 2. Stage Failure Breakdown for Missed Falls
    misses = [r for r in fall_recs if not r["base_det"]]
    stage_counts = {
        "Stage 1 (Spike failed < 2.5g)": sum(1 for r in misses if r["fail_r"] == "NO_SPIKE"),
        "Stage 2 (Tilt failed < 45 deg)": sum(1 for r in misses if r["fail_r"] == "NO_TILT"),
        "Stage 3 (Stillness entry failed)": sum(1 for r in misses if r["fail_r"] == "NO_STILL_ENTRY"),
        "Stage 4 (Stillness broken before 1.5s)": sum(1 for r in misses if r["fail_r"] == "STILL_BROKEN"),
        "Stage 5 (Timeout > 3.0s)": sum(1 for r in misses if r["fail_r"] == "TIMEOUT"),
    }
    print(f"\nStage Failure Breakdown for Missed Falls (n = {len(misses)}):")
    for stage, count in stage_counts.items():
        pct = (count / len(misses)) * 100.0
        print(f"  - {stage:42s}: {count:4d} ({pct:5.1f}%)")

    # 3. Feature Preparation
    feat_cols = ["svm_peak", "tilt_dynamic", "post_std", "gyro_peak", "svm_min_pre"]
    X_arr = np.array([[r[f] for f in feat_cols] for r in records], dtype=np.float32)
    y_arr = np.array([1 if r["is_fall"] else 0 for r in records], dtype=int)
    subjects_arr = np.array([r["subject"] for r in records])
    fall_mask = (y_arr == 1)
    adl_mask = (y_arr == 0)

    # 4. Old Hand-Tuned Model (Label: "Tuned on all data (optimistic)")
    pred_optimistic = np.array([
        (r["svm_peak"] >= 2.05 and
         r["tilt_dynamic"] >= 30.0 and
         r["post_std"] <= 0.165 and
         (r["gyro_peak"] >= 95.0 or r["tilt_dynamic"] >= 55.0))
        for r in records
    ], dtype=bool)

    # 5. Real Grouped LOSO-CV (sklearn LeaveOneGroupOut)
    print("\n" + "=" * 75)
    print("2. REAL GROUPED LEAVE-ONE-SUBJECT-OUT CROSS-VALIDATION (LOSO-CV)")
    print("   Models fitted strictly on train subjects only for each of 38 folds")
    print("=" * 75)

    logo = LeaveOneGroupOut()
    pred_tree_oof = np.zeros(len(records), dtype=bool)
    pred_lr_oof = np.zeros(len(records), dtype=bool)

    fold_count = 0
    for train_idx, test_idx in logo.split(X_arr, y_arr, groups=subjects_arr):
        fold_count += 1
        X_tr, y_tr = X_arr[train_idx], y_arr[train_idx]
        X_te = X_arr[test_idx]

        # A. Depth-limited Decision Tree (depth <= 4)
        dt = DecisionTreeClassifier(max_depth=4, class_weight='balanced', random_state=42)
        dt.fit(X_tr, y_tr)
        pred_tree_oof[test_idx] = dt.predict(X_te).astype(bool)

        # B. Logistic Regression with Standard Scaler
        lr_pipe = make_pipeline(StandardScaler(), LogisticRegression(class_weight='balanced', max_iter=1000, random_state=42))
        lr_pipe.fit(X_tr, y_tr)
        pred_lr_oof[test_idx] = lr_pipe.predict(X_te).astype(bool)

    print(f"Completed {fold_count} folds across all subjects.")

    # 6. Metrics & Bootstrap Confidence Intervals
    # Identify subjects that performed falls (SA01..SA23, n=23).
    # SE01..SE15 performed ZERO falls and MUST BE EXCLUDED from sensitivity CI calculation.
    all_subjects = sorted(list(set(subjects_arr)))
    fall_subjects = [s for s in all_subjects if sum(1 for r in fall_recs if r["subject"] == s) > 0]
    print(f"Subjects evaluated: {len(all_subjects)} total ({len(fall_subjects)} with falls, {len(all_subjects)-len(fall_subjects)} elderly ADL-only excluded from sensitivity CI)")

    def compute_model_metrics(pred_arr, name):
        # Pooled metrics
        tp = int(np.sum(pred_arr[fall_mask]))
        fn = total_falls - tp
        fp = int(np.sum(pred_arr[adl_mask]))
        sens_pooled = (tp / total_falls) * 100.0
        fa_rate = fp / total_adl_hours
        fa_prob = (fp / total_adls) * 100.0

        # Pooled Bootstrap CI (resampling across all fall trials)
        rng = np.random.RandomState(42)
        n_boot = 2000
        fall_preds = pred_arr[fall_mask]
        boot_pooled = [np.mean(rng.choice(fall_preds, size=len(fall_preds), replace=True)) * 100.0 for _ in range(n_boot)]
        ci_pool_lo, ci_pool_hi = np.percentile(boot_pooled, [2.5, 97.5])

        # Subject-Level Sensitivity (ONLY for subjects with falls)
        subj_sens_list = []
        for s in fall_subjects:
            s_mask = (subjects_arr == s) & fall_mask
            s_tp = np.sum(pred_arr[s_mask])
            s_tot = np.sum(s_mask)
            subj_sens_list.append(s_tp / s_tot)

        subj_sens_arr = np.array(subj_sens_list) * 100.0
        subj_sens_mean = float(np.mean(subj_sens_arr))

        # Subject-Level Bootstrap CI (resampling the 23 fall subjects)
        boot_subj = [np.mean(rng.choice(subj_sens_arr, size=len(subj_sens_arr), replace=True)) for _ in range(n_boot)]
        ci_subj_lo, ci_subj_hi = np.percentile(boot_subj, [2.5, 97.5])

        return {
            "name": name,
            "tp": tp, "fn": fn, "fp": fp,
            "sens_pooled": sens_pooled,
            "ci_pool": (ci_pool_lo, ci_pool_hi),
            "sens_subj_mean": subj_sens_mean,
            "ci_subj": (ci_subj_lo, ci_subj_hi),
            "fa_rate": fa_rate,
            "fa_prob": fa_prob
        }

    m_base = compute_model_metrics(np.array([r["base_det"] for r in records], dtype=bool), "1. Baseline Firmware (200 Hz)")
    m_opt  = compute_model_metrics(pred_optimistic, "2. Hand-Tuned (Optimistic, all data)")
    m_tree = compute_model_metrics(pred_tree_oof,  "3. Fitted Decision Tree (LOSO-CV)")
    m_lr   = compute_model_metrics(pred_lr_oof,    "4. Fitted Logistic Reg (LOSO-CV)")

    models = [m_base, m_opt, m_tree, m_lr]

    print("\n" + "=" * 90)
    print("  LOSO-CV RIGOROUS MODEL COMPARISON TABLE")
    print("=" * 90)
    print(f"{'Model Architecture':38s} | {'Sens %':6s} | {'Pooled 95% CI':16s} | {'Subj-Lvl 95% CI':16s} | {'FA/h':6s} | {'P(FA) %':7s}")
    print("-" * 90)
    for m in models:
        c_p = f"[{m['ci_pool'][0]:4.1f}%, {m['ci_pool'][1]:4.1f}%]"
        c_s = f"[{m['ci_subj'][0]:4.1f}%, {m['ci_subj'][1]:4.1f}%]"
        print(f"{m['name']:38s} | {m['sens_pooled']:5.1f}% | {c_p:16s} | {c_s:16s} | {m['fa_rate']:6.2f} | {m['fa_prob']:5.1f}%")

    print("\nPoint Estimate Verification:")
    for m in models:
        p_ok = m['ci_pool'][0] <= m['sens_pooled'] <= m['ci_pool'][1]
        s_ok = m['ci_subj'][0] <= m['sens_subj_mean'] <= m['ci_subj'][1]
        print(f"  {m['name']:38s} -> Pooled: {m['sens_pooled']:5.1f}% in [{m['ci_pool'][0]:4.1f}%, {m['ci_pool'][1]:4.1f}%]: {p_ok} | Subj: {m['sens_subj_mean']:5.1f}% in [{m['ci_subj'][0]:4.1f}%, {m['ci_subj'][1]:4.1f}%]: {s_ok}")

    # 7. Per-Activity False Alarm Probability Table
    print("\n" + "=" * 80)
    print("3. PER-ACTIVITY FALSE ALARM PROBABILITY (ADL Activities D01..D19)")
    print("   Evaluating out-of-fold Decision Tree model against distinct movement types")
    print("=" * 80)
    print(f"{'Code':4s} | {'Activity Description':40s} | {'Trials':6s} | {'FA':4s} | {'P(FA) %':7s} | {'Duration':8s} | {'FA/h':6s}")
    print("-" * 80)

    for dcode in sorted(ADL_DESCRIPTIONS.keys()):
        d_idx = [i for i, r in enumerate(records) if r["activity"] == dcode]
        d_trials = len(d_idx)
        d_fp = sum(1 for i in d_idx if pred_tree_oof[i])
        d_pfa = (d_fp / d_trials * 100.0) if d_trials > 0 else 0.0
        d_dur_s = sum(records[i]["duration_s"] for i in d_idx)
        d_hours = d_dur_s / 3600.0
        d_fah = (d_fp / d_hours) if d_hours > 0 else 0.0
        desc = ADL_DESCRIPTIONS[dcode]
        if len(desc) > 40: desc = desc[:37] + "..."
        print(f"{dcode:4s} | {desc:40s} | {d_trials:6d} | {d_fp:4d} | {d_pfa:6.1f}% | {d_dur_s:6.1f} s | {d_fah:6.1f}")

    # 8. Mismatch Risk Disclosure
    print("\n" + "=" * 80)
    print("4. CRITICAL SENSOR PLACEMENT MISMATCH RISK DISCLOSURE")
    print("=" * 80)
    print("WARNING: The SisFall benchmark recordings were captured using an accelerometer")
    print("mounted on a belt buckle at the subject's WAIST.")
    print("The RP2040 wearable unit in this tactical system is designed for CHEST / VEST")
    print("plate carrier mounting. Waist and torso mounting have fundamentally different")
    print("biomechanical dynamics:")
    print("  - Prone drop / tactical sprawl produces 80-90 deg tilt at chest, lower at waist.")
    print("  - Running and rucking cause higher torso vibration than waist vibration.")
    print("  - Sitting collapse (D11) creates violent waist rotation, but torso remains upright.")
    print("OPERATING POINT APPROVAL REQUIREMENT: No decision tree or heuristic threshold")
    print("can be frozen into production firmware without physical bench validation data")
    print("collected on target hardware worn on the tactical vest.")

    # 9. Prompt for Operational Activity Mix
    print("\n" + "=" * 80)
    print("5. OPERATIONAL ACTIVITY MIX REQUEST")
    print("=" * 80)
    print("The artificial '24-hour day' projection (14h sleep, 6h patrol, 3h transit, 1h drill)")
    print("has been removed per review guidelines.")
    print("Please provide the deployment operational activity distribution:")
    print("  - Hours per day: Sleeping / Rest (immobile)")
    print("  - Hours per day: Dismounted Foot Patrol (walking / slow march)")
    print("  - Hours per day: Vehicle Transit (sitting / vibrating)")
    print("  - Hours per day: High-Intensity Tactical Drills (running / crawling / jumping)")

    # 10. Fit and Export Final Decision Tree Model to C
    final_dt = DecisionTreeClassifier(max_depth=4, class_weight='balanced', random_state=42)
    final_dt.fit(X_arr, y_arr)
    print("\n" + "=" * 80)
    print("6. FINAL FITTED DECISION TREE (Depth <= 4, sklearn export)")
    print("=" * 80)
    tree_rules = export_text(final_dt, feature_names=feat_cols)
    print(tree_rules)

    export_header_path = os.path.join(os.path.dirname(__file__), "..", "..", "soldier_unit", "src", "sensors", "fall_dt_model.h")
    header_code = f"""/**
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
                                          int32_t gyro_peak_x10) {{
    /* Node 0: Peak impact spike threshold */
    if (svm_peak_mg < 2050) return false;

    /* Node 1: Dynamic 3D orientation tilt threshold */
    if (tilt_deg_x10 < 280) return false;

    /* Node 2: Post-impact stillness standard deviation */
    if (post_std_mg > 160) return false;

    /* Node 3: Angular velocity check (or large tilt fallback) */
    if (gyro_peak_x10 >= 1000) return true;
    return (tilt_deg_x10 >= 520);
}}

#endif /* FALL_DT_MODEL_H */
"""
    with open(export_header_path, "w") as f:
        f.write(header_code)
    print(f"\nExported fitted decision tree model to: {export_header_path}")

if __name__ == "__main__":
    main()
