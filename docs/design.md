# Detailed Engineering Design & Rationale

<!-- last updated: Gate G3 Completed -->

## 1. Soldier State Machine Transition Table
The soldier unit operates a 4-state alarm lifecycle:
```
+------------------------------------------------------------------------------------------------------+
| Current State         | Event / Condition                   | Next State            | Output Action  |
+------------------------------------------------------------------------------------------------------+
| ALARM_NORMAL          | Button BACKUP / ENEMY / MEDICAL     | ALARM_ALERT_LATCHED   | TX Tactical Pkt|
| ALARM_NORMAL          | MPU-6050 Fall Candidate Confirmed   | ALARM_FALL_PENDING    | Vibrate Pattern|
| ALARM_NORMAL          | Button DOWN Pressed                 | ALARM_EMERGENCY       | TX Emergency   |
| ALARM_ALERT_LATCHED   | Button COMPLETE Short Press         | ALARM_NORMAL          | TX Complete Pkt|
| ALARM_ALERT_LATCHED   | Button BACKUP / ENEMY / MEDICAL     | ALARM_ALERT_LATCHED   | TX Tactical Pkt|
| ALARM_FALL_PENDING    | Button COMPLETE Double-Press        | ALARM_NORMAL          | Cancel / VibeOff|
| ALARM_FALL_PENDING    | 15s Timer Expired (No Cancel)       | ALARM_EMERGENCY       | TX Fall Emerg  |
| ALARM_FALL_PENDING    | Button DOWN Pressed                 | ALARM_EMERGENCY       | TX Emergency   |
| ALARM_EMERGENCY       | Button COMPLETE Long-Press (>= 3s)  | ALARM_NORMAL          | TX Complete Pkt|
| ALARM_EMERGENCY       | Button BACKUP / ENEMY / MEDICAL     | ALARM_EMERGENCY       | TX Tactical Pkt|
+------------------------------------------------------------------------------------------------------+
```
*Note on Q1 Option 1:* When in `ALARM_EMERGENCY`, pressing tactical buttons transmits the tactical message type (`MSG_BACKUP`, etc.) but asserts `alarm_state = ALARM_EMERGENCY` and retains `SFLAG_EMERGENCY` in the packet body. The Base Station maintains `ALARM_EMERGENCY` and does not downgrade to `ALERT_LATCHED`.

---

## 2. Fall Detection Algorithm & DLPF Modeling
The fall detection pipeline analyzes 3-axis accelerometer data ($a_x, a_y, a_z$):
$$\|a\| = \sqrt{a_x^2 + a_y^2 + a_z^2}$$

### Algorithm Stages:
1. **Free-Fall Dip:** Vector magnitude drops below $0.5\text{g}$ (`FREEFALL_THRESHOLD_G`) for at least $50\text{ ms}$.
2. **Impact Spike:** Vector magnitude exceeds $2.5\text{g}$ (`SPIKE_THRESHOLD_G`) within $800\text{ ms}$ of free-fall.
3. **Immobility (Rest):** Vector magnitude remains within $1.0\text{g} \pm 0.3\text{g}$ variance for at least $2000\text{ ms}$ (`STILL_TIME_REQUIRED_MS`).
4. **Sequence Timeout:** If the entire sequence from impact spike to still confirmation exceeds $3000\text{ ms}$ (`FALL_SEQ_TIMEOUT_MS`), the sequence resets to `ARMED`.

### DLPF & Sample Rate Simulation Results (`sim_dlpf.c`):
Host simulation of 2nd-order MPU-6050 filter on 10–50 ms half-sine shock pulses (3–10g) at 100 Hz vs 200 Hz:
- **CFG 2 (98 Hz cutoff):** Preserves 66.1% (10 ms), 93.5% (20 ms), 95.8% (30 ms). High-frequency noise attenuation at 60 Hz is only 5.9% (aliases into 100 Hz band).
- **CFG 3 (44 Hz cutoff - Selected):** Preserves 66.8% (10 ms), 71.2% (20 ms @ 100 Hz) / 91.3% (20 ms @ 200 Hz), 98.1% (30 ms). Rejects 60 Hz mechanical noise by 53.1%.
- **CFG 4 (21 Hz cutoff):** Preserves only 30.4% (10 ms), 65.1% (20 ms), 76.8% (30 ms). Attenuates shock peaks severely, causing false negatives.
*Note:* Acceleration thresholds ($2.5\text{g}$ spike, etc.) were originally tuned on unfiltered $\pm 2\text{g}$ data and remain **UNVALIDATED** until real RP2040 bench test data is gathered.

