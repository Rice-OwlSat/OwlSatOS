cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD mainboard CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/settings.cmake)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/toolchain.cmake)


include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)


project(OwlSatOS C CXX ASM)

pico_sdk_init()

FILE(GLOB OWLSAT_SOURCES src/*.c src/*.cpp)

add_executable(OwlSatOS ${OWLSAT_SOURCES})

pico_set_program_name(OwlSatOS "OwlSatOS")
pico_set_program_version(OwlSatOS "0.1")

# FreeRTOS
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/freertos)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

target_link_libraries(OwlSatOS #PRIVATE
        pico_stdlib
        hardware_pwm
        FreeRTOS-Kernel-Core
        FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_usb(OwlSatOS 1)
pico_enable_stdio_uart(OwlSatOS 0)

target_include_directories(OwlSatOS PRIVATE
        ./
        ${CMAKE_CURRENT_LIST_DIR}
        ${FREERTOS_KERNEL_PATH}/include
        ${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/RP2350_ARM_NTZ/non_secure
        include/)

pico_add_extra_outputs(OwlSatOS)

if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(OwlSatOS PRIVATE --target=arm-none-eabi -mcpu=cortex-m33 -mthumb -mfloat-abi=hard)
endif ()

