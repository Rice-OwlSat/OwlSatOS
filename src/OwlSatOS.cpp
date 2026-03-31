#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>


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
        vTaskDelay(pdMS_TO_TICKS(250));
        //printf("on\n");
        gpio_put(LED_PIN, GPIO_OFF);
        vTaskDelay(pdMS_TO_TICKS(250));
        //printf("off\n");
        //printf("blink alive\n");
        fflush(stdout);
    }
}

struct state {
    bool curState : 1;
    bool prevState : 1;
};

state ButtonState{};

uint8_t counter {0};

void set_pins(uint8_t);

void ReadButtonTask(void *param) {
    for (;;) {
        ButtonState.prevState = ButtonState.curState;
        ButtonState.curState = !gpio_get(16);

        if (!ButtonState.prevState && ButtonState.curState) printf("press down.\n");
        if (ButtonState.prevState && !ButtonState.curState) printf("release.\n");
        //gpio_put(LED_PIN, ButtonState.curState);
        if (!ButtonState.prevState && ButtonState.curState) {
            counter++;
            counter %= 10;
            set_pins(counter);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void set_pins(uint8_t num) {
    switch (num) {
        case 0:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_ON);
        gpio_put(E,GPIO_ON);
        gpio_put(F,GPIO_ON);
        gpio_put(G,GPIO_OFF);
        break;
    case 1:
        gpio_put(A,GPIO_OFF);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_OFF);
        gpio_put(E,GPIO_OFF);
        gpio_put(F,GPIO_OFF);
        gpio_put(G,GPIO_OFF);
        break;
    case 2:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_OFF);
        gpio_put(D,GPIO_ON);
        gpio_put(E,GPIO_ON);
        gpio_put(F,GPIO_OFF);
        gpio_put(G,GPIO_ON);
        break;
    case 3:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_ON);
        gpio_put(E,GPIO_OFF);
        gpio_put(F,GPIO_OFF);
        gpio_put(G,GPIO_ON);
        break;
    case 4:
        gpio_put(A,GPIO_OFF);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_OFF);
        gpio_put(E,GPIO_OFF);
        gpio_put(F,GPIO_ON);
        gpio_put(G,GPIO_ON);
        break;
    case 5:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_OFF);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_ON);
        gpio_put(E,GPIO_OFF);
        gpio_put(F,GPIO_ON);
        gpio_put(G,GPIO_ON);
        break;
    case 6:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_OFF);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_ON);
        gpio_put(E,GPIO_ON);
        gpio_put(F,GPIO_ON);
        gpio_put(G,GPIO_ON);
        break;
    case 7:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_OFF);
        gpio_put(E,GPIO_OFF);
        gpio_put(F,GPIO_OFF);
        gpio_put(G,GPIO_OFF);
        break;
    case 8:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_ON);
        gpio_put(E,GPIO_ON);
        gpio_put(F,GPIO_ON);
        gpio_put(G,GPIO_ON);
        break;
    case 9:
        gpio_put(A,GPIO_ON);
        gpio_put(B,GPIO_ON);
        gpio_put(C,GPIO_ON);
        gpio_put(D,GPIO_OFF);
        gpio_put(E,GPIO_OFF);
        gpio_put(F,GPIO_ON);
        gpio_put(G,GPIO_ON);
        break;
    default:
        break;
    }
}


int main() {
    stdio_init_all();

    //sleep_ms(500);

    gpio_init(16);
    gpio_set_dir(16, GPIO_IN);
    gpio_pull_up(16);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_init(A);
    gpio_set_dir(A, GPIO_OUT);
    gpio_init(B);
    gpio_set_dir(B, GPIO_OUT);
    gpio_init(C);
    gpio_set_dir(C, GPIO_OUT);
    gpio_init(D);
    gpio_set_dir(D, GPIO_OUT);
    gpio_init(E);
    gpio_set_dir(E, GPIO_OUT);
    gpio_init(F);
    gpio_set_dir(F, GPIO_OUT);
    gpio_init(G);
    gpio_set_dir(G, GPIO_OUT);

    set_pins(counter);

    xTaskCreate(BoardLEDTask, "Board LED", 256, nullptr, tskIDLE_PRIORITY+ 1, nullptr);
    xTaskCreate(ReadButtonTask, "Read Button", 512, nullptr, tskIDLE_PRIORITY+2, nullptr);
    //xTaskCreate(consoleTask, "Console", 1024, nullptr, tskIDLE_PRIORITY+1, nullptr);
    vTaskStartScheduler();

    for (;;) {}
}