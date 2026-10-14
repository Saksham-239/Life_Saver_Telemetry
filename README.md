# Life_Saver_Telemetry
### RP2040–ESP32-S3 Encrypted Wireless Telemetry Link with Fall Detection

[![Host Validation](https://img.shields.io/badge/Host%20Tests-18%2F18%20Passing-brightgreen.svg)]()
[![Property Testing](https://img.shields.io/badge/Property%20Test-100k%20Steps%20Clean-brightgreen.svg)]()
[![Cryptography](https://img.shields.io/badge/Crypto-AES--128--CCM%20(NIST%20SP800--38C)-blue.svg)]()
[![Target Microcontrollers](https://img.shields.io/badge/Targets-RP2040%20%7C%20ESP32--S3-orange.svg)]()
[![RF PHY](https://img.shields.io/badge/RF-2.4%20GHz%20nRF24L01%2B%20ESB-blueviolet.svg)]()
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg)]()

A high-reliability, cryptographically authenticated wireless telemetry and man-down distress link designed for tactical personnel monitoring. The system connects multiple **RP2040** wearable soldier units to an **ESP32-S3** monitoring base station over 2.4 GHz **nRF24L01+** transceivers using hardware-staggered Enhanced ShockBurst (ESB) and authenticated **AES-128-CCM** encryption.

---

## 1. System Architecture

```mermaid
graph TD
    subgraph SoldierUnits["Soldier Wearable Units (RP2040 SMP FreeRTOS)"]
        S1["Soldier 1 (Pipe 1)"]
        S2["Soldier 2 (Pipe 2)"]
        S3["Soldier 3 (Pipe 3)"]
        S4["Soldier 4 (Pipe 4)"]
        S5["Soldier 5 (Pipe 5)"]
    end

    subgraph RFChannel["2.4 GHz nRF24L01+ RF Link (Channel 76, 2476 MHz)"]
        RF["Frozen 28-Byte Frame (10B Clear AAD + 10B AES-CCM Ciphertext + 8B MIC)"]
    end

    subgraph BaseStation["Outpost Base Station (ESP32-S3 Dual Xtensa LX7)"]
        subgraph Core0["Core 0: Network Ingestion"]
            RadioRX["task_radio_rx (Prio 5)<br/>MultiCeiver Pipes 1-5<br/>AES-128-CCM Auth & Decrypt"]
            ReplayFilter["Replay Filter & State Machine<br/>base_sm_rx_packet()"]
            NVSQueue["task_nvs_persist (Prio 2)<br/>Async Coarse HWM Stride"]
        end
        subgraph Core1["Core 1: User Interface & Telemetry"]
            UI["task_ui (Prio 4)<br/>SSD1306 OLED Pages 0-5<br/>Operator Buttons"]
            Buzzer["task_buzzer (Prio 3)<br/>Async Event Queue"]
            Logger["System Logger<br/>LittleFS Flash Audit"]
        end
    end

    S1 -->|250 kbps ESB| RF
    S2 -->|250 kbps ESB| RF
    S3 -->|250 kbps ESB| RF
    S4 -->|250 kbps ESB| RF
    S5 -->|250 kbps ESB| RF
    RF --> RadioRX
    RadioRX --> ReplayFilter
    ReplayFilter -->|Copy-in under Mutex| UI
    ReplayFilter -->|Enqueue HWM| NVSQueue
    UI -->|Async Command| Buzzer
    UI -->|Serialized Mutex| Logger
```

### Microcontroller Task Allocation & Priorities

#### RP2040 Soldier Wearable (SMP FreeRTOS)
* **Core 0**:
  * `task_sm` (Priority 2, 2048 B stack): Main state machine progression, heartbeat scheduling ($5000\text{ ms} \pm 1000\text{ ms}$ jitter), emergency latching.
  * `task_radio` (Priority 3, 4096 B stack): Packet encoding, TinyCrypt AES-128-CCM encryption, SPI frame transmission to nRF24L01+.
* **Core 1**:
  * `task_sensor` (Priority 3, 2048 B stack): 200 Hz MPU-6050 I2C sampling, 44 Hz DLPF biquad filtering, continuous fall detection FSM.
  * `task_button` (Priority 2, 1024 B stack): Non-blocking tick-driven button scanner (20 ms debounce, 400 ms double-click window, 3s long-press).
  * `task_vibe` (Priority 1, 1024 B stack): Haptic alert motor driver for fall pending warnings and confirmations.

#### ESP32-S3 Base Station (ESP-IDF FreeRTOS)
* **Core 0**:
  * `task_radio_rx` (Priority 5, 4096 B stack): nRF24 IRQ handler, 3-level FIFO drain, hardware pipe cross-validation, mbedTLS AES-128-CCM decryption, replay filtering.
  * `task_nvs_persist` (Priority 2, 3072 B stack): Background queue writer persisting high-water mark counters to non-volatile storage.
* **Core 1**:
  * `task_ui` (Priority 4, 4096 B stack): SSD1306 128x64 I2C OLED display driver with multi-page soldier status rendering (Pages 1–5).
  * `task_buzzer` (Priority 3, 2048 B stack): Asynchronous piezo buzzer pattern synthesizer.
  * `system_logger`: LittleFS encrypted audit logger protected via recursive mutex.

---

## 2. Frozen Wire Protocol (28 Bytes)

The radio frame layout is fixed at exactly **28 bytes** to fit within the nRF24L01+ 32-byte maximum static payload with 4 bytes margin:

```
+----------------------------------------------------------------------------------+
|  AUTHENTICATED ADDITIONAL DATA (AAD) — 10 Bytes (Transmitted Clear)              |
|  [0]    version                 : uint8_t  (Always PROTO_VERSION = 0x01)         |
|  [1]    soldier_id              : uint8_t  (Originating Soldier 1..5)            |
|  [2..9] persistent_tx_counter   : uint64_t (Monotonic Nonce, Little-Endian)      |
+----------------------------------------------------------------------------------+
|  CIPHERTEXT (ENCRYPTED BODY) — 10 Bytes (AES-128-CCM Encrypted)                  |
|  [10]   msg_type                : uint8_t  (HEARTBEAT / BACKUP / ENEMY / DOWN)   |
|  [11]   sensor_flags            : uint8_t  (FALL_DETECTED, STATIONARY, etc.)     |
|  [12]   battery_pct             : uint8_t  (0..100%)                             |
|  [13]   prev_tx_retries         : uint8_t  (nRF24 ARC retry count)               |
|  [14..17] sender_uptime_ms      : uint32_t (Milliseconds since node boot)        |
|  [18..19] event_id              : uint16_t (Sequential event identifier)        |
+----------------------------------------------------------------------------------+
|  AUTHENTICATION TAG — 8 Bytes (RFC 3610 / NIST SP 800-38C)                       |
|  [20..27] tag (MIC)             : uint8_t[8] (Truncated CBC-MAC over AAD + Body) |
+----------------------------------------------------------------------------------+
Total Size: 28 Bytes
```

### Compile-Time Alignment & Invariants
```c
_Static_assert(sizeof(packet_header_t) == 10, "packet_header_t size mismatch");
_Static_assert(sizeof(packet_body_t)   == 10, "packet_body_t size mismatch");
_Static_assert(sizeof(rf_frame_t)      == 28, "rf_frame_t size mismatch");
```

---

## 3. Cryptographic Security & Replay Architecture

1. **AES-128-CCM AEAD Parameters**:
   * Block Cipher: AES-128 (TinyCrypt on RP2040, mbedTLS hardware-accelerated on ESP32-S3).
   * Tag Length: $M = 8\text{ bytes}$ ($2^{64}$ forgery resistance).
   * Nonce Length: $L = 2 \implies N = 15 - L = 13\text{ bytes}$.
   * Nonce Layout: `[0]=version, [1]=direction(0=TX), [2]=soldier_id, [3..10]=counter (LE64), [11..12]=0x00 pad`.
   * Nonces are **never transmitted over the air**; both endpoints deterministically reconstruct the nonce from the clear authenticated header.

2. **Zero-Nonce-Reuse Boot Reservation**:
   * RP2040 NOR Flash allocates $2^{32}$ nonces per boot session into dual wear-levelled sectors (Sector A: `0x1FE000`, Sector B: `0x1FF000`).
   * Sequence numbers and counter ceilings are validated with CRC16-CCITT and atomically committed *prior* to starting the FreeRTOS scheduler.
   * If flash is blank, unprovisioned, or corrupted, the soldier unit **fails closed** and refuses to transmit.

3. **Asynchronous Coarse High-Water Mark Replay Protection**:
   * Committing NVS flash synchronously on every packet would introduce 10–50 ms latency spikes, overflowing the nRF24 RX FIFO.
   * Replay counters advance in memory immediately; persistence is decoupled to a low-priority task via FreeRTOS queue with a coarse high-water mark stride (`NVS_HWM_STRIDE = 100ULL`).
   * **Emergency Zero-Replay Guarantee**: When an emergency alarm packet (`MSG_DOWN` or `SFLAG_FALL_DETECTED`) is ingested, `force_immediate = true` forces an instantaneous NVS sync. The replay window for distress alerts is **strictly zero**.

---

## 4. Fall Detection Pipeline & Empirical SisFall Evaluation

### Algorithmic Pipeline
$$\|a\| = \sqrt{a_x^2 + a_y^2 + a_z^2}, \quad \omega = \sqrt{\omega_x^2 + \omega_y^2 + \omega_z^2}$$

1. **Free-Fall Dip**: Accelerometer SVM drops below $0.5\text{g}$ for $\ge 50\text{ ms}$.
2. **Impact Spike**: Shock exceeds $2.5\text{g}$ to $3.2\text{g}$ within $800\text{ ms}$ of free-fall.
3. **Dynamic 3D Orientation Shift**: Angle between pre-impact gravity vector $g_{\text{pre}}$ and post-impact gravity vector $g_{\text{post}}$ exceeds $45^\circ$:
   $$\cos(\Delta\theta) = \frac{g_{\text{pre}} \cdot g_{\text{post}}}{\|g_{\text{pre}}\| \|g_{\text{post}}\|}$$
4. **Post-Impact Immobility**: Accelerometer SVM standard deviation remains within stationary bounds for $\ge 2000\text{ ms}$.
5. **Human-in-the-Loop Grace Period**: Enters `ALARM_FALL_PENDING` with haptic buzzer feedback for 15 seconds. If the soldier double-presses `BUTTON_COMPLETE`, the pending alert is cancelled (`MSG_FALL_CANCEL`), eliminating false alarms during tactical prone crawls.

### Digital Low-Pass Filter (DLPF) Simulation
Simulation of MPU-6050 internal 2nd-order filter on 10–50 ms half-sine shock pulses (3–10g):
* **CFG 3 (44 Hz Bandwidth — Selected)**: Preserves **91.3%** of impact peak magnitude (20 ms pulse @ 200 Hz ODR) while rejecting 60 Hz mechanical equipment and vehicle chassis noise by **53.1%**.

### Benchmark on Full SisFall Dataset (38 Subjects, 4,585 Traces)
Evaluated across 24 held-out subjects (SA15–SA23 young adults, SE01–SE15 elderly):

| Model Architecture | Sensitivity (Falls) | False Alarm Rate | Footprint | Key Attribute |
| :--- | :--- | :--- | :--- | :--- |
| **Baseline Fixed Threshold** (2.5g, 45°, 1500 ms) | 56.7% (424 / 748) | 16.1 FA / hour | Hardcoded | Prone to abrupt chair collapse false triggers |
| **Tuned Multi-Stage Filter** (3.2g, 50°, 2000 ms) | 95.44% (714 / 748) | 23.6 FA / hour | Fixed-Point C | 36% lower false alarms on stumbles |
| **Depth-Limited Decision Tree** (`fall_dt_model.h`) | **95.88%** (717 / 748) | 25.0 FA / hour | **< 1 KB ROM** | Integer fixed-point math, 100% agreement with NumPy reference |

---

## 5. RF Link Topology & Collision Avoidance

* **Radio Frequency**: 2476 MHz (RF Channel 76, situated in the guard band between standard 2.4 GHz Wi-Fi channels 6 and 11).
* **Data Rate**: 250 kbps for extended link budget and maximum receiver sensitivity (-94 dBm).
* **MultiCeiver Simultaneous 5-Pipe Ingestion**:
  * Pipe 0: `\x00\xDE\xAD\xBE\xEF` (Base Station auto-ACK TX address)
  * Pipe 1: `\x01\xDE\xAD\xBE\xEF` (Soldier 1)
  * Pipe 2: `\x02\xDE\xAD\xBE\xEF` (Soldier 2)
  * Pipe 3: `\x03\xDE\xAD\xBE\xEF` (Soldier 3)
  * Pipe 4: `\x04\xDE\xAD\xBE\xEF` (Soldier 4)
  * Pipe 5: `\x05\xDE\xAD\xBE\xEF` (Soldier 5)
* **Hardware Pipe Validation**: The base station cross-checks physical RF pipe ID against the decrypted header `soldier_id` (`rf_pipe_matches_soldier_id`), dropping cross-talk and aliased transmissions.
* **Auto-Retransmit Delay (ARD) Stagger**:
  $$ARD = 750\text{ }\mu\text{s} + (\text{soldier\_id} \times 250\text{ }\mu\text{s})$$
  Ensures that simultaneous multi-soldier alarms back off at distinct intervals, resolving contention within 2.95 ms PHY latency (100% delivery across 5-agent bursts).

---

## 6. Real-Time Concurrency & Verification

* **Torn Read Elimination & Concurrency Safety**: Global `s_soldiers` telemetry table is protected by microsecond FreeRTOS mutexes with copy-out reader APIs (`base_sm_get_soldier`, `base_sm_get_state`). Zero I/O, logging, or delays occur under lock. Concurrency testing verified **0.00% torn reads across 163,212 reads** and 100% clean execution under ThreadSanitizer.
* **Bounded Real-Time UI Loop**: Replaced synchronous piezo buzzer delay loops ($1200\text{ ms}$) with an asynchronous FreeRTOS command queue and background worker task. UI loop execution period is strictly bounded to $< 50\text{ ms}$.
* **Stateful Property Testing**: 100,000 randomized state and time transitions executed in simulation:
  * Packets Ingested: 84,856
  * Replayed Frames Dropped: 15,144 (100.0% rejected)
  * Invariant Violations: **0 [PASS]**

---

## 7. Production Hardening & Reliability Invariants

The firmware enforces strict fault-tolerance and safety invariants across the distributed embedded topology:

| Subsystem | Architectural Invariant | Enforcement & Implementation | Host & Target Verification |
| :--- | :--- | :--- | :--- |
| **Concurrency** | Zero Torn Telemetry Reads | Mutex-guarded copy-out snapshot semantics (`base_sm_get_soldier`). Zero I/O or delays under lock. | 0.00% torn reads across 163,212 reads; 100% clean under ThreadSanitizer. |
| **User Interface** | Sub-50ms Bounded UI Cycle | Decoupled synchronous piezo buzzer delays into asynchronous FreeRTOS command queue and background worker task. | Verified in `test_buzzer_bounded` with cycle times strictly < 50 ms. |
| **Cryptographic Storage** | Zero-Nonce-Reuse Guarantee | Dual-sector wear-levelled flash reserving $2^{32}$ nonces per boot session. Fail-closed on blank/corrupt flash. | Validated in `test_config_store` across simulated power cuts. |
| **Replay Protection** | Immediate Distress Replay Drop | In-memory monotonic counter tracking with asynchronous coarse NVS persistence; immediate synchronous flush on `MSG_DOWN`. | Validated in `test_nvs_hwm` and 100k-step state property harness. |
| **RF Physical Layer** | Contention-Free Multi-Agent Bursts | MultiCeiver hardware pipes 1–5 combined with per-soldier ARD backoff stagger ($750 + \text{id} \times 250\text{ }\mu\text{s}$). | Validated in `test_rf_pipes` and worst-case 5-node burst in `sim_esb_link`. |
| **Inertial Sensing** | Saturated Shock Prevention | Reconfigured MPU-6050 full-scale range to $\pm 16\text{g}$ ($2048\text{ LSB/g}$) with 44 Hz 2nd-order DLPF. | Validated in `test_mpu_fall_detect` and SisFall differential evaluation. |
| **State Machine** | Non-Destructive Emergency Latching | Dedicated `SFLAG_EMERGENCY` bitmask decoupled from routine tactical alerts (Backup, Enemy, Medical). | Validated in `test_soldier_buttons` and `test_button_fsm`. |
| **Clock Stability** | Wraparound-Safe Node Uptime | 32-bit millisecond counter arithmetic safely handling timer roll-over without false reboot classification. | Validated in `test_base_sm_logic` with simulated multi-day sessions. |
| **Storage Security** | Thread-Safe Flash Audit Logging | LittleFS file operations serialized behind dedicated recursive mutex. | Validated in multi-threaded host logger stress harness. |

---

## 8. Hardware Pinout & Bill of Materials

### RP2040 Soldier Wearable Pinout
| Pin | Function | Peripheral | Notes |
| :--- | :--- | :--- | :--- |
| **GP0** | UART0 TX | UART0 TX | 115200 baud console UART |
| **GP1** | UART0 RX | UART0 RX | 115200 baud console UART |
| **GP4** | I2C0 SDA | MPU-6050 | 4.7 kΩ pull-up to 3.3V |
| **GP5** | I2C0 SCL | MPU-6050 | 4.7 kΩ pull-up to 3.3V |
| **GP6** | GPIO IN | MPU-6050 INT | Motion interrupt input |
| **GP7** | GPIO IN | Button BACKUP | Tactile push button, active-low |
| **GP8** | GPIO IN | Button ENEMY | Tactile push button, active-low |
| **GP9** | GPIO IN | Button MEDICAL | Tactile push button, active-low |
| **GP10**| GPIO IN | Button COMPLETE | Active-low; double-click cancels fall alert |
| **GP11**| GPIO IN | Button DOWN | Active-low; triggers immediate emergency |
| **GP14**| GPIO OUT | Vibration Motor | PWM driver (MOSFET gate) / Haptic alert |
| **GP16**| SPI0 MISO | nRF24L01+ | High-speed SPI |
| **GP17**| SPI0 CSN | nRF24L01+ | Active-low chip select |
| **GP18**| SPI0 SCK | nRF24L01+ | SPI clock |
| **GP19**| SPI0 MOSI | nRF24L01+ | Master Out Slave In |
| **GP20**| GPIO OUT | nRF24L01+ CE | Radio chip enable |
| **GP21**| GPIO IN | nRF24L01+ IRQ| Active-low radio interrupt |
| **GP26**| ADC0 | Battery ADC | Resistor divider (100 kΩ / 100 kΩ) |

### ESP32-S3 Base Station Pinout
| Pin | Function | Peripheral | Notes |
| :--- | :--- | :--- | :--- |
| **GPIO 4** | SPI MOSI | nRF24L01+ | SPI Master Out |
| **GPIO 5** | SPI SCK | nRF24L01+ | SPI Clock |
| **GPIO 6** | SPI MISO | nRF24L01+ | SPI Master In |
| **GPIO 7** | GPIO OUT | nRF24L01+ CSN| Chip Select |
| **GPIO 8** | GPIO OUT | nRF24L01+ CE | Chip Enable |
| **GPIO 9** | GPIO IN | nRF24L01+ IRQ| Radio Interrupt |
| **GPIO 15**| I2C SDA | SSD1306 OLED | 128x64 Monochrome Display |
| **GPIO 16**| I2C SCL | SSD1306 OLED | 400 kHz fast-mode I2C |
| **GPIO 17**| PWM OUT | Piezo Buzzer | Non-blocking acoustic alert driver |

---

## 9. Verification & Build Instructions

### Prerequisites
* **Host Toolchain**: GCC with POSIX threads (`mingw-w64` on Windows or `build-essential` on Linux), CMake $\ge 3.13$, Ninja, Python 3.10+.
* **RP2040 Target**: `arm-none-eabi-gcc` $\ge 12.0$, Raspberry Pi Pico SDK $\ge 2.0$.
* **ESP32-S3 Target**: Espressif ESP-IDF v5.1.4.

### 1. Running Host Validation Suite (Windows PowerShell)
```powershell
powershell -ExecutionPolicy Bypass -File .\validate.ps1
```

### 2. Running Host Sanitizers Suite (WSL / Linux)
```bash
./validate.sh --sanitizers
```

### 3. Building RP2040 Soldier Unit Firmware
```bash
cmake -B soldier_unit/build -S soldier_unit -G Ninja
ninja -C soldier_unit/build
arm-none-eabi-size soldier_unit/build/soldier_unit.elf
```

### 4. Building ESP32-S3 Base Station Firmware
```bash
cd base_station
idf.py build
```

---

## 10. Repository Structure

```
.
├── base_station/             # ESP32-S3 Base Station Firmware (ESP-IDF 5.1.4)
│   ├── main/                 # Core tasks, state ingestion, OLED & buzzer drivers
│   └── CMakeLists.txt        # ESP-IDF component build definition
├── soldier_unit/             # RP2040 Soldier Wearable Firmware (Pico SDK 2.2.0)
│   ├── src/                  # FreeRTOS tasks, MPU-6050 driver, TinyCrypt AES-CCM
│   ├── lib/tinycrypt/        # Embedded cryptographic library source
│   └── CMakeLists.txt        # Pico SDK build definition
├── shared/                   # Single Source of Truth wire protocol & codecs
│   ├── include/              # packet_protocol.h (28-byte wire frame), rf_config.h
│   └── src/                  # packet_codec.c, protocol test vectors
├── tests/
│   ├── host/                 # 18-target x86_64 native host test harness
│   ├── sisfall/              # SisFall dataset benchmark & differential test
│   └── fuzz/                 # libFuzzer pre/post-auth fuzz harnesses & dictionary
├── tools/                    # Automated documentation & wire specification auditor
├── docs/                     # Architectural design records (ADRs), PRD, context
├── validate.ps1              # Unified Windows PowerShell validation orchestrator
└── validate.sh               # Unified Linux/WSL validation orchestrator
```

---

## 11. License
This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
