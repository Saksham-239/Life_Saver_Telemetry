/**
 * main.c — Soldier unit (RP2040) top-level entry point.
 *
 * Milestone 1: bare-metal blocking poll loop.  No FreeRTOS.
 * After the loopback smoke-test passes (§14 step 2), the FreeRTOS task
 * decomposition (§7) replaces this loop — see soldier_sm.h for the interface.
 *
 * Boot sequence:
 *   1. stdio init (USB CDC + UART0 for debug / provisioning)
 *   2. Config load from flash (reserves counter block; may use defaults)
 *   3. Provisioning menu if BUTTON_COMPLETE held at boot
 *   4. Peripheral init: radio, MPU6050, buttons, battery, vibration
 *   5. Protocol codec test (§16): run golden-byte vectors; halt on failure
 *   6. Heartbeat phase delay
 *   7. Main poll loop:
 *        - Read MPU6050 → fall detect
 *        - Poll buttons (long-hold, double-press)
 *        - Read battery (every BATTERY_READ_INTERVAL_MS)
 *        - Advance state machine → transmit if needed
 *        - Vibrate on emergency_active
 *        - Watchdog kick
 */

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <stdlib.h>   /* srand */
#include <time.h>

/* Protocol shared layer */
#include "../../shared/include/packet_protocol.h"

/* Soldier unit modules */
#include "src/features/config_store.h"
#include "src/features/soldier_sm.h"
#include "src/features/battery.h"
#include "src/sensors/mpu6050.h"
#include "src/sensors/fall_detect.h"
#include "src/radio/radio.h"
#include "src/ui/buttons.h"
#include "src/ui/vibration.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* RTOS Tasks */
#include "src/rtos/rtos_tasks.h"

/* ── Global Queues & Config ──────────────────────────────────────────────── */
QueueHandle_t g_sm_queue;
QueueHandle_t g_radio_queue;
QueueHandle_t g_vibe_queue;

soldier_config_t g_cfg;

/* ── Test vectors (§16) ─────────────────────────────────────────────────── */
int protocol_vectors_run_all(void);

/* ── Entropy for rand() using ROSC ──────────────────────────────────────── */
static uint32_t rosc_rand_seed(void) {
    volatile uint32_t *rosc_randombit = (volatile uint32_t *)0x4006001Cu;
    uint32_t seed = 0;
    for (int i = 0; i < 32; i++) {
        seed = (seed << 1) | (*rosc_randombit & 1u);
    }
    return seed;
}

int main(void) {
    /* ── 1. stdio init ───────────────────────────────────────────────────── */
    stdio_init_all();
    sleep_ms(500);  
    printf("\n\n=== RP2040 Soldier Unit — Boot (FreeRTOS SMP) ===\n");

    srand(rosc_rand_seed());

    /* ── 2. Config load ──────────────────────────────────────────────────── */
    bool cfg_valid = config_store_load(&g_cfg);
    if (!cfg_valid) {
        printf("[WARN] Flash unprovisioned — entering mandatory provisioning mode!\n");
        config_provisioning_run(&g_cfg);
        if (!config_store_load(&g_cfg)) {
            printf("[FATAL] Flash provisioning aborted or failed. Halting to prevent nonce reuse.\n");
            while (1) {
                watchdog_update();
                sleep_ms(100);
            }
        }
    }
    printf("[main] Soldier ID: %d\n", g_cfg.soldier_id);

    /* ── 3. Peripheral init ──────────────────────────────────────────────── */
    battery_init();
    if (!radio_init()) {
        printf("[FATAL] nRF24L01+ not responding.\n");
        while(1) sleep_ms(100);
    }
    radio_set_soldier_id(g_cfg.soldier_id);

    /* Codec test */
    if (protocol_vectors_run_all() != 0) {
        printf("[FATAL] Codec test FAILED\n");
        while (1) sleep_ms(100);
    }

    /* ── 4. FreeRTOS Queues ──────────────────────────────────────────────── */
    g_sm_queue = xQueueCreate(10, sizeof(sm_event_msg_t));
    g_radio_queue = xQueueCreate(5, sizeof(radio_tx_req_t));
    g_vibe_queue = xQueueCreate(1, sizeof(vibe_pattern_t)); /* Mailbox */

    /* Initialize mailbox with NONE */
    vibe_pattern_t initial_vibe = VIBE_PATTERN_NONE;
    xQueueOverwrite(g_vibe_queue, &initial_vibe);

    /* ── 5. FreeRTOS Tasks & Core Affinity ───────────────────────────────── */
    TaskHandle_t h_sm, h_radio, h_sensor, h_button, h_vibe;

    /* Core 0 tasks */
    xTaskCreate(task_sm, "SM", 512, NULL, 2, &h_sm);
    vTaskCoreAffinitySet(h_sm, (1 << 0));

    xTaskCreate(task_radio, "Radio", 512, NULL, 3, &h_radio);
    vTaskCoreAffinitySet(h_radio, (1 << 0));

    /* Core 1 tasks */
    xTaskCreate(task_sensor, "Sensor", 512, NULL, 3, &h_sensor);
    vTaskCoreAffinitySet(h_sensor, (1 << 1));

    xTaskCreate(task_button, "Button", 256, NULL, 2, &h_button);
    vTaskCoreAffinitySet(h_button, (1 << 1));

    xTaskCreate(task_vibration, "Vibe", 256, NULL, 1, &h_vibe);
    vTaskCoreAffinitySet(h_vibe, (1 << 1));

    printf("[main] Starting FreeRTOS Scheduler...\n");
    
    /* ── 6. Start Scheduler ──────────────────────────────────────────────── */
    vTaskStartScheduler();

    /* Unreachable */
    while (1) {}
    return 0;
}

void vApplicationMallocFailedHook( void ) {
    printf("[FATAL] FreeRTOS Malloc Failed!\n");
    taskDISABLE_INTERRUPTS();
    for( ;; );
}

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName ) {
    ( void ) pxTask;
    printf("[FATAL] FreeRTOS Stack Overflow in task: %s\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for( ;; );
}
