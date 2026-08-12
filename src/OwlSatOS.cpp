/**
 * @file OwlSatOS.cpp
 * @brief Entry point and core FreeRTOS tasks for OwlSatOS.
 *
 * Initialises GPIO, creates the board LED blink task and button-read task,
 * then hands control to the FreeRTOS scheduler.
 *
 * @date 6.10.2026
 */

#include "FreeRTOS.h"
#include "task.h"
#include <cstdio>
#include <pico/stdlib.h>

#define GPIO_ON  1 ///< Logic high — drive GPIO pin high.
#define GPIO_OFF 0 ///< Logic low  — drive GPIO pin low.

constexpr uint LED_PIN = PICO_DEFAULT_LED_PIN;

void blink(void *param) {
  unsigned long n = 0;
  for (;;) {
    gpio_put(LED_PIN, GPIO_ON);
    printf("heartbeat %lu (LED pin %u HIGH)\n", n++, LED_PIN);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_put(LED_PIN, GPIO_OFF);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

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
  sleep_ms(300);  // let the UART settle before first output

  // Boot banner: if this prints, main() runs and UART works (independent of FreeRTOS).
  printf("\n=== OwlSatOS boot: main() reached, stdio up ===\n");

  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  BaseType_t ok = xTaskCreate(blink, "blink", 128, nullptr, tskIDLE_PRIORITY+1, nullptr);
  printf("xTaskCreate returned %ld (pdPASS=%ld); starting scheduler...\n",
         (long)ok, (long)pdPASS);

  vTaskStartScheduler();

  // Should never reach here unless the scheduler failed to start.
  printf("ERROR: scheduler returned!\n");

  return 0;
}