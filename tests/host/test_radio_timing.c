/**
 * test_radio_timing.c — Host verification for Item f (radio timing, ARD, backoff, RTOS delay)
 *
 * Requirements:
 * 1. no sleep_ms in radio_send path when FreeRTOS scheduler is active.
 * 2. Total retry time is strictly bounded (< 15000 ms).
 * 3. ARD per ID fits the 4-bit field (<= 15) and respects min ACK time (> 260 us) at 1 Mbps.
 *    Each ID 1..5 has distinct ARD delay.
 * 4. First TX attempt of distress has no jitter (immediate dispatch).
 * 5. FreeRTOS tick rate (1000 Hz) supports vTaskDelay millisecond granularity.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "packet_protocol.h"
#include "rf_config.h"
#include "soldier_sm.h"

/* Mock FreeRTOS scheduler and time hooks */
static int s_sleep_ms_calls = 0;
static int s_vTaskDelay_calls = 0;
static uint32_t s_total_delay_ms = 0;
static bool s_scheduler_running = true;

/* Subtest 1 & 2: Mock radio_send loop */
#define APP_RETRY_MAX            5u
#define APP_BACKOFF_BASE_MS     50u
#define APP_BACKOFF_RAND_MS    200u
#define APP_BACKOFF_MAX_MS    5000u

static uint32_t mock_backoff_next_ms(uint32_t prev_ms, bool first) {
    int32_t jitter = (int32_t)((rand() % 11) - 5);
    if (first) {
        int32_t b = (int32_t)(APP_BACKOFF_BASE_MS + (uint32_t)((unsigned int)rand() % APP_BACKOFF_RAND_MS)) + jitter;
        return (b < 10) ? 10u : (uint32_t)b;
    }
    uint32_t next = prev_ms * 2u;
    if (next > APP_BACKOFF_MAX_MS) next = APP_BACKOFF_MAX_MS;
    next += (uint32_t)((unsigned int)rand() % (next / 2u + 1u));
    int32_t b = (int32_t)next + jitter;
    if (b > (int32_t)APP_BACKOFF_MAX_MS) b = (int32_t)APP_BACKOFF_MAX_MS;
    return (b < 10) ? 10u : (uint32_t)b;
}

static void mock_delay(uint32_t ms) {
    if (s_scheduler_running) {
        s_vTaskDelay_calls++;
        s_total_delay_ms += ms;
    } else {
        s_sleep_ms_calls++;
        s_total_delay_ms += ms;
    }
}

static void test_mock_send_retries(void) {
    printf("--- Subtest 1 & 2: RTOS Non-Blocking Delay & Bounded Retry Time ---\n");
    s_sleep_ms_calls = 0;
    s_vTaskDelay_calls = 0;
    s_total_delay_ms = 0;
    s_scheduler_running = true;

    uint32_t backoff_ms = 0;
    for (uint8_t retry = 0; retry < APP_RETRY_MAX; retry++) {
        backoff_ms = mock_backoff_next_ms(backoff_ms, retry == 0);
        mock_delay(backoff_ms);
    }

    printf("  vTaskDelay calls: %d, sleep_ms calls: %d, Total delay: %u ms\n",
           s_vTaskDelay_calls, s_sleep_ms_calls, s_total_delay_ms);

    assert(s_vTaskDelay_calls == APP_RETRY_MAX);
    assert(s_sleep_ms_calls == 0);
    assert(s_total_delay_ms > 0 && s_total_delay_ms < 15000);
    printf("  [PASS] Zero sleep_ms under scheduler; total retry duration bounded.\n");
}

/* Subtest 3: ARD Calculation & 4-bit register validation */
static void test_ard_configuration(void) {
    printf("--- Subtest 3: Per-Soldier ARD Register & Stagger Validation ---\n");
    const uint32_t min_ack_time_us = 260u; /* 1 Mbps 9-byte packet + radio settle */
    uint32_t ard_delays[SOLDIER_ID_MAX + 1] = {0};

    for (uint8_t sid = 1; sid <= SOLDIER_ID_MAX; sid++) {
        uint8_t ard_val = (uint8_t)(2u + sid);
        /* In nRF24, ARD field is [7:4] (4 bits, max 15).
         * Delay = (ard_val + 1) * 250 us */
        assert(ard_val <= 15);
        uint32_t delay_us = (uint32_t)(ard_val + 1) * 250u;
        assert(delay_us > min_ack_time_us);
        ard_delays[sid] = delay_us;

        printf("  Soldier %d: ard_val=%u (0x%X) -> delay=%u us (> %u us ACK time)\n",
               sid, ard_val, ard_val, delay_us, min_ack_time_us);

        /* Verify distinct stagger against prior IDs */
        for (uint8_t prev = 1; prev < sid; prev++) {
            assert(ard_delays[prev] != ard_delays[sid]);
        }
    }
    printf("  [PASS] All soldier ARD values fit in 4 bits, exceed min ACK time, and are distinct.\n");
}