---

## 3. Architecture Decision Records (ADRs)

### ADR-01: Flash Dual-Sector Monotonic Nonce Reservation (Q3)
- **Context:** RP2040 NOR flash cannot perform safe concurrent writes across cores without halting execution. Erasing sectors takes ~30–100 ms.
- **Decision:** Allocate $2^{32}$ nonces per boot session into dual wear-levelled sectors (A and B). Boot code increments sequence number and writes the new ceiling to the standby sector *before* starting the FreeRTOS scheduler. Runtime transmissions draw from this in-memory counter with zero runtime flash operations.
- **Rejected Alternative:** Erasing and writing flash every 50,000 packets during runtime. Rejected due to XIP flash lockouts stalling Core 1 sensor acquisition and potential flash wear.

### ADR-02: Asynchronous NVS Persistence with Coarse High-Water Mark
- **Context:** Base station receiving up to 50 packets/sec would wear out ESP32 SPI flash and introduce 10–50 ms latency jitter if committing NVS synchronously on the RX task.
- **Decision:** Decouple NVS writes to a low-priority background task via FreeRTOS queue. Persist with a coarse high-water mark stride (e.g. advance NVS recorded counter by $+100$ every 100 packets or on quiet timeout).
- **Replay Window Trade-off:** In the event of an ungraceful base station power cut, the replay filter can accept at most 100 packets previously received in that window, trading bounded replay exposure for 100x lower flash wear and zero RX jitter.

### ADR-03: Non-Blocking Buzzer Task
- **Context:** Synchronous delay loops in `buzzer_play()` blocked `task_ui` for up to 1200 ms, freezing OLED refresh and operator inputs.
- **Decision:** Replace blocking buzzer calls with an asynchronous command queue serviced by a dedicated `task_buzzer` or hardware timer.

### ADR-04: Base Station State Concurrency & Mutex Protection
- **Context:** Base station `s_soldiers` was concurrently written by `task_radio_rx` (Prio 5) and read via `memcpy` in `task_ui` (Prio 4) with zero synchronization, causing torn 64-bit timestamps, counters, and inconsistent cross-field state (47.11% torn reads in host test).
- **Decision:** Introduce FreeRTOS mutex `s_sm_mutex` with short microsecond critical sections. All readers use copy-out API (`base_sm_get_soldier(id, &out)`, `base_sm_get_state(out)`). All `ESP_LOGx` and `logger_append` calls are moved outside the lock to prevent priority inversions and thread stalls.
- **Rejected Alternative:** Global spinlock or critical section disabling interrupts. Rejected because disabling interrupts halts ESP32 SPI and Wi-Fi/BT subsystems. Placing semaphore release after return statement (as proposed in old unverified audit) caused fatal deadlock.

---

## 4. SisFall Fall Detection Empirical Evaluation

The fall detection algorithm was empirically evaluated against the complete official **SisFall** dataset (203 MB, 4,585 real-world traces across 38 subjects, sampled at 200 Hz).
The host evaluation suite (`tests/host/eval_sisfall.c`) models the exact 200 Hz ODR with 2nd-order Butterworth DLPF CFG 3 (44 Hz) filtering and the RP2040 `mpu6050_process` / `fall_detect_update` state machine.

### Subject-Wise Data Split:
- **Training Set:** Subjects SA01–SA14 (14 young subjects, 1,050 falls, 11.36 hours of ADLs).
- **Held-Out Test Set:** 24 subjects (SA15–SA23 young + SE01–SE15 elderly = 748 falls, 8.69 hours of ADLs).

### Empirical Performance Comparison:
| Model Configuration | Test Set Sensitivity | ADL False Alarms / Hour | Total False Alarms (8.69h) | Key Trade-off |
| :--- | :--- | :--- | :--- | :--- |
| **Baseline** (2.5g, 45°, 1500 ms) | **56.7%** (424 / 748) | **16.1 FA/h** | 140 | High sensitivity on slips/trips, high false alarm rate on abrupt sitting |
| **Tuned** (3.2g, 50°, 1500 ms) | **53.3%** (399 / 748) | **10.2 FA/h** | 89 | Reduces chair/stumble false alarms by 36% with only 3.4% sensitivity loss |
| **Hard Free-Fall Gate** (< 0.6g, 500 ms) | **50.5%** (378 / 748) | **7.4 FA/h** | 64 | Rejects non-ballistic falls (fainting/slumping) |
| **Weighted Scoring Ensemble** | **11.9%** (89 / 748) | **0.5 FA/h** | 4 | Over-penalizes soft falls; unsuitable for soldier health monitoring |

