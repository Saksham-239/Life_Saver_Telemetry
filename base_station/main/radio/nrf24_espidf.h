/* nrf24_espidf.h — radio.h re-export for base station (ESP32-S3).
 * The implementation is in nrf24_espidf.c; the interface is radio.h. */
#pragma once
#include "../../../shared/include/packet_protocol.h"
/* See soldier_unit/src/radio/radio.h for the full API documentation.
 * This header exists so CMake can include_directories on the base station
 * without referencing the soldier_unit tree. */
#include "radio_bs.h"
