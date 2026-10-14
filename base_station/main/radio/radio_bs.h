/**
 * radio_bs.h — Radio abstraction interface for the ESP32-S3 base station.
 * Identical semantics to soldier_unit/src/radio/radio.h.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../../shared/include/packet_protocol.h"

typedef struct {
    bool    delivered;
    uint8_t retry_count;
    bool    max_rt_hit;
} radio_tx_result_t;

bool              radio_init(void);
void              radio_set_soldier_id(uint8_t sid);
uint32_t          radio_heartbeat_phase_ms(uint8_t sid);
radio_tx_result_t radio_send(const packet_t *pkt, const uint8_t key[AES_KEY_LEN]);
bool              radio_recv_raw(rf_frame_t *frame);
bool              radio_recv_raw_pipe(rf_frame_t *frame, uint8_t *pipe_num);
uint8_t           radio_link_quality(void);
