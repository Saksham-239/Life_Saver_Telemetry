/**
 * packet_protocol.h — RF Soldier Safety System  (protocol v2)
 *
 * SINGLE SOURCE OF TRUTH for the wire protocol shared between:
 *   Soldier unit  (RP2040 / arm-none-eabi-gcc / tinycrypt CCM)
 *   Base station  (ESP32-S3 / xtensa-esp32s3-elf-gcc / mbedTLS CCM)
 *
 * Include path (both projects reference this file via CMake, not by copying):
 *   soldier_unit:  target_include_directories(... ${CMAKE_SOURCE_DIR}/../shared/include)
 *   base_station:  target_include_directories(... ${CMAKE_SOURCE_DIR}/../../shared/include)
 *
 * ── Wire frame layout (28 bytes total) ────────────────────────────────────
 *
 *   [ packet_header_t : 10 bytes ]  Clear — authenticated but NOT encrypted (AAD)
 *   [ ciphertext      : 10 bytes ]  AES-128-CCM encrypted packet_body_t
 *   [ tag             :  8 bytes ]  AES-128-CCM authentication tag
 *
 *   Fits nRF24L01+ maximum static payload (32 bytes) with 4 bytes spare.
 *
 * ── AES-CCM parameters ────────────────────────────────────────────────────
 *
 *   Algorithm:  AES-128-CCM per RFC 3610 / NIST SP 800-38C
 *   Tag length: CCM_TAG_LEN = 8 bytes  (2^64 forgery resistance)
 *   Nonce:      CCM_NONCE_LEN = 13 bytes, L=2 → max message 65535 bytes
 *   Nonce is NOT transmitted — both sides derive it from the clear header.
 *
 *   Library:
 *     ESP32-S3: mbedtls_ccm_encrypt_and_tag() / mbedtls_ccm_auth_decrypt()
 *     RP2040:   tinycrypt tc_ccm_generation_encryption() / tc_ccm_decryption_verification()
 *               (Intel/Zephyr; MIT; ~3 KB ROM; ships its own known-answer tests)
 *
 * ── Persistent TX counter ─────────────────────────────────────────────────
 *
 *   persistent_tx_counter is a uint64_t that NEVER repeats for a given key,
 *   including across power cycles.
 *
 *   Counter reservation strategy (boot-time flash write only):
 *     1. Read last_ceiling from flash.
 *     2. session_base = last_ceiling
 *     3. Write new_ceiling = last_ceiling + COUNTER_BLOCK_SIZE to flash.
 *     4. Increment session_base per packet in RAM during normal operation.
 *     5. If a session exhausts the block, write another (edge case only).
 *   Flash write is [O] (XIP-safe RP2040 requirement); interface is defined here.
 *
 * ── Security scope ─────────────────────────────────────────────────────────
 *
 *   This is symmetric pre-shared key encryption + authentication per soldier.
 *   It is NOT a full PKI / secure-boot solution.  Key provisioning is out-of-band
 *   (serial/USB menu on both nodes, §6.4 / §10.11).
 */

#ifndef PACKET_PROTOCOL_H
#define PACKET_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* ── Protocol version ───────────────────────────────────────────────────── */
#define PROTO_VERSION           0x01u

/* ── Size constants ─────────────────────────────────────────────────────── */
#define PACKET_HEADER_LEN       10u   /**< packet_header_t wire size                   */
#define PACKET_BODY_LEN         10u   /**< packet_body_t wire size (before encryption)  */
#define CCM_TAG_LEN              8u   /**< AES-CCM authentication tag (8 = safe minimum)*/
#define CCM_NONCE_LEN           13u   /**< RFC 3610 nonce; L=2 (max msg 65535 bytes)   */
#define RF_FRAME_LEN            28u   /**< Total wire frame: header+ciphertext+tag      */
#define AES_KEY_LEN             16u   /**< AES-128 key size                            */
#define SOLDIER_ID_MAX           5u   /**< Max simultaneously tracked soldiers          */

/* ── Counter reservation ─────────────────────────────────────────────────── */
/**
 * Number of counter values reserved in flash per boot session.
 * At 1 packet/second worst-case, 1000 covers ~16 minutes.
 * For a 30-60s heartbeat device, 1000 covers > 8 hours of operation.
 */
#define COUNTER_BLOCK_SIZE      (1ULL << 32)

/* ── CCM direction byte (nonce byte [1]) ─────────────────────────────────── */
#define CCM_DIR_SOLDIER_TO_BASE 0x00u
#define CCM_DIR_BASE_TO_SOLDIER 0x01u   /* Reserved — no base→soldier packets in v1 */

