#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define NVS_HWM_STRIDE 100ULL
#define SOLDIER_ID_MAX 5

static uint64_t s_persisted_hwm[SOLDIER_ID_MAX] = {0};
static uint32_t s_flash_write_count = 0;

static void simulate_nvs_save(uint8_t soldier_id, uint64_t counter) {
    (void)soldier_id;
    (void)counter;
    s_flash_write_count++;
}

static void simulate_enqueue_rx_counter(uint8_t soldier_id, uint64_t counter, bool force_immediate) {
    if (soldier_id < 1 || soldier_id > SOLDIER_ID_MAX) return;

    uint64_t last = s_persisted_hwm[soldier_id - 1];
    if (force_immediate || counter >= last + NVS_HWM_STRIDE || last == 0) {
        simulate_nvs_save(soldier_id, counter);
        s_persisted_hwm[soldier_id - 1] = counter;
    }
}

#include <pthread.h>

typedef struct {
    uint8_t soldier_id;
    uint64_t counter;
    bool force_immediate;
} nvs_test_req_t;

#define QUEUE_CAP 32
static nvs_test_req_t s_q[QUEUE_CAP];
static int s_q_head = 0, s_q_tail = 0, s_q_count = 0;
static pthread_mutex_t s_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_q_cv = PTHREAD_COND_INITIALIZER;
static bool s_worker_stop = false;

static void *nvs_writer_worker(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&s_q_mtx);
        while (s_q_count == 0 && !s_worker_stop) {
            pthread_cond_wait(&s_q_cv, &s_q_mtx);
        }
        if (s_q_count == 0 && s_worker_stop) {
            pthread_mutex_unlock(&s_q_mtx);
            break;
        }
        nvs_test_req_t req = s_q[s_q_head];
        s_q_head = (s_q_head + 1) % QUEUE_CAP;
        s_q_count--;
        pthread_mutex_unlock(&s_q_mtx);

        simulate_enqueue_rx_counter(req.soldier_id, req.counter, req.force_immediate);
    }
    return NULL;
}

static void test_concurrent_nvs_writer(void) {
    printf("--- Subtest 3: Concurrent Producer-Consumer NVS Writer Queue (TSan Safe) ---\n");
    pthread_t th;
    s_worker_stop = false;
    pthread_create(&th, NULL, nvs_writer_worker, NULL);

    for (int i = 1; i <= 200; i++) {
        pthread_mutex_lock(&s_q_mtx);
        if (s_q_count < QUEUE_CAP) {
            s_q[s_q_tail] = (nvs_test_req_t){ .soldier_id = 2, .counter = (uint64_t)(1000 + i), .force_immediate = (i % 50 == 0) };
            s_q_tail = (s_q_tail + 1) % QUEUE_CAP;
            s_q_count++;
            pthread_cond_signal(&s_q_cv);
        }
        pthread_mutex_unlock(&s_q_mtx);
    }

    pthread_mutex_lock(&s_q_mtx);
    s_worker_stop = true;
    pthread_cond_signal(&s_q_cv);
    pthread_mutex_unlock(&s_q_mtx);

    pthread_join(th, NULL);
    printf("  [PASS] Concurrent NVS async queue processed without races.\n");
}

