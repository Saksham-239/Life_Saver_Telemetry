#include "logger.h"
#include <stdio.h>
#include <stdarg.h>

bool logger_init(void) {
    return true;
}

int g_esp_log_quiet = 0;

void logger_append(const char *format, ...) {
    (void)format;
}
