# System Architecture & Hardware Topology

<!-- last updated: Gate G3 Completed -->

## 1. System Module Map & Cores / Tasks Topology

### RP2040 Soldier Unit (SMP FreeRTOS)
| Core | Task Name | Priority | Stack Size | Responsibilities |
| :--- | :--- | :--- | :--- | :--- |
| **Core 0** | `task_sm` | 2 (Low-Med) | 2048 B | State machine progression, fall candidate processing, heartbeat scheduling |
| **Core 0** | `task_radio` | 3 (Med) | 4096 B | Frame encoding, TinyCrypt AES-CCM encryption, nRF24 SPI transmission |
| **Core 1** | `task_sensor`| 3 (Med) | 2048 B | MPU-6050 I2C sampling (100 Hz), vector magnitude, fall sequence detection |
| **Core 1** | `task_button`| 2 (Low-Med) | 1024 B | GPIO scanning (20 ms tick), debouncing, click/hold event dispatch |
| **Core 1** | `task_vibe` | 1 (Low) | 1024 B | Haptic vibration motor PWM pattern driver |

### ESP32-S3 Base Station (ESP-IDF FreeRTOS)
| Core | Task Name | Priority | Stack Size | Responsibilities |
| :--- | :--- | :--- | :--- | :--- |
| **Core 0** | `task_radio_rx` | 5 (High) | 4096 B | nRF24 IRQ polling, hardware FIFO pull, AES-CCM decryption, replay filtering, state ingestion |
| **Core 0** | `task_nvs_persist`| 2 (Low) | 3072 B | Asynchronous NVS high-water mark replay persistence via queue |
| **Core 1** | `task_ui` | 4 (Med-High) | 4096 B | SSD1306 I2C OLED display rendering (Pages 0–5), operator button handling |
| **Core 1** | `task_buzzer` | 3 (Med) | 2048 B | Non-blocking PWM piezo buzzer pattern generator via event queue |

---

## 2. Frozen Wire Format (28 Bytes)
The over-the-air frame format is fixed at exactly 28 bytes:
```
+-------------------------------------------------------+
|  Header (10 Bytes)                                    |
|  - uint8_t  magic (0xA5)                              |
|  - uint8_t  soldier_id (1..5)                         |
|  - uint64_t persistent_tx_counter (Little-Endian)     |
+-------------------------------------------------------+
|  Body (10 Bytes - Encrypted via AES-128-CCM)          |
|  - uint8_t  msg_type (0=HB, 1=BACKUP, 2=ENEMY, etc.)  |
|  - uint8_t  alarm_state (0=NORM, 1=ALERT, 2=PENDING)  |
|  - uint8_t  sensor_flags (bit0=FALL, bit3=EMERGENCY)  |
|  - uint8_t  battery_pct (0..100)                      |
|  - uint16_t raw_batt_mv (mV)                          |
|  - uint32_t sender_uptime_ms (ms)                     |
+-------------------------------------------------------+
|  Authentication Tag (8 Bytes)                         |
|  - uint8_t  mic[8] (AES-CCM MAC over Header + Body)   |
+-------------------------------------------------------+
Total Length = 28 Bytes
```

---

## 3. Flash Memory Map (RP2040 Dual-Sector Wear-Levelling)
The RP2040 uses a 2 MB external NOR flash (`0x10000000` XIP Base). The top two 4096-byte sectors are reserved for configuration and counter persistence:
- **Sector A:** `0x1FE000` (Offset from flash start: $2097152 - 8192$)
- **Sector B:** `0x1FF000` (Offset from flash start: $2097152 - 4096$)

### Sector Layout:
```
Offset 0x00: uint8_t  magic (0xC5)
Offset 0x01: uint8_t  soldier_id (1..5)
Offset 0x02: uint8_t  aes_key[16] (16 bytes)
Offset 0x12: uint64_t counter_ceiling (8 bytes)
Offset 0x1A: uint32_t sequence_number (4 bytes)
Offset 0x1E: uint16_t crc16_ccitt (2 bytes)
Offset 0x20..0x1000: 0xFF (Unused / Padded to 4096 bytes)
```

---

## 4. RF Configuration (nRF24L01+)
- **Data Rate:** 250 kbps (Long range mode)
- **RF Channel:** 2476 MHz (Channel 76, outside nominal Wi-Fi 1/6/11 lobes)
- **Output Power:** 0 dBm (PA_MAX)
- **Enhanced ShockBurst (ESB):** Dynamic Payload Length (DPL) disabled, fixed payload size 28 bytes. Auto-ACK enabled.
- **Physical Pipe Addresses:**
  - Pipe 0: `\x00\xDE\xAD\xBE\xEF` / `{0x00, 0xDE, 0xAD, 0xBE, 0xEF}` (Base Station TX address for auto-ACKs)
  - Pipe 1: `\x01\xDE\xAD\xBE\xEF` / `{0x01, 0xDE, 0xAD, 0xBE, 0xEF}` (Soldier 1)
  - Pipe 2: `\x02\xDE\xAD\xBE\xEF` / `{0x02, 0xDE, 0xAD, 0xBE, 0xEF}` (Soldier 2, LSByte=0x02, shares top 4 bytes with Pipe 1)
  - Pipe 3: `\x03\xDE\xAD\xBE\xEF` / `{0x03, 0xDE, 0xAD, 0xBE, 0xEF}` (Soldier 3, LSByte=0x03, shares top 4 bytes with Pipe 1)
  - Pipe 4: `\x04\xDE\xAD\xBE\xEF` / `{0x04, 0xDE, 0xAD, 0xBE, 0xEF}` (Soldier 4, LSByte=0x04, shares top 4 bytes with Pipe 1)
  - Pipe 5: `\x05\xDE\xAD\xBE\xEF` / `{0x05, 0xDE, 0xAD, 0xBE, 0xEF}` (Soldier 5, LSByte=0x05, shares top 4 bytes with Pipe 1)
- **Auto-Retransmit Delay (ARD):** Staggered per soldier: $ARD = 750\text{ }\mu\text{s} + (ID \times 250\text{ }\mu\text{s})$.

---

## 5. Locking & Concurrency Rules
1. **Base Station `s_soldiers` State Array:**
   - Protected by `portMUX_TYPE` (or dedicated FreeRTOS mutex).
   - Critical sections MUST be microsecond-duration: copy-in on rx, copy-out on UI read.
   - Strictly NO logging (`ESP_LOGx`), NO NVS writes, and NO buzzer delays inside the mutex!
   - Every exit path (including early returns) MUST release the mutex.
2. **Base Station System Logger (`logger_append`):**
   - Protected by dedicated recursive mutex to serialize filesystem access across tasks.
