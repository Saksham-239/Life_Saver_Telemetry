#include "logger.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#else
#include <pthread.h>
#include "esp_log.h"
#include "esp_timer.h"
#include <unistd.h>
#endif

static const char *TAG = "logger";

#define LOG_MAX_SIZE (15 * 1024) /* 15 KB max per file, approx 250 entries (500 total) */
#define LOG_FILE_CURRENT "/lfs/log.txt"
#define LOG_FILE_BACKUP  "/lfs/log.1.txt"

#if defined(ESP_PLATFORM)
static SemaphoreHandle_t s_log_mutex = NULL;

static inline void log_lock(void) {
    if (s_log_mutex != NULL) {
        xSemaphoreTakeRecursive(s_log_mutex, portMAX_DELAY);
    }
}

static inline void log_unlock(void) {
    if (s_log_mutex != NULL) {
        xSemaphoreGiveRecursive(s_log_mutex);
    }
}
#else
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline void log_lock(void) {
    pthread_mutex_lock(&s_log_mutex);
}

static inline void log_unlock(void) {
    pthread_mutex_unlock(&s_log_mutex);
}
#endif

bool logger_init(void) {
#if defined(ESP_PLATFORM)
    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateRecursiveMutex();
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/lfs",
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        return false;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted. Total: %d, Used: %d", (int)total, (int)used);
    return true;
#else
    return true;
#endif
}

static void rotate_logs(void) {
    struct stat st;
    if (stat(LOG_FILE_CURRENT, &st) == 0) {
        if (st.st_size >= LOG_MAX_SIZE) {
            ESP_LOGI(TAG, "Rotating logs (size=%ld)", (long)st.st_size);
            unlink(LOG_FILE_BACKUP);
            rename(LOG_FILE_CURRENT, LOG_FILE_BACKUP);
        }
    }
}

void logger_append(const char *format, ...) {
    log_lock();
    rotate_logs();

    FILE *f = fopen(LOG_FILE_CURRENT, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open log file for appending");
        log_unlock();
        return;
    }

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    fprintf(f, "[%lu] ", (unsigned long)uptime_s);

    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
    log_unlock();
}

void logger_dump(void) {
    log_lock();
    ESP_LOGI(TAG, "--- LOG DUMP START ---");
    
    /* Dump backup log if it exists */
    FILE *f1 = fopen(LOG_FILE_BACKUP, "r");
    if (f1) {
        char line[256];
        while (fgets(line, sizeof(line), f1)) {
            printf("%s", line);
        }
        fclose(f1);
    }

    /* Dump current log */
    FILE *f2 = fopen(LOG_FILE_CURRENT, "r");
    if (f2) {
        char line[256];
        while (fgets(line, sizeof(line), f2)) {
            printf("%s", line);
        }
        fclose(f2);
    }
    
    ESP_LOGI(TAG, "--- LOG DUMP END ---");
    log_unlock();
}

void logger_clear(void) {
    log_lock();
    unlink(LOG_FILE_CURRENT);
    unlink(LOG_FILE_BACKUP);
    ESP_LOGI(TAG, "Logs cleared");
    log_unlock();
}
