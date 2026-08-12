/**
 * @file console.h
 * @brief Serial console task interface for OwlSatOS.
 */

#pragma once

/**
 * @brief FreeRTOS task that runs the interactive serial console.
 *
 * Reads lines from stdin, echoes them back, and will eventually dispatch
 * commands to the OS subsystems. Must be created with xTaskCreate() and
 * a stack of at least 1024 words.
 *
 * @param params Unused task parameter (pass nullptr).
 */
void consoleTask(void *params);
