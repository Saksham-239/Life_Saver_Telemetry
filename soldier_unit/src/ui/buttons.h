#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BUTTON_NONE     = -1,
    BUTTON_BACKUP   = 0,   /* GP7  */
    BUTTON_ENEMY    = 1,   /* GP8  */
    BUTTON_MEDICAL  = 2,   /* GP9  */
    BUTTON_COMPLETE = 3,   /* GP10 */
    BUTTON_DOWN     = 4,   /* GP11 */
    BUTTON_COUNT    = 5,
} button_id_t;

typedef enum {
    BTN_EV_NONE = 0,
    BTN_EV_SHORT_PRESS,
    BTN_EV_DOUBLE_PRESS,
    BTN_EV_LONG_PRESS,
} button_event_type_t;

typedef struct {
    button_id_t         btn;
    button_event_type_t type;
} button_event_t;

void buttons_init(void);

typedef bool (*button_gpio_fn_t)(button_id_t btn);
void buttons_set_gpio_fn(button_gpio_fn_t fn);

bool buttons_tick(uint32_t now_ms, button_event_t *ev_out);

button_id_t button_poll(void);
button_id_t button_poll_blocking(void);
bool button_is_held(button_id_t btn, uint32_t hold_ms);
bool button_double_press_detected(void);

#endif /* BUTTONS_H */
