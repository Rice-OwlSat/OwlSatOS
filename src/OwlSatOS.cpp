/**
 * @file OwlSatOS.cpp
 * @brief Entry point and core FreeRTOS tasks for OwlSatOS.
 *
 * Initialises GPIO, creates the board LED blink task and button-read task,
 * then hands control to the FreeRTOS scheduler.
 *
 * @date 6.10.2026
 */

#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>


/**
 * @brief Firmware entry point.
 *
 * Initialises stdio and all GPIO pins, then creates FreeRTOS tasks and
 * starts the scheduler. Never returns.
 *
 * @return int Never reached.
 */
int main() {
  stdio_init_all();

  const uint LED_PIN = 25;

  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  while(true) {
    gpio_put(LED_PIN, 1);
    sleep_ms(500);
    gpio_put(LED_PIN, 0);
    sleep_ms(500);
  }

  return 0;
}