/**
 * base_sm.c — Base station per-soldier state machine implementation.
 */

#include "base_sm.h"
#include "../system/logger.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_sm_mutex = NULL;
static inline void sm_lock(void) {
    if (s_sm_mutex != NULL) {
        xSemaphoreTake(s_sm_mutex, portMAX_DELAY);
    }
}
static inline void sm_unlock(void) {
    if (s_sm_mutex != NULL) {
        xSemaphoreGive(s_sm_mutex);
    }
}
#else
#include <pthread.h>
static pthread_mutex_t s_sm_mutex = PTHREAD_MUTEX_INITIALIZER;
static inline void sm_lock(void) {
    pthread_mutex_lock(&s_sm_mutex);
}
static inline void sm_unlock(void) {
    pthread_mutex_unlock(&s_sm_mutex);
}
#endif

static const char *TAG = "base_sm";

/* Internal table: index = soldier_id - 1 */
static soldier_record_t s_soldiers[SOLDIER_ID_MAX];

void base_sm_init(void) {
#if defined(ESP_PLATFORM)
    if (s_sm_mutex == NULL) {
        s_sm_mutex = xSemaphoreCreateMutex();
    }
#endif
    sm_lock();
    memset(s_soldiers, 0, sizeof(s_soldiers));
    for (uint8_t i = 0; i < (uint8_t)SOLDIER_ID_MAX; i++) {
        s_soldiers[i].soldier_id      = (uint8_t)(i + 1);
        s_soldiers[i].alarm           = ALARM_NORMAL;
        s_soldiers[i].link            = LINK_ONLINE; /* Assumed online until first timeout */
        s_soldiers[i].last_rx_counter = 0;
        s_soldiers[i].has_received    = false;
        s_soldiers[i].base_rx_time_ms = 0;
    }
    sm_unlock();
    ESP_LOGI(TAG, "Base state machine initialised (%d soldier slots)", SOLDIER_ID_MAX);
}

void base_sm_set_rx_counter(uint8_t soldier_id, uint64_t counter) {
    if (soldier_id >= 1 && soldier_id <= SOLDIER_ID_MAX) {
        sm_lock();
        s_soldiers[soldier_id - 1].last_rx_counter = counter;
        s_soldiers[soldier_id - 1].has_received = true;
        sm_unlock();
    }
}

/* 5 minutes = 300,000 ms. Large enough to bury the 2ms OS jitter to < 7 PPM */
#define DRIFT_WINDOW_MS          300000u
/* A sudden shift of 100 PPM over 5 mins indicates crystal or power failure */
#define DRIFT_ANOMALY_THRESH_PPM 100

/**
 * Called under sm_lock() from base_sm_rx_packet().
 * Performs pure arithmetic and state updates with zero I/O or logging.
 */
