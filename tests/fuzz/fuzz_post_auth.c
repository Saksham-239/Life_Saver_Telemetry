/**
 * fuzz_post_auth.c - LLVM libFuzzer structure-aware post-auth state machine harness
 *
 * Tests base station state machine (base_sm_rx_packet, base_sm_tick, readers)
 * against arbitrary valid-structured frames with randomized IDs, types, flags,
 * counters, uptimes, and temporal sequences.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "packet_protocol.h"
#include "packet_codec.h"
#include "base_sm.h"

static bool s_initialized = false;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 18) {
        return 0;
    }

    if (!s_initialized) {
        base_sm_init();
        s_initialized = true;
    }

    packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.hdr.version = PROTO_VERSION;
    pkt.hdr.soldier_id = (data[0] % SOLDIER_ID_MAX) + 1; /* 1..5 */
    uint64_t ctr = 0;
    memcpy(&ctr, data + 1, sizeof(uint64_t));
    pkt.hdr.persistent_tx_counter = ctr;

    pkt.body.msg_type = (data[9] % 16);
    pkt.body.sensor_flags = data[10];
    pkt.body.battery_pct = data[11] % 101;
    pkt.body.prev_tx_retries = data[12] % 16;
    uint32_t upt = 0;
    memcpy(&upt, data + 12, sizeof(uint32_t));
    pkt.body.sender_uptime_ms = upt;
    pkt.body.event_id = (uint16_t)(data[16] | (data[17] << 8));

    uint32_t rx_time_ms = 1000u + (upt % 1000000u);

    /* Process packet */
    base_sm_rx_packet(&pkt, rx_time_ms);

    /* Thread-safe reader verification */
    soldier_record_t rec;
    bool found = base_sm_get_soldier(pkt.hdr.soldier_id, &rec);
    if (found) {
        assert(rec.soldier_id == pkt.hdr.soldier_id);
    }

    /* Exercise time-based and snapshot paths */
    if (data[0] & 0x04) {
        base_sm_tick(rx_time_ms + 100000u);
    }
    if (data[0] & 0x08) {
        soldier_record_t all[SOLDIER_ID_MAX];
        base_sm_get_state(all);
    }
    if (data[0] & 0x10) {
        const soldier_record_t *ptr = base_sm_get_soldier_ptr(pkt.hdr.soldier_id);
        (void)ptr;
    }

    return 0;
}
