/**
 * soldier_sm.h — Soldier-unit application state machine.
 *
 * Two-axis model (§18.2 / Terra correction):
 *   Alarm axis:  ALARM_NORMAL → ALARM_FALL_PENDING → ALARM_ALERT_LATCHED
 *                                                  → ALARM_EMERGENCY
 *   Link axis:   LINK_ONLINE ↔ LINK_LOST
 *   Both axes update independently; display reflects the combined state.
 *
 * Key protocol decisions (session notes):
 *   - Hardware ShockBurst ACK = transport delivery status only; NEVER changes alarm state.
 *   - MSG_COMPLETE clears ALARM_ALERT_LATCHED only.  It has no effect on ALARM_EMERGENCY.
 *   - MSG_EMERGENCY_CLEAR (long-hold ≥ 3 s COMPLETE) clears ALARM_EMERGENCY.
 *   - A confirmed fall goes directly to ALARM_EMERGENCY — NOT through ALARM_ALERT_LATCHED.
 *   - event_id increments per new alarm event; persists until alarm is cleared.
 *
 * Interface contract:
 *   main() calls soldier_sm_init() once, then calls soldier_sm_tick() in the
 *   main loop after polling buttons, sensors, and battery.
 *   soldier_sm_tick() is responsible for calling radio_send() when needed.
 */

#ifndef SOLDIER_SM_H
#define SOLDIER_SM_H

#include <stdbool.h>
#include <stdint.h>
#include "../../shared/include/packet_protocol.h"
#include "../features/config_store.h"
#include "../sensors/fall_detect.h"
#include "../ui/buttons.h"
#include "../rtos/rtos_tasks.h"

/**
 * sm_tx_action_t — Return type for state transition functions.
 *
 * Structural invariant: msg_type is determined at the point of state
 * transition, not independently by the caller.  This prevents the class
 * of bug where a transition sets alarm state in one direction (e.g. RAISE)
 * but the caller tags the outgoing packet with the opposite (e.g. CLEAR).
 */
typedef struct {
    bool       send;      /**< True if a packet should be transmitted      */
    msg_type_t msg_type;  /**< Valid only when send == true                */
} sm_tx_action_t;

typedef struct {
    alarm_state_t alarm;
    link_state_t  link;           /* Soldier unit tracks its own link quality  */
    uint16_t      event_id;       /* Increments per new alarm, persists until clear */
    bool          fall_detected;  /* True ONLY if emergency originated from fall sensor */
    uint32_t      fall_pending_start_ms;  /* When ALARM_FALL_PENDING began     */
    uint32_t      last_tx_ms;     /* When we last sent any packet              */
    uint32_t      next_heartbeat_ms;      /* Scheduled next heartbeat time     */
    uint8_t       prev_arc_cnt;   /* Cached ARC_CNT from previous TX          */
} soldier_sm_t;

/**
 * Initialise the state machine.
 * @param sm    State machine instance (caller-allocated).
 */
void soldier_sm_init(soldier_sm_t *sm);

/**
 * Handle a fall candidate event.
 * Transitions ALARM_NORMAL → ALARM_FALL_PENDING; emits MSG_FALL_PENDING.
 */
sm_tx_action_t soldier_sm_on_fall(soldier_sm_t *sm, uint32_t now_ms);

/**
 * Handle operator long-hold COMPLETE: clear an active EMERGENCY.
 * Transitions ALARM_EMERGENCY → ALARM_NORMAL; emits MSG_EMERGENCY_CLEAR.
 * No-op (and no send) if alarm is not ALARM_EMERGENCY.
 */
sm_tx_action_t soldier_sm_on_clear_emergency(soldier_sm_t *sm, uint32_t now_ms);

/**
 * Handle fall cancel (double press).
 * Transitions ALARM_FALL_PENDING → ALARM_NORMAL; emits MSG_FALL_CANCEL.
 */
sm_tx_action_t soldier_sm_on_cancel(soldier_sm_t *sm, uint32_t now_ms);

/**
 * Advance periodic logic (heartbeats, fall confirmation).
 * Returns the action to take (send heartbeat, confirm fall, or nothing).
 */
sm_tx_action_t soldier_sm_tick(soldier_sm_t *sm, uint32_t now_ms);

/** Return the current alarm state (for display / vibration logic). */
alarm_state_t soldier_sm_alarm(const soldier_sm_t *sm);

/** Return a short string for alarm state (debug logging). */
const char *alarm_state_name(alarm_state_t a);

/** Helper to process an event and optionally produce a TX request */
bool soldier_sm_process_event(soldier_sm_t *sm, const sm_event_msg_t *ev, uint32_t now_ms, radio_tx_req_t *tx_req);

#endif /* SOLDIER_SM_H */