static void process_hardware_health(soldier_record_t *rec, uint32_t now_ms, uint32_t upt_ms, bool is_new_boot_session) {
    /* 1. Counter-jump session detection & secondary uptime regression check (DEF-11).
     * Primary: Boot counter jump >= 2^31 proves a new flash reservation session (new boot).
     * Secondary: If within same session, uptime stepping backward indicates clock reset/reboot.
     * Long outages past 24.8 days with normal counter progress do NOT trigger a false reboot. */
    if (is_new_boot_session) {
        rec->anchor_valid = false;
        rec->health_initialized = false;
    } else if (rec->last_upt_ms > 0) {
        uint32_t base_elapsed = (rec->base_rx_time_ms > 0) ? (now_ms - rec->base_rx_time_ms) : 0;
        /* If base elapsed time is < 24.8 days, diff < 0 is an unambiguous backward regression.
         * If base elapsed time is >= 24.8 days and counter did not jump, time advanced forward past 2^31 ms */
        if (base_elapsed < 0x80000000u) {
            int32_t diff = (int32_t)(upt_ms - rec->last_upt_ms);
            if (diff < 0) {
                /* Secondary: Backward uptime step detected */
                rec->anchor_valid = false;
                rec->health_initialized = false;
            }
        }
    }
    rec->last_upt_ms = upt_ms;

    /* 2. Initialisation check using explicit flag */
    if (!rec->anchor_valid) {
        rec->anchor_rx_ms = now_ms;
        rec->anchor_upt_ms = upt_ms;
        rec->anchor_valid = true;
        return;
    }

    /* 3. Windowed Drift Computation */
    uint32_t delta_upt = upt_ms - rec->anchor_upt_ms;
    if (delta_upt >= DRIFT_WINDOW_MS) {
        uint32_t delta_base = now_ms - rec->anchor_rx_ms;
        
        /* Positive drift_ms means base is running faster than soldier */
        int32_t drift_ms = (int32_t)delta_base - (int32_t)delta_upt;
        
        /* 64-bit math to prevent overflow: 300,000 ms * 1M fits in int64 */
        int32_t window_ppm = (int32_t)(((int64_t)drift_ms * 1000000LL) / delta_upt);

        if (!rec->health_initialized) {
            rec->ema_drift_ppm = window_ppm;
            rec->health_initialized = true;
            
            if (!rec->long_term_initialized) {
                rec->long_term_ema_ppm = window_ppm;
                rec->long_term_initialized = true;
            } else {
                rec->long_term_ema_ppm = ((rec->long_term_ema_ppm * 7) + window_ppm) / 8;
            }
        } else {
            int32_t shift_from_session = abs(window_ppm - rec->ema_drift_ppm);
            int32_t shift_from_lt      = abs(window_ppm - rec->long_term_ema_ppm);
            
            if (shift_from_session > DRIFT_ANOMALY_THRESH_PPM || shift_from_lt > DRIFT_ANOMALY_THRESH_PPM) {
                rec->hw_anomaly_flag = true;
            } else {
                rec->ema_drift_ppm = ((rec->ema_drift_ppm * 3) + window_ppm) / 4;
            }
        }

        /* Slide the window forward */
        rec->anchor_rx_ms = now_ms;
        rec->anchor_upt_ms = upt_ms;
    }
}

typedef enum {
    LOG_NONE = 0,
    LOG_ALERT_LATCHED,
    LOG_ALERT_IGNORED_EMERGENCY,
    LOG_ALERT_CLEARED,
    LOG_EMERGENCY_SET,
    LOG_FALL_PENDING,
    LOG_FALL_CANCELLED,
    LOG_EMERGENCY_CLEARED,
    LOG_EMERGENCY_CLEAR_MISMATCH,
    LOG_UNKNOWN_MSG
} sm_log_action_t;

