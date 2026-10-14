#ifndef PICO_TIME_H
#define PICO_TIME_H

#include <stdint.h>

typedef uint64_t absolute_time_t;
extern uint32_t g_sim_now_ms;

static inline absolute_time_t get_absolute_time(void) {
    return (absolute_time_t)g_sim_now_ms;
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
    return (uint32_t)t;
}

#endif /* PICO_TIME_H */
