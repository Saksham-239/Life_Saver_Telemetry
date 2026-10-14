/**
 * nrf24_spi.c — RP2040 raw SPI driver for nRF24L01+ PA/LNA
 *
 * Implements radio.h using Pico SDK hardware_spi + hardware_gpio.
 * Zero dependency on any Arduino/RF24 library.
 *
 * ── Pin mapping (§11) ─────────────────────────────────────────────────────
 *   SPI0  SCK  → GP18   (GPIO_FUNC_SPI)
 *   SPI0  MOSI → GP19   (GPIO_FUNC_SPI)
 *   SPI0  MISO → GP16   (GPIO_FUNC_SPI)
 *   CSN        → GP17   (GPIO_OUT, active-low)
 *   CE         → GP20   (GPIO_OUT, active-high)
 *
 * ── Register references ────────────────────────────────────────────────────
 *   Nordic nRF24L01+ Product Specification v1.0.
 *   Every register address and bit position below is cross-referenced to
 *   that document.  If any value looks suspicious, check Table 28 (register
 *   map) and the corresponding section before modifying — hallucinated
 *   register maps are the leading cause of raw-SPI bring-up failures.
 *
 * ── SPI protocol ──────────────────────────────────────────────────────────
 *   Mode 0 (CPOL=0, CPHA=0), MSB first.
 *   Max SPI clock: 10 MHz (datasheet §8.3).  Starting at 1 MHz.
 *   CSN held low for the entire transaction; CE controlled separately.
 *   First byte of every transaction is a command byte; data bytes follow.
 *
 * ── Frame size ─────────────────────────────────────────────────────────────
 *   RF_FRAME_LEN = 28 bytes (header 10 + ciphertext 10 + CCM tag 8).
 *   Static payload width.  nRF24 max payload = 32 bytes; 4 bytes headroom.
 *
 * ── Collision avoidance ────────────────────────────────────────────────────
 *   On MAX_RT (hardware retry budget exhausted), randomised exponential
 *   backoff is used at the application layer before retrying.
 *   Heartbeat scheduling uses a soldier-ID-derived phase offset to prevent
 *   multiple soldiers power-cycling simultaneously and entering lockstep.
 */

#include "radio.h"
#include "../../shared/include/aes_wrap.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "../../shared/include/rf_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdlib.h>   /* rand() — seeded with RP2040 ROSC entropy at init */

/* ── Pin definitions (§11) ──────────────────────────────────────────────── */
#define NRF_SPI_PORT    spi0
#define NRF_SCK_PIN     18u
#define NRF_MOSI_PIN    19u
#define NRF_MISO_PIN    16u
#define NRF_CSN_PIN     17u
#define NRF_CE_PIN      20u
#define NRF_SPI_BAUD    1000000u   /* 1 MHz for bring-up; raise to 4 MHz after validation */

/* ── nRF24L01+ SPI commands (Table 19, datasheet §8.3.1) ─────────────────
 * Command encoding: [CMD_bits | REG_addr] for register commands              */
#define CMD_R_REGISTER      0x00u  /* Read register  (OR with 5-bit reg addr)  */
#define CMD_W_REGISTER      0x20u  /* Write register (OR with 5-bit reg addr)  */
#define CMD_R_RX_PAYLOAD    0x61u  /* Read RX payload                          */
#define CMD_W_TX_PAYLOAD    0xA0u  /* Write TX payload                         */
#define CMD_FLUSH_TX        0xE1u  /* Flush TX FIFO                            */
#define CMD_FLUSH_RX        0xE2u  /* Flush RX FIFO                            */
#define CMD_NOP             0xFFu  /* No operation — returns STATUS             */

