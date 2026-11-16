# USB reboot interface for Debug builds.
#
# owlsat_debug_usb_reset(<target>) gives <target> the Pico SDK's USB reset interface in Debug builds,
# so a running board can be rebooted from the host without anyone pressing BOOTSEL:
#
#   picotool reboot -f -u            running app -> BOOTSEL
#   picotool reboot -f               running app -> restart the app
#   picotool load -v -x <uf2> -f     running app -> BOOTSEL -> load -> run
#   open the USB CDC port at 1200    running app -> BOOTSEL, for hosts without picotool
#
# tools/reboot.ps1 and tools/reboot.sh wrap the first three.
#
# It never changes where stdout goes:
#   - A target that asked for USB stdio itself (OwlSatOS) keeps printing over USB, as before.
#   - Any other target (the bench images, which print on UART0 for the debug probe) gets USB for the
#     reset interface only. The SDK has no switch for that, since the reset interface is part of
#     pico_stdio_usb, so the target is linked with --wrap=stdio_init_all and src/debug/usb_reset_only.c
#     takes USB back off stdio right after stdio_init_all() brings it up. stdout and stdin stay on
#     UART; the USB CDC port enumerates but stays silent.
# The decision reads the target's stdio settings, so call this AFTER its pico_enable_stdio_usb() /
# pico_enable_stdio_uart() lines.
#
# Every reset switch is set explicitly rather than left to the SDK defaults, so a later SDK or a stray
# define cannot quietly take it away. Outside Debug the call does nothing.
#
# The interface only answers while the USB stack does. A board that has halted with interrupts off
# (HaltForWatchdog, a FreeRTOS fatal hook, an assert) will not respond, and still needs BOOTSEL by hand.

option(OWLSAT_DEBUG_USB_RESET "Give every Debug target the USB reset interface (picotool reboot -f)" ON)

# Captured at include time: CMAKE_CURRENT_FUNCTION_LIST_DIR needs CMake 3.17, the project allows 3.13
set(OWLSAT_DEBUG_USB_RESET_ONLY_SOURCE ${CMAKE_CURRENT_LIST_DIR}/../src/debug/usb_reset_only.c)

function(owlsat_debug_usb_reset target)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT OWLSAT_DEBUG_USB_RESET)
        return()
    endif()

    # Whether the target asked for USB stdio itself. Unset means the SDK's global default
    get_target_property(usb_stdio ${target} PICO_TARGET_STDIO_USB)
    if(usb_stdio STREQUAL "usb_stdio-NOTFOUND" OR usb_stdio STREQUAL "")
        set(usb_stdio ${PICO_STDIO_USB})
    endif()

    pico_enable_stdio_usb(${target} 1)
    target_compile_definitions(${target} PRIVATE
            PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1           # picotool's reset interface
            PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_BOOTSEL=1    # picotool reboot -u
            PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_RESET_TO_FLASH_BOOT=1 # picotool reboot (back to the app)
            PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR=1 # Windows binds WinUSB itself, no Zadig
            PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=1                  # 1200-baud touch
            OWLSAT_DEBUG_USB_RESET=1                                     # for code that wants to know
    )

    if(NOT usb_stdio)
        target_sources(${target} PRIVATE ${OWLSAT_DEBUG_USB_RESET_ONLY_SOURCE})
        target_link_options(${target} PRIVATE "LINKER:--wrap=stdio_init_all")
        target_compile_definitions(${target} PRIVATE OWLSAT_DEBUG_USB_RESET_ONLY=1)
    endif()
endfunction()
