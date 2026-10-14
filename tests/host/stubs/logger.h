#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>

bool logger_init(void);
void logger_append(const char *format, ...);

#endif /* LOGGER_H */