/* ── nRF24L01+ register addresses (Table 28, datasheet §9) ─────────────── */
#define REG_CONFIG          0x00u
#define REG_EN_AA           0x01u  /* Auto-acknowledgement enable per pipe      */
#define REG_EN_RXADDR       0x02u  /* RX pipe enable                            */
#define REG_SETUP_AW        0x03u  /* Address width                             */
#define REG_SETUP_RETR      0x04u  /* Auto-retransmit delay and count           */
#define REG_RF_CH           0x05u  /* RF channel (0–125)                        */
#define REG_RF_SETUP        0x06u  /* PA level, data rate                       */
#define REG_STATUS          0x07u  /* Status                                    */
#define REG_OBSERVE_TX      0x08u  /* Lost packet count [7:4], retry count [3:0]*/
#define REG_RX_ADDR_P0      0x0Au  /* RX pipe 0 address (5 bytes)               */
#define REG_TX_ADDR         0x10u  /* TX address (5 bytes)                      */
#define REG_RX_PW_P0        0x11u  /* Payload width pipe 0                      */
#define REG_FIFO_STATUS     0x17u  /* FIFO status flags                         */

/* ── CONFIG register bit definitions (§9.1) ─────────────────────────────── */
#define CONFIG_EN_CRC       (1u << 3)
#define CONFIG_CRCO         (1u << 2)  /* 2-byte CRC                           */
#define CONFIG_PWR_UP       (1u << 1)
#define CONFIG_PRIM_RX      (1u << 0)  /* 1 = RX mode, 0 = TX mode             */

/* ── RF_SETUP register (§9.6) ────────────────────────────────────────────── */
/* RF_PWR [2:1]: 00=-18dBm, 01=-12dBm, 10=-6dBm, 11=0dBm (chip level;
 * PA/LNA module adds external gain on top of this).                          */
#define RF_SETUP_PWR_MAX    (0x03u << 1)  /* 0 dBm at chip output              */
#define RF_SETUP_1MBPS      (0x00u)       /* RF_DR_HIGH=0, RF_DR_LOW=0 → 1Mbps */

/* ── STATUS register bit definitions (§9.7) ─────────────────────────────── */
#define STATUS_RX_DR        (1u << 6)  /* RX data ready                        */
#define STATUS_TX_DS        (1u << 5)  /* TX data sent (ACK received)           */
#define STATUS_MAX_RT       (1u << 4)  /* Max retries reached                   */

/* ── SETUP_RETR (§9.4) ───────────────────────────────────────────────────── */
/* ARD [7:4] = 5 → 1500 µs delay between retries (safe for PA/LNA at 1Mbps)
 * ARC [3:0] = 15 → 15 hardware retries before MAX_RT                        */
#define SETUP_RETR_VAL      ((5u << 4) | RF_CONFIG_RETRY_COUNT)

/* ── App-layer retry parameters (exponential backoff, §collision avoidance) ─ */
#define APP_RETRY_MAX            5u       /* Max app-layer retries after MAX_RT  */
#define APP_BACKOFF_BASE_MS     50u       /* First backoff window minimum (ms)   */
#define APP_BACKOFF_RAND_MS    200u       /* First backoff random range (ms)     */
#define APP_BACKOFF_MAX_MS    5000u       /* Hard cap on any single backoff (ms) */

/* ── Module state ───────────────────────────────────────────────────────── */
static uint8_t  s_last_arc_cnt    = 0;
static uint8_t  s_soldier_id      = 1;   /* Set by radio_set_soldier_id() */

/* ── Internal helpers ───────────────────────────────────────────────────── */

static inline void csn_low(void)  { gpio_put(NRF_CSN_PIN, 0); }
static inline void csn_high(void) { gpio_put(NRF_CSN_PIN, 1); }
static inline void ce_low(void)   { gpio_put(NRF_CE_PIN,  0); }
static inline void ce_high(void)  { gpio_put(NRF_CE_PIN,  1); }

static uint8_t spi_byte(uint8_t tx) {
    uint8_t rx;
    spi_write_read_blocking(NRF_SPI_PORT, &tx, &rx, 1);
    return rx;
}

static uint8_t nrf_read_reg(uint8_t reg) {
    csn_low();
    spi_byte(CMD_R_REGISTER | (reg & 0x1Fu));
    uint8_t val = spi_byte(CMD_NOP);
    csn_high();
    return val;
}

