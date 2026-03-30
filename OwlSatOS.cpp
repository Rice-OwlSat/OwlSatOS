#include "pico/stdlib.h"

#include "hardware/watchdog.h"

#include <array>

#define LENGTH 100

std::array<bool, 12> code {1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0}; 

int main() {
    // GPIO 25 is the onboard LED on the Pico
    const uint LED_PIN = 25;

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    while (true) {
        for (auto bit : code) {
            gpio_put(LED_PIN, 1);
            if (bit) sleep_ms(LENGTH * 3 - 50); else sleep_ms(LENGTH - 50);
            gpio_put(LED_PIN, 0);
            sleep_ms(50);
        }
        
        if (false) watchdog_reboot(0,0,0);
        
    }
}