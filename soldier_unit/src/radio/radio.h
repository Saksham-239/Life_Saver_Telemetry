/**
 * radio.h — Radio abstraction layer
 *
 * The ONLY header that application code (state machine, buttons, fall detect,
 * battery, logging) may include to interact with the radio.  No nRF24-specific
 * register names, SPI calls, or crypto calls appear outside the .c implementation.
 *
 * Swapping to LoRa (§17) means replacing only the .c file behind this header.
 * All callers remain unchanged.
 */

#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>
#include "../../shared/include/packet_protocol.h"

/* ── TX result ───────────────────────────────────────────────────────────── */

typedef struct {
    bool    delivered;     /**< True if link-layer ACK received (hardware ShockBurst)  */
    uint8_t retry_count;   /**< Hardware retry count from last successful attempt (0–15)*/
    bool    max_rt_hit;    /**< True if MAX_RT fired and app-layer retries also exhausted*/
} radio_tx_result_t;

/* ── Init / config ───────────────────────────────────────────────────────── */

/**
 * Initialise the radio peripheral and configure nRF24 registers.
 * Must be called once before any other radio_* function.
 * @return true on success (STATUS register reads expected default).
 * @return false if SPI communication with the nRF24 failed — check wiring.
 */
bool radio_init(void);

/**
 * Set the soldier ID used for heartbeat phase calculation.
 * Call after config_store_load() returns the provisioned soldier_id.
 */
void radio_set_soldier_id(uint8_t soldier_id);

/**
 * Return the initial heartbeat delay for this soldier.
 * Phase = soldier_id × (HEARTBEAT_BASE_MS / SOLDIER_ID_MAX).
 * Sleep this many ms after boot before sending the first heartbeat, to
 * prevent lockstep collisions when multiple soldiers restart simultaneously.
 */
uint32_t radio_heartbeat_phase_ms(uint8_t soldier_id);

/* ── TX / RX ─────────────────────────────────────────────────────────────── */

/**
 * Encrypt (AES-128-CCM) and transmit one packet.
 *
 * Blocks until:
 *   (a) ShockBurst delivers successfully (delivered=true), or
 *   (b) All hardware retries and APP_RETRY_MAX app-layer backoff retries
 *       are exhausted (delivered=false, max_rt_hit=true).
 *
 * App-layer retry uses randomised exponential backoff starting at ~50–250 ms,
 * doubling with jitter per retry, capped at 5 seconds.
 *
 * @param pkt  Packet to transmit (hdr + body must be fully populated).
 * @param key  16-byte AES-128 key for pkt->hdr.soldier_id.
 */
radio_tx_result_t radio_send(const packet_t *pkt, const uint8_t key[AES_KEY_LEN]);

/**
 * Check for a received frame and decrypt + authenticate it.
 * Non-blocking: returns false immediately if the RX FIFO is empty.
 *
 * The caller must supply the correct AES key for the soldier_id found
 * in the clear frame header.  On the base station, look up the key table
 * using frame->header.soldier_id before calling this (done inside
 * nrf24_espidf.c on that side).
 *
 * @param pkt  Output: decoded packet on success.
 * @param key  16-byte AES-128 key.
 * @return true  if a valid, authenticated frame was received and decoded.
 * @return false if FIFO empty, CCM tag mismatch, or header/body invalid.
 */
bool radio_recv(packet_t *pkt, const uint8_t key[AES_KEY_LEN]);

/**
 * Link quality estimate: 0 = best (no retries), 15 = worst.
 * On nRF24: cached ARC_CNT from last radio_send().
 * On LoRa (future): normalised value derived from RSSI/SNR.
 * Valid only after at least one radio_send() call this session.
 */
uint8_t radio_link_quality(void);

#endif /* RADIO_H */