void base_sm_rx_packet(const packet_t *pkt, uint32_t base_rx_ms) {
    uint8_t id = pkt->hdr.soldier_id;
    if (id < 1 || id > SOLDIER_ID_MAX) {
        ESP_LOGW(TAG, "Received packet with invalid soldier_id=%d — drop", id);
        return;
    }

    msg_type_t mt = (msg_type_t)pkt->body.msg_type;
    uint16_t event_id = pkt->body.event_id;
    sm_log_action_t log_action = LOG_NONE;
    uint8_t batt_pct = 0;
    uint8_t lq = 0;
    uint32_t upt_s = 0;

    sm_lock();
    soldier_record_t *rec = &s_soldiers[id - 1];

    /* ── B1. Replay detection (MISS-02 fix using has_received flag) ─────────── */
    if (rec->has_received &&
        pkt->hdr.persistent_tx_counter <= rec->last_rx_counter) {
        uint64_t last_ctr = rec->last_rx_counter;
        sm_unlock();
        (void)last_ctr;
        ESP_LOGW(TAG, "Soldier %d: REPLAY DETECTED (rx ctr %llu <= last %llu) — drop",
                 id, (unsigned long long)pkt->hdr.persistent_tx_counter,
                 (unsigned long long)last_ctr);
        return;
    }

    bool is_new_boot_session = false;
    if (rec->has_received) {
        uint64_t ctr_jump = pkt->hdr.persistent_tx_counter - rec->last_rx_counter;
        if (ctr_jump >= (1ULL << 31)) {
            is_new_boot_session = true;
        }
    }

    rec->has_received = true;
    rec->last_rx_counter = pkt->hdr.persistent_tx_counter;

    /* ── Hardware Health Monitoring (DEF-11 fix) ───────────────────────────── */
    process_hardware_health(rec, base_rx_ms, pkt->body.sender_uptime_ms, is_new_boot_session);

    /* ── Update diagnostics ─────────────────────────────────────────────────── */
    rec->battery_pct      = pkt->body.battery_pct;
    rec->link_quality     = pkt->body.prev_tx_retries;
    rec->base_rx_time_ms  = base_rx_ms;
    rec->sender_uptime_ms = pkt->body.sender_uptime_ms;
    rec->last_msg_type    = pkt->body.msg_type;
    rec->link             = LINK_ONLINE;

    batt_pct = rec->battery_pct;
    lq = rec->link_quality;
    upt_s = rec->sender_uptime_ms / 1000;

    /* ── B6. Heartbeat: link & diagnostics update only ──────────────────────── */
    if (mt == MSG_HEARTBEAT) {
        sm_unlock();
        (void)batt_pct; (void)lq; (void)upt_s;
        ESP_LOGD(TAG, "Soldier %d HB: bat=%d%% lq=%d upt=%us", id, batt_pct, lq, (unsigned int)upt_s);
        return;
    }

    /* ── Alarm state transitions ────────────────────────────────────────────── */
    switch (mt) {
    case MSG_BACKUP:
    case MSG_ENEMY:
    case MSG_MEDICAL:
        if (rec->alarm != ALARM_EMERGENCY) {
            rec->alarm           = ALARM_ALERT_LATCHED;
            rec->active_event_id = event_id;
            log_action = LOG_ALERT_LATCHED;
        } else {
            log_action = LOG_ALERT_IGNORED_EMERGENCY;
        }
        break;

    case MSG_COMPLETE:
        /* B4: clears ALERT_LATCHED only */
        if (rec->alarm == ALARM_ALERT_LATCHED) {
            rec->alarm = ALARM_NORMAL;
            rec->active_event_id = event_id;
            log_action = LOG_ALERT_CLEARED;
        }
        break;

    case MSG_DOWN:
        /* B3: set EMERGENCY directly */
        rec->alarm           = ALARM_EMERGENCY;
        rec->active_event_id = event_id;
        log_action = LOG_EMERGENCY_SET;
        break;

    case MSG_FALL_PENDING:
        if (rec->alarm == ALARM_NORMAL) {
            rec->alarm           = ALARM_FALL_PENDING;
            rec->active_event_id = event_id;
            log_action = LOG_FALL_PENDING;
        }
        break;

    case MSG_FALL_CANCEL:
        if (rec->alarm == ALARM_FALL_PENDING) {
            rec->alarm = ALARM_NORMAL;
            log_action = LOG_FALL_CANCELLED;
        }
        break;

    case MSG_EMERGENCY_CLEAR:
        /* B3: validate event_id before clearing EMERGENCY */
        if (rec->alarm == ALARM_EMERGENCY) {
            if (event_id == rec->active_event_id) {
                rec->alarm = ALARM_NORMAL;
                log_action = LOG_EMERGENCY_CLEARED;
            } else {
                log_action = LOG_EMERGENCY_CLEAR_MISMATCH;
            }
        }
        break;

    default:
        log_action = LOG_UNKNOWN_MSG;
        break;
    }

    sm_unlock();

    /* ── Logging executed outside critical section ─────────────────────────── */
    switch (log_action) {
    case LOG_ALERT_LATCHED:
        ESP_LOGI(TAG, "Soldier %d: ALERT_LATCHED (msg=%d event=%u)", id, mt, event_id);
        logger_append("[SID:%d] ALERT_LATCHED (msg=%d event=%u)", id, mt, event_id);
        break;
    case LOG_ALERT_IGNORED_EMERGENCY:
        ESP_LOGW(TAG, "Soldier %d: alert msg=%d ignored — EMERGENCY active", id, mt);
        break;
    case LOG_ALERT_CLEARED:
        ESP_LOGI(TAG, "Soldier %d: ALERT cleared (MSG_COMPLETE event=%u)", id, event_id);
        logger_append("[SID:%d] ALERT cleared (event=%u)", id, event_id);
        break;
    case LOG_EMERGENCY_SET:
        ESP_LOGE(TAG, "Soldier %d: *** EMERGENCY — SOLDIER DOWN *** (event=%u)", id, event_id);
        logger_append("[SID:%d] *** EMERGENCY - SOLDIER DOWN *** (event=%u)", id, event_id);
        break;
    case LOG_FALL_PENDING:
        ESP_LOGW(TAG, "Soldier %d: FALL_PENDING — 10s confirmation window (event=%u)", id, event_id);
        logger_append("[SID:%d] FALL_PENDING (event=%u)", id, event_id);
        break;
    case LOG_FALL_CANCELLED:
        ESP_LOGI(TAG, "Soldier %d: Fall CANCELLED (event=%u)", id, event_id);
        logger_append("[SID:%d] Fall CANCELLED (event=%u)", id, event_id);
        break;
    case LOG_EMERGENCY_CLEARED:
        ESP_LOGI(TAG, "Soldier %d: EMERGENCY CLEARED (event=%u)", id, event_id);
        logger_append("[SID:%d] EMERGENCY CLEARED (event=%u)", id, event_id);
        break;
    case LOG_EMERGENCY_CLEAR_MISMATCH:
        ESP_LOGW(TAG, "Soldier %d: EMERGENCY_CLEAR event_id mismatch (%u) — drop", id, event_id);
        break;
    default:
        break;
    }
}

