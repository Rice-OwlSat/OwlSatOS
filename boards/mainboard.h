/**
 * @file boards/mainboard.h
 * @brief board summary for compiler specific stuff
 *
 * @todo check if this needs to update for MRAM stuff
 * @todo also change this to not be the pico 2
 *
 */

#ifndef _BOARDS_MY_BOARD_H
#define _BOARDS_MY_BOARD_H

// NOTE: pico_board_cmake_set is NOT a C statement and needs no semicolon.
// It is a dual-purpose marker: CMake's generic_board.cmake text-scans this
// line and runs set(PICO_PLATFORM rp2350), while in C it expands to nothing
// (pico.h defines `#define pico_board_cmake_set(x, y)` as an empty macro).
pico_board_cmake_set(PICO_PLATFORM, rp2350)

// RP2350A (QFN-60, 30 GPIOs). 0 would mean RP2350B (QFN-80, 48 GPIOs).
#define PICO_RP2350A 1

// Defaults for your hardware:
#define PICO_DEFAULT_LED_PIN 25
#define PICO_DEFAULT_UART 0
#define PICO_DEFAULT_UART_TX_PIN 0
#define PICO_DEFAULT_UART_RX_PIN 1

// Flash chip / size on your board:
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#define PICO_FLASH_SPI_CLKDIV 4
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

#endif
