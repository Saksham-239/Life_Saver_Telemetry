#include "hardware/flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define FLASH_TOTAL_SIZE (2u * 1024u * 1024u)
#define FLASH_BASE_ADDR  0x10000000u

static uint8_t *s_flash_mem = NULL;
static flash_stub_hook_t s_hook = NULL;

void flash_stub_init(void) {
    if (!s_flash_mem) {
#ifdef _WIN32
        s_flash_mem = (uint8_t *)VirtualAlloc((void *)FLASH_BASE_ADDR, FLASH_TOTAL_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        s_flash_mem = (uint8_t *)mmap((void *)FLASH_BASE_ADDR, FLASH_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
        if (!s_flash_mem) {
            fprintf(stderr, "flash_stub_init: failed to allocate memory at 0x%08X\n", FLASH_BASE_ADDR);
            exit(1);
        }
        memset(s_flash_mem, 0xFF, FLASH_TOTAL_SIZE);
    }
}

uint8_t *flash_stub_get_mem(void) {
    flash_stub_init();
    return s_flash_mem;
}

void flash_stub_set_hook(flash_stub_hook_t hook) {
    s_hook = hook;
}

void flash_range_erase(uint32_t offset, size_t count) {
    flash_stub_init();
    if (s_hook) {
        s_hook("erase", offset, count, NULL);
    }
    if (offset + count <= FLASH_TOTAL_SIZE) {
        memset(s_flash_mem + offset, 0xFF, count);
    }
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    flash_stub_init();
    if (s_hook) {
        s_hook("program", offset, count, data);
    }
    if (offset + count <= FLASH_TOTAL_SIZE) {
        /* NOR flash physics: bits can only be cleared (ANDed) from 1 to 0 */
        for (size_t i = 0; i < count; i++) {
            s_flash_mem[offset + i] &= data[i];
        }
    }
}

void flash_stub_corrupt_byte(uint32_t offset, uint8_t val) {
    flash_stub_init();
    if (offset < FLASH_TOTAL_SIZE) {
        s_flash_mem[offset] = val;
    }
}