static void nrf_write_reg(uint8_t reg, uint8_t val) {
    csn_low();
    spi_byte(CMD_W_REGISTER | (reg & 0x1Fu));
    spi_byte(val);
    csn_high();
}

static void nrf_write_reg_buf(uint8_t reg, const uint8_t *buf, uint8_t len) {
    csn_low();
    spi_byte(CMD_W_REGISTER | (reg & 0x1Fu));
    spi_write_blocking(NRF_SPI_PORT, buf, len);
    csn_high();
}

static uint8_t nrf_cmd_only(uint8_t cmd) {
    csn_low();
    uint8_t status = spi_byte(cmd);
    csn_high();
    return status;
}

static inline uint8_t nrf_status(void) { return nrf_cmd_only(CMD_NOP); }

static void nrf_clear_irq_flags(void) {
    nrf_write_reg(REG_STATUS, STATUS_RX_DR | STATUS_TX_DS | STATUS_MAX_RT);
}

/**
 * Compute a random delay_ms for exponential backoff.
 * First call: base_ms in [APP_BACKOFF_BASE_MS, APP_BACKOFF_BASE_MS+APP_BACKOFF_RAND_MS).
 * Subsequent calls: double previous, add jitter up to half the new base, cap at APP_BACKOFF_MAX_MS.
 */
static uint32_t backoff_next_ms(uint32_t prev_ms, bool first) {
    int32_t jitter = (int32_t)((rand() % 11) - 5); /* +/- 5 ms */
    if (first) {
        int32_t b = (int32_t)(APP_BACKOFF_BASE_MS + (uint32_t)(rand() % APP_BACKOFF_RAND_MS)) + jitter;
        return (b < 10) ? 10u : (uint32_t)b;
    }
    uint32_t next = prev_ms * 2u;
    if (next > APP_BACKOFF_MAX_MS) next = APP_BACKOFF_MAX_MS;
    next += (uint32_t)(rand() % (next / 2u + 1u));
    int32_t b = (int32_t)next + jitter;
    if (b > (int32_t)APP_BACKOFF_MAX_MS) b = (int32_t)APP_BACKOFF_MAX_MS;
    return (b < 10) ? 10u : (uint32_t)b;
}

/* ── Low-level TX (single attempt, no app-layer retry) ──────────────────── */

