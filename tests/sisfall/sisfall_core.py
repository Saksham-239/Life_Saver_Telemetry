import os, sys, glob, math, time, json
import numpy as np
from scipy import signal

DATASET_ROOT = os.path.join(os.path.dirname(__file__), "data", "SisFall_dataset")

# Official descriptions per SisFall Readme.txt
FALL_DESCRIPTIONS = {
    "F01": "Fall forward while walking caused by a slip",
    "F02": "Fall backward while walking caused by a slip",
    "F03": "Lateral fall while walking caused by a slip",
    "F04": "Fall forward while walking caused by a trip",
    "F05": "Fall forward while jogging caused by a trip",
    "F06": "Vertical fall while walking caused by fainting",
    "F07": "Fall while walking, with use of hands in a table to dampen fall, caused by fainting",
    "F08": "Fall forward when trying to get up",
    "F09": "Lateral fall when trying to get up",
    "F10": "Fall forward when trying to sit down",
    "F11": "Fall backward when trying to sit down",
    "F12": "Lateral fall when trying to sit down",
    "F13": "Fall forward while sitting, caused by fainting or falling asleep",
    "F14": "Fall backward while sitting, caused by fainting or falling asleep",
    "F15": "Lateral fall while sitting, caused by fainting or falling asleep",
}

ADL_DESCRIPTIONS = {
    "D01": "Walking slowly (100s)",
    "D02": "Walking quickly (100s)",
    "D03": "Jogging slowly (100s)",
    "D04": "Jogging quickly (100s)",
    "D05": "Walking upstairs and downstairs slowly (25s)",
    "D06": "Walking upstairs and downstairs quickly (25s)",
    "D07": "Slowly sit in half-height chair, wait, up slowly (12s)",
    "D08": "Quickly sit in half-height chair, wait, up quickly (12s)",
    "D09": "Slowly sit in low height chair, wait, up slowly (12s)",
    "D10": "Quickly sit in low height chair, wait, up quickly (12s)",
    "D11": "Sitting a moment, trying to get up, and collapse into chair (12s)",
    "D12": "Sitting a moment, lying slowly, wait, sit again (12s)",
    "D13": "Sitting a moment, lying quickly, wait, sit again (12s)",
    "D14": "Being on back change to lateral, wait, back to back (12s)",
    "D15": "Standing, slowly bending at knees, getting up (12s)",
    "D16": "Standing, slowly bending without bending knees, getting up (12s)",
    "D17": "Standing, get into car, remain seated, get out (25s)",
    "D18": "Stumble while walking (12s)",
    "D19": "Gently jump without falling (12s)",
}

BUTTER_B, BUTTER_A = signal.butter(2, 44.0, btype="low", fs=200.0)

def load_trial(filepath):
    with open(filepath, "r") as f:
        lines = [line.strip().rstrip(";").split(",") for line in f if line.strip()]
    if not lines:
        return None
    raw = np.array([[float(x) for x in l[:6]] for l in lines], dtype=np.float32)
    # ADXL345: 256 LSB/g
    adxl = raw[:, 0:3] / 256.0
    # ITG3200: 16.384 LSB/dps
    itg = raw[:, 3:6] / 16.384

    # Map to firmware axes: ax = x, ay = z, az = -y (waist belt vertical)
    ax_raw = adxl[:, 0]
    ay_raw = adxl[:, 2]
    az_raw = -adxl[:, 1]

    # CFG 3 Butterworth 44 Hz DLPF
    ax = signal.lfilter(BUTTER_B, BUTTER_A, ax_raw)
    ay = signal.lfilter(BUTTER_B, BUTTER_A, ay_raw)
    az = signal.lfilter(BUTTER_B, BUTTER_A, az_raw)

    return {
        "ax": ax, "ay": ay, "az": az,
        "gx": itg[:, 0], "gy": itg[:, 2], "gz": -itg[:, 1],
        "n_samples": len(ax),
        "duration_s": len(ax) / 200.0
    }

