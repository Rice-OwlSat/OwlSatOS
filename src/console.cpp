/**
 * @file console.cpp
 * @brief Implementation of the OwlSatOS interactive serial console task.
 */

#include <cstdio>
#include <cstring>
#include "console.h"

/**
 * @brief FreeRTOS task body for the serial console.
 *
 * Prints a prompt, reads a line from stdin, strips the trailing newline,
 * and echoes the input back. Loops indefinitely; never returns.
 *
 * @param params Unused (FreeRTOS task parameter, pass nullptr).
 */
void consoleTask(void *params) {
    printf("OwlSatOS Console Ready\n");
    fflush(stdout);
    char buf[128];
    while (true) {
        printf("> ");
        fflush(stdout);
        if (fgets(buf, sizeof(buf), stdin) != nullptr) {
            buf[strcspn(buf, "\n")] = '\0';
            printf("Received: %s\n",buf);
            fflush(stdout);
        }
    }
}