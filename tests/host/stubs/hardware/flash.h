#ifndef HARDWARE_FLASH_H
#define HARDWARE_FLASH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FLASH_PAGE_SIZE    256u
#define FLASH_SECTOR_SIZE  4096u
#define FLASH_TOTAL_SIZE   (2u * 1024u * 1024u)

void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);

typedef void (*flash_stub_hook_t)(const char *op, uint32_t offset, size_t count, const uint8_t *data);
void flash_stub_set_hook(flash_stub_hook_t hook);
void flash_stub_init(void);
void flash_stub_corrupt_byte(uint32_t offset, uint8_t val);
uint8_t *flash_stub_get_mem(void);

#endif /* HARDWARE_FLASH_H */