def run_baseline_detector(t):
    ax, ay, az = t["ax"], t["ay"], t["az"]
    n = len(ax)
    mag = np.sqrt(ax*ax + ay*ay + az*az)
    cos_tilt = np.clip(az / np.maximum(mag, 0.01), -1.0, 1.0)
    tilt = np.arccos(cos_tilt) * (180.0 / np.pi)

    stat = np.zeros(n, dtype=bool)
    for i in range(1, n):
        w = mag[max(0, i - 79):i + 1]
        if len(w) >= 2:
            stat[i] = (np.var(w) < 0.01)

    state = "IDLE"
    spike_t = 0
    state_t = 0
    tilt_at_spike = 0.0
    stage_reached = 0
    fail_reason = "NONE"

    for i in range(n):
        now_ms = int(i * 5)
        if state != "IDLE" and (now_ms - spike_t > 3000):
            fail_reason = "TIMEOUT"
            state = "IDLE"

        if state == "IDLE":
            if mag[i] > 2.5:
                state = "SPIKE_SEEN"
                spike_t = now_ms
                state_t = now_ms
                tilt_at_spike = tilt[i]
                stage_reached = max(stage_reached, 1)
        elif state == "SPIKE_SEEN":
            if abs(tilt[i] - tilt_at_spike) > 45.0:
                state = "TILTED"
                stage_reached = max(stage_reached, 2)
        elif state == "TILTED":
            if stat[i]:
                state = "STILL"
                state_t = now_ms
                stage_reached = max(stage_reached, 3)
        elif state == "STILL":
            if not stat[i]:
                state = "IDLE"
                fail_reason = "STILL_BROKEN"
            elif (now_ms - state_t >= 1500):
                return True, 4, "EMITTED"

    if fail_reason == "NONE":
        if stage_reached == 0: fail_reason = "NO_SPIKE"
        elif stage_reached == 1: fail_reason = "NO_TILT"
        elif stage_reached == 2: fail_reason = "NO_STILL_ENTRY"

    return False, stage_reached, fail_reason

def extract_features(t):
    ax, ay, az = t["ax"], t["ay"], t["az"]
    gx, gy, gz = t["gx"], t["gy"], t["gz"]
    n = len(ax)

    mag = np.sqrt(ax*ax + ay*ay + az*az)
    omega = np.sqrt(gx*gx + gy*gy + gz*gz)

    peak_idx = int(np.argmax(mag))
    svm_peak = float(mag[peak_idx])
    gyro_peak = float(np.max(omega))

    pre_start = max(0, peak_idx - 100)
    svm_min_pre = float(np.min(mag[pre_start:peak_idx])) if peak_idx > pre_start else float(mag[peak_idx])

    jerk = np.abs(np.diff(mag)) * 200.0
    jerk_peak = float(np.max(jerk)) if len(jerk) > 0 else 0.0

    pre_w_start = max(0, peak_idx - 120)
    pre_w_end = max(1, peak_idx - 20)
    if pre_w_end > pre_w_start:
        g_pre = np.array([np.mean(ax[pre_w_start:pre_w_end]),
                          np.mean(ay[pre_w_start:pre_w_end]),
                          np.mean(az[pre_w_start:pre_w_end])])
    else:
        g_pre = np.array([ax[0], ay[0], az[0]])

    post_w_start = min(n - 1, peak_idx + 200)
    post_w_end = min(n, peak_idx + 400)
    if post_w_end > post_w_start:
        g_post = np.array([np.mean(ax[post_w_start:post_w_end]),
                           np.mean(ay[post_w_start:post_w_end]),
                           np.mean(az[post_w_start:post_w_end])])
        post_std = float(np.std(mag[post_w_start:post_w_end]))
    else:
        g_post = g_pre
        post_std = 0.5

    norm_pre = np.linalg.norm(g_pre)
    norm_post = np.linalg.norm(g_post)
    if norm_pre > 0.1 and norm_post > 0.1:
        cos_dot = np.clip(np.dot(g_pre, g_post) / (norm_pre * norm_post), -1.0, 1.0)
        tilt_dynamic = float(np.arccos(cos_dot) * (180.0 / np.pi))
    else:
        tilt_dynamic = 0.0

    return {
        "svm_peak": svm_peak,
        "svm_min_pre": svm_min_pre,
        "jerk_peak": jerk_peak,
        "tilt_dynamic": tilt_dynamic,
        "post_std": post_std,
        "gyro_peak": gyro_peak
    }

