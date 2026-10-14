/**
 * soldier_sm.c — Soldier-unit application state machine implementation.
 *
 * Rules enforced here (from session decisions, do not change without updating
 * the session notes and the base station state machine symmetrically):
 *
 *   R1. Hardware ACK (radio_send result.delivered) is transport-only.
 *       It is used for retry-count diagnostics and logged, but it NEVER
 *       transitions alarm state.  The alarm advances on button press or
 *       sensor event, regardless of whether the packet was ACKed.
 *
 *   R2. MSG_COMPLETE clears ALARM_ALERT_LATCHED → ALARM_NORMAL.
 *       MSG_COMPLETE has NO EFFECT on ALARM_EMERGENCY.
 *
 *   R3. Confirmed fall (FALL_EVENT_CANDIDATE + 10s no cancel) →
 *       ALARM_EMERGENCY directly.  No intermediate ALARM_ALERT_LATCHED step.
 *
 *   R4. ALARM_EMERGENCY is cleared ONLY by:
 *         (a) soldier sends MSG_EMERGENCY_CLEAR (long-hold COMPLETE ≥ 3 s)
 *         Locally: alarm → ALARM_NORMAL, event_id unchanged (base validates it).
 *
 *   R5. event_id increments on every NEW alarm event (button press, fall confirm).
 *       event_id is stable during ALARM_FALL_PENDING window.
 *       MSG_EMERGENCY_CLEAR carries the current event_id for base validation.
 *
 *   R6. Heartbeat scheduling: phase offset at boot (soldier_id-derived),
 *       then HEARTBEAT_BASE_MS ± random HEARTBEAT_JITTER_MS each cycle.
 *       Heartbeat is sent as MSG_HEARTBEAT with current flags/battery.
 *
 *   R7. Link status on the soldier side: we track s_link as LINK_ONLINE
 *       normally and LINK_LOST if consecutive TX failures exceed threshold.
 *       This is informational only (no alarm state change on link loss).
 */

#include "soldier_sm.h"
#include "../rtos/rtos_tasks.h"
#include "battery.h"
#include <stdlib.h>    /* rand() */
#include <stdio.h>
#include <string.h>

#define CONSECUTIVE_FAIL_LINK_LOST  5u   /* N consecutive TX failures → LINK_LOST */
static uint8_t s_consecutive_fails = 0;
static uint8_t s_cached_battery = 0;

/* ── Private helpers ─────────────────────────────────────────────────────── */

/** Compute a random heartbeat jitter in ± HEARTBEAT_JITTER_MS. */
static int32_t random_jitter(void) {
    int32_t span = (int32_t)(2 * HEARTBEAT_JITTER_MS + 1);
    return (int32_t)(rand() % (int)span) - (int32_t)HEARTBEAT_JITTER_MS;
}

static void schedule_next_heartbeat(soldier_sm_t *sm, uint32_t now_ms) {
    int32_t j = random_jitter();
    sm->next_heartbeat_ms = now_ms + HEARTBEAT_BASE_MS + (uint32_t)j;
}

/** Build sensor_flags bitmask from current alarm state. */
static uint8_t build_sensor_flags(const soldier_sm_t *sm) {
    uint8_t f = 0;
    if (sm->fall_detected)               f |= SFLAG_FALL_DETECTED;
    if (sm->alarm == ALARM_FALL_PENDING) f |= SFLAG_FALL_PENDING;
    if (s_cached_battery < 20)          f |= SFLAG_LOW_BATTERY; // Assume <20% is low
    return f;
}

static void populate_tx_req(const soldier_sm_t *sm, msg_type_t mt, uint32_t now_ms, radio_tx_req_t *req) {
    req->msg_type         = mt;
    req->sensor_flags     = build_sensor_flags(sm);
    req->battery_pct      = s_cached_battery;
    req->prev_tx_retries  = sm->prev_arc_cnt;
    req->sender_uptime_ms = now_ms;
    req->event_id         = sm->event_id;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void soldier_sm_init(soldier_sm_t *sm) {
    memset(sm, 0, sizeof(*sm));
    sm->alarm             = ALARM_NORMAL;
    sm->link              = LINK_ONLINE;
    sm->event_id          = 0;
    sm->fall_detected     = false;
    sm->prev_arc_cnt      = 0;
    sm->last_tx_ms        = 0;

    /* Heartbeat phase should be initialized by caller using config. */
}

sm_tx_action_t soldier_sm_on_fall(soldier_sm_t *sm, uint32_t now_ms) {
    if (sm->alarm == ALARM_NORMAL) {
        sm->event_id++;
        sm->alarm = ALARM_FALL_PENDING;
        sm->fall_detected = false;
        sm->fall_pending_start_ms = now_ms;
        printf("[SM] Fall candidate → ALARM_FALL_PENDING (event_id=%u)\n", sm->event_id);
        return (sm_tx_action_t){ true, MSG_FALL_PENDING };
    }
    return (sm_tx_action_t){ false, MSG_HEARTBEAT };
}

sm_tx_action_t soldier_sm_on_clear_emergency(soldier_sm_t *sm, uint32_t now_ms) {
    (void)now_ms;
    if (sm->alarm == ALARM_EMERGENCY) {
        sm->alarm = ALARM_NORMAL;
        sm->fall_detected = false;
        printf("[SM] EMERGENCY cleared by operator (event_id=%u)\n", sm->event_id);
        return (sm_tx_action_t){ true, MSG_EMERGENCY_CLEAR };
    }
    printf("[SM] Clear-emergency ignored: alarm=%s\n", alarm_state_name(sm->alarm));
    return (sm_tx_action_t){ false, MSG_HEARTBEAT };
}

sm_tx_action_t soldier_sm_on_cancel(soldier_sm_t *sm, uint32_t now_ms) {
    (void)now_ms;
    if (sm->alarm == ALARM_FALL_PENDING) {
        printf("[SM] Fall cancelled by double-press\n");
        sm->alarm = ALARM_NORMAL;
        sm->fall_detected = false;
        return (sm_tx_action_t){ true, MSG_FALL_CANCEL };
    }
    return (sm_tx_action_t){ false, MSG_HEARTBEAT };
}

sm_tx_action_t soldier_sm_tick(soldier_sm_t *sm, uint32_t now_ms) {
    /* Fall confirmation timeout */
    if (sm->alarm == ALARM_FALL_PENDING) {
        if ((now_ms - sm->fall_pending_start_ms) >= FALL_CONFIRM_WINDOW_MS) {
            printf("[SM] Fall confirmed → ALARM_EMERGENCY\n");
            sm->alarm = ALARM_EMERGENCY;
            sm->fall_detected = true;
            return (sm_tx_action_t){ true, MSG_DOWN };
        }
    }

    /* Heartbeat schedule */
    if (now_ms >= sm->next_heartbeat_ms) {
        schedule_next_heartbeat(sm, now_ms);
        return (sm_tx_action_t){ true, MSG_HEARTBEAT };
    }

    return (sm_tx_action_t){ false, MSG_HEARTBEAT };
}

alarm_state_t soldier_sm_alarm(const soldier_sm_t *sm) {
    return sm->alarm;
}

const char *alarm_state_name(alarm_state_t a) {
    switch (a) {
    case ALARM_NORMAL:        return "NORMAL";
    case ALARM_FALL_PENDING:  return "FALL_PENDING";
    case ALARM_ALERT_LATCHED: return "ALERT_LATCHED";
    case ALARM_EMERGENCY:     return "EMERGENCY";
    default:                  return "UNKNOWN";
    }
}

bool soldier_sm_process_event(soldier_sm_t *sm, const sm_event_msg_t *ev, uint32_t now_ms, radio_tx_req_t *tx_req) {
    sm_tx_action_t action = { false, MSG_HEARTBEAT };

    switch (ev->type) {
    case SM_EV_FALL_CANDIDATE:
        action = soldier_sm_on_fall(sm, now_ms);
        break;

    case SM_EV_BUTTON_CLEAR_EMERGENCY:
        action = soldier_sm_on_clear_emergency(sm, now_ms);
        break;

    case SM_EV_BUTTON_CANCEL:
        action = soldier_sm_on_cancel(sm, now_ms);
        break;

    case SM_EV_BUTTON_BACKUP:
    case SM_EV_BUTTON_ENEMY:
    case SM_EV_BUTTON_MEDICAL: {
        msg_type_t mt = (ev->type == SM_EV_BUTTON_BACKUP) ? MSG_BACKUP :
                        (ev->type == SM_EV_BUTTON_ENEMY)  ? MSG_ENEMY : MSG_MEDICAL;
        if (sm->alarm == ALARM_EMERGENCY) {
            action = (sm_tx_action_t){ true, mt };
        } else {
            sm->event_id++;
            sm->alarm = ALARM_ALERT_LATCHED;
            action = (sm_tx_action_t){ true, mt };
        }
        break;
    }

    case SM_EV_BUTTON_DOWN:
        sm->event_id++;
        sm->alarm = ALARM_EMERGENCY;
        sm->fall_detected = false; /* Manual man-down emergency, NOT an accelerometer fall */
        action = (sm_tx_action_t){ true, MSG_DOWN };
        break;

    case SM_EV_BUTTON_COMPLETE:
        if (sm->alarm == ALARM_ALERT_LATCHED) {
            sm->alarm = ALARM_NORMAL;
            action = (sm_tx_action_t){ true, MSG_COMPLETE };
        }
        break;

    case SM_EV_BATTERY_UPDATE:
        s_cached_battery = ev->data.battery_pct;
        break;

    case SM_EV_RADIO_RESULT:
        sm->prev_arc_cnt = ev->data.tx_result.retry_count;
        if (!ev->data.tx_result.delivered) {
            if (s_consecutive_fails < CONSECUTIVE_FAIL_LINK_LOST) {
                s_consecutive_fails++;
            }
            if (s_consecutive_fails == CONSECUTIVE_FAIL_LINK_LOST) {
                sm->link = LINK_LOST;
            }
        } else {
            s_consecutive_fails = 0;
            sm->link = LINK_ONLINE;
        }
        break;

    default:
        break;
    }

    /* Time-based transitions only if no event-driven send is pending.
     * A heartbeat deferred by one tick (20 ms) is negligible against
     * the 30-second heartbeat interval.                              */
    if (!action.send) {
        action = soldier_sm_tick(sm, now_ms);
    }

    if (action.send) {
        populate_tx_req(sm, action.msg_type, now_ms, tx_req);
    }

    return action.send;
}
