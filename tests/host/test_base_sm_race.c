#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include "base_sm.h"

#define NUM_ITERATIONS 100000

#include <stdatomic.h>

static atomic_bool s_stop = false;
static uint32_t s_torn_reads = 0;
static uint64_t s_total_reads = 0;

/* Writer thread: simulates task_radio_rx updating soldier record */
static void *rx_worker(void *arg) {
    (void)arg;
    for (uint64_t i = 1; i <= NUM_ITERATIONS && !atomic_load(&s_stop); i++) {
        packet_t pkt = {0};
        pkt.hdr.version = PROTO_VERSION;
        pkt.hdr.soldier_id = 1;
        pkt.hdr.persistent_tx_counter = i;
        if (i % 2 == 1) {
            pkt.body.msg_type = MSG_BACKUP;
            pkt.body.event_id = 1;
            pkt.body.sender_uptime_ms = 1000;
        } else {
            pkt.body.msg_type = MSG_COMPLETE;
            pkt.body.event_id = 2;
            pkt.body.sender_uptime_ms = 2000;
        }

        base_sm_rx_packet(&pkt, (uint32_t)i);
    }
    atomic_store(&s_stop, true);
    return NULL;
}

/* Reader thread: simulates task_ui taking snapshot via base_sm_get_state */
static void *ui_worker(void *arg) {
    (void)arg;
    soldier_record_t snapshot[SOLDIER_ID_MAX];

    while (!atomic_load(&s_stop)) {
        base_sm_get_state(snapshot);
        s_total_reads++;
        uint16_t eid = snapshot[0].active_event_id;
        alarm_state_t alm = snapshot[0].alarm;

        /* Cross-field Invariant: eid==1 -> ALARM_ALERT_LATCHED, eid==2 -> ALARM_NORMAL */
        if ((eid == 1 && alm != ALARM_ALERT_LATCHED) ||
            (eid == 2 && alm != ALARM_NORMAL)) {
            s_torn_reads++;
        }
    }
    return NULL;
}

int main(void) {
    printf("=== TEST: Base Station s_soldiers Concurrency Race (DEF-04 Verification) ===\n");
    base_sm_init();

    pthread_t th_rx, th_ui;
    pthread_create(&th_rx, NULL, rx_worker, NULL);
    pthread_create(&th_ui, NULL, ui_worker, NULL);

    pthread_join(th_rx, NULL);
    pthread_join(th_ui, NULL);

    printf("Writer packets: %d, Total reader snapshots: %llu\n", 
           NUM_ITERATIONS, (unsigned long long)s_total_reads);
    printf("Cross-field torn reads observed: %u (%.2f%% of reader snapshots)\n", 
           s_torn_reads, s_total_reads > 0 ? ((double)s_torn_reads * 100.0 / (double)s_total_reads) : 0.0);

    /* On current code, s_soldiers has NO mutex protection at all */
    if (s_torn_reads > 0) {
        fprintf(stderr, "FAIL (EXPECTED ON UNMODIFIED BASELINE): Detected %u torn reads due to unsynchronized s_soldiers access!\n", s_torn_reads);
        return 1;
    } else {
        printf("Note: Even if torn reads did not trigger on this single core/CPU schedule,\n");
        printf("s_soldiers lacks any mutex or critical section in base_sm.c lines 15 & 256.\n");
        return 0;
    }
}
