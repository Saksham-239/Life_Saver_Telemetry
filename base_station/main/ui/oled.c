/**
 * oled.c — SSD1306 128×64 OLED driver (ESP-IDF I2C master).
 *
 * Initialisation sequence and page-addressing mode follow:
 *   Solomon Systech SSD1306 Application Note Revision 0, 2008.
 *
 * Text rendering:
 *   6×8 pixel font (5 pixel wide glyph + 1 pixel column gap).
 *   21 characters per row (128 / 6 ≈ 21).
 *   8 text rows available (64 / 8 = 8).  We use 5.
 *
 * Inverted row (ALARM_EMERGENCY):
 *   Rendered with the SSD1306 "Set Normal/Inverse Display" command (0xA6/0xA7)
 *   applied to the whole screen, or by XOR-inverting the row's glyph bytes
 *   before writing.  We use per-row XOR inversion for granularity.
 */

#include "oled.h"
#include "board_pins.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG __attribute__((unused)) = "oled";

/* ── I2C configuration ───────────────────────────────────────────────────── */
#define OLED_I2C_PORT       I2C_NUM_1
#define OLED_I2C_ADDR       0x3Cu
#define OLED_I2C_FREQ_HZ    400000

#define OLED_WIDTH          128u
#define OLED_PAGES          8u
#define OLED_FONT_W         6u
#define OLED_CHARS_PER_ROW  (OLED_WIDTH / OLED_FONT_W)  /* 21 */

/* ── SSD1306 I2C protocol ─────────────────────────────────────────────────── */
/* First data byte after address: 0x00 = command stream, 0x40 = data stream */
#define SSD1306_CMD_STREAM  0x00u
#define SSD1306_DATA_STREAM 0x40u

/* ── Minimal 6×8 ASCII font (space 0x20 through tilde 0x7E) ─────────────── */
/* Each character is 5 bytes (columns); rendered in 6 columns with 1 trailing gap.
 * Source: public-domain 5×7 font adapted for SSD1306 page addressing.      */
static const uint8_t FONT6x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' 0x20 */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x10,0x08,0x08,0x10,0x08}, /* '~' */
};

/* ── Frame buffer ────────────────────────────────────────────────────────── */
static uint8_t s_fb[OLED_PAGES][OLED_WIDTH];
static bool    s_dirty = false;

/* ── I2C helpers ─────────────────────────────────────────────────────────── */

static void ssd1306_cmds(const uint8_t *cmds, size_t len) {
    /* Build buffer: control byte + command bytes */
    uint8_t buf[len + 1];
    buf[0] = SSD1306_CMD_STREAM;
    for (size_t i = 0; i < len; i++) buf[i + 1] = cmds[i];
    i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDR,
                               buf, len + 1, pdMS_TO_TICKS(10));
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool oled_init(void) {
    i2c_config_t conf = {
        .mode          = I2C_MODE_MASTER,
        .sda_io_num    = OLED_SDA_PIN,
        .scl_io_num    = OLED_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = OLED_I2C_FREQ_HZ,
    };
    i2c_param_config(OLED_I2C_PORT, &conf);
    i2c_driver_install(OLED_I2C_PORT, conf.mode, 0, 0, 0);

    /* SSD1306 initialisation sequence (128×64, no COM remap) */
    static const uint8_t init_seq[] = {
        0xAE,       /* Display OFF                          */
        0x20, 0x00, /* Horizontal addressing mode           */
        0xB0,       /* Page start address = 0               */
        0xC8,       /* COM scan direction: remapped         */
        0x00,       /* Lower col start addr nibble          */
        0x10,       /* Upper col start addr nibble          */
        0x40,       /* Display start line = 0               */
        0x81, 0xFF, /* Contrast maximum                     */
        0xA1,       /* Segment remap: column 127 is SEG0   */
        0xA6,       /* Normal display (not inverted)        */
        0xA8, 0x3F, /* MUX ratio 64                        */
        0xA4,       /* Display follows RAM                  */
        0xD3, 0x00, /* Display offset = 0                   */
        0xD5, 0xF0, /* Divide ratio / Oscillator freq       */
        0xD9, 0x22, /* Pre-charge period                    */
        0xDA, 0x12, /* COM pins config: sequential, no remap*/
        0xDB, 0x20, /* VCOMH deselect level                 */
        0x8D, 0x14, /* Charge pump enable                   */
        0xAF,       /* Display ON                           */
    };
    ssd1306_cmds(init_seq, sizeof(init_seq));

    memset(s_fb, 0, sizeof(s_fb));
    ESP_LOGI(TAG, "SSD1306 initialised");
    return true;
}

void oled_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
    s_dirty = true;
}