/* ── Timing constants ────────────────────────────────────────────────────── */
#define HEARTBEAT_BASE_MS       30000u  /**< Nominal heartbeat interval (ms)           */
#define HEARTBEAT_JITTER_MS      3000u  /**< ± random jitter added to interval         */
#define FALL_CONFIRM_WINDOW_MS  10000u  /**< Countdown before MSG_DOWN is sent         */
#define LINK_TIMEOUT_MS         90000u  /**< No packet in this window → LINK_LOST      */
#define EMERGENCY_CLEAR_HOLD_MS  3000u  /**< COMPLETE hold time to send EMERGENCY_CLEAR*/

/* ── msg_type enum ───────────────────────────────────────────────────────── */
/**
 * Application-level message type, carried in packet_body_t.msg_type.
 *
 * Rules:
 *  - Hardware ShockBurst ACK is transport-layer only; it NEVER drives app state (§18.1).
 *  - MSG_COMPLETE clears ALARM_ALERT_LATCHED only.  It does NOT clear ALARM_EMERGENCY.
 *  - MSG_EMERGENCY_CLEAR clears ALARM_EMERGENCY.  Base validates event_id match.
 *  - Values 9–15 are RESERVED for future operator-ACK and OTA config.
 */
typedef enum {
    MSG_HEARTBEAT        = 0,  /**< Keepalive.  Updates LINK state only; no alarm effect.*/
    MSG_BACKUP           = 1,  /**< Button press: need backup                            */
    MSG_ENEMY            = 2,  /**< Button press: enemy spotted                         */
    MSG_MEDICAL          = 3,  /**< Button press: medical emergency                     */
    MSG_COMPLETE         = 4,  /**< Alert cleared (ALERT_LATCHED only, not EMERGENCY)   */
    MSG_DOWN             = 5,  /**< Confirmed fall → ALARM_EMERGENCY on base            */
    MSG_FALL_PENDING     = 6,  /**< Fall candidate; 10-second confirmation started      */
    MSG_FALL_CANCEL      = 7,  /**< Confirmation window cancelled (double-press)        */
    MSG_EMERGENCY_CLEAR  = 8,  /**< Long-hold COMPLETE; base checks event_id match      */
    /* 9–15 reserved */
    MSG_TYPE_MAX_VALID   = 8u,
} msg_type_t;

/* ── sensor_flags bit masks ─────────────────────────────────────────────── */
#define SFLAG_FALL_DETECTED  (1u << 0)  /**< Fall confirmed and latched              */
#define SFLAG_STATIONARY     (1u << 1)  /**< Still for > STILL_WINDOW seconds        */
#define SFLAG_FALL_PENDING   (1u << 2)  /**< In confirmation countdown window        */
#define SFLAG_LOW_BATTERY    (1u << 3)  /**< Battery below low threshold             */

/* ── Alarm and link state (shared across both nodes) ────────────────────── */

/** Per-soldier alarm state — orthogonal to link state (§18.2 decision) */
typedef enum {
    ALARM_NORMAL         = 0,
    ALARM_FALL_PENDING   = 1,  /**< Fall candidate, awaiting confirm or cancel       */
    ALARM_ALERT_LATCHED  = 2,  /**< Non-critical alert (BACKUP / ENEMY / MEDICAL)   */
    ALARM_EMERGENCY      = 3,  /**< SOLDIER DOWN — cleared only by MSG_EMERGENCY_CLEAR*/
} alarm_state_t;

/** Link health — driven purely by heartbeat timeout */
typedef enum {
    LINK_ONLINE = 0,
    LINK_LOST   = 1,
} link_state_t;

/* ── Wire structures ─────────────────────────────────────────────────────── */

/**
 * packet_header_t — 10 bytes, transmitted in clear.
 *
 * Authenticated by AES-CCM (part of AAD) but NOT encrypted.
 * The base reads soldier_id and persistent_tx_counter from this before decryption,
 * which is necessary to look up the correct key and reconstruct the nonce.
 *
 * Byte layout (explicit, for codec):
 *   [0]    version              (uint8)
 *   [1]    soldier_id           (uint8, 1–SOLDIER_ID_MAX)
 *   [2–9]  persistent_tx_counter (uint64, little-endian)
 */
typedef struct __attribute__((packed)) {
    uint8_t  version;                /**< Always PROTO_VERSION (0x01)                */
    uint8_t  soldier_id;             /**< Originating soldier (1–SOLDIER_ID_MAX)     */
    uint64_t persistent_tx_counter;  /**< Monotonic; reserved in flash at boot       */
} packet_header_t;

