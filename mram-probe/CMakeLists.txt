
cmake_minimum_required(VERSION 3.13)

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

project(OwlSatOS C CXX ASM)

pico_sdk_init()



