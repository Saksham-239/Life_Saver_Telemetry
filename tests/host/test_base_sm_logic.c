#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "base_sm.h"

int test_def11_reboot_detection(void) {
    printf("--- Subtest DEF-11: Reboot Detection on Monotonic Gap > 1h ---\n");
    base_sm_init();

    packet_t p1 = {0};
    p1.hdr.version = PROTO_VERSION;
    p1.hdr.soldier_id = 1;
    p1.hdr.persistent_tx_counter = 100;
    p1.body.msg_type = MSG_HEARTBEAT;
    p1.body.sender_uptime_ms = 1000000; /* 1000 seconds */

    /* Packet 1 at 1000s */
    base_sm_rx_packet(&p1, 1000000);

    /* Packet 2 at 1350s (+350s > 300s DRIFT_WINDOW_MS) to initialize health tracking */
    packet_t p2 = p1;
    p2.hdr.persistent_tx_counter = 101;
    p2.body.sender_uptime_ms = 1000000 + 350000;
    base_sm_rx_packet(&p2, 1000000 + 350000);

    soldier_record_t rec;
    if (!base_sm_get_soldier(1, &rec) || !rec.health_initialized) {
        fprintf(stderr, "Precondition failed: health_initialized not true after drift window\n");
        return 2;
    }

    /* Out of range for 65 minutes (3,900,000 ms), unit never rebooted (monotonic uptime increases) */
    packet_t p3 = p1;
    p3.hdr.persistent_tx_counter = 102;
    p3.body.sender_uptime_ms = 1000000 + 350000 + 3900000; /* 4,900,000 ms */

    base_sm_rx_packet(&p3, 1000000 + 350000 + 3900000);
    base_sm_get_soldier(1, &rec);

    printf("After 65 min monotonic gap: health_initialized=%d (expected 1)\n", rec.health_initialized);
    if (!rec.health_initialized) {
        fprintf(stderr, "FAIL: Out-of-range gap > 1h caused false reboot detection and wiped health_initialized!\n");
        return 1;
    }

    printf("PASS: No false reboot detected on 65 min monotonic gap.\n");
    return 0;
}

int test_def11_counter_jump_session_detection(void) {
    printf("--- Subtest DEF-11: Counter-Jump New Boot Session Detection (jump >= 2^31) ---\n");
    base_sm_init();

    packet_t p1 = {0};
    p1.hdr.version = PROTO_VERSION;
    p1.hdr.soldier_id = 1;
    p1.hdr.persistent_tx_counter = 50;
    p1.body.msg_type = MSG_HEARTBEAT;
    p1.body.sender_uptime_ms = 100000;

    base_sm_rx_packet(&p1, 100000);

    /* Initialize health tracking (+350s window) */
    packet_t p2 = p1;
    p2.hdr.persistent_tx_counter = 51;
    p2.body.sender_uptime_ms = 100000 + 350000;
    base_sm_rx_packet(&p2, 100000 + 350000);

    soldier_record_t rec;
    base_sm_get_soldier(1, &rec);
    assert(rec.health_initialized == true);

    /* Node reboots: new flash reservation session adds 2^32 to counter base.
     * Counter jumps by 2^32 (4,294,967,296 >= 2^31). Uptime resets to 5,000 ms */
    packet_t p_reboot = p1;
    p_reboot.hdr.persistent_tx_counter = 51ULL + (1ULL << 32);
    p_reboot.body.sender_uptime_ms = 5000;
    base_sm_rx_packet(&p_reboot, 100000 + 350000 + 10000);

    base_sm_get_soldier(1, &rec);
    printf("After boot counter jump: health_initialized=%d (expected 0 on reboot reset)\n", rec.health_initialized);
    if (rec.health_initialized) {
        fprintf(stderr, "FAIL: Boot counter jump failed to reset health tracking!\n");
        return 1;
    }

    printf("PASS: Counter jump >= 2^31 successfully detected new boot session.\n");
    return 0;
}

