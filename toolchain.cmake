# =============================================================================
# Custom Pico SDK 2.2.0 toolchain for the RP2350 (Cortex-M33).
#
# All machine-/project-specific knobs live in settings.cmake (paths, compiler
# choice, board, etc.). This file consumes them and configures CMake's compiler.
#
#   PICO_COMPILER_KIND = "gcc"   -> delegate to the SDK's stock arm-none-eabi-gcc
#                                   toolchain (the SDK supports GCC natively).
#   PICO_COMPILER_KIND = "clang" -> the custom ATfE (LLVM) setup below.
#
# Why the clang path is custom
# ----------------------------
# The Pico SDK 2.2.0 clang toolchains (pico_arm_cortex_m33_clang.cmake ->
# util/pico_arm_clang_common.cmake) were written for the old "LLVM embedded
# toolchain for Arm" layout, where each multilib variant was a self-contained
# sysroot (lib/clang-runtimes/arm-none-eabi/<variant>/{include,lib}); the SDK
# probes "<variant>/include/stdio.h" and passes "--sysroot <variant>".
#
# ATfE 22.1.0 reorganised the runtimes: shared headers at
# lib/clang-runtimes/arm-none-eabi/include, per-variant libs at
# arm-none-eabi/<variant>/lib, selected by multilib.yaml, with "_size"-suffixed
# variant names. The SDK's probe finds nothing and aborts with "Could not find
# an llvm runtime". The fix is to let clang's own multilib machinery pick the
# include/lib dirs from the target flags -- no --sysroot needed. For
# -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 ATfE selects
# arm-none-eabi/armv8m.main_hard_fp_exn_rtti_unaligned_size: a genuine Armv8-M
# Main + single-precision-FPU picolibc build matching the RP2350's FPv5-SP FPU.
# =============================================================================

# Load the central configuration (paths, compiler kind, board, ...).
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/settings.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/settings.cmake)
endif()

# Make the SDK path available to try_compile (which re-processes this file in a
# context without the CMake cache) and to pico_sdk_import.cmake.
set(ENV{PICO_SDK_PATH} "${PICO_SDK_PATH}")

# =============================================================================
# GCC branch: hand off entirely to the SDK's stock Cortex-M33 GCC toolchain.
# =============================================================================
if (PICO_COMPILER_KIND STREQUAL "gcc")
    if (GCC_TOOLCHAIN_PATH)
        set(ENV{PICO_TOOLCHAIN_PATH} "${GCC_TOOLCHAIN_PATH}")
        set(PICO_TOOLCHAIN_PATH "${GCC_TOOLCHAIN_PATH}" CACHE PATH "Path to arm-none-eabi-gcc")
    endif()
    include(${PICO_SDK_PATH}/cmake/preload/toolchains/pico_arm_cortex_m33_gcc.cmake)

