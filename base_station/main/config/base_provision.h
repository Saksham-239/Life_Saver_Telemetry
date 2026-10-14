/**
 * base_provision.h — Base station provisioning menu over UART0.
 */
#pragma once

/**
 * Check if the user requests provisioning (presses 'p' within 3 seconds of boot).
 * If so, blocks and runs the serial menu to configure AES-128 keys per soldier.
 * Reboot the ESP32-S3 automatically upon exiting the menu.
 */
void base_provision_check_and_run(void);
