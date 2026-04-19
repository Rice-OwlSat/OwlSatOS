#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>
#include "console.h"

extern "C" {
    #include "tusb.h"
    void msc_disk_init(void);
}

#define A 0
#define B 1
#define C 2
#define D 3
#define E 4
#define F 5
#define G 6


#define GPIO_ON 1
#define GPIO_OFF 0

const int LED_PIN = PICO_DEFAULT_LED_PIN;

void BoardLEDTask(void *param) {
    for (;;) {
        gpio_put(LED_PIN, GPIO_ON);
        // std::cout << "on" << std::endl;
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_put(LED_PIN, GPIO_OFF);
        // std::cout << "off" << std::endl;
        vTaskDelay(pdMS_TO_TICKS(250));
        // fflush(stdout);
    }
}

struct state {
    bool curState : 1;
    bool prevState : 1;
};

state ButtonState{};

uint8_t counter {0};

void set_pins(uint8_t);

void vUSBTask(void *param) {
    (void)param;
    msc_disk_init();
    tusb_init();
    for (;;) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int main() {
    stdio_init_all();
    printf("UART alive\n");   // add this
    fflush(stdout);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    xTaskCreate(vUSBTask,      "USB",       512, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    xTaskCreate(BoardLEDTask,  "Board LED", 256, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    xTaskCreate(consoleTask,   "Console",   512, nullptr, tskIDLE_PRIORITY + 1, nullptr);

    vTaskStartScheduler();

    for (;;) {}
}