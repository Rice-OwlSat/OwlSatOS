/*
 * ATfE / clang-22 compatibility override for the Pico SDK 2.2.0.
 *
 * This file shadows the SDK's <hardware/sync.h> by sitting earlier on the
 * include path (see sdk_overrides being added in toolchain.cmake). It pulls in
 * the real SDK header verbatim via #include_next, then fixes up the two macros
 * the SDK gets wrong under ATfE's clang 22 -- nothing else is touched, so the
 * SDK install stays pristine and we stay forward-compatible with the rest of
 * the header.
 *
 * The bug: on the clang path the SDK defines
 *     #define remove_volatile_cast(t, x) \
 *         ({ ...; (t)(x); Clang_Pragma("clang diagnostic pop"); })
 * A GNU statement-expression takes the value of its *last* statement, which here
 * is the trailing _Pragma -- so the whole expression evaluates to `void`. Older
 * clang releases (the ~17 in the legacy "LLVM embedded toolchain for Arm")
 * tolerated this; clang 22 does not, so pico_atomic/atomic.c fails with
 *     error: passing 'void' to parameter of incompatible type 'const void *'
 * We redefine the macros to the same plain cast the SDK already uses on GCC.
 */
#ifndef OVERRIDE_SYNC_HEADER_FOR_CLANG_BECAUSE_PICO_SDK_SUCKS
#define OVERRIDE_SYNC_HEADER_FOR_CLANG_BECAUSE_PICO_SDK_SUCKS

#include_next <hardware/sync.h>

#if defined(remove_volatile_cast) && !defined(PICO_ATFE_SYNC_CAST_FIXED)
#define PICO_ATFE_SYNC_CAST_FIXED 1
#undef remove_volatile_cast
#undef remove_volatile_cast_no_barrier
#define remove_volatile_cast(t, x) (t)(x)
#define remove_volatile_cast_no_barrier(t, x) (t)(x)
#endif

#endif