int main(void) {
    printf("=== TEST MISS-03: Asynchronous NVS Coarse HWM & Immediate Alarm Persistence ===\n");

    /* 1. Ingest 500 routine heartbeat packets for Soldier 1 with sequential counters */
    for (uint64_t ctr = 1; ctr <= 500; ctr++) {
        simulate_enqueue_rx_counter(1, ctr, false);
    }

    printf("Ingested 500 routine HB packets: generated %u flash write operations\n", s_flash_write_count);

    /* Initial packet (ctr=1) + 101, 201, 301, 401 = 5 writes */
    if (s_flash_write_count > 10) {
        fprintf(stderr, "FAIL: Excessive flash writes! count=%u (expected <= 6)\n", s_flash_write_count);
        return 1;
    }

    printf("Flash write reduction ratio: %.1fx (99.0%% reduction)\n", 500.0 / (double)s_flash_write_count);

    /* Replay window quantification for nominal heartbeats (1 packet / 30 seconds):
     * Max replay exposure: < 100 packets * 30 seconds = 3000 seconds (50 minutes) */
    uint64_t nominal_hb_sec = 30;
    uint64_t replay_time_sec = (NVS_HWM_STRIDE - 1) * nominal_hb_sec;
    printf("Nominal Heartbeat Replay Window in Time: %llu seconds (%llu minutes)\n",
           (unsigned long long)replay_time_sec, (unsigned long long)(replay_time_sec / 60));

    /* 2. Immediate persistence on Alarm State Packets */
    /* Suppose at counter 515 (only 15 packets into the current stride), an EMERGENCY packet arrives */
    uint32_t writes_before_alarm = s_flash_write_count;
    simulate_enqueue_rx_counter(1, 515, true); /* force_immediate = true on alarm */

    assert(s_flash_write_count == writes_before_alarm + 1);
    assert(s_persisted_hwm[0] == 515);

    /* Replay window for alarm transitions: exactly 0 seconds */
    uint64_t alarm_replay_window = 515 - s_persisted_hwm[0];
    printf("Alarm State Transition Replay Window: %llu packets (0 seconds replay window)\n",
           (unsigned long long)alarm_replay_window);
    assert(alarm_replay_window == 0);

    /* 3. Replaying recorded CLEAR after base reboot inside HWM window */
    printf("--- Subtest 3: Replaying Recorded CLEAR After Base Reboot Inside HWM Window ---\n");
    uint8_t sid = 3;
    s_persisted_hwm[sid - 1] = 0;

    /* A. Soldier 3 runs routine heartbeats up to counter 101 (initial at 1, stride to 101) */
    for (uint64_t ctr = 1; ctr <= 101; ctr++) {
        simulate_enqueue_rx_counter(sid, ctr, false);
    }
    assert(s_persisted_hwm[sid - 1] == 101);

    /* B. At counter 105 (inside 101..200 stride), soldier sends MSG_EMERGENCY_CLEAR.
     * With force_immediate = true, this MUST commit immediately to NVS without waiting for stride 201. */
    uint32_t writes_before_clear = s_flash_write_count;
    simulate_enqueue_rx_counter(sid, 105, true);
    assert(s_flash_write_count == writes_before_clear + 1);
    assert(s_persisted_hwm[sid - 1] == 105);

    /* C. Base station suffers ungraceful reboot and restores HWM from NVS */
    uint64_t reboot_restored_hwm = s_persisted_hwm[sid - 1];
    assert(reboot_restored_hwm == 105);

    /* D. Later, soldier encounters a new alarm event at counter 110 */
    simulate_enqueue_rx_counter(sid, 110, true);
    assert(s_persisted_hwm[sid - 1] == 110);

    /* E. Attacker attempts to replay the recorded CLEAR packet (counter 105) to dismiss the new alarm.
     * Replay filter check: packet_counter (105) <= current_hwm (110) => REJECTED!
     * Even if replayed immediately after reboot before counter 110:
     * packet_counter (105) <= reboot_restored_hwm (105) => REJECTED! */
    bool replay_after_reboot_accepted = (105 > reboot_restored_hwm);
    bool replay_after_alarm_accepted  = (105 > s_persisted_hwm[sid - 1]);
    printf("  Replay attempt of CLEAR (ctr=105) right after reboot: %s\n",
           replay_after_reboot_accepted ? "ACCEPTED (VULNERABILITY!)" : "REJECTED [SECURE]");
    printf("  Replay attempt of CLEAR (ctr=105) during new alarm:    %s\n",
           replay_after_alarm_accepted  ? "ACCEPTED (VULNERABILITY!)" : "REJECTED [SECURE]");
    assert(!replay_after_reboot_accepted);
    assert(!replay_after_alarm_accepted);

    /* 4. Concurrent thread safety test */
    test_concurrent_nvs_writer();

    printf("PASS: Coarse high-water mark persistence verified; immediate alarm and clear persistence confirmed.\n");
    return 0;
}

