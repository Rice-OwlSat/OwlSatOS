Write-Output "Initializing submodules..."
$Error.Clear()
$output = & git submodule update --init --recursive
if ($Error[0]) {
    Write-Output "Problem: $($output)"
}

$content = @'
# =============================================================================
# settings.cmake — machine- and project-specific configuration.
#
# This is the ONE file to edit when moving the project to a new machine or a new
# board. It is read early by toolchain.cmake (before project()) and again by
# CMakeLists.txt, so every value below is visible to both.
#
# Precedence for each value: a -D<NAME>=... on the cmake command line or an
# environment variable of the same name wins; otherwise the default here is used.
# (Prefer editing this file — command-line -D of non-standard cache vars is not
# forwarded into CMake's internal try_compile step, the defaults here are.)
# =============================================================================

# Set <name> to <value> unless it is already defined as a normal/cache variable
# or as an environment variable of the same name.
function(_setting name value)
    if(DEFINED ${name})
        return()
    endif()
    if(DEFINED ENV{${name}})
        set(${name} "$ENV{${name}}" PARENT_SCOPE)
    else()
        set(${name} "${value}" PARENT_SCOPE)
    endif()
endfunction()

# --- Compiler ----------------------------------------------------------------
# "clang" = Arm Toolchain for Embedded (ATfE / LLVM-based).
# "gcc"   = Arm GNU Toolchain (arm-none-eabi-gcc); uses the SDK's stock toolchain.
_setting(PICO_COMPILER_KIND "clang")

# --- Toolchain locations (machine-specific) ----------------------------------
# ATfE install root (used when PICO_COMPILER_KIND = clang).
_setting(ATFE_PATH "PATH/TO/ATfE-version-platform-architecture")
# arm-none-eabi-gcc install root (used when PICO_COMPILER_KIND = gcc).
# Leave blank to find arm-none-eabi-gcc on the system PATH.
_setting(GCC_TOOLCHAIN_PATH "PATH/TO/ARM-NONE-EABI-GCC")

# --- SDK & libraries (machine-specific) --------------------------------------
_setting(PICO_SDK_PATH "PATH/TO/pico-sdk-version")
# FreeRTOS-Kernel checkout. Leave blank if the project does not use FreeRTOS;
# when set, CMakeLists.txt wires the kernel in automatically.
_setting(FREERTOS_KERNEL_PATH "")

# --- Target (project-specific) -----------------------------------------------
_setting(PICO_PLATFORM "rp2350b")
_setting(PICO_BOARD    "")
# Extra directories to search for "<PICO_BOARD>.h" (for custom/out-of-tree
# boards, e.g. an RP2350B board). Point this at your project's boards/ dir.
_setting(PICO_BOARD_HEADER_DIRS "")

# --- Language standards -------------------------------------------------------
_setting(PROJECT_C_STANDARD   "11")
_setting(PROJECT_CXX_STANDARD "20")

'@

New-Item -Name settings.cmake -ItemType "File" -Value