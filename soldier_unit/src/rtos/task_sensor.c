#include "rtos_tasks.h"
#include "../sensors/mpu6050.h"
#include "../sensors/fall_detect.h"
#include "../features/battery.h"
#include <stdio.h>
#include "pico/time.h"

void task_sensor(void *pvParameters) {
    printf("[task_sensor] Started on Core %d\n", get_core_num());
    
    mpu6050_init();
    fall_detector_t fd;
    fall_detect_init(&fd);
    
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(5); /* 200 Hz */
    uint32_t last_batt_ms = 0;
    
    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        
        mpu6050_raw_t data;
        mpu6050_processed_t processed;
        mpu6050_read_raw(&data);
        
        mpu6050_process(&data, &processed);
        uint32_t now_ms = (uint32_t)(to_ms_since_boot(get_absolute_time()));
        fall_event_t event = fall_detect_update(&fd, &data, &processed, now_ms);
        
        if (event == FALL_EVENT_CANDIDATE) {
            sm_event_msg_t ev = { .type = SM_EV_FALL_CANDIDATE };
            xQueueSendToBack(g_sm_queue, &ev, 0);
            fall_detect_reset(&fd);
        }

        /* Sample battery every 1 second */
        if (now_ms - last_batt_ms >= 1000) {
            last_batt_ms = now_ms;
            uint8_t batt = battery_read_pct();
            sm_event_msg_t ev = { .type = SM_EV_BATTERY_UPDATE, .data.battery_pct = batt };
            xQueueSendToBack(g_sm_queue, &ev, 0);
        }
    }
}
