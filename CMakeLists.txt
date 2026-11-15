# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

# Compiler override: point PICO_GCC_PATH at an arm-none-eabi-gcc executable to build with a
# different toolchain than the one the Pico extension installed, e.g.
#   cmake --preset pico2-release -DPICO_GCC_PATH=C:/gcc-arm/bin/arm-none-eabi-gcc.exe
if(WIN32)
    set(gccSuffix ".exe")
else()
    set(gccSuffix "")
endif()
set(PICO_GCC_PATH "${USERHOME}/.pico-sdk/toolchain/${toolchainVersion}/bin/arm-none-eabi-gcc${gccSuffix}"
        CACHE FILEPATH "arm-none-eabi-gcc executable used to build OwlSatOS")
if(EXISTS ${PICO_GCC_PATH})
    get_filename_component(gccBinDir "${PICO_GCC_PATH}" DIRECTORY)
    get_filename_component(PICO_TOOLCHAIN_PATH "${gccBinDir}" DIRECTORY)
    set(ENV{PICO_TOOLCHAIN_PATH} "${PICO_TOOLCHAIN_PATH}")
else()
    message(WARNING "PICO_GCC_PATH does not exist (${PICO_GCC_PATH}); falling back to PICO_TOOLCHAIN_PATH")
endif()

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

# FreeRTOS
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/freertos)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

project(OwlSatOS C CXX ASM)

pico_sdk_init()

# CONFIGURE_DEPENDS makes the build system re-run the glob on every build, so adding a file to
# src/ picks it up without a manual re-configure. Without it, a new source silently does not
# compile and surfaces as an undefined reference at link time.
FILE(GLOB OWLSAT_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)

add_executable(OwlSatOS ${OWLSAT_SOURCES})

pico_set_program_name(OwlSatOS "OwlSatOS")
pico_set_program_version(OwlSatOS "0.1")

target_link_libraries(OwlSatOS #PRIVATE
        pico_stdlib
        hardware_pwm
        hardware_flash
        pico_flash
        FreeRTOS-Kernel-Core
        FreeRTOS-Kernel-Heap4
)

# The MRAM driver suspends XIP through flash_safe_execute(). FreeRTOS runs on core 0 only and
# core 1 is never started, so pico_flash only has to mask interrupts.
target_compile_definitions(OwlSatOS PRIVATE PICO_FLASH_ASSUME_CORE1_SAFE=1)

pico_enable_stdio_usb(OwlSatOS 1)
pico_enable_stdio_uart(OwlSatOS 0)

target_include_directories(OwlSatOS PRIVATE
        ./
        ${CMAKE_CURRENT_LIST_DIR}
        ${FREERTOS_KERNEL_PATH}/include
        ${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/non_secure
        include/)

pico_add_extra_outputs(OwlSatOS)

# Bench-only MRAM interface-mode probe (docs/mram_mode_probe.md). Loaded into SRAM over BOOTSEL and
# never written to the MRAM. Shares the MRAM driver with the flight image.
file(GLOB MRAM_Probe_SOURCES CONFIGURE_DEPENDS mram-probe/src/*.cpp mram-probe/src/*.c)
add_executable(MRAM_Probe ${MRAM_Probe_SOURCES} src/mram_transactions.cpp)

target_link_libraries(MRAM_Probe pico_stdlib pico_bootrom hardware_watchdog)

pico_set_binary_type(MRAM_Probe no_flash)      # runs from SRAM, never touches the MRAM for booting
pico_enable_stdio_usb(MRAM_Probe 1)
pico_enable_stdio_uart(MRAM_Probe 0)
pico_add_extra_outputs(MRAM_Probe)

target_include_directories(MRAM_Probe PRIVATE
        ./
        ${CMAKE_CURRENT_LIST_DIR}
        include/
)

# Bench-only MRAM driver API tests (mram-test/src/mram_api_test.cpp). SRAM-only like the probe, with
# stdio on UART0 (GP0 TX / GP1 RX, 115200) for a debug probe. MRAM_Test_Read only reads;
# MRAM_Test_Write also writes a register (same value) and the upper half of the array (restored).
foreach(MRAM_TEST_VARIANT IN ITEMS Read Write)
    set(MRAM_TEST_TARGET MRAM_Test_${MRAM_TEST_VARIANT})
    add_executable(${MRAM_TEST_TARGET} mram-test/src/mram_api_test.cpp src/mram.cpp src/mram_transactions.cpp)

    if(MRAM_TEST_VARIANT STREQUAL "Write")
        target_compile_definitions(${MRAM_TEST_TARGET} PRIVATE MRAM_TEST_WRITES=1)
    else()
        target_compile_definitions(${MRAM_TEST_TARGET} PRIVATE MRAM_TEST_WRITES=0)
    endif()

    target_link_libraries(${MRAM_TEST_TARGET} pico_stdlib pico_bootrom)
    target_include_directories(${MRAM_TEST_TARGET} PRIVATE include/)

    pico_set_binary_type(${MRAM_TEST_TARGET} no_flash)
    pico_enable_stdio_usb(${MRAM_TEST_TARGET} 0)
    pico_enable_stdio_uart(${MRAM_TEST_TARGET} 1)
    pico_add_extra_outputs(${MRAM_TEST_TARGET})
endforeach()

# Bench-only: puts the MRAM into pre-flight storage mode (Config::MRAMPreflight) and verifies it.
# SRAM-only, stdio on UART0 for the debug probe. Writes nonvolatile registers; idempotent.
add_executable(MRAM_Preflight mram-preflight/src/mram_preflight.cpp src/mram.cpp src/mram_transactions.cpp)
target_link_libraries(MRAM_Preflight pico_stdlib pico_bootrom)
target_include_directories(MRAM_Preflight PRIVATE include/)
pico_set_binary_type(MRAM_Preflight no_flash)
pico_enable_stdio_usb(MRAM_Preflight 0)
pico_enable_stdio_uart(MRAM_Preflight 1)
pico_add_extra_outputs(MRAM_Preflight)

# The same suite from a FreeRTOS task in an image that executes from the MRAM, through the driver's
# XIP sessions. Flashing it REPLACES the image in the bottom of the MRAM (e.g. blinky).
add_executable(MRAM_Test_XIP mram-test/src/mram_api_test.cpp src/mram.cpp src/mram_transactions.cpp)
target_compile_definitions(MRAM_Test_XIP PRIVATE MRAM_TEST_WRITES=1 MRAM_TEST_XIP=1 PICO_FLASH_ASSUME_CORE1_SAFE=1)
target_link_libraries(MRAM_Test_XIP pico_stdlib pico_bootrom hardware_flash pico_flash FreeRTOS-Kernel-Core FreeRTOS-Kernel-Heap4)
target_include_directories(MRAM_Test_XIP PRIVATE
        ${FREERTOS_KERNEL_PATH}/include
        ${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/non_secure
        include/)
pico_enable_stdio_usb(MRAM_Test_XIP 0)
pico_enable_stdio_uart(MRAM_Test_XIP 1)
pico_add_extra_outputs(MRAM_Test_XIP)