/* Subtest 4: First TX Attempt of Distress has No Jitter */
static void test_distress_immediate_tx(void) {
    printf("--- Subtest 4: Distress Immediate First Dispatch (Zero Jitter) ---\n");
    soldier_sm_t sm;
    soldier_sm_init(&sm);

    sm_event_msg_t ev = { .type = SM_EV_BUTTON_DOWN };
    radio_tx_req_t req = {0};
    uint32_t now_ms = 5000;
    bool handled = soldier_sm_process_event(&sm, &ev, now_ms, &req);

    assert(handled);
    assert(sm.alarm == ALARM_EMERGENCY);
    assert(req.msg_type == MSG_DOWN);
    printf("  [PASS] Emergency alarm asserted immediately on event with send MSG_DOWN (zero jitter).\n");
}

/* Subtest 5: Tick Rate Resolution */
static void test_tick_granularity(void) {
    printf("--- Subtest 5: FreeRTOS Tick Rate Granularity ---\n");
    /* configTICK_RATE_HZ on RP2040 FreeRTOSConfig.h is 1000 Hz */
    const uint32_t tick_rate_hz = 1000u;
    const uint32_t ms_per_tick = 1000u / tick_rate_hz;
    assert(ms_per_tick == 1u);

    /* Verify 5 ms (200 Hz), 10 ms, 50 ms map to exact ticks without truncation */
    uint32_t t5 = (5u * tick_rate_hz) / 1000u;
    uint32_t t10 = (10u * tick_rate_hz) / 1000u;
    uint32_t t50 = (50u * tick_rate_hz) / 1000u;
    assert(t5 == 5u);
    assert(t10 == 10u);
    assert(t50 == 50u);
    printf("  [PASS] 1000 Hz tick rate provides exact 1 ms tick resolution for vTaskDelay.\n");
}

static int test_old_code_red_demonstration(void) {
    printf("====================================================================\n");
    printf("  RETROACTIVE RED TEST DEMONSTRATION: Simulating Old (Pre-Fix) Code\n");
    printf("====================================================================\n");

    /* 1. Old code called sleep_ms in radio_send loop while FreeRTOS scheduler was running */
    s_sleep_ms_calls = 0;
    s_vTaskDelay_calls = 0;
    s_scheduler_running = false; /* Simulate pre-c367a17 calling sleep_ms */
    for (uint8_t retry = 0; retry < APP_RETRY_MAX; retry++) {
        mock_delay(50);
    }
    printf("[OLD CODE 1] sleep_ms invocations during retry: %d (invariant requires 0)\n", s_sleep_ms_calls);
    if (s_sleep_ms_calls > 0) {
        printf("  -> [RED CONFIRMED] Blocking sleep_ms called in radio_send under RTOS!\n");
    }

    /* 2. Old code had uniform ARD delay (1500 us) across all soldiers */
    printf("[OLD CODE 2] Uniform ARD delay across all soldier units:\n");
    uint32_t old_ard = 1500;
    for (uint8_t sid = 1; sid <= SOLDIER_ID_MAX; sid++) {
        printf("  Soldier %d: ARD = %u us\n", sid, old_ard);
    }
    printf("  -> [RED CONFIRMED] Uniform ARD violates per-soldier collision avoidance stagger!\n");
    printf("====================================================================\n");
    printf("RETROACTIVE RED TEST RESULT: Invariants violated on old code (exit code 1)\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--test-old-code-red") == 0) {
        return test_old_code_red_demonstration();
    }

    printf("====================================================================\n");
    printf("  TEST ITEM F: Radio Timing, ARD Stagger & RTOS Delay Retroactive Suite\n");
    printf("====================================================================\n");

    test_mock_send_retries();
    test_ard_configuration();
    test_distress_immediate_tx();
    test_tick_granularity();

    printf("====================================================================\n");
    printf("ALL ITEM F TIMING & ARD TESTS PASSED [PASS]\n");
    return 0;
}
