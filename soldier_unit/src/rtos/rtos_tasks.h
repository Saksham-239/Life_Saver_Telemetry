#ifndef RTOS_TASKS_H
#define RTOS_TASKS_H

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "../../shared/include/packet_protocol.h"

/* ── Queue Items ─────────────────────────────────────────────────────────── */

typedef enum {
    SM_EV_NONE = 0,
    SM_EV_TICK,             /* Periodic bounded wait tick */
    SM_EV_FALL_CANDIDATE,
    SM_EV_BUTTON_CLEAR_EMERGENCY, /* Long press COMPLETE — clears EMERGENCY */
    SM_EV_BUTTON_CANCEL,    /* Double press COMPLETE */
    SM_EV_BUTTON_BACKUP,    /* Short press BACKUP */
    SM_EV_BUTTON_ENEMY,     /* Short press ENEMY */
    SM_EV_BUTTON_MEDICAL,   /* Short press MEDICAL */
    SM_EV_BUTTON_COMPLETE,  /* Short press COMPLETE */
    SM_EV_BUTTON_DOWN,      /* Short press DOWN */
    SM_EV_BATTERY_UPDATE,
    SM_EV_RADIO_RESULT      /* TX completed by radio task */
} sm_event_type_t;

typedef struct {
    sm_event_type_t type;
    union {
        uint8_t battery_pct;
        struct {
            bool delivered;
            uint8_t retry_count;
        } tx_result;
    } data;
} sm_event_msg_t;

typedef struct {
    msg_type_t msg_type;
    uint8_t    sensor_flags;
    uint8_t    battery_pct;
    uint8_t    prev_tx_retries;
    uint32_t   sender_uptime_ms;
    uint16_t   event_id;
} radio_tx_req_t;

typedef enum {
    VIBE_PATTERN_NONE = 0,
    VIBE_PATTERN_PENDING,
    VIBE_PATTERN_EMERGENCY,
    VIBE_PATTERN_ACK_OK
} vibe_pattern_t;

/* ── Global Queues ───────────────────────────────────────────────────────── */
extern QueueHandle_t g_sm_queue;
extern QueueHandle_t g_radio_queue;
extern QueueHandle_t g_vibe_queue;

/* ── Task Entries ────────────────────────────────────────────────────────── */
void task_sensor(void *pvParameters);
void task_button(void *pvParameters);
void task_sm(void *pvParameters);
void task_radio(void *pvParameters);
void task_vibration(void *pvParameters);

#endif /* RTOS_TASKS_H */