static radio_tx_result_t nrf_tx_single(const rf_frame_t *frame) {
    radio_tx_result_t r = {0};

    /* Switch to TX mode, CE low (standby-I) */
    ce_low();
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP);

    /* Clear stale interrupt flags and flush TX FIFO */
    nrf_write_reg(REG_STATUS, STATUS_TX_DS | STATUS_MAX_RT);
    nrf_cmd_only(CMD_FLUSH_TX);

    /* Write RF_FRAME_LEN (28) bytes to TX FIFO */
    csn_low();
    spi_byte(CMD_W_TX_PAYLOAD);
    spi_write_blocking(NRF_SPI_PORT, (const uint8_t *)frame, RF_FRAME_LEN);
    csn_high();

    /* Pulse CE ≥ 10 µs to initiate TX (§6.1.3) */
    ce_high();
    sleep_us(15);
    ce_low();

    /* Poll for TX_DS or MAX_RT.
     * Max time = 15 retries × 1500 µs delay + 1500 µs first attempt ≈ 25.5 ms.
     * Timeout at 35 ms to cover any oscillator tolerance.                      */
    const uint32_t timeout_us = 35000u;
    uint32_t waited_us = 0;
    uint8_t status;
    do {
        status = nrf_status();
        sleep_us(200u);
        waited_us += 200u;
    } while (!(status & (STATUS_TX_DS | STATUS_MAX_RT)) && waited_us < timeout_us);

    /* Read ARC_CNT from OBSERVE_TX [3:0] (datasheet §9.8) */
    r.retry_count = nrf_read_reg(REG_OBSERVE_TX) & 0x0Fu;
    r.max_rt_hit  = (status & STATUS_MAX_RT) != 0u;
    r.delivered   = (status & STATUS_TX_DS)  != 0u;

    /* Clear flags; flush stuck payload on MAX_RT */
    nrf_write_reg(REG_STATUS, STATUS_TX_DS | STATUS_MAX_RT);
    if (r.max_rt_hit) {
        nrf_cmd_only(CMD_FLUSH_TX);
    }

    return r;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool radio_init(void) {
    /* 1. SPI peripheral */
    spi_init(NRF_SPI_PORT, NRF_SPI_BAUD);
    spi_set_format(NRF_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(NRF_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(NRF_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(NRF_MISO_PIN, GPIO_FUNC_SPI);

    /* 2. CE and CSN as GPIO outputs, safe defaults */
    gpio_init(NRF_CSN_PIN); gpio_set_dir(NRF_CSN_PIN, GPIO_OUT); csn_high();
    gpio_init(NRF_CE_PIN);  gpio_set_dir(NRF_CE_PIN,  GPIO_OUT); ce_low();

    /* 3. nRF24 power-on reset: ≥ 100 ms after VCC stable (§6.1.7) */
    sleep_ms(100);

    /* 4. Verify SPI communication.
     * After reset, STATUS = 0x0E (RX_P_NO=0b111, all flags clear).
     * 0x00 or 0xFF → no response → wiring fault.                     */
    uint8_t status = nrf_status();
    if (status == 0x00u || status == 0xFFu) {
        return false;
    }

    /* 5. Power down to configure from known state */
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO);
    sleep_ms(2);

    /* 6. Auto-ACK on pipe 0 only */
    nrf_write_reg(REG_EN_AA, 0x01u);

    /* 7. Enable RX pipe 0 */
    nrf_write_reg(REG_EN_RXADDR, 0x01u);

    /* 8. Address width: 5 bytes (AW = 0b11) */
    nrf_write_reg(REG_SETUP_AW, 0x03u);

    /* 9. Auto-retransmit: 1500 µs, 15 retries */
    nrf_write_reg(REG_SETUP_RETR, SETUP_RETR_VAL);

    /* 10. RF channel */
    nrf_write_reg(REG_RF_CH, RF_CONFIG_CHANNEL);

    /* 11. RF setup: 1 Mbps, 0 dBm chip output
     * RF_SETUP = 0b00000110 → RF_DR_HIGH=0, RF_DR_LOW=0 (1Mbps), RF_PWR=11 (0dBm) */
    nrf_write_reg(REG_RF_SETUP, RF_SETUP_PWR_MAX | RF_SETUP_1MBPS);

    /* 12. TX address and RX pipe 0 address (auto-ACK requires them to match) */
    nrf_write_reg_buf(REG_TX_ADDR,   RF_CONFIG_ADDRESS, 5);
    nrf_write_reg_buf(REG_RX_ADDR_P0, RF_CONFIG_ADDRESS, 5);

    /* 13. Static payload width: RF_FRAME_LEN = 28 bytes */
    nrf_write_reg(REG_RX_PW_P0, RF_FRAME_LEN);

    /* 14. Clear all interrupt flags and flush FIFOs */
    nrf_clear_irq_flags();
    nrf_cmd_only(CMD_FLUSH_TX);
    nrf_cmd_only(CMD_FLUSH_RX);

    /* 15. Power up in TX mode (CE low = standby-I, not yet transmitting) */
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP);
    sleep_ms(2);   /* tpd2stby ≤ 1.5 ms (§6.1.7) */

    return true;
}

void radio_set_soldier_id(uint8_t sid) {
    if (sid < 1 || sid > SOLDIER_ID_MAX) {
        sid = 1;
    }
    s_soldier_id = sid;

    /* Set per-soldier physical pipe address (<sid>\xDE\xAD\xBE\xEF) */
    uint8_t addr[5] = { sid, 0xDE, 0xAD, 0xBE, 0xEF };
    nrf_write_reg_buf(REG_TX_ADDR, addr, 5);
    nrf_write_reg_buf(REG_RX_ADDR_P0, addr, 5);

    /* Per-soldier ARD stagger: ARD = 750 us + (sid * 250 us)
     * In nRF24, ARD register value N gives (N + 1) * 250 us delay.
     * N = 2 + sid (e.g., sid 1 -> 3 [1000 us], sid 5 -> 7 [2000 us]) */
    uint8_t ard_val = (uint8_t)(2u + sid);
    nrf_write_reg(REG_SETUP_RETR, (uint8_t)((ard_val << 4) | (RF_CONFIG_RETRY_COUNT & 0x0Fu)));
}

radio_tx_result_t radio_send(const packet_t *pkt, const uint8_t key[AES_KEY_LEN]) {
    radio_tx_result_t result = {0};

    /* Encrypt into wire frame */
    rf_frame_t frame;
    if (!aes_ccm_encrypt(pkt, key, &frame)) {
        /* Encryption failure — should not occur with a valid 16-byte key */
        return result;
    }

    /* First attempt */
    result = nrf_tx_single(&frame);
    s_last_arc_cnt = result.retry_count;

    if (result.delivered) {
        return result;
    }

    /* Exponential backoff retry on MAX_RT or timeout ─────────────────────── */
    uint32_t backoff_ms = 0;
    for (uint8_t app_retry = 0; app_retry < APP_RETRY_MAX; app_retry++) {
        backoff_ms = backoff_next_ms(backoff_ms, app_retry == 0);
        if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        } else {
            sleep_ms(backoff_ms);
        }

        radio_tx_result_t r = nrf_tx_single(&frame);
        /* Accumulate worst-case retry count across attempts */
        if (r.retry_count > s_last_arc_cnt) s_last_arc_cnt = r.retry_count;

        if (r.delivered) {
            result = r;
            result.retry_count = s_last_arc_cnt;
            return result;
        }
    }

    result.delivered    = false;
    result.max_rt_hit   = true;
    result.retry_count  = s_last_arc_cnt;
    return result;
}

