#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>



#define GPIO_ON 1
#define GPIO_OFF 0

const int LED_PIN = PICO_DEFAULT_LED_PIN;

void BoardLEDTask(void *param) {
    for (;;) {
        gpio_put(LED_PIN, GPIO_ON);
        vTaskDelay(pdMS_TO_TICKS(250));
        //printf("on\n");
        gpio_put(LED_PIN, GPIO_OFF);
        vTaskDelay(pdMS_TO_TICKS(250));
        //printf("off\n");
        //printf("blink alive\n");
        fflush(stdout);
    }
}

int main() {
    stdio_init_all();

    //sleep_ms(500);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    xTaskCreate(BoardLEDTask, "Board LED", 1024, nullptr, tskIDLE_PRIORITY+ 1, nullptr);
    //xTaskCreate(consoleTask, "Console", 1024, nullptr, tskIDLE_PRIORITY+1, nullptr);
    vTaskStartScheduler();

    for (;;) {}
}