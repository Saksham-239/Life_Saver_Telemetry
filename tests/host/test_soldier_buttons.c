#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "soldier_sm.h"
#include "../../soldier_unit/src/rtos/rtos_tasks.h"
#include "../../soldier_unit/src/ui/buttons.h"

int main(void) {
    printf("=== TEST: Soldier Button Events & Alarm Retention (DEF-03 & Q1 Option 1) ===\n");

    soldier_sm_t sm;
    soldier_sm_init(&sm);

    radio_tx_req_t tx_req;
    sm_event_msg_t ev;

    /* Subtest 1: BUTTON_BACKUP from NORMAL transitions to ALERT_LATCHED */
    ev.type = SM_EV_BUTTON_BACKUP;
    bool handled = soldier_sm_process_event(&sm, &ev, 1000, &tx_req);

    printf("Normal -> BACKUP: handled=%d, tx_msg=%d, alarm=%d\n", 
           handled, tx_req.msg_type, soldier_sm_alarm(&sm));

    if (!handled || tx_req.msg_type != MSG_BACKUP || soldier_sm_alarm(&sm) != ALARM_ALERT_LATCHED) {
        fprintf(stderr, "FAIL: Button backup event failed to transition to ALERT_LATCHED!\n");
        return 1;
    }

    /* Subtest 2: BUTTON_COMPLETE clears ALERT_LATCHED */
    ev.type = SM_EV_BUTTON_COMPLETE;
    handled = soldier_sm_process_event(&sm, &ev, 1100, &tx_req);
    if (!handled || tx_req.msg_type != MSG_COMPLETE || soldier_sm_alarm(&sm) != ALARM_NORMAL) {
        fprintf(stderr, "FAIL: BUTTON_COMPLETE failed to clear ALERT_LATCHED!\n");
        return 1;
    }
    printf("ALERT_LATCHED -> COMPLETE: cleared to NORMAL (alarm=%d)\n", soldier_sm_alarm(&sm));

    /* Subtest 3: BUTTON_DOWN triggers EMERGENCY without fall flag */
    ev.type = SM_EV_BUTTON_DOWN;
    handled = soldier_sm_process_event(&sm, &ev, 1200, &tx_req);
    if (!handled || tx_req.msg_type != MSG_DOWN || soldier_sm_alarm(&sm) != ALARM_EMERGENCY) {
        fprintf(stderr, "FAIL: BUTTON_DOWN failed to trigger EMERGENCY!\n");
        return 1;
    }
    if ((tx_req.sensor_flags & SFLAG_FALL_DETECTED) != 0) {
        fprintf(stderr, "FAIL: BUTTON_DOWN improperly asserted SFLAG_FALL_DETECTED without an accelerometer fall!\n");
        return 1;
    }
    printf("BUTTON_DOWN: triggered EMERGENCY (alarm=%d, sensor_flags=0x%02X, no SFLAG_FALL_DETECTED)\n",
           soldier_sm_alarm(&sm), tx_req.sensor_flags);

    /* Subtest 4 (Q1 Option 1 from BUTTON_DOWN): In EMERGENCY, tactical button sends tactical msg but stays EMERGENCY */
    ev.type = SM_EV_BUTTON_BACKUP;
    handled = soldier_sm_process_event(&sm, &ev, 1300, &tx_req);
    printf("BUTTON_DOWN EMERGENCY -> BACKUP: handled=%d, tx_msg=%d, alarm=%d, sensor_flags=0x%02X\n",
           handled, tx_req.msg_type, soldier_sm_alarm(&sm), tx_req.sensor_flags);

    if (!handled || tx_req.msg_type != MSG_BACKUP) {
        fprintf(stderr, "FAIL: Tactical msg not transmitted in EMERGENCY!\n");
        return 1;
    }
    if (soldier_sm_alarm(&sm) != ALARM_EMERGENCY) {
        fprintf(stderr, "FAIL: Soldier SM downgraded from EMERGENCY on tactical msg!\n");
        return 1;
    }
    if ((tx_req.sensor_flags & SFLAG_FALL_DETECTED) != 0) {
        fprintf(stderr, "FAIL: BUTTON_DOWN emergency unexpectedly has SFLAG_FALL_DETECTED on tactical msg!\n");
        return 1;
    }

    /* Subtest 4b: Fall-induced EMERGENCY carries SFLAG_FALL_DETECTED */
    soldier_sm_t sm_fall;
    soldier_sm_init(&sm_fall);
    soldier_sm_on_fall(&sm_fall, 2000); /* Enters FALL_PENDING */
    soldier_sm_tick(&sm_fall, 2000 + FALL_CONFIRM_WINDOW_MS); /* Confirms fall -> ALARM_EMERGENCY */
    if (sm_fall.alarm != ALARM_EMERGENCY || !sm_fall.fall_detected) {
        fprintf(stderr, "FAIL: Fall confirmation failed to set ALARM_EMERGENCY with fall_detected=true!\n");
        return 1;
    }
    /* Now press tactical backup in fall emergency */
    ev.type = SM_EV_BUTTON_BACKUP;
    handled = soldier_sm_process_event(&sm_fall, &ev, 2000 + FALL_CONFIRM_WINDOW_MS + 100, &tx_req);
    if (!handled || tx_req.msg_type != MSG_BACKUP || sm_fall.alarm != ALARM_EMERGENCY) {
        fprintf(stderr, "FAIL: Tactical msg failed in fall emergency!\n");
        return 1;
    }
    if ((tx_req.sensor_flags & SFLAG_FALL_DETECTED) == 0) {
        fprintf(stderr, "FAIL: Fall-induced emergency did not assert SFLAG_FALL_DETECTED on tactical msg!\n");
        return 1;
    }
    printf("Fall EMERGENCY -> BACKUP: handled=%d, tx_msg=%d, alarm=%d, sensor_flags=0x%02X (SFLAG_FALL_DETECTED set)\n",
           handled, tx_req.msg_type, soldier_sm_alarm(&sm_fall), tx_req.sensor_flags);

    /* Subtest 5: BUTTON_COMPLETE cannot clear EMERGENCY */
    ev.type = SM_EV_BUTTON_COMPLETE;
    handled = soldier_sm_process_event(&sm, &ev, 1400, &tx_req);
    if (soldier_sm_alarm(&sm) != ALARM_EMERGENCY) {
        fprintf(stderr, "FAIL: BUTTON_COMPLETE inappropriately cleared EMERGENCY!\n");
        return 1;
    }

    /* Subtest 6: Long-press COMPLETE clears EMERGENCY */
    ev.type = SM_EV_BUTTON_CLEAR_EMERGENCY;
    handled = soldier_sm_process_event(&sm, &ev, 1500, &tx_req);
    if (!handled || tx_req.msg_type != MSG_EMERGENCY_CLEAR || soldier_sm_alarm(&sm) != ALARM_NORMAL) {
        fprintf(stderr, "FAIL: Long-press COMPLETE failed to clear EMERGENCY!\n");
        return 1;
    }
    printf("EMERGENCY -> Long-press COMPLETE: cleared to NORMAL (alarm=%d)\n", soldier_sm_alarm(&sm));

    printf("PASS: Soldier button state machine fully validated.\n");
    return 0;
}
