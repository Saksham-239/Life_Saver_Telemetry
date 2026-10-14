/**
 * vibration.h / vibration.c — ERM vibration motor driver (GP14).
 *
 * Circuit: GP14 → 1 kΩ base resistor → 2N2222 NPN transistor
 *          Collector → ERM motor (+) → 3.3 V rail
 *          Emitter → GND
 *          Flyback diode 1N4148 across motor terminals (cathode to +)
 *
 * GP14 HIGH = transistor on = motor runs.
 * ERM (Eccentric Rotating Mass) motor — DC switched, no special driver needed.
 * Do NOT substitute an LRA motor with this circuit.
 */

#ifndef VIBRATION_H
#define VIBRATION_H
#include <stdint.h>
void vibration_init(void);
void vibration_on(void);
void vibration_off(void);
/** Buzz for duration_ms then stop.  Blocking. */
void vibration_pulse(uint32_t duration_ms);
/** 3 short pulses — fall-pending confirmation cue. */
void vibration_fall_pending_cue(void);
#endif