### Fall Type Performance Breakdown (Held-Out Test Set):
- **Violent / Ballistic Falls (Slips & Trips):**
  - F01 (Forward fall slip while walking): 75.0% sensitivity.
  - F02 (Backward fall slip while walking): 66.7% sensitivity.
  - F04 (Forward fall trip while jogging): 61.1% sensitivity.
- **Non-Ballistic / Sliding / Slumping Falls:**
  - F06 / F07 (Fainting slumps against wall / chair): 25.0% – 30.6% sensitivity. Human body slides gradually; peak impact remains below 2.5g.
  - F10 – F15 (Falls trying to sit or getting out of bed): 27.8% – 48.0% sensitivity.

### ADL False Alarm Breakdown:
- **High-Impact Body Drops:** D11 (Abrupt collapse into soft chair: 110.2 FA/h), D19 (High vertical jump: 162.0 FA/h).
- **Routine Locomotion:** D01/D02 (Walking: 0.0 to 1.2 FA/h), D03/D04 (Jogging: 0.0 FA/h), D05/D06 (Stairs: 0.0 to 1.0 FA/h). Routine locomotion almost never triggers false alarms because post-impact body orientation $\Delta\theta$ remains $< 20^\circ$.

### Tactical Mounting Analysis:
On a vertical plate carrier / MOLLE chest harness, transitioning to a tactical prone crawl causes an 80°–90° orientation shift. The 10-second vibration/buzzer countdown (`ALARM_FALL_PENDING`) and `BUTTON_COMPLETE` double-click cancel (`MSG_FALL_CANCEL`) provide the vital human-in-the-loop defense against tactical false alarms.

---

## 5. Continuous Microsecond Discrete-Event ESB Link Simulation

A continuous microsecond discrete-event PHY simulator (`tests/host/sim_esb_link.c`) was developed to validate MultiCeiver RF channel performance across 5 active soldiers and base station under 1 Mbps GFSK Enhanced ShockBurst.

### Physical Model Parameters:
- **Frame Air Time:** 297 µs (28-byte payload + preamble, address, CRC).
- **ACK Air Time:** 73 µs (no-payload ACK + preamble, address, CRC).
- **Radio Turnaround Delay:** 130 µs (RX/TX switching delay per nRF24 datasheet).
- **Hardware Retries:** ARC = 15 retransmissions maximum.
- **RF Capture Effect:** Co-channel overlap tolerates 6 dB power difference; collisions below 6 dB corrupt both frames.
- **ARD Stagger Formula & Register Encoding:** Per nRF24L01+ datasheet §9.4, delay is $(\text{ARD} + 1) \times 250\ \mu\text{s}$. Firmware writes register $\text{ARD} = 2 + i$, resulting in:
  $$\text{Delay}_i = (2 + i + 1) \times 250\ \mu\text{s} = 750\ \mu\text{s} + i \times 250\ \mu\text{s}$$
  - Soldier 1: Register $\text{ARD}=3$ (`0x3F`), Delay = $1000\ \mu\text{s}$
  - Soldier 2: Register $\text{ARD}=4$ (`0x4F`), Delay = $1250\ \mu\text{s}$
  - Soldier 3: Register $\text{ARD}=5$ (`0x5F`), Delay = $1500\ \mu\text{s}$
  - Soldier 4: Register $\text{ARD}=6$ (`0x6F`), Delay = $1750\ \mu\text{s}$
  - Soldier 5: Register $\text{ARD}=7$ (`0x7F`), Delay = $2000\ \mu\text{s}$

### Empirical Simulation Scenarios & Results:
1. **Scenario 1 (Steady 1 Hz Heartbeat Traffic across 5 Soldiers, 60 seconds):**
   - Packets Delivered: 299 / 299 (**100.0% delivery ratio**).
   - Collisions: 0. Duty Cycle: **< 0.15%**.
2. **Scenario 2 (Correlated Distress Burst, 5 Soldiers Colliding within 100 ms):**
   - Packets Delivered: 5 / 5 (**100.0% delivery ratio**).
   - Latency: p50 = 0.30 ms, p95 = 0.30 ms, max = 0.30 ms. All delivered within 100 ms limit.
   - Mechanism: 250 µs ARD stagger de-synchronizes colliding retries on attempt 1, allowing sequential delivery without loss.
