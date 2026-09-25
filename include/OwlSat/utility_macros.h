/**

    @file      utility_macros.h
    @brief     
    @details   ~
    @author    Viola Case
    @date      23.09.2026
    @copyright © Viola Case, 2026. All right reserved.

**/

#pragma once

#if defined (__GNUC__) || defined (__clang__)
#define OWLSAT_INLINE inline __attribute__((always_inline))
#else
#define OWLSAT_INLINE inline
#endif

