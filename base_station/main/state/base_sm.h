/**
 * base_sm.h — Base station per-soldier state machine.
 *
 * Maintains a soldier_record_t array (up to SOLDIER_ID_MAX entries).
 * All protocol rules are applied here:
 *
 *   B1. Replay detection: drop packet if counter ≤ last_rx_counter for that soldier.
 *   B2. LINK_LOST: if no packet received from a soldier in LINK_TIMEOUT_MS, mark LINK_LOST.
 *   B3. ALARM_EMERGENCY: set on MSG_DOWN.  Cleared ONLY on MSG_EMERGENCY_CLEAR
 *       with matching event_id.
 *   B4. MSG_COMPLETE: clears ALARM_ALERT_LATCHED → ALARM_NORMAL only.
 *       Has NO effect on ALARM_EMERGENCY.
 *   B5. Hardware ACK is transport-only and is NOT carried in the received packet.
 *       (The base receives the payload — ACK was a link-layer event between the two nRF24s.)
 *   B6. Heartbeat (MSG_HEARTBEAT): updates link state and diagnostics only.
 *       No alarm state change.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../../../shared/include/packet_protocol.h"

/**
 * Initialise the base state machine.
 * Must be called from app_main() before base_sm_rx_packet().
 */
void base_sm_init(void);

/**
 * Set the baseline RX counter for a soldier (used during boot).
 */
void base_sm_set_rx_counter(uint8_t soldier_id, uint64_t counter);

/**
 * Feed a received, authenticated, decoded packet into the base state machine.
 *
 * Performs replay detection and all alarm state transitions.
 * Updates the soldier_record_t for pkt->hdr.soldier_id.
 *
 * @param pkt       Decoded packet (from aes_ccm_decrypt).
 * @param base_rx_ms  Base station receipt time in ms (esp_timer_get_time() / 1000).
 */
void base_sm_rx_packet(const packet_t *pkt, uint32_t base_rx_ms);

/**
 * Periodic tick — drives LINK_LOST transitions on heartbeat timeout.
 * Call every second from the base main loop.
 *
 * @param now_ms  Current time in ms.
 */
void base_sm_tick(uint32_t now_ms);

/**
 * Get a snapshot of the current state for all soldiers.
 * @param out    Array of SOLDIER_ID_MAX soldier_record_t structs (caller allocated).
 */
void base_sm_get_state(soldier_record_t out[SOLDIER_ID_MAX]);

/**
 * Get a copy of the record for a single soldier under lock (thread-safe).
 * @param soldier_id 1-indexed ID (1..SOLDIER_ID_MAX).
 * @param out        Pointer to destination struct.
 * @return true if valid soldier_id and copied successfully.
 */
bool base_sm_get_soldier(uint8_t soldier_id, soldier_record_t *out);

/** Direct pointer access for single-threaded init/tests. */
const soldier_record_t *base_sm_get_soldier_ptr(uint8_t soldier_id);
