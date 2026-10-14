#include "rtos_tasks.h"
#include "../radio/radio.h"
#include "../features/config_store.h"
#include <stdio.h>
#include "pico/time.h"

extern soldier_config_t g_cfg; /* Exported from main or config_store */

void task_radio(void *pvParameters) {
    printf("[task_radio] Started on Core %d\n", get_core_num());
    
    while (1) {
        radio_tx_req_t req;
        if (xQueueReceive(g_radio_queue, &req, portMAX_DELAY) == pdPASS) {
            
            uint64_t counter;
            if (!config_store_next_counter(&counter)) {
                printf("[task_radio] TX blocked: counter allocation failed (flash error)\n");
                
                /* Notify SM of failure */
                sm_event_msg_t res_ev = { .type = SM_EV_RADIO_RESULT };
                res_ev.data.tx_result.delivered = false;
                res_ev.data.tx_result.retry_count = 0;
                xQueueSendToBack(g_sm_queue, &res_ev, 0);
                continue;
            }

            packet_t pkt = {0};
            pkt.hdr.version               = PROTO_VERSION;
            pkt.hdr.soldier_id            = g_cfg.soldier_id;
            pkt.hdr.persistent_tx_counter = counter;

            pkt.body.msg_type         = (uint8_t)req.msg_type;
            pkt.body.sensor_flags     = req.sensor_flags;
            pkt.body.battery_pct      = req.battery_pct;
            pkt.body.prev_tx_retries  = req.prev_tx_retries;
            pkt.body.sender_uptime_ms = req.sender_uptime_ms;
            pkt.body.event_id         = req.event_id;

            radio_tx_result_t r = radio_send(&pkt, g_cfg.aes_key);
            
            sm_event_msg_t res_ev = { .type = SM_EV_RADIO_RESULT };
            res_ev.data.tx_result.delivered = r.delivered;
            res_ev.data.tx_result.retry_count = r.retry_count;
            xQueueSendToBack(g_sm_queue, &res_ev, 0);
        }
    }
}
