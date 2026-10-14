#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <stdio.h>

extern int g_esp_log_quiet;

#define ESP_LOGI(tag, fmt, ...) do { /* quiet in test by default */ } while (0)
#define ESP_LOGW(tag, fmt, ...) do { /* quiet in test by default */ } while (0)
#define ESP_LOGE(tag, fmt, ...) do { if (!g_esp_log_quiet) fprintf(stderr, "[E][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { /* quiet */ } while (0)

#endif /* ESP_LOG_H */
