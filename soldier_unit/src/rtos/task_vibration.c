#include "rtos_tasks.h"
#include "../ui/vibration.h"
#include <stdio.h>
#include "pico/time.h"

void task_vibration(void *pvParameters) {
    printf("[task_vibration] Started on Core %d\n", get_core_num());
    
    vibration_init();
    
    vibe_pattern_t current_pattern = VIBE_PATTERN_NONE;
    int phase = 0;
    
    while (1) {
        vibe_pattern_t next_pattern;
        /* Peek mailbox to see if state changed, don't wait long, we are polling the pattern */
        if (xQueuePeek(g_vibe_queue, &next_pattern, 0) == pdPASS) {
            if (next_pattern != current_pattern) {
                current_pattern = next_pattern;
                phase = 0;
                vibration_off(); /* Reset motor state on transition */
            }
        }
        
        uint32_t delay_ms = 100;
        
        switch (current_pattern) {
            case VIBE_PATTERN_EMERGENCY:
                /* Fast continuous SOS-like pulses */
                if (phase % 2 == 0) {
                    vibration_on();
                    delay_ms = 200;
                } else {
                    vibration_off();
                    delay_ms = 200;
                }
                phase++;
                break;
                
            case VIBE_PATTERN_PENDING:
                /* Slower warning pulses */
                if (phase % 2 == 0) {
                    vibration_on();
                    delay_ms = 500;
                } else {
                    vibration_off();
                    delay_ms = 1000;
                }
                phase++;
                break;
                
            case VIBE_PATTERN_ACK_OK:
                /* Single blip */
                if (phase == 0) {
                    vibration_on();
                    delay_ms = 100;
                    phase++;
                } else {
                    vibration_off();
                    current_pattern = VIBE_PATTERN_NONE; /* Auto reset */
                }
                break;
                
            case VIBE_PATTERN_NONE:
            default:
                vibration_off();
                delay_ms = 100;
                /* Wait until something appears in the queue */
                xQueueReceive(g_vibe_queue, &next_pattern, portMAX_DELAY);
                xQueueSendToBack(g_vibe_queue, &next_pattern, 0); /* Put it back as mailbox */
                continue;
        }
        
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
