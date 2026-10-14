#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "buzzer.h"

#if defined(_WIN32)
#include <windows.h>
static uint64_t get_time_ms(void) {
    return (uint64_t)GetTickCount64();
}
#else
#include <sys/time.h>
static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}
#endif

int main(void) {
    printf("=== TEST DEF-05: Non-Blocking Buzzer & Bounded UI Loop Period ===\n");

    buzzer_init();

    /* 1. Test buzzer_play execution duration (must return immediately) */
    uint64_t t0 = get_time_ms();
    buzzer_play(BUZZ_EMERGENCY);
    uint64_t dt_emergency = get_time_ms() - t0;

    printf("buzzer_play(BUZZ_EMERGENCY) returned in %llu ms (expected < 20 ms, buggy was 1200 ms)\n",
           (unsigned long long)dt_emergency);

    if (dt_emergency >= 50) {
        fprintf(stderr, "FAIL (EXPECTED ON UNMODIFIED BASELINE): buzzer_play blocked for %llu ms (>= 50 ms)!\n",
                (unsigned long long)dt_emergency);
        return 1;
    }

    /* 2. Simulate 5 UI loop cycles with EMERGENCY active */
    uint64_t loop_start = get_time_ms();
    for (int cycle = 0; cycle < 5; cycle++) {
        uint64_t cycle_t0 = get_time_ms();
        /* Simulated UI tasks */
        buzzer_play(BUZZ_EMERGENCY);
        uint64_t cycle_dt = get_time_ms() - cycle_t0;
        if (cycle_dt >= 50) {
            fprintf(stderr, "FAIL: UI loop cycle %d blocked for %llu ms!\n", cycle, (unsigned long long)cycle_dt);
            return 1;
        }
    }
    uint64_t total_loop_time = get_time_ms() - loop_start;
    printf("5 simulated UI cycles completed in %llu ms (expected < 100 ms)\n",
           (unsigned long long)total_loop_time);

    printf("PASS: Buzzer is non-blocking and UI loop execution period is strictly bounded.\n");
    return 0;
}
