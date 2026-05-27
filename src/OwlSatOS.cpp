/**
 * @file OwlSatOS.cpp
 * @brief Entry point and core FreeRTOS tasks for OwlSatOS.
 *
 * Initialises GPIO, creates the board LED blink task and button-read task,
 * then hands control to the FreeRTOS scheduler.
 */

#include <pico/stdlib.h>
#include "FreeRTOS.h"
#include "task.h"

#include <cstdio>

#include "console.h"

/** @defgroup seg7_pins 7-Segment Display GPIO Pins
 *  GPIO pin assignments for the seven segments (A–G).
 *  @{
 */
#define A 0 ///< Segment A — GPIO 0
#define B 1 ///< Segment B — GPIO 1
#define C 2 ///< Segment C — GPIO 2
#define D 3 ///< Segment D — GPIO 3
#define E 4 ///< Segment E — GPIO 4
#define F 5 ///< Segment F — GPIO 5
#define G 6 ///< Segment G — GPIO 6
/** @} */

#define GPIO_ON  1 ///< Logic high — drive GPIO pin high.
#define GPIO_OFF 0 ///< Logic low  — drive GPIO pin low.

/** GPIO number of the on-board LED (PICO_DEFAULT_LED_PIN). */
const int LED_PIN = PICO_DEFAULT_LED_PIN;

/**
 * @brief FreeRTOS task that blinks the on-board LED at 2 Hz (250 ms on/off).
 * @param param Unused task parameter.
 */
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

/**
 * @brief Tracks the current and previous debounce state of a GPIO button.
 *
 * Both fields are single-bit bitfields packed into one byte.
 */
struct state {
    bool curState  : 1; ///< Button level sampled this tick (true = pressed).
    bool prevState : 1; ///< Button level sampled last tick.
};

/** Shared button state updated by ReadButtonTask. */
state ButtonState{};

/** Rolling counter incremented on each button press (wraps at 10). */
uint8_t counter {0};

/** @brief Drive the 7-segment display GPIOs for digit @p num (0–9). */
void set_pins(uint8_t);

/**
 * @brief FreeRTOS task that polls GPIO 16 for button presses at 1 kHz.
 *
 * Detects rising and falling edges, prints press/release events, and
 * increments the global counter (mod 10) on each press.
 *
 * @param param Unused task parameter.
 */
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
            //set_pins(counter);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// void set_pins(uint8_t num) {
//     switch (num) {
//         case 0:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_ON);
//         gpio_put(E,GPIO_ON);
//         gpio_put(F,GPIO_ON);
//         gpio_put(G,GPIO_OFF);
//         break;
//     case 1:
//         gpio_put(A,GPIO_OFF);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_OFF);
//         gpio_put(E,GPIO_OFF);
//         gpio_put(F,GPIO_OFF);
//         gpio_put(G,GPIO_OFF);
//         break;
//     case 2:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_OFF);
//         gpio_put(D,GPIO_ON);
//         gpio_put(E,GPIO_ON);
//         gpio_put(F,GPIO_OFF);
//         gpio_put(G,GPIO_ON);
//         break;
//     case 3:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_ON);
//         gpio_put(E,GPIO_OFF);
//         gpio_put(F,GPIO_OFF);
//         gpio_put(G,GPIO_ON);
//         break;
//     case 4:
//         gpio_put(A,GPIO_OFF);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_OFF);
//         gpio_put(E,GPIO_OFF);
//         gpio_put(F,GPIO_ON);
//         gpio_put(G,GPIO_ON);
//         break;
//     case 5:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_OFF);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_ON);
//         gpio_put(E,GPIO_OFF);
//         gpio_put(F,GPIO_ON);
//         gpio_put(G,GPIO_ON);
//         break;
//     case 6:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_OFF);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_ON);
//         gpio_put(E,GPIO_ON);
//         gpio_put(F,GPIO_ON);
//         gpio_put(G,GPIO_ON);
//         break;
//     case 7:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_OFF);
//         gpio_put(E,GPIO_OFF);
//         gpio_put(F,GPIO_OFF);
//         gpio_put(G,GPIO_OFF);
//         break;
//     case 8:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_ON);
//         gpio_put(E,GPIO_ON);
//         gpio_put(F,GPIO_ON);
//         gpio_put(G,GPIO_ON);
//         break;
//     case 9:
//         gpio_put(A,GPIO_ON);
//         gpio_put(B,GPIO_ON);
//         gpio_put(C,GPIO_ON);
//         gpio_put(D,GPIO_OFF);
//         gpio_put(E,GPIO_OFF);
//         gpio_put(F,GPIO_ON);
//         gpio_put(G,GPIO_ON);
//         break;
//     default:
//         break;
//     }
// }


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