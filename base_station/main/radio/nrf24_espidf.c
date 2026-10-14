/**
 * nrf24_espidf.c — ESP32-S3 SPI master driver for nRF24L01+ PA/LNA.
 *
 * Implements the same radio.h interface as the RP2040 side.
 * Uses ESP-IDF SPI master API (spi_master.h).
 *
 * Pin mapping (from board_pins.h — safe GPIO for ESP32-S3 N16R8):
 *   SPI2 (HSPI)  SCK  → GPIO12
 *   SPI2         MOSI → GPIO11
 *   SPI2         MISO → GPIO13
 *   CSN                → GPIO10  (GPIO_OUTPUT, active-low)
 *   CE                 → GPIO14  (GPIO_OUTPUT, active-high)
 *
 * This is the BASE STATION receiver side.  It operates in RX mode most of
 * the time, switching briefly to TX mode only for a future operator-ACK
 * message (MSG type 9+, reserved in v1).  For v1, the only TX path is the
 * initial loopback smoke-test (§14 step 2).
 *
 * Key difference from soldier side:
 *   The base must identify which soldier sent a frame BEFORE decrypting it,
 *   because each soldier has a different AES-128 key.  The clear header
 *   (packet_header_t) carries soldier_id for exactly this reason.
 *   The key lookup is performed in nvs_config.c; this driver only calls
 *   aes_ccm_decrypt() with the key supplied by the caller.
 */

#include "nrf24_espidf.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

#include "../../../shared/include/aes_wrap.h"
#include "../../../shared/src/packet_codec.h"
#include "../../../shared/include/rf_config.h"
#include "../board_pins.h"

static const char *TAG = "nrf24";

/* ── SPI definitions ─────────────────────────────────────────────────────── */
#define NRF_SPI_HOST    SPI2_HOST
#define NRF_SPI_BAUD    1000000   /* 1 MHz for bring-up */

/* ── Register addresses (same as soldier side) ───────────────────────────── */
#define REG_CONFIG      0x00u
#define REG_EN_AA       0x01u
#define REG_EN_RXADDR   0x02u
#define REG_SETUP_AW    0x03u
#define REG_SETUP_RETR  0x04u
#define REG_RF_CH       0x05u
#define REG_RF_SETUP    0x06u
#define REG_STATUS      0x07u
#define REG_RX_ADDR_P0  0x0Au
#define REG_RX_ADDR_P1  0x0Bu
#define REG_RX_ADDR_P2  0x0Cu
#define REG_RX_ADDR_P3  0x0Du
#define REG_RX_ADDR_P4  0x0Eu
#define REG_RX_ADDR_P5  0x0Fu
#define REG_TX_ADDR     0x10u
#define REG_RX_PW_P0    0x11u
#define REG_RX_PW_P1    0x12u
#define REG_RX_PW_P2    0x13u
#define REG_RX_PW_P3    0x14u
#define REG_RX_PW_P4    0x15u
#define REG_RX_PW_P5    0x16u
#define REG_FIFO_STATUS 0x17u  /* FIFO status: [5]TX_FULL [4]TX_EMPTY [1]RX_FULL [0]RX_EMPTY */

/* ── Commands ────────────────────────────────────────────────────────────── */
#define CMD_R_REGISTER      0x00u
#define CMD_W_REGISTER      0x20u
#define CMD_R_RX_PAYLOAD    0x61u
#define CMD_W_TX_PAYLOAD    0xA0u
#define CMD_FLUSH_TX        0xE1u
#define CMD_FLUSH_RX        0xE2u
#define CMD_NOP             0xFFu

/* ── STATUS bits ─────────────────────────────────────────────────────────── */
#define STATUS_RX_DR    (1u << 6)
#define STATUS_TX_DS    (1u << 5)
#define STATUS_MAX_RT   (1u << 4)

/* ── CONFIG bits ─────────────────────────────────────────────────────────── */
#define CONFIG_EN_CRC   (1u << 3)
#define CONFIG_CRCO     (1u << 2)
#define CONFIG_PWR_UP   (1u << 1)
#define CONFIG_PRIM_RX  (1u << 0)

/* ── RF config ───────────────────────────────────────────────────────────── */
#define SETUP_RETR_VAL  ((5u << 4) | RF_CONFIG_RETRY_COUNT)

/* ── App-layer retry (same parameters as soldier side) ───────────────────── */
#define APP_RETRY_MAX           5u
#define APP_BACKOFF_BASE_MS    50u
#define APP_BACKOFF_RAND_MS   200u
#define APP_BACKOFF_MAX_MS   5000u

/* ── Module state ─────────────────────────────────────────────────────────── */
static spi_device_handle_t s_spi_dev;
static uint8_t s_last_arc_cnt = 0;

/* ── SPI helpers ─────────────────────────────────────────────────────────── */

static inline void csn_low(void)  { gpio_set_level(NRF_CSN_PIN, 0); }
static inline void csn_high(void) { gpio_set_level(NRF_CSN_PIN, 1); }
static inline void ce_low(void)   { gpio_set_level(NRF_CE_PIN,  0); }
static inline void ce_high(void)  { gpio_set_level(NRF_CE_PIN,  1); }

/* Full-duplex byte-level transfer (no DMA — short transactions) */
static uint8_t spi_xfer(uint8_t tx) {
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &tx,
        .rx_buffer = NULL,
        .flags     = SPI_TRANS_USE_RXDATA,
    };
    spi_device_transmit(s_spi_dev, &t);
    return t.rx_data[0];
}

static uint8_t nrf_read_reg(uint8_t reg) {
    csn_low();
    spi_xfer(CMD_R_REGISTER | (reg & 0x1Fu));
    uint8_t val = spi_xfer(CMD_NOP);
    csn_high();
    return val;
}

static void nrf_write_reg(uint8_t reg, uint8_t val) {
    csn_low();
    spi_xfer(CMD_W_REGISTER | (reg & 0x1Fu));
    spi_xfer(val);
    csn_high();
}

static void nrf_write_reg_buf(uint8_t reg, const uint8_t *buf, uint8_t len) {
    csn_low();
    spi_xfer(CMD_W_REGISTER | (reg & 0x1Fu));
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = buf,
    };
    spi_device_transmit(s_spi_dev, &t);
    csn_high();
}

static uint8_t nrf_cmd(uint8_t cmd) {
    csn_low();
    uint8_t s = spi_xfer(cmd);
    csn_high();
    return s;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

bool radio_init(void) {
    /* SPI bus configuration */
    spi_bus_config_t bus = {
        .mosi_io_num   = NRF_MOSI_PIN,
        .miso_io_num   = NRF_MISO_PIN,
        .sclk_io_num   = NRF_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(NRF_SPI_HOST, &bus, SPI_DMA_DISABLED));

    spi_device_interface_config_t dev = {
        .mode          = 0,         /* SPI mode 0 (CPOL=0, CPHA=0) */
        .clock_speed_hz = NRF_SPI_BAUD,
        .spics_io_num  = -1,        /* CSN managed manually */
        .queue_size    = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(NRF_SPI_HOST, &dev, &s_spi_dev));

    /* CE and CSN as GPIO outputs */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << NRF_CSN_PIN) | (1ULL << NRF_CE_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    csn_high();
    ce_low();

    /* Power-on reset delay */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify SPI */
    uint8_t status = nrf_cmd(CMD_NOP);
    if (status == 0x00u || status == 0xFFu) {
        ESP_LOGE(TAG, "nRF24 not responding (STATUS=0x%02X)", status);
        return false;
    }

    /* Configure MultiCeiver on Base Station:
     * Enable Auto-ACK and RX addresses on Pipes 0-5 (0x3F) */
    nrf_write_reg(REG_CONFIG,      CONFIG_EN_CRC | CONFIG_CRCO);
    vTaskDelay(pdMS_TO_TICKS(2));
    nrf_write_reg(REG_EN_AA,       0x3Fu);
    nrf_write_reg(REG_EN_RXADDR,   0x3Fu);
    nrf_write_reg(REG_SETUP_AW,    0x03u);
    nrf_write_reg(REG_SETUP_RETR,  SETUP_RETR_VAL);
    nrf_write_reg(REG_RF_CH,       RF_CONFIG_CHANNEL);
    nrf_write_reg(REG_RF_SETUP,    (0x03u << 1));   /* 0 dBm, 1 Mbps */

    /* Pipe 0: broadcast / loopback */
    nrf_write_reg_buf(REG_RX_ADDR_P0, RF_PIPE0_ADDR, 5);
    nrf_write_reg_buf(REG_TX_ADDR,    RF_PIPE0_ADDR, 5);
    nrf_write_reg(REG_RX_PW_P0,    RF_FRAME_LEN);

    /* Pipe 1: Soldier 1 (Full 5-byte address \xDE\xAD\xBE\xEF\x01) */
    nrf_write_reg_buf(REG_RX_ADDR_P1, RF_PIPE1_ADDR, 5);
    nrf_write_reg(REG_RX_PW_P1,    RF_FRAME_LEN);

    /* Pipes 2-5: Soldiers 2-5 (LSB only, sharing top 4 bytes with Pipe 1) */
    nrf_write_reg(REG_RX_ADDR_P2,  0x02u);
    nrf_write_reg(REG_RX_PW_P2,    RF_FRAME_LEN);

    nrf_write_reg(REG_RX_ADDR_P3,  0x03u);
    nrf_write_reg(REG_RX_PW_P3,    RF_FRAME_LEN);

    nrf_write_reg(REG_RX_ADDR_P4,  0x04u);
    nrf_write_reg(REG_RX_PW_P4,    RF_FRAME_LEN);

    nrf_write_reg(REG_RX_ADDR_P5,  0x05u);
    nrf_write_reg(REG_RX_PW_P5,    RF_FRAME_LEN);

    nrf_write_reg(REG_STATUS, 0x70u);  /* Clear all interrupt flags */
    nrf_cmd(CMD_FLUSH_RX);
    nrf_cmd(CMD_FLUSH_TX);

    /* Start in RX mode */
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP | CONFIG_PRIM_RX);
    vTaskDelay(pdMS_TO_TICKS(2));
    ce_high();  /* RX active */

    ESP_LOGI(TAG, "nRF24L01+ initialised (RX MultiCeiver Pipes 1-5, CH%d, 28-byte payload)", RF_CONFIG_CHANNEL);
    return true;
}

void radio_set_soldier_id(uint8_t sid) {
    (void)sid;  /* Not used on base station */
}

uint32_t radio_heartbeat_phase_ms(uint8_t soldier_id) {
    (void)soldier_id;
    return 0;  /* Not used on base station */
}

bool radio_recv_raw_pipe(rf_frame_t *frame, uint8_t *pipe_num) {
    /* CE is already high (RX mode active from init).
     *
     * Check FIFO_STATUS.RX_EMPTY (bit 0) rather than STATUS.RX_DR alone.
     * RX_DR is set on the first frame arrival but is cleared by the caller;
     * if additional frames arrived while the first was being processed,
     * they sit in the 3-deep FIFO with RX_DR already cleared.
     * FIFO_STATUS.RX_EMPTY is the definitive "is there data?" signal.
     * (Nordic nRF24L01+ Product Specification §9.17)
     */
    uint8_t fifo = nrf_read_reg(REG_FIFO_STATUS);
    if (fifo & 0x01u) {
        /* RX FIFO empty — nothing to read */
        return false;
    }

    /* Read STATUS to determine which pipe received the payload at top of RX FIFO.
     * Bits [3:1] = RX_P_NO (0b000..0b101 for pipes 0..5) */
    uint8_t status = nrf_read_reg(REG_STATUS);
    uint8_t pipe = (uint8_t)((status >> 1) & 0x07u);
    if (pipe_num) {
        *pipe_num = pipe;
    }

    /* Read 28-byte payload */
    csn_low();
    spi_xfer(CMD_R_RX_PAYLOAD);
    spi_transaction_t t = {
        .length    = RF_FRAME_LEN * 8,
        .rx_buffer = frame,
        .tx_buffer = NULL,
        .flags     = 0,
    };
    /* Fill TX with 0xFF (NOP bytes) during RX */
    static uint8_t nop_buf[RF_FRAME_LEN];
    memset(nop_buf, CMD_NOP, RF_FRAME_LEN);
    t.tx_buffer = nop_buf;
    spi_device_transmit(s_spi_dev, &t);
    csn_high();

    /* Clear RX_DR flag (harmless if already clear) */
    nrf_write_reg(REG_STATUS, STATUS_RX_DR);

    return true;
}