class CausalDetector:
    def __init__(self):
        self.ring_ax = np.zeros(128, dtype=np.float32)
        self.ring_ay = np.zeros(128, dtype=np.float32)
        self.ring_az = np.zeros(128, dtype=np.float32)
        self.ring_head = 0
        self.samples_received = 0
        self.state = "IDLE"
        self.spike_time_ms = 0
        self.lockout_until_ms = 0
        self.pre_svm_min = 0.0
        self.g_pre = np.zeros(3, dtype=np.float32)
        self.peak_svm = 0.0
        self.peak_jerk = 0.0
        self.peak_gyro = 0.0
        self.last_svm = 0.0
        self.post_count = 0
        self.sum_ax = 0.0
        self.sum_ay = 0.0
        self.sum_az = 0.0
        self.sum_svm = 0.0
        self.sum_svm_sq = 0.0

    def feed(self, ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps, now_ms):
        svm = float(np.sqrt(ax_g*ax_g + ay_g*ay_g + az_g*az_g))
        omega = float(np.sqrt(gx_dps*gx_dps + gy_dps*gy_dps + gz_dps*gz_dps))
        jerk = float(abs(svm - self.last_svm) * 200.0)
        self.last_svm = svm

        self.ring_ax[self.ring_head] = ax_g
        self.ring_ay[self.ring_head] = ay_g
        self.ring_az[self.ring_head] = az_g
        self.ring_head = (self.ring_head + 1) % 128
        self.samples_received += 1

        if self.state == "IDLE":
            if now_ms < self.lockout_until_ms:
                return None
            if svm >= 2.5 and self.samples_received >= 120:
                self.spike_time_ms = now_ms
                self.state = "IMPACT_TRACK"
                # g_pre from [-120 .. -20]
                idxs = [(self.ring_head + 128 - offset) % 128 for offset in range(120, 19, -1)]
                self.g_pre = np.array([np.mean(self.ring_ax[idxs]),
                                       np.mean(self.ring_ay[idxs]),
                                       np.mean(self.ring_az[idxs])], dtype=np.float32)
                # min_pre from [-100 .. -1]
                min_idxs = [(self.ring_head + 128 - offset) % 128 for offset in range(100, 0, -1)]
                mags = np.sqrt(self.ring_ax[min_idxs]**2 + self.ring_ay[min_idxs]**2 + self.ring_az[min_idxs]**2)
                self.pre_svm_min = float(np.min(mags))
                self.peak_svm = svm
                self.peak_jerk = jerk
                self.peak_gyro = omega

        elif self.state == "IMPACT_TRACK":
            self.peak_svm = max(self.peak_svm, svm)
            self.peak_jerk = max(self.peak_jerk, jerk)
            self.peak_gyro = max(self.peak_gyro, omega)
            if now_ms >= self.spike_time_ms + 100:
                self.state = "POST_WAIT"

        elif self.state == "POST_WAIT":
            if now_ms >= self.spike_time_ms + 1000:
                self.state = "POST_ACCUM"
                self.post_count = 0
                self.sum_ax = 0.0
                self.sum_ay = 0.0
                self.sum_az = 0.0
                self.sum_svm = 0.0
                self.sum_svm_sq = 0.0

        elif self.state == "POST_ACCUM":
            self.sum_ax += ax_g
            self.sum_ay += ay_g
            self.sum_az += az_g
            self.sum_svm += svm
            self.sum_svm_sq += svm * svm
            self.post_count += 1
            if now_ms >= self.spike_time_ms + 2000:
                N = self.post_count
                g_post = np.array([self.sum_ax / N, self.sum_ay / N, self.sum_az / N], dtype=np.float32)
                norm_pre = float(np.linalg.norm(self.g_pre))
                norm_post = float(np.linalg.norm(g_post))
                tilt_dyn = 0.0
                if norm_pre > 0.1 and norm_post > 0.1:
                    cos_dot = np.clip(np.dot(self.g_pre, g_post) / (norm_pre * norm_post), -1.0, 1.0)
                    tilt_dyn = float(np.arccos(cos_dot) * (180.0 / np.pi))
                mean_svm = self.sum_svm / N
                var_svm = (self.sum_svm_sq / N) - (mean_svm * mean_svm)
                post_std = float(np.sqrt(max(0.0, var_svm)))
                self.lockout_until_ms = now_ms + 1500
                self.state = "IDLE"
                return {
                    "svm_peak": self.peak_svm,
                    "svm_min_pre": self.pre_svm_min,
                    "jerk_peak": self.peak_jerk,
                    "tilt_dynamic": tilt_dyn,
                    "post_std": post_std,
                    "gyro_peak": self.peak_gyro
                }
        return None

def extract_causal_features_stream(t):
    ax, ay, az = t["ax"], t["ay"], t["az"]
    gx, gy, gz = t["gx"], t["gy"], t["gz"]
    n = len(ax)
    det = CausalDetector()
    events = []
    for i in range(n):
        now_ms = int(i * 5)
        res = det.feed(ax[i], ay[i], az[i], gx[i], gy[i], gz[i], now_ms)
        if res is not None:
            events.append(res)
    return events
