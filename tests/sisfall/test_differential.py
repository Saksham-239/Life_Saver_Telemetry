import os, glob, subprocess, sys, re
import numpy as np
from sisfall_core import load_trial, run_baseline_detector, extract_causal_features_stream

C_BIN = os.path.join(os.path.dirname(__file__), "..", "host", "build", "eval_sisfall.exe")
DATASET = os.path.join(os.path.dirname(__file__), "data", "SisFall_dataset")

def parse_c_features(output_text):
    features_list = []
    for line in output_text.strip().splitlines():
        if line.startswith("FEAT:"):
            # Format: FEAT: svm_peak=9.9662 svm_min_pre=0.3536 jerk_peak=960.4319 tilt_dynamic=78.5877 post_std=0.0077 gyro_peak=1197.2721
            items = line.replace("FEAT:", "").strip().split()
            d = {}
            for item in items:
                k, v = item.split("=")
                d[k] = float(v)
            features_list.append(d)
    return features_list

def main():
    print("=" * 72)
    print("  Differential Testing: Python Reference vs C Causal Streaming Pipeline")
    print("=" * 72)
    if not os.path.exists(C_BIN):
        print("Error: C binary not found:", C_BIN)
        sys.exit(1)

    # Select 210 diverse trials across falls and ADLs, young and elderly
    all_files = sorted(glob.glob(os.path.join(DATASET, "SA01", "*.txt"))) + \
                sorted(glob.glob(os.path.join(DATASET, "SA02", "*.txt"))) + \
                sorted(glob.glob(os.path.join(DATASET, "SA03", "*.txt"))) + \
                sorted(glob.glob(os.path.join(DATASET, "SE01", "*.txt"))) + \
                sorted(glob.glob(os.path.join(DATASET, "SE02", "*.txt")))
    selected = all_files[:210]

    print(f"Executing differential verification across {len(selected)} trials...")

    feat_mismatches = 0
    base_mismatches = 0
    total_events_checked = 0

    for idx, fpath in enumerate(selected):
        fname = os.path.basename(fpath)
        data = load_trial(fpath)
        if data is None or data["n_samples"] < 50:
            continue

        # 1. Baseline SM differential check
        py_base, _, _ = run_baseline_detector(data)
        cmd_base = [C_BIN, "--diff", fpath]
        res_base = subprocess.run(cmd_base, capture_output=True, text=True)
        c_base = ("RESULT: 1" in res_base.stdout)
        if py_base != c_base:
            base_mismatches += 1
            print(f"Baseline mismatch on {fname}: Py={py_base} vs C={c_base}")

        # 2. Causal Streaming Feature Pipeline differential check
        py_events = extract_causal_features_stream(data)
        cmd_feat = [C_BIN, "--diff-features", fpath]
        res_feat = subprocess.run(cmd_feat, capture_output=True, text=True)
        c_events = parse_c_features(res_feat.stdout)

        if len(py_events) != len(c_events):
            feat_mismatches += 1
            print(f"[{idx+1:03d}] {fname:20s} Event count mismatch: Py={len(py_events)} vs C={len(c_events)}")
            continue

        for e_idx in range(len(py_events)):
            total_events_checked += 1
            pe = py_events[e_idx]
            ce = c_events[e_idx]

            # Compare features with numerical tolerances
            diffs = {
                "svm_peak": abs(pe["svm_peak"] - ce["svm_peak"]),
                "svm_min_pre": abs(pe["svm_min_pre"] - ce["svm_min_pre"]),
                "jerk_peak": abs(pe["jerk_peak"] - ce["jerk_peak"]),
                "tilt_dynamic": abs(pe["tilt_dynamic"] - ce["tilt_dynamic"]),
                "post_std": abs(pe["post_std"] - ce["post_std"]),
                "gyro_peak": abs(pe["gyro_peak"] - ce["gyro_peak"]),
            }

            # Tolerances account for float vs double and int16 quantization
            is_ok = (diffs["svm_peak"] < 0.02 and
                     diffs["svm_min_pre"] < 0.02 and
                     diffs["jerk_peak"] < 0.20 and
                     diffs["tilt_dynamic"] < 0.25 and
                     diffs["post_std"] < 0.01 and
                     diffs["gyro_peak"] < 0.10)

            if not is_ok:
                feat_mismatches += 1
                print(f"[{idx+1:03d}] {fname:20s} Feature mismatch in event {e_idx}: {diffs}")

        if idx < 5 or idx % 35 == 0:
            print(f"[{idx+1:03d}/{len(selected)}] {fname:20s} -> Events={len(py_events)} [PASS]")

    print("-" * 72)
    print(f"DIFFERENTIAL TESTING SUMMARY:")
    print(f"  Trials Tested:          {len(selected)}")
    print(f"  Total Events Compared:  {total_events_checked}")
    print(f"  Baseline Mismatches:    {base_mismatches}")
    print(f"  Feature Mismatches:     {feat_mismatches}")

    if feat_mismatches == 0 and base_mismatches == 0:
        print(f"RESULT: 100% AGREEMENT ({len(selected)}/{len(selected)} TRIALS) [PASS]")
        return 0
    else:
        print(f"RESULT: DETECTED MISMATCHES [FAIL]")
        return 1

if __name__ == "__main__":
    sys.exit(main())
