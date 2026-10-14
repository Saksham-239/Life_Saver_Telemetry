#ifndef HARDWARE_I2C_H
#define HARDWARE_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define i2c0 ((void*)0)

static inline void i2c_init(void *i2c, uint32_t baud) {
    (void)i2c; (void)baud;
}

static inline int i2c_write_blocking(void *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)src; (void)len; (void)nostop;
    return (int)len;
}

static inline int i2c_read_blocking(void *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop) {
    (void)i2c; (void)addr; (void)dst; (void)len; (void)nostop;
    return (int)len;
}

#endif /* HARDWARE_I2C_H */