# =============================================================================
# Clang branch: Arm Toolchain for Embedded (ATfE).
# =============================================================================
else()
    # --- Locate ATfE --------------------------------------------------------
    # Export from the source of truth (ATFE_PATH); the SDK's find_compiler reads
    # ENV{PICO_TOOLCHAIN_PATH}. FORCE so a stale/empty cache entry can't win.
    set(ENV{PICO_TOOLCHAIN_PATH} "${ATFE_PATH}")
    set(PICO_TOOLCHAIN_PATH "${ATFE_PATH}" CACHE PATH "Path to the ATfE install root" FORCE)

    set(_pico_toolchain_util "${PICO_SDK_PATH}/cmake/preload/toolchains/util")
    if (NOT EXISTS "${_pico_toolchain_util}/find_compiler.cmake")
        message(FATAL_ERROR
            "Pico SDK toolchain helpers not found under '${_pico_toolchain_util}'. "
            "Set PICO_SDK_PATH in settings.cmake to your Pico SDK 2.2.0 checkout.")
    endif()

    # Use the SDK's PICO platform module just like the stock SDK toolchains do.
    set(CMAKE_SYSTEM_NAME PICO)
    set(CMAKE_SYSTEM_PROCESSOR cortex-m33)

    # --- Find the ATfE binaries --------------------------------------------
    include(${_pico_toolchain_util}/find_compiler.cmake)

    pico_find_compiler(PICO_COMPILER_CC clang)
    pico_find_compiler(PICO_COMPILER_CXX clang++)
    set(PICO_COMPILER_ASM "${PICO_COMPILER_CC}" CACHE INTERNAL "")
    pico_find_compiler(PICO_OBJCOPY llvm-objcopy)
    pico_find_compiler(PICO_OBJDUMP llvm-objdump)

    set(CMAKE_C_COMPILER   "${PICO_COMPILER_CC}"  CACHE FILEPATH "C compiler")
    set(CMAKE_CXX_COMPILER "${PICO_COMPILER_CXX}" CACHE FILEPATH "C++ compiler")
    set(CMAKE_ASM_COMPILER "${PICO_COMPILER_ASM}" CACHE FILEPATH "ASM compiler")
    set(CMAKE_OBJCOPY      "${PICO_OBJCOPY}"      CACHE FILEPATH "")
    set(CMAKE_OBJDUMP      "${PICO_OBJDUMP}"      CACHE FILEPATH "")

    # Workaround for projects that don't enable the ASM language (mirrors the SDK).
    set(CMAKE_ASM_COMPILE_OBJECT "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT>   -c <SOURCE>")
    set(CMAKE_INCLUDE_FLAG_ASM "-I")

    foreach(LANG IN ITEMS C CXX ASM)
        set(CMAKE_${LANG}_OUTPUT_EXTENSION .o)
    endforeach()

    # Restrict find_* to the toolchain tree for includes/libs, but allow host
    # programs to be found normally (mirrors the SDK).
    get_filename_component(PICO_COMPILER_DIR "${PICO_COMPILER_CC}" DIRECTORY)
    get_filename_component(CMAKE_FIND_ROOT_PATH "${PICO_COMPILER_DIR}" DIRECTORY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

    # Clang prefers -Oz over CMake's default -Os for minimum size.
    foreach(LANG IN ITEMS C CXX ASM)
        set(CMAKE_${LANG}_FLAGS_MINSIZEREL_INIT "-Oz -DNDEBUG")
    endforeach()

    # --- Target flags ------------------------------------------------------
    # Cortex-M33, Thumb, hardware FPU, hard-float ABI. These flags drive ATfE's
    # multilib selection to armv8m.main_hard_fp_exn_rtti_unaligned_size; no
    # --sysroot is needed because clang derives the include/lib dirs from them.
    set(PICO_COMMON_LANG_FLAGS "-mcpu=cortex-m33 --target=arm-none-eabi -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16")
    set(PICO_DISASM_OBJDUMP_ARGS --mcpu=cortex-m33 --arch=armv8m.main+fp+dsp)

    # ATfE's C library is picolibc; tell the SDK to link pico_picolibc_interface.
    set(PICO_CLIB "picolibc" CACHE INTERNAL "")
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PICO_CLIB)

    # Apply the common flag initialisation (CMAKE_<LANG>_FLAGS_INIT, debug/release
    # flavours, build-id, try_compile -nostdlib, etc.).
    include(${_pico_toolchain_util}/set_flags.cmake)

    # --- ATfE / picolibc compatibility shims -------------------------------
    # The SDK's clang support predates ATfE and assumes a newlib-style libc:
    #
    #   * picolibc's <sys/cdefs.h> does not define __printflike (newlib's did),
    #     yet pico/stdio.h uses it -> provide it ourselves.
    #
    #   * Intrinsics __wfe/__sev/__wfi: hardware/sync.h defines them only when
    #     "!__has_builtin(...)", but clang reports __has_builtin(__wfe) true even
    #     though the name is merely declared by <arm_acle.h> and is not a bare
    #     callable builtin. So sync.h skips them and TUs that use them via the
    #     lock_core macros without pulling in arm_acle.h (pico_time/time.c,
    #     pico_util/queue.c) fail. We can't force-include arm_acle.h (its
    #     __nop/__dmb/__dsb/__isb signatures clash with sync.h's own unconditional
    #     definitions), so map exactly these three onto clang's real ARM builtins
    #     -- no header needed, emits wfe/sev/wfi directly, no collision.
    #
    # One space-separated string; each -D is free of internal spaces so the build
    # tool tokenises them correctly.
    set(_atfe_shims "-D__printflike(a,b)=__attribute__((__format__(__printf__,a,b))) -D__wfe()=__builtin_arm_wfe() -D__sev()=__builtin_arm_sev() -D__wfi()=__builtin_arm_wfi()")
    foreach(LANG IN ITEMS C CXX ASM)
        set(CMAKE_${LANG}_FLAGS_INIT "${CMAKE_${LANG}_FLAGS_INIT} ${_atfe_shims}")
    endforeach()

    #   * pico_atomic/atomic.c uses hardware/sync.h's remove_volatile_cast()
    #     macros, whose clang-path statement-expressions end in a trailing
    #     _Pragma and so evaluate to void under clang 22. sync.h #defines them
    #     unconditionally, so we shadow the header with a thin #include_next
    #     wrapper in sdk_overrides/ placed first on the include path (-I is
    #     searched before the SDK's -isystem dirs; the wrapper then #include_next
    #     reaches the real header). C/C++ only; .S never pulls in sync.h.
    get_filename_component(_atfe_overrides "${CMAKE_CURRENT_LIST_DIR}/sdk_overrides" ABSOLUTE)
    foreach(LANG IN ITEMS C CXX)
        set(CMAKE_${LANG}_FLAGS_INIT "${CMAKE_${LANG}_FLAGS_INIT} -I${_atfe_overrides}")
    endforeach()

    # Default the linker to lld (ATfE ships ld.lld, not GNU ld).
    foreach(TYPE IN ITEMS EXE SHARED MODULE)
        set(CMAKE_${TYPE}_LINKER_FLAGS_INIT "-fuse-ld=lld")
    endforeach()
endif()
