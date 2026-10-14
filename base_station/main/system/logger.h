#pragma once
#include <stdbool.h>

/**
 * Initialize the LittleFS partition for logging.
 * Returns true on success.
 */
bool logger_init(void);

/**
 * Append a log entry to the current log file.
 * Automatically rotates the file if it exceeds the maximum size.
 */
void logger_append(const char *format, ...);

/**
 * Dump the contents of all log files to the serial console.
 */
void logger_dump(void);

/**
 * Clear all log files.
 */
void logger_clear(void);