bool radio_recv(packet_t *pkt, const uint8_t key[AES_KEY_LEN]) {
    /* Switch to RX mode */
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP | CONFIG_PRIM_RX);
    ce_high();
    sleep_us(150);  /* tRXSETTLE ≤ 130 µs (§6.1.3) */

    uint8_t status = nrf_status();
    if (!(status & STATUS_RX_DR)) {
        ce_low();
        nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP);
        return false;
    }

    /* Read payload */
    rf_frame_t frame;
    csn_low();
    spi_byte(CMD_R_RX_PAYLOAD);
    spi_read_blocking(NRF_SPI_PORT, CMD_NOP, (uint8_t *)&frame, RF_FRAME_LEN);
    csn_high();

    nrf_write_reg(REG_STATUS, STATUS_RX_DR);

    /* Return to TX standby */
    ce_low();
    nrf_write_reg(REG_CONFIG, CONFIG_EN_CRC | CONFIG_CRCO | CONFIG_PWR_UP);

    /* Decrypt and authenticate */
    return aes_ccm_decrypt(&frame, key, pkt);
}

uint8_t radio_link_quality(void) {
    return s_last_arc_cnt;  /* 0=best, 15=worst; cached from last TX */
}

/**
 * Compute the heartbeat phase offset for this soldier.
 *
 * Each soldier delays its first heartbeat by:
 *   phase_ms = soldier_id × (HEARTBEAT_BASE_MS / SOLDIER_ID_MAX)
 *
 * This spreads 5 soldiers evenly across the 30-second base interval at
 * power-on, preventing lockstep collisions when multiple units restart
 * simultaneously (e.g. after a base-camp power cycle).
 *
 * Call once at startup; sleep for the returned value before the first heartbeat.
 */
uint32_t radio_heartbeat_phase_ms(uint8_t soldier_id) {
    return (uint32_t)soldier_id * (HEARTBEAT_BASE_MS / SOLDIER_ID_MAX);
}
