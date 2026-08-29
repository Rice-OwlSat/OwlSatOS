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
