# Product Requirements Document (PRD)

<!-- last updated: Gate G2 Review -> Gate G3 Baseline -->

## 1. Executive Summary & Goals
The objective of this project is to build an ultra-reliable, cryptographically secure wireless telemetry and emergency alert link connecting field personnel (Soldier Units) to a monitoring outpost (Base Station).

### Primary Goals
- **Real-Time Telemetry:** Deliver periodic health, battery, uptime, and tactical status reports from up to 5 soldier nodes to a base station over 2.4 GHz RF.
- **Automated Fall & Immobility Detection:** Detect sudden physical shock impacts followed immediately by immobility using an onboard 6-axis IMU (MPU-6050).
- **Manual Tactical & Emergency Signalling:** Provide positive, distinct button interfaces for immediate assistance requests (`BACKUP`, `ENEMY_CONTACT`, `MEDICAL`, `MANUAL_EMERGENCY`).
- **Cryptographic Security:** Prevent spoofing, packet tampering, and replay attacks using authenticated AES-128-CCM encryption with monotonic 64-bit nonces.
- **High Concurrency & Determinism:** Base station must concurrently ingest packets, service display interfaces, write non-volatile replay logs, and trigger audio buzzers without race conditions, dropped packets, or watchdog panics.

### Non-Goals
- **Two-Way Voice Communication:** The radio channel is strictly simplex telemetry with Enhanced ShockBurst (ESB) auto-acknowledgements.
- **Mesh Multi-Hop Routing:** Units communicate in a single-hop star topology directly with the Base Station.
- **Dynamic Field Pairing:** Cryptographic keys and Soldier IDs are pre-provisioned via a secure UART/CDC provisioning interface prior to deployment.

---

## 2. Key Operational Scenarios & Users

### Scenario 1: Nominal Routine Patrol
- Soldier unit sends periodic heartbeats every $5000\text{ ms} \pm 1000\text{ ms}$ jitter.
- Base station receives packets, tracks crystal clock drift, logs link quality, and updates SSD1306 OLED display.

### Scenario 2: Traumatic Fall Event
- Soldier suffers an explosive blast or rapid fall: free-fall dip -> sudden impact shock ($>2.5\text{g}$) -> motionless state for $\ge 2000\text{ ms}$.
- Unit enters `ALARM_FALL_PENDING` with local vibration warning and a 15-second grace period.
- If soldier does not cancel via double-press of `COMPLETE`, unit transitions to `ALARM_EMERGENCY`, transmitting emergency telemetry frames (`SFLAG_FALL_DETECTED | SFLAG_EMERGENCY`).
- Base station enters visual emergency mode, sounds audible alarm, and latches status.

### Scenario 3: Accidental Fall Stumble (False Positive Cancellation)
- Soldier stumbles into cover, triggering a pending alarm. Soldier double-clicks `COMPLETE` within 15 seconds.
- Alarm returns immediately to `NORMAL`; base station is never alerted with an emergency false alarm.

### Scenario 4: Manual Emergency Under Fire
- Soldier manually presses `BUTTON_DOWN`. Unit immediately enters `ALARM_EMERGENCY` (`SFLAG_EMERGENCY`).
- `SFLAG_FALL_DETECTED` is explicitly NOT set, informing base operators that this was a deliberate distress call, not an unconscious fall.

---

## 3. Engineering Decisions & Specifications (Q1–Q3)

| Decision ID | Date | Subject | Decision Details & Architectural Resolution |
| :--- | :--- | :--- | :--- |
| **Q1** | 2026-10-10 | Alarm State vs Tactical Messaging | **Option 1 Selected:** If a unit is in `ALARM_EMERGENCY`, transmitting a tactical message (`BACKUP`, `ENEMY`, `MEDICAL`) transmits with `msg_type = MSG_<TACTICAL>` but `alarm_state = ALARM_EMERGENCY` and retains `SFLAG_EMERGENCY` on air. The base station MUST NEVER downgrade a soldier from `ALARM_EMERGENCY` to `ALERT_LATCHED` upon receiving tactical packets. |
| **Q2** | 2026-10-10 | MPU-6050 Accelerometer Range & Filtering | Configure MPU-6050 to $\pm 16\text{g}$ full-scale range ($2048\text{ LSB/g}$). Sample rate $100\text{ Hz}$, $DLPF\_CFG = 3$ ($44\text{ Hz}$). Shock thresholds marked UNVALIDATED pending physical calibration data on RP2040 hardware. |
| **Q3** | 2026-10-10 | Flash Nonce Counter Boot Allocation | Allocate $2^{32}$ nonces per boot session into dual-sector wear-levelled flash storage. Fail closed on blank, corrupt, or unprovisioned flash to strictly prevent cryptographic nonce reuse. |

---

## 4. Acceptance Criteria
1. **Zero Nonce Collisions:** Under all power-loss, bit-flip, and reboot conditions, the 64-bit transmit counter strictly increases monotonically and never repeats a nonce.
2. **Zero Struct Tearing:** ThreadSanitizer running $100,000$ packets on concurrent writer and reader threads must yield zero torn reads on `s_soldiers`.
3. **Bounded UI Loop Period:** `task_ui` execution period must remain strictly $\le 50\text{ ms}$ under all alarm and buzzer conditions.
4. **Target Compilation:** RP2040 firmware must compile with `arm-none-eabi-gcc` + `pico-sdk` and ESP32-S3 firmware must compile with `idf.py build` without errors or warnings.
