#include <cstdio>
#include <cstring>
#include "console.h"
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