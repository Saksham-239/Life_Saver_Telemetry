/**
 * base_provision.c — Base station UART0 provisioning menu.
 */

#include "base_provision.h"
#include "nvs_config.h"
#include "../system/logger.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

static void flush_stdin(void) {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    while (fgetc(stdin) != EOF) {}
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

static void get_line(char *buf, size_t max_len) {
    size_t len = 0;
    while (len < max_len - 1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            usleep(10000);
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (len > 0) break; /* Skip leading newlines */
            continue;
        }
        /* Echo and store */
        putchar(c);
        fflush(stdout);
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    printf("\n");
}

static void run_menu(void) {
    printf("\n\n=== BASE STATION PROVISIONING MENU ===\n");
    while (true) {
        printf("\nEnter soldier ID (1-%d) to configure, 'l' to dump logs, or 'q' to quit: ", SOLDIER_ID_MAX);
        fflush(stdout);
        
        char line[64];
        get_line(line, sizeof(line));
        if (line[0] == 'q' || line[0] == 'Q') break;
        if (line[0] == 'l' || line[0] == 'L') {
            logger_init();
            logger_dump();
            continue;
        }
        
        int sid = atoi(line);
        if (sid < 1 || sid > SOLDIER_ID_MAX) {
            printf("Invalid ID.\n");
            continue;
        }
        
        uint8_t existing_key[AES_KEY_LEN];
        if (nvs_config_get_key(sid, existing_key)) {
            printf("Soldier %d has a key configured.\n", sid);
        } else {
            printf("Soldier %d has NO key.\n", sid);
        }
        
        printf("Enter 32 hex characters for new AES-128 key (or empty/invalid to cancel):\n> ");
        fflush(stdout);
        get_line(line, sizeof(line));
        
        if (strlen(line) != 32) {
            printf("Cancelled (need exactly 32 hex chars).\n");
            continue;
        }
        
        uint8_t new_key[AES_KEY_LEN];
        bool ok = true;
        for (int i = 0; i < 16; i++) {
            unsigned int byte;
            char hex[3] = { line[i * 2], line[i * 2 + 1], '\0' };
            if (sscanf(hex, "%x", &byte) != 1) {
                ok = false;
                break;
            }
            new_key[i] = (uint8_t)byte;
        }
        
        if (!ok) {
            printf("Invalid hex format. Cancelled.\n");
            continue;
        }
        
        if (nvs_config_set_key(sid, new_key)) {
            printf("Key saved successfully for Soldier %d.\n", sid);
        } else {
            printf("Error saving key to NVS!\n");
        }
    }
}

void base_provision_check_and_run(void) {
    printf("\n>>> Press 'p' within 3 seconds to enter provisioning menu... <<<\n");
    fflush(stdout);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    int64_t end_time = esp_timer_get_time() + 3000000;
    bool enter = false;
    
    while (esp_timer_get_time() < end_time) {
        int c = fgetc(stdin);
        if (c == 'p' || c == 'P') {
            enter = true;
            break;
        }
        usleep(10000);
    }
    
    fcntl(STDIN_FILENO, F_SETFL, flags); /* Restore blocking mode */

    if (enter) {
        flush_stdin();
        run_menu();
        printf("Provisioning complete. Rebooting in 1s...\n");
        fflush(stdout);
        usleep(1000000);
        esp_restart();
    } else {
        printf("Continuing boot sequence...\n");
    }
}
