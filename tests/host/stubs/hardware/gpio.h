#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#define GPIO_FUNC_I2C 3
#define GPIO_IN       0
#define GPIO_OUT      1

static inline void gpio_set_function(uint32_t gpio, int fn) { (void)gpio; (void)fn; }
static inline void gpio_pull_up(uint32_t gpio) { (void)gpio; }
static inline void gpio_init(uint32_t gpio) { (void)gpio; }
static inline void gpio_set_dir(uint32_t gpio, int dir) { (void)gpio; (void)dir; }
extern uint32_t g_sim_gpio_in;
static inline bool gpio_get(uint32_t gpio) {
    return (g_sim_gpio_in & (1u << gpio)) != 0;
}

#endif /* HARDWARE_GPIO_H */
