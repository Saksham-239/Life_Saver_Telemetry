/**
 * battery.h
 */
#ifndef BATTERY_H
#define BATTERY_H
#include <stdint.h>
#include <stdbool.h>
void    battery_init(void);
uint8_t battery_read_pct(void);       /* Triggers an ADC read and returns 0–100 */
uint8_t battery_get_cached_pct(void); /* Returns last read value without ADC    */
bool    battery_is_low(void);         /* true if cached_pct <= LOW_BATTERY_PCT  */
#endif
