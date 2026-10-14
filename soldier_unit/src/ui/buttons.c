#include "buttons.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <string.h>

#define DEBOUNCE_MS             20u
#define DOUBLE_PRESS_WINDOW_MS  400u
#define LONG_PRESS_MS           3000u

static const uint8_t BTN_PINS[BUTTON_COUNT] = { 7, 8, 9, 10, 11 };

typedef enum {
    BTN_FSM_IDLE = 0,
    BTN_FSM_PRESSED,
    BTN_FSM_LONG_HELD,
    BTN_FSM_SECOND_PRESS,
} btn_fsm_state_t;

typedef struct {
    btn_fsm_state_t state;
    bool            debounced_state;
    uint32_t        debounce_timer;
    uint32_t        press_start_ms;
    uint32_t        last_release_ms;
    bool            pending_double;
    bool            double_press_latched;
} btn_tracker_t;

static btn_tracker_t s_btn[BUTTON_COUNT];
static button_gpio_fn_t s_gpio_fn = NULL;

static bool default_raw_pressed(button_id_t btn) {
    return !gpio_get(BTN_PINS[btn]);
}

void buttons_set_gpio_fn(button_gpio_fn_t fn) {
    s_gpio_fn = fn;
}

static inline bool read_raw_btn(button_id_t btn) {
    if (s_gpio_fn) {
        return s_gpio_fn(btn);
    }
    return default_raw_pressed(btn);
}

void buttons_init(void) {
    memset(s_btn, 0, sizeof(s_btn));
    for (int i = 0; i < BUTTON_COUNT; i++) {
        gpio_init(BTN_PINS[i]);
        gpio_set_dir(BTN_PINS[i], GPIO_IN);
        gpio_pull_up(BTN_PINS[i]);
    }
}

bool buttons_tick(uint32_t now_ms, button_event_t *ev_out) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        btn_tracker_t *b = &s_btn[i];
        bool raw = read_raw_btn((button_id_t)i);

        if (raw != b->debounced_state) {
            if (b->debounce_timer == 0) {
                b->debounce_timer = now_ms;
            } else if ((now_ms - b->debounce_timer) >= DEBOUNCE_MS) {
                b->debounced_state = raw;
                b->debounce_timer = 0;
            }
        } else {
            b->debounce_timer = 0;
        }

        switch (b->state) {
        case BTN_FSM_IDLE:
            if (b->debounced_state) {
                b->press_start_ms = now_ms;
                if (b->pending_double) {
                    b->state = BTN_FSM_SECOND_PRESS;
                    b->pending_double = false;
                } else {
                    b->state = BTN_FSM_PRESSED;
                }
            } else if (b->pending_double && ((now_ms - b->last_release_ms) > DOUBLE_PRESS_WINDOW_MS)) {
                b->pending_double = false;
                if (ev_out) {
                    ev_out->btn = (button_id_t)i;
                    ev_out->type = BTN_EV_SHORT_PRESS;
                }
                return true;
            }
            break;

        case BTN_FSM_PRESSED:
            if (b->debounced_state) {
                if ((now_ms - b->press_start_ms) >= LONG_PRESS_MS) {
                    b->state = BTN_FSM_LONG_HELD;
                    b->pending_double = false;
                    if (ev_out) {
                        ev_out->btn = (button_id_t)i;
                        ev_out->type = BTN_EV_LONG_PRESS;
                    }
                    return true;
                }
            } else {
                b->last_release_ms = now_ms;
                b->pending_double = true;
                b->state = BTN_FSM_IDLE;
            }
            break;

        case BTN_FSM_LONG_HELD:
            if (!b->debounced_state) {
                b->state = BTN_FSM_IDLE;
                b->pending_double = false;
            }
            break;

        case BTN_FSM_SECOND_PRESS:
            if (!b->debounced_state) {
                b->state = BTN_FSM_IDLE;
                b->pending_double = false;
                if (i == BUTTON_COMPLETE) {
                    b->double_press_latched = true;
                }
                if (ev_out) {
                    ev_out->btn = (button_id_t)i;
                    ev_out->type = BTN_EV_DOUBLE_PRESS;
                }
                return true;
            }
            break;
        }
    }
    return false;
}

button_id_t button_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    button_event_t ev;
    buttons_tick(now, &ev);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (s_btn[i].debounced_state) {
            return (button_id_t)i;
        }
    }
    return BUTTON_NONE;
}

button_id_t button_poll_blocking(void) {
    button_id_t btn;
    while ((btn = button_poll()) == BUTTON_NONE) {
        sleep_ms(5);
    }
    while (read_raw_btn(btn)) {
        sleep_ms(5);
    }
    return btn;
}

bool button_is_held(button_id_t btn, uint32_t hold_ms) {
    if (btn < 0 || btn >= BUTTON_COUNT) return false;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    button_event_t ev;
    buttons_tick(now, &ev);
    if (!s_btn[btn].debounced_state) return false;
    return (now - s_btn[btn].press_start_ms) >= hold_ms;
}

bool button_double_press_detected(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    button_event_t ev;
    buttons_tick(now, &ev);
    if (s_btn[BUTTON_COMPLETE].double_press_latched) {
        s_btn[BUTTON_COMPLETE].double_press_latched = false;
        return true;
    }
    return false;
}