void base_sm_tick(uint32_t now_ms) {
    uint8_t lost_mask = 0;
    uint32_t lost_elapsed[SOLDIER_ID_MAX] = {0};

    sm_lock();
    for (uint8_t i = 0; i < (uint8_t)SOLDIER_ID_MAX; i++) {
        soldier_record_t *rec = &s_soldiers[i];
        if (rec->base_rx_time_ms == 0) continue;

        uint32_t since_last_rx = now_ms - rec->base_rx_time_ms;
        if (since_last_rx > LINK_TIMEOUT_MS && rec->link == LINK_ONLINE) {
            rec->link = LINK_LOST;
            lost_mask |= (uint8_t)(1u << i);
            lost_elapsed[i] = since_last_rx;
        }
    }
    sm_unlock();

    for (uint8_t i = 0; i < (uint8_t)SOLDIER_ID_MAX; i++) {
        if (lost_mask & (1u << i)) {
            uint8_t sid = (uint8_t)(i + 1);
            ESP_LOGW(TAG, "Soldier %d: LINK_LOST (no packet for %u ms)",
                     sid, (unsigned int)lost_elapsed[i]);
            logger_append("[SID:%d] LINK_LOST (no packet for %u ms)", sid, (unsigned int)lost_elapsed[i]);
        }
    }
}

void base_sm_get_state(soldier_record_t out[SOLDIER_ID_MAX]) {
    sm_lock();
    memcpy(out, s_soldiers, sizeof(s_soldiers));
    sm_unlock();
}

bool base_sm_get_soldier(uint8_t soldier_id, soldier_record_t *out) {
    if (soldier_id < 1 || soldier_id > SOLDIER_ID_MAX || out == NULL) {
        return false;
    }
    sm_lock();
    *out = s_soldiers[soldier_id - 1];
    sm_unlock();
    return true;
}

const soldier_record_t *base_sm_get_soldier_ptr(uint8_t soldier_id) {
    if (soldier_id < 1 || soldier_id > SOLDIER_ID_MAX) {
        return NULL;
    }
    return &s_soldiers[soldier_id - 1];
}
