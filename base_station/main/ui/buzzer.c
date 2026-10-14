#include "buzzer.h"
#include <stdio.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "buzzer";
#define BUZZER_QUEUE_LEN 4

static QueueHandle_t s_buzzer_queue = NULL;
static volatile bool s_buzzer_active = false;

static inline void bz_on(void)  { gpio_set_level(BUZZER_GPIO, 1); }
static inline void bz_off(void) { gpio_set_level(BUZZER_GPIO, 0); }

static void task_buzzer(void *arg) {
    (void)arg;
    buzz_pattern_t pattern;
    while (1) {
        if (xQueueReceive(s_buzzer_queue, &pattern, portMAX_DELAY) == pdTRUE) {
            s_buzzer_active = true;
            switch (pattern) {
            case BUZZ_ALERT:
                for (int i = 0; i < 2; i++) {
                    bz_on();  vTaskDelay(pdMS_TO_TICKS(100));
                    bz_off(); vTaskDelay(pdMS_TO_TICKS(100));
                }
                break;
            case BUZZ_EMERGENCY:
                for (int i = 0; i < 5; i++) {
                    bz_on();  vTaskDelay(pdMS_TO_TICKS(80));
                    bz_off(); vTaskDelay(pdMS_TO_TICKS(60));
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case BUZZ_LINK_LOST:
                bz_on();  vTaskDelay(pdMS_TO_TICKS(50));
                bz_off(); vTaskDelay(pdMS_TO_TICKS(950));
                break;
            default:
                break;
            }
            bz_off();
            s_buzzer_active = false;
        }
    }
}

void buzzer_init(void) {
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0);

    if (s_buzzer_queue == NULL) {
        s_buzzer_queue = xQueueCreate(BUZZER_QUEUE_LEN, sizeof(buzz_pattern_t));
        xTaskCreate(task_buzzer, "task_buzzer", 2048, NULL, 3, NULL);
        ESP_LOGI(TAG, "Asynchronous buzzer task initialized (queue len=%d)", BUZZER_QUEUE_LEN);
    }
}

void buzzer_play(buzz_pattern_t pattern) {
    if (s_buzzer_queue != NULL) {
        if (pattern == BUZZ_EMERGENCY) {
            /* EMERGENCY pattern must preempt any queued alerts and must NEVER be dropped */
            if (xQueueSendToFront(s_buzzer_queue, &pattern, 0) != pdTRUE) {
                /* Queue is full of lower-priority alerts: drop oldest item and force EMERGENCY to front */
                buzz_pattern_t dropped;
                xQueueReceive(s_buzzer_queue, &dropped, 0);
                xQueueSendToFront(s_buzzer_queue, &pattern, 0);
            }
        } else {
            /* Routine alerts drop if queue is full to preserve bounded UI loop time */
            xQueueSend(s_buzzer_queue, &pattern, 0);
        }
    }
}

bool buzzer_is_active(void) {
    return s_buzzer_active;
}

#else
/* Host (POSIX / Windows) Simulation */
#include <pthread.h>
#if defined(_WIN32)
#include <windows.h>
#define host_sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define host_sleep_ms(ms) usleep((ms) * 1000)
#endif

static pthread_t s_buzz_thread;
static pthread_mutex_t s_buzz_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_buzz_cond = PTHREAD_COND_INITIALIZER;
static buzz_pattern_t s_current_pattern = BUZZ_NONE;
static volatile bool s_buzzer_active = false;
static volatile bool s_buzz_running = false;

static void *host_buzz_worker(void *arg) {
    (void)arg;
    while (s_buzz_running) {
        pthread_mutex_lock(&s_buzz_mutex);
        while (s_current_pattern == BUZZ_NONE && s_buzz_running) {
            pthread_cond_wait(&s_buzz_cond, &s_buzz_mutex);
        }
        if (!s_buzz_running) {
            pthread_mutex_unlock(&s_buzz_mutex);
            break;
        }
        buzz_pattern_t p = s_current_pattern;
        s_current_pattern = BUZZ_NONE;
        s_buzzer_active = true;
        pthread_mutex_unlock(&s_buzz_mutex);

        switch (p) {
        case BUZZ_ALERT:
            host_sleep_ms(400);
            break;
        case BUZZ_EMERGENCY:
            host_sleep_ms(1200);
            break;
        case BUZZ_LINK_LOST:
            host_sleep_ms(1000);
            break;
        default:
            break;
        }
        s_buzzer_active = false;
    }
    return NULL;
}

void buzzer_init(void) {
    if (!s_buzz_running) {
        s_buzz_running = true;
        pthread_create(&s_buzz_thread, NULL, host_buzz_worker, NULL);
    }
}

void buzzer_play(buzz_pattern_t pattern) {
    pthread_mutex_lock(&s_buzz_mutex);
    if (pattern == BUZZ_EMERGENCY || s_current_pattern != BUZZ_EMERGENCY) {
        s_current_pattern = pattern;
    }
    pthread_cond_signal(&s_buzz_cond);
    pthread_mutex_unlock(&s_buzz_mutex);
}

bool buzzer_is_active(void) {
    return s_buzzer_active;
}
#endif