/* Write frame buffer to display */
static void fb_flush(void) {
    /* Set col and page to (0,0) */
    static const uint8_t addr_cmds[] = {
        0x21, 0x00, 0x7F,   /* Column address: 0–127   */
        0x22, 0x00, 0x07,   /* Page address: 0–7       */
    };
    ssd1306_cmds(addr_cmds, sizeof(addr_cmds));

    /* Write all 1024 bytes as data */
    uint8_t buf[OLED_WIDTH + 1];
    buf[0] = SSD1306_DATA_STREAM;
    for (unsigned int page = 0; page < OLED_PAGES; page++) {
        memcpy(&buf[1], s_fb[page], OLED_WIDTH);
        i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDR,
                                   buf, OLED_WIDTH + 1, pdMS_TO_TICKS(20));
    }
    s_dirty = false;
}

/* Draw a character at (col, page).  col = pixel column (0–127), page = 0–7.
 * invert = true → render as inverted (for EMERGENCY rows).                  */
static void draw_char(uint8_t c, int col, int page, bool invert) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = FONT6x8[c - 0x20];
    for (int i = 0; i < 5; i++) {
        if (col + i >= (int)OLED_WIDTH) break;
        uint8_t b = glyph[i];
        s_fb[page][col + i] = invert ? ~b : b;
    }
    /* Gap column */
    if (col + 5 < (int)OLED_WIDTH) {
        s_fb[page][col + 5] = invert ? 0xFF : 0x00;
    }
}

/* Draw a null-terminated string at (col, page). */
static void draw_str(const char *str, int col, int page, bool invert) {
    while (*str && col < (int)OLED_WIDTH) {
        draw_char((uint8_t)*str, col, page, invert);
        col += OLED_FONT_W;
        str++;
    }
    /* Fill remainder of line if inverted (full-width inversion) */
    if (invert) {
        for (int x = col; x < (int)OLED_WIDTH; x++) {
            s_fb[page][x] = 0xFF;
        }
    }
}

static const char *alarm_label(alarm_state_t a) {
    switch (a) {
    case ALARM_NORMAL:        return "OK";
    case ALARM_FALL_PENDING:  return "PENDING";
    case ALARM_ALERT_LATCHED: return "ALERT";
    case ALARM_EMERGENCY:     return "EMERGENCY";
    default:                  return "?";
    }
}

static const char *msg_label(uint8_t mt) {
    switch ((msg_type_t)mt) {
    case MSG_HEARTBEAT:        return "HB";
    case MSG_BACKUP:           return "BK";
    case MSG_ENEMY:            return "EN";
    case MSG_MEDICAL:          return "MD";
    case MSG_COMPLETE:         return "OK";
    case MSG_DOWN:             return "DN";
    case MSG_FALL_PENDING:     return "FP";
    case MSG_FALL_CANCEL:      return "FC";
    case MSG_EMERGENCY_CLEAR:  return "EC";
    default:                   return "--";
    }
}

void oled_render_status(const soldier_record_t soldiers[SOLDIER_ID_MAX],
                        uint32_t base_uptime_ms,
                        bool blink_on) {
    memset(s_fb, 0, sizeof(s_fb));

    /* ── Row 0: Header ───────────────────────────────────────────────────── */
    uint32_t total_s  = base_uptime_ms / 1000u;
    uint32_t hh = total_s / 3600u;
    uint32_t mm = (total_s % 3600u) / 60u;
    uint32_t ss = total_s % 60u;
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "BASE B+%02u:%02u:%02u", (unsigned int)hh, (unsigned int)mm, (unsigned int)ss);
    draw_str(hdr, 0, 0, false);

    /* ── Rows 1–5: one row per soldier slot (Pages 1–5 on SSD1306) ────────── */
    int max_display = SOLDIER_ID_MAX < (int)(OLED_PAGES - 1) ? SOLDIER_ID_MAX : (int)(OLED_PAGES - 1);
    for (int i = 0; i < max_display; i++) {
        const soldier_record_t *rec = &soldiers[i];
        int page = i + 1;

        bool invert = (rec->alarm == ALARM_EMERGENCY);

        /* Build row string */
        char row[32] = {0};
        char blink_char = ' ';
        if (rec->alarm == ALARM_FALL_PENDING && blink_on) blink_char = '!';
        if (rec->alarm == ALARM_EMERGENCY)                blink_char = '*';

        if (rec->base_rx_time_ms == 0) {
            snprintf(row, sizeof(row), "[%d] NO DATA", rec->soldier_id);
        } else if (rec->link == LINK_LOST) {
            snprintf(row, sizeof(row), "[%d]%c%-8s NO LINK",
                     rec->soldier_id, blink_char,
                     alarm_label(rec->alarm));
        } else {
            snprintf(row, sizeof(row), "[%d]%c%-8s%3d%% %s",
                     rec->soldier_id, blink_char,
                     alarm_label(rec->alarm),
                     rec->battery_pct,
                     msg_label(rec->last_msg_type));
        }
        draw_str(row, 0, page, invert);
    }

    fb_flush();
}