int test_def11_uptime_wrap_and_outage_past_24d(void) {
    printf("--- Subtest DEF-11: Outage Past 24.8 Days & 32-bit Uptime Wrap (~49.7 Days) ---\n");
    base_sm_init();

    /* Scenario A: Outage past 24.8 days (> 2,147,483,648 ms) without reboot */
    packet_t p1 = {0};
    p1.hdr.version = PROTO_VERSION;
    p1.hdr.soldier_id = 1;
    p1.hdr.persistent_tx_counter = 1000;
    p1.body.msg_type = MSG_HEARTBEAT;
    p1.body.sender_uptime_ms = 100000;

    base_sm_rx_packet(&p1, 100000);

    /* Initialize health */
    packet_t p2 = p1;
    p2.hdr.persistent_tx_counter = 1001;
    p2.body.sender_uptime_ms = 100000 + 350000;
    base_sm_rx_packet(&p2, 100000 + 350000);

    soldier_record_t rec;
    base_sm_get_soldier(1, &rec);
    assert(rec.health_initialized == true);

    /* Long outage: 25 days (2,160,000,000 ms > 2^31).
     * Normal counter progression (e.g. counter + 50, < 2^31).
     * Because counter jump is small, it proves SAME boot session! */
    packet_t p_long = p1;
    p_long.hdr.persistent_tx_counter = 1050;
    p_long.body.sender_uptime_ms = (uint32_t)(100000 + 350000 + 2160000000ULL);
    base_sm_rx_packet(&p_long, (uint32_t)(100000 + 350000 + 2160000000ULL));

    base_sm_get_soldier(1, &rec);
    printf("After 25-day outage with same session counter: health_initialized=%d (expected 1)\n", rec.health_initialized);
    if (!rec.health_initialized) {
        fprintf(stderr, "FAIL: Outage past 24.8 days falsely triggered reboot in same counter session!\n");
        return 1;
    }

    /* Scenario B: Natural 32-bit uptime wrap around ~49.7 days */
    base_sm_init();
    packet_t pw1 = {0};
    pw1.hdr.version = PROTO_VERSION;
    pw1.hdr.soldier_id = 1;
    pw1.hdr.persistent_tx_counter = 2000;
    pw1.body.msg_type = MSG_HEARTBEAT;
    pw1.body.sender_uptime_ms = 0xFFFFFF00u; /* 256 ms before 32-bit wrap */
    base_sm_rx_packet(&pw1, 1000);

    /* Initialize anchor */
    packet_t pw2 = pw1;
    pw2.hdr.persistent_tx_counter = 2001;
    pw2.body.sender_uptime_ms = 0xFFFFFF00u + 350000u; /* wraps naturally */
    base_sm_rx_packet(&pw2, 1000 + 350000);
    base_sm_get_soldier(1, &rec);
    assert(rec.health_initialized == true);

    /* Next packet after wrap: uptime = 100 ms */
    packet_t pw3 = pw1;
    pw3.hdr.persistent_tx_counter = 2002;
    pw3.body.sender_uptime_ms = (uint32_t)(pw2.body.sender_uptime_ms + 10000u);
    base_sm_rx_packet(&pw3, 1000 + 350000 + 10000);

    base_sm_get_soldier(1, &rec);
    printf("After natural 32-bit wrap: health_initialized=%d (expected 1)\n", rec.health_initialized);
    if (!rec.health_initialized) {
        fprintf(stderr, "FAIL: 32-bit wrap falsely triggered reboot!\n");
        return 1;
    }

    printf("PASS: Outage past 24.8 days and 32-bit wrap handled cleanly.\n");
    return 0;
}

int test_miss02_replay_zero(void) {
    printf("--- Subtest MISS-02: Counter=0 Replay Rejection ---\n");
    base_sm_init();

    packet_t p1 = {0};
    p1.hdr.version = PROTO_VERSION;
    p1.hdr.soldier_id = 2;
    p1.hdr.persistent_tx_counter = 0; /* Counter = 0 */
    p1.body.msg_type = MSG_HEARTBEAT;
    p1.body.sender_uptime_ms = 5000;

    base_sm_rx_packet(&p1, 5000);
    soldier_record_t rec;
    if (!base_sm_get_soldier(2, &rec) || rec.base_rx_time_ms != 5000) {
        fprintf(stderr, "Precondition failed: packet 1 (ctr=0) not accepted\n");
        return 2;
    }

    /* Send replayed duplicate packet with counter = 0 */
    packet_t p2 = p1;
    p2.body.sender_uptime_ms = 5001;
    base_sm_rx_packet(&p2, 6000);

    base_sm_get_soldier(2, &rec);
    printf("After replaying ctr=0: base_rx_time_ms=%u (expected 5000 if dropped)\n",
           (unsigned)rec.base_rx_time_ms);

    if (rec.base_rx_time_ms == 6000) {
        fprintf(stderr, "FAIL: Replayed packet with counter=0 was accepted!\n");
        return 1;
    }

    printf("PASS: Replayed counter=0 rejected.\n");
    return 0;
}

int test_q1_no_downgrade_on_tactical_msg(void) {
    printf("--- Subtest Q1: Base SM Does Not Downgrade EMERGENCY on Tactical Msg ---\n");
    base_sm_init();

    /* Put soldier 3 in EMERGENCY via MSG_DOWN */
    packet_t p_down = {0};
    p_down.hdr.version = PROTO_VERSION;
    p_down.hdr.soldier_id = 3;
    p_down.hdr.persistent_tx_counter = 50;
    p_down.body.msg_type = MSG_DOWN;
    p_down.body.event_id = 1;
    base_sm_rx_packet(&p_down, 1000);

    soldier_record_t rec;
    if (!base_sm_get_soldier(3, &rec) || rec.alarm != ALARM_EMERGENCY) {
        fprintf(stderr, "Precondition failed: soldier not in EMERGENCY\n");
        return 2;
    }

    /* Soldier sends MSG_BACKUP while still in EMERGENCY (Q1 Option 1) */
    packet_t p_backup = {0};
    p_backup.hdr.version = PROTO_VERSION;
    p_backup.hdr.soldier_id = 3;
    p_backup.hdr.persistent_tx_counter = 51;
    p_backup.body.msg_type = MSG_BACKUP;
    p_backup.body.sensor_flags = SFLAG_FALL_DETECTED;
    p_backup.body.event_id = 1;
    base_sm_rx_packet(&p_backup, 2000);

    base_sm_get_soldier(3, &rec);
    if (rec.alarm != ALARM_EMERGENCY) {
        fprintf(stderr, "FAIL: Base station downgraded soldier from EMERGENCY to %d on MSG_BACKUP!\n", rec.alarm);
        return 1;
    }

    printf("PASS: Base station maintained EMERGENCY state on tactical message.\n");
    return 0;
}

int test_stale_counter_rejection_before_jump_subtraction(void) {
    printf("--- Subtest: Stale Counter (counter <= last) Rejected Before Jump Subtraction ---\n");
    base_sm_init();

    packet_t p1 = {0};
    p1.hdr.version = PROTO_VERSION;
    p1.hdr.soldier_id = 4;
    p1.hdr.persistent_tx_counter = 1000;
    p1.body.msg_type = MSG_HEARTBEAT;
    p1.body.sender_uptime_ms = 50000;
    base_sm_rx_packet(&p1, 50000);

    soldier_record_t rec;
    base_sm_get_soldier(4, &rec);
    assert(rec.last_rx_counter == 1000);

    /* An attacker transmits a stale packet with counter = 500 (500 <= 1000).
     * Because line 180 (counter <= last_rx_counter) sits BEFORE line 193
     * (counter - last_rx_counter), the stale packet is immediately dropped.
     * If subtraction occurred without the prior replay check,
     * (500 - 1000) would underflow into 0xFFFFFFFFFFFFFFF0ULL in unsigned 64-bit math,
     * which is >= 2^31, falsely triggering a reboot session reset! */
    packet_t p_stale = p1;
    p_stale.hdr.persistent_tx_counter = 500;
    p_stale.body.sender_uptime_ms = 60000;
    base_sm_rx_packet(&p_stale, 60000);

    base_sm_get_soldier(4, &rec);
    /* Verify packet was rejected: last_rx_counter remains 1000, base_rx_time_ms remains 50000 */
    if (rec.last_rx_counter != 1000 || rec.base_rx_time_ms != 50000) {
        fprintf(stderr, "FAIL: Stale counter was processed! last_rx_counter=%llu base_rx_time_ms=%u\n",
                (unsigned long long)rec.last_rx_counter, (unsigned)rec.base_rx_time_ms);
        return 1;
    }

    printf("PASS: Stale counter 500 <= 1000 rejected before jump subtraction; no unsigned underflow.\n");
    return 0;
}

int main(void) {
    printf("=== TEST: Base State Machine Logic (DEF-11 & MISS-02 Verification) ===\n");
    int r1 = test_def11_reboot_detection();
    int r2 = test_def11_counter_jump_session_detection();
    int r3 = test_def11_uptime_wrap_and_outage_past_24d();
    int r4 = test_miss02_replay_zero();
    int r5 = test_q1_no_downgrade_on_tactical_msg();
    int r6 = test_stale_counter_rejection_before_jump_subtraction();
    return (r1 != 0 || r2 != 0 || r3 != 0 || r4 != 0 || r5 != 0 || r6 != 0) ? 1 : 0;
}
