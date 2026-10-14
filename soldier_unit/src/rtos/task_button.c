#include "rtos_tasks.h"
#include "../ui/buttons.h"
#include "pico/time.h"
#include <stdio.h>

void task_button(void *pvParameters) {
    (void)pvParameters;
    printf("[task_button] Started on Core %d\n", get_core_num());
    
    buttons_init();
    
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20);
    
    button_event_t ev;
    
    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        uint32_t now = to_ms_since_boot(get_absolute_time());
        
        while (buttons_tick(now, &ev)) {
            sm_event_msg_t msg = { .type = SM_EV_NONE };
            if (ev.btn == BUTTON_COMPLETE && ev.type == BTN_EV_LONG_PRESS) {
                msg.type = SM_EV_BUTTON_CLEAR_EMERGENCY;
            } else if (ev.btn == BUTTON_COMPLETE && ev.type == BTN_EV_DOUBLE_PRESS) {
                msg.type = SM_EV_BUTTON_CANCEL;
            } else if (ev.type == BTN_EV_SHORT_PRESS) {
                switch (ev.btn) {
                case BUTTON_BACKUP:   msg.type = SM_EV_BUTTON_BACKUP; break;
                case BUTTON_ENEMY:    msg.type = SM_EV_BUTTON_ENEMY; break;
                case BUTTON_MEDICAL:  msg.type = SM_EV_BUTTON_MEDICAL; break;
                case BUTTON_COMPLETE: msg.type = SM_EV_BUTTON_COMPLETE; break;
                case BUTTON_DOWN:     msg.type = SM_EV_BUTTON_DOWN; break;
                default: break;
                }
            }
            if (msg.type != SM_EV_NONE) {
                xQueueSendToBack(g_sm_queue, &msg, 0);
            }
        }
    }
}
