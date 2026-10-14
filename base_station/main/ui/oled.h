/**
 * oled.h — SSD1306 128×64 OLED display driver for the ESP32-S3 base station.
 *
 * I2C1 on GPIO21 (SDA) / GPIO22 (SCL), address 0x3C.
 * Display layout (6 rows of text on a 64px tall display, 8 pages available):
 *
 *   Row 0 (Page 0): Header bar  "BASE B+HH:MM:SS"
 *   Row 1 (Page 1): Soldier 1   "[1] NORMAL   B:95% LQ:0"
 *   Row 2 (Page 2): Soldier 2   "[2] EMERGENCY   B:30% LQ:15"  ← inverted video
 *   Row 3 (Page 3): Soldier 3   "[3] PENDING  B:72% LQ:3 !"    ← blink indicator
 *   Row 4 (Page 4): Soldier 4   "[4] OFFLINE  B:---  LINK LOST"
 *   Row 5 (Page 5): Soldier 5   "[5] NORMAL   B:88% LQ:1"
 *
 * Severity encoding (text-only, no colour — per §4 spec decision):
 *   ALARM_NORMAL         — plain text
 *   ALARM_FALL_PENDING   — trailing "!" blink (alternates every 500 ms)
 *   ALARM_ALERT_LATCHED  — row text bold prefix "** "
 *   ALARM_EMERGENCY      — full row inverted (white on black → black on white)
 *   LINK_LOST            — append " [NO LINK]"
 *
 * Font: minimal 6×8 ASCII font (printable chars 0x20–0x7E).
 * The font table is included inline in oled.c.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../../../shared/include/packet_protocol.h"

/** Initialise I2C and SSD1306. Returns false if device doesn't ACK. */
bool oled_init(void);

/** Clear the entire display. */
void oled_clear(void);

/**
 * Render the full status screen for all tracked soldiers.
 * Call from the main display-update loop (e.g. every 500 ms).
 * @param soldiers   Array of SOLDIER_ID_MAX soldier_record_t structs.
 * @param base_uptime_ms  Base station uptime in ms (for header timestamp).
 * @param blink_on   true/false alternating every 500 ms for blink indicator.
 */
void oled_render_status(const soldier_record_t soldiers[SOLDIER_ID_MAX],
                        uint32_t base_uptime_ms,
                        bool blink_on);
