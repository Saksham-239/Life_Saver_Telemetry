#include "vibration.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#define VIB_PIN 14u

void vibration_init(void) {
    gpio_init(VIB_PIN);
    gpio_set_dir(VIB_PIN, GPIO_OUT);
    gpio_put(VIB_PIN, 0);
}

void vibration_on(void)  { gpio_put(VIB_PIN, 1); }
void vibration_off(void) { gpio_put(VIB_PIN, 0); }

void vibration_pulse(uint32_t duration_ms) {
    vibration_on();
    sleep_ms(duration_ms);
    vibration_off();
}

void vibration_fall_pending_cue(void) {
    /* 3 × 200 ms on, 150 ms off */
    for (int i = 0; i < 3; i++) {
        vibration_pulse(200);
        sleep_ms(150);
    }
}
