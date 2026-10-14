/**
 * buzzer.h/c — Active buzzer driver for the ESP32-S3 base station.
 *
 * GPIO32 → 100Ω → base of NPN (2N2222) → buzzer → 3.3V
 * Active buzzer (DC-driven, internal oscillator): GPIO HIGH = beep.
 *
 * Non-blocking architecture (DEF-05 remediation):
 * Patterns are enqueued asynchronously to a background task so task_ui is NEVER
 * blocked by multi-tone sleep intervals.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "board_pins.h"
#else
#define BUZZER_GPIO 32
#endif

typedef enum {
    BUZZ_NONE = 0,
    BUZZ_ALERT,
    BUZZ_EMERGENCY,
    BUZZ_LINK_LOST,
} buzz_pattern_t;

/** Initialize the buzzer GPIO and spawn the asynchronous FreeRTOS worker task. */
void buzzer_init(void);

/**
 * Play a pattern asynchronously via an internal command queue.
 * Non-blocking: returns immediately (< 1 ms).
 */
void buzzer_play(buzz_pattern_t pattern);

/** Returns true if buzzer is currently playing a pattern. */
bool buzzer_is_active(void);
