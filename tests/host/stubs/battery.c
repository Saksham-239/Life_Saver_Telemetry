#include "battery.h"

void battery_init(void) {}
uint8_t battery_read_pct(void) { return 85; }
uint8_t battery_get_cached_pct(void) { return 85; }
bool battery_is_low(void) { return false; }