bool radio_recv_raw(rf_frame_t *frame) {
    return radio_recv_raw_pipe(frame, NULL);
}

/* TX path (future use; needed for smoke-test loopback) */
static radio_tx_result_t nrf_tx_single(const rf_frame_t *frame) {
    radio_tx_result_t r = {0};

    ce_low();
    /* Switch to TX */
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP);
    nrf_cmd(CMD_FLUSH_TX);
    nrf_write_reg(REG_STATUS, STATUS_TX_DS | STATUS_MAX_RT);

    csn_low();
    spi_xfer(CMD_W_TX_PAYLOAD);
    spi_transaction_t t = {
        .length    = RF_FRAME_LEN * 8,
        .tx_buffer = frame,
    };
    spi_device_transmit(s_spi_dev, &t);
    csn_high();

    ce_high();
    esp_rom_delay_us(15);
    ce_low();

    /* Poll for TX_DS / MAX_RT up to 35 ms */
    uint8_t status;
    int64_t deadline = esp_timer_get_time() + 35000;
    do {
        status = nrf_cmd(CMD_NOP);
        esp_rom_delay_us(200);
    } while (!(status & (STATUS_TX_DS | STATUS_MAX_RT))
             && esp_timer_get_time() < deadline);

    r.retry_count = nrf_read_reg(0x08u) & 0x0Fu;
    r.max_rt_hit  = (status & STATUS_MAX_RT) != 0;
    r.delivered   = (status & STATUS_TX_DS)  != 0;
    nrf_write_reg(REG_STATUS, STATUS_TX_DS | STATUS_MAX_RT);
    if (r.max_rt_hit) nrf_cmd(CMD_FLUSH_TX);

    /* Return to RX mode */
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP | CONFIG_PRIM_RX);
    ce_high();

    return r;
}

static uint32_t backoff_next_ms(uint32_t prev_ms, bool first) {
    if (first) return APP_BACKOFF_BASE_MS + (uint32_t)(rand() % APP_BACKOFF_RAND_MS);
    uint32_t next = prev_ms * 2u;
    if (next > APP_BACKOFF_MAX_MS) next = APP_BACKOFF_MAX_MS;
    next += (uint32_t)(rand() % (next / 2u + 1u));
    if (next > APP_BACKOFF_MAX_MS) next = APP_BACKOFF_MAX_MS;
    return next;
}

radio_tx_result_t radio_send(const packet_t *pkt, const uint8_t key[AES_KEY_LEN]) {
    radio_tx_result_t result = {0};
    rf_frame_t frame;
    if (!aes_ccm_encrypt(pkt, key, &frame)) return result;

    result = nrf_tx_single(&frame);
    s_last_arc_cnt = result.retry_count;
    if (result.delivered) return result;

    uint32_t backoff_ms = 0;
    for (uint8_t i = 0; i < APP_RETRY_MAX; i++) {
        backoff_ms = backoff_next_ms(backoff_ms, i == 0);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        radio_tx_result_t r = nrf_tx_single(&frame);
        if (r.retry_count > s_last_arc_cnt) s_last_arc_cnt = r.retry_count;
        if (r.delivered) {
            result = r;
            result.retry_count = s_last_arc_cnt;
            return result;
        }
    }
    result.delivered  = false;
    result.max_rt_hit = true;
    result.retry_count = s_last_arc_cnt;
    return result;
}

uint8_t radio_link_quality(void) {
    return s_last_arc_cnt;
}