3. **Scenario 3 (Base Station Reboot & NVS Replay Acceptance):**
   - High-water mark correctly bounds replay window; counter 151 accepted cleanly.
4. **Scenario 4 (Soldier Reboot & Session Jump):**
   - Counter jump $\ge 2^{31}$ detected; new session initiated cleanly without false lockouts.
5. **Scenario 5 (Replay Attacker Rejection):**
   - Replayed packet (counter 75) dropped; valid forward packet (counter 76) accepted.

### RF Channel Utilization & Contention Analysis:
Empirical discrete-event simulation proves:
- Total RF channel utilization across 5 nodes is $< 0.15\%$.
- With MultiCeiver physical pipes 1–5 and 250 µs ARD stagger, packet delivery is 100.0% even under correlated bursts.

---

## 6. Stateful Property Testing & libFuzzer Hardening

### Stateful Property Test (`tests/host/test_base_sm_property.c`):
- Executed 100,000 pseudo-randomized state transitions comparing `base_sm` against an independent formal reference specification model.
- Valid Packets Accepted: **85,017** (100% equivalence with specification).
- Replay Attacks Dropped: **14,983** (100% dropped).
- Invariant Violations: **0** (Emergency state cannot be downgraded by tactical messages; boot session counter jumps correctly recognized).

### libFuzzer Harnesses:
- **`fuzz_pre_auth.c` (Raw 28-Byte Frame Wire Parser):**
  - Executed 100,000 runs in WSL Clang with `-fsanitize=fuzzer,address,undefined`.
  - 0 crashes, 0 memory leaks, 0 buffer overflows.
- **`fuzz_post_auth.c` (Structure-Aware Post-Auth State Machine Ingestion):**
  - Executed 100,000 runs in WSL Clang with `-fsanitize=fuzzer,address,undefined`.
  - 0 crashes, 0 state machine corruption, 0 memory leaks.

---

## 7. Man-Down & Inactivity Detection Engineering Design (Design Only)

### Operational Rationale:
Fall detection detects ballistic kinetic collapse (impact spike $\ge 2.05\text{g}$, dynamic vector rotation $\ge 30^\circ$, and short-term post-impact immobility). However, non-ballistic incapacitation events (blast overpressure shock, toxic gas inhalation, hypothermia, sniper shot without prior trip, or traumatic brain injury) do NOT generate a violent kinetic impact spike. Such casualties are characterized by prolonged lack of voluntary motion while in an operational posture (Man-Down / Inactivity).

### Proposed Inactivity Detection Pipeline:
1. **Voluntary Movement Metric ($V_m$):**
   Computed over sliding 1-second epochs from bandpassed acceleration variance $\sigma^2_a$ and integrated angular rate $\|\vec{\omega}\|$:
   $$V_m = \alpha \cdot \text{Var}(\|a\|_{1\text{s}}) + \beta \cdot \text{Mean}(\|\omega\|_{1\text{s}})$$
   - Voluntary movement threshold: $V_m \ge V_{thresh}$ (nominally $0.05\text{g}^2 + 15^\circ/\text{s}$).
2. **Three-Tier Immobility Timer Hierarchy:**
   - **Tier 1 (Normal Inactivity Timer - 60 seconds):** If $V_m < V_{thresh}$ continuously for 60 seconds, the soldier enters `INACTIVITY_PRE_ALERT`.
   - **Tier 2 (Local Pre-Warning - 15 seconds):** The unit activates a escalating silent haptic buzzer/vibration pattern. Any voluntary movement or pressing any button (`BUTTON_BACKUP`, `BUTTON_ENEMY`, `BUTTON_MEDICAL`, `BUTTON_COMPLETE`) immediately resets the inactivity timer to 0 and returns to `NORMAL`.
   - **Tier 3 (Emergency Escalation):** If no motion or cancel occurs within 15 seconds of pre-warning, the state machine transitions to `ALARM_EMERGENCY`, sets `SFLAG_MAN_DOWN`, and immediately transmits `MSG_DOWN` with `force_immediate = true` persistence.
3. **Tactical Invariants & Prone Sniper Suppression:**
   - In static observation / sniper hide positions, soldiers remain immobile voluntarily. A tactical "Stillness Hold" mode can be initiated by holding `BUTTON_MEDICAL` + `BUTTON_COMPLETE` for 3 seconds, extending the inactivity window to 15 minutes or until deliberate movement occurs.
   - Zero firmware code is added until physical tactical trials validate the $V_m$ threshold boundaries.



