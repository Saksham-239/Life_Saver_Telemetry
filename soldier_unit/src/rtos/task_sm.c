#include "rtos_tasks.h"
#include <stdio.h>
#include "pico/time.h"
#include "../features/soldier_sm.h"

/* Sole owner of the state machine */
static soldier_sm_t s_sm;

void task_sm(void *pvParameters) {
    printf("[task_sm] Started on Core %d\n", get_core_num());
    
    soldier_sm_init(&s_sm);
    
    while (1) {
        sm_event_msg_t ev;
        /* Bounded wait: 20ms */
        if (xQueueReceive(g_sm_queue, &ev, pdMS_TO_TICKS(20)) != pdPASS) {
            ev.type = SM_EV_TICK;
        }

        uint32_t now_ms = (uint32_t)(to_ms_since_boot(get_absolute_time()));
        radio_tx_req_t tx_req;
        
        bool send = soldier_sm_process_event(&s_sm, &ev, now_ms, &tx_req);
        
        if (send) {
            /* Send TX request to Radio task */
            if (xQueueSendToBack(g_radio_queue, &tx_req, 0) != pdPASS) {
                printf("[task_sm] WARNING: Radio queue full!\n");
            }
        }
        
        /* Evaluate vibration needs */
        alarm_state_t alarm = soldier_sm_alarm(&s_sm);
        vibe_pattern_t p = VIBE_PATTERN_NONE;
        if (alarm == ALARM_EMERGENCY) p = VIBE_PATTERN_EMERGENCY;
        else if (alarm == ALARM_FALL_PENDING || alarm == ALARM_ALERT_LATCHED) p = VIBE_PATTERN_PENDING;
        
        /* Overwrite the vibration queue to ensure it reflects current state */
        xQueueOverwrite(g_vibe_queue, &p);
    }
}
