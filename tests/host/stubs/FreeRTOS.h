#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef void * QueueHandle_t;
typedef void * TaskHandle_t;
typedef uint32_t TickType_t;
typedef void * SemaphoreHandle_t;
typedef long BaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif /* FREERTOS_H */