/**
 * packet_body_t — 10 bytes, encrypted inside rf_frame_t.
 *
 * Byte layout (explicit, for codec):
 *   [0]    msg_type             (uint8, msg_type_t)
 *   [1]    sensor_flags         (uint8, SFLAG_* bitmask)
 *   [2]    battery_pct          (uint8, 0–100)
 *   [3]    prev_tx_retries      (uint8, 0–15; ARC_CNT from previous TX)
 *   [4–7]  sender_uptime_ms     (uint32, little-endian; boot-relative ms)
 *   [8–9]  event_id             (uint16, little-endian; per-alarm identifier)
 *
 * Notes:
 *   - No start_byte or app-CRC: CCM tag provides integrity; nRF24 handles framing.
 *   - prev_tx_retries is the retry count from the PREVIOUS packet's TX (OBSERVE_TX.ARC_CNT).
 *     It is a lagged link-quality signal (the freshest reading available at TX time).
 *   - event_id increments per new alarm event within a session.
 *     MSG_EMERGENCY_CLEAR must carry the event_id of the active emergency;
 *     base rejects a mismatch (prevents replayed clears from a prior emergency).
 */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;            /**< msg_type_t value (stored as uint8)            */
    uint8_t  sensor_flags;        /**< SFLAG_* bitmask                               */
    uint8_t  battery_pct;         /**< 0–100 %                                       */
    uint8_t  prev_tx_retries;     /**< ARC_CNT from previous TX                      */
    uint32_t sender_uptime_ms;    /**< Boot-relative ms (wraps at ~49.7 days)        */
    uint16_t event_id;            /**< Per-alarm event identifier                    */
} packet_body_t;

/**
 * packet_t — logical packet, NOT a wire type.
 *
 * This is what application code (state machine, buttons, fall detect) works with.
 * Serialisation into a wire frame happens exclusively through aes_ccm_encrypt()
 * in aes_wrap.h.
 */
typedef struct {
    packet_header_t hdr;
    packet_body_t   body;
} packet_t;

/**
 * rf_frame_t — the 28-byte payload placed in the nRF24 TX/RX FIFO.
 *
 * Do NOT access hdr.soldier_id or hdr.persistent_tx_counter from inside
 * the encrypted portion — they are in the clear header for exactly this reason.
 *
 * Layout:
 *   [0–9]   header.version | soldier_id | persistent_tx_counter  (plain)
 *   [10–19] AES-CCM ciphertext (encrypted packet_body_t)
 *   [20–27] AES-CCM authentication tag (8 bytes)
 */
typedef struct __attribute__((packed)) {
    packet_header_t header;                  /**< Clear authenticated header (AAD)   */
    uint8_t         ciphertext[PACKET_BODY_LEN]; /**< Encrypted body                */
    uint8_t         tag[CCM_TAG_LEN];        /**< AES-CCM tag                       */
} rf_frame_t;

/* ── Compile-time size guards ───────────────────────────────────────────── */
_Static_assert(sizeof(packet_header_t) == PACKET_HEADER_LEN,
               "packet_header_t size mismatch — check __attribute__((packed))");
_Static_assert(sizeof(packet_body_t)   == PACKET_BODY_LEN,
               "packet_body_t size mismatch — check __attribute__((packed))");
_Static_assert(sizeof(rf_frame_t)      == RF_FRAME_LEN,
               "rf_frame_t size mismatch — check __attribute__((packed))");

/* ── Base-station per-soldier record (shared type for state machines) ────── */

/**
 * soldier_record_t — base station's runtime state for one tracked soldier.
 *
 * Two axes, independently updated (§18.2 decision):
 *   alarm: driven by received msg_type
 *   link:  driven by heartbeat timeout (LINK_TIMEOUT_MS)
 *
 * Both axes can be "bad" simultaneously, e.g. ALARM_EMERGENCY + LINK_LOST
 * (soldier went down, then radio died) — this is the most dangerous state
 * and must be displayed distinctly on the OLED.
 */
typedef struct {
    uint8_t       soldier_id;
    alarm_state_t alarm;
    link_state_t  link;

    uint8_t       battery_pct;
    uint8_t       link_quality;          /**< prev_tx_retries from last packet (0=best) */
    uint64_t      last_rx_counter;       /**< For replay detection: drop ≤ this counter */
    bool          has_received;          /**< True after first valid packet accepted    */

    uint32_t      base_rx_time_ms;       /**< Base receipt time (esp_timer_get_time)    */
    uint32_t      sender_uptime_ms;      /**< From last packet body (diagnostic)        */

    uint16_t      active_event_id;       /**< event_id of current alarm (for EMERG_CLR) */
    uint8_t       last_msg_type;

    /* Hardware health tracking (drift analysis) */
    uint32_t      last_upt_ms;           /**< For trivial monotonicity/reboot check     */
    uint32_t      anchor_rx_ms;          /**< Base time at start of drift window        */
    uint32_t      anchor_upt_ms;         /**< Soldier uptime at start of drift window   */
    bool          anchor_valid;          /**< Explicit flag for anchor initialization   */
    int32_t       ema_drift_ppm;         /**< Short-term (per-session) drift EMA        */
    bool          health_initialized;    /**< True once short-term EMA has settled      */
    int32_t       long_term_ema_ppm;     /**< Cross-reboot long-term drift baseline     */
    bool          long_term_initialized; /**< True once long-term EMA is seeded         */
    bool          hw_anomaly_flag;       /**< Latch: true if crystal/power is failing   */
} soldier_record_t;

#endif /* PACKET_PROTOCOL_H */
