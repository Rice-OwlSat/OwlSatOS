/**

    @file      usb_reset_only.c
    @brief     Debug builds: USB up for picotool's reset interface, but not carrying stdout.
    @details   Linked only into targets that owlsat_debug_usb_reset() (cmake/debug_usb_reset.cmake)
               gave USB to for the reset interface alone, i.e. targets whose stdio is on UART.

               The SDK has no switch for "USB without USB stdio": the reset interface is part of
               pico_stdio_usb, and stdio_usb_init() always registers USB as a stdio driver. So the
               target is linked with -Wl,--wrap=stdio_init_all, which routes main()'s call here: the
               real stdio_init_all() brings up UART and USB as usual, then USB is taken off stdio.
               The USB stack keeps running from its own interrupt, so picotool's reset interface and
               the 1200-baud BOOTSEL trigger still answer. The CDC port still enumerates, and stays
               silent. stdin is UART-only too.

               Nothing to call: a target picks this up from the CMake function alone.

    @author    Viola Case
    @date      15.11.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#include <pico/stdio.h>
#include <pico/stdio_usb.h>

bool __real_stdio_init_all(void);

bool __wrap_stdio_init_all(void) {
  bool any = __real_stdio_init_all();
  stdio_set_driver_enabled(&stdio_usb, false);
  return any;
}
