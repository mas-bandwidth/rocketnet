/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0-or-later or MBSL-1.0. See LICENSE and MBSL.md. */
#pragma once

#ifndef ROCKETNET_TESTS_RED_ZONES_H
#define ROCKETNET_TESTS_RED_ZONES_H

#include <stddef.h>
#include <stdint.h>

#include "../sdk/rocketnet_memory.h"

// A case that reads or writes a whole block byte for byte (a copy of a
// region to compare after a refused call, a scan for a planted secret, a
// pattern written over a part) crosses the red zones an AddressSanitizer
// build leaves between carved arrays (sdk/rocketnet_memory.h). These helpers
// do that copy, fill and scan outside ASan's checks: a red zone's bytes are
// never written by the library, so a copy of them compares equal and a
// scan finds nothing in them. In every other build they are a plain loop.

#if ROCKETNET_RED_ZONES
#if defined( _MSC_VER ) && !defined( __clang__ )
#define ROCKETNET_TEST_RAW_ACCESS __declspec( no_sanitize_address )
#elif defined( __clang__ )
// clang-cl defines _MSC_VER but refuses the __declspec form (-Wignored-attributes);
// turning off undefined and coverage beside address keeps a whole-block restore
// out of UBSan and libFuzzer's per-edge counter increments
#define ROCKETNET_TEST_RAW_ACCESS __attribute__( ( no_sanitize( "address", "undefined", "coverage" ) ) )
#else
#define ROCKETNET_TEST_RAW_ACCESS __attribute__( ( no_sanitize( "address" ) ) )
#endif // #if defined( _MSC_VER ) && !defined( __clang__ )
#else
#define ROCKETNET_TEST_RAW_ACCESS
#endif // #if ROCKETNET_RED_ZONES

#if defined( __clang__ ) || defined( __GNUC__ )
typedef uint64_t __attribute__( ( __may_alias__ ) ) rocketnet_test_raw_u64_t;
#else
typedef uint64_t rocketnet_test_raw_u64_t;
#endif // #if defined( __clang__ ) || defined( __GNUC__ )

// copy bytes from src to dst; either may hold red zones
ROCKETNET_TEST_RAW_ACCESS static inline void rocketnet_test_raw_copy( void * dst, const void * src, size_t bytes )
{
    size_t i = 0;
#if ROCKETNET_RED_ZONES
    if ( ( ( (uintptr_t) dst | (uintptr_t) src ) & 7u ) == 0 )
    {
        volatile rocketnet_test_raw_u64_t * to64 = (volatile rocketnet_test_raw_u64_t *) dst;
        const volatile rocketnet_test_raw_u64_t * from64 = (const volatile rocketnet_test_raw_u64_t *) src;
        const size_t words = bytes >> 3;
        size_t w = 0;
        for ( ; w + 4 <= words; w += 4 )
        {
            const rocketnet_test_raw_u64_t v0 = from64[w + 0];
            const rocketnet_test_raw_u64_t v1 = from64[w + 1];
            const rocketnet_test_raw_u64_t v2 = from64[w + 2];
            const rocketnet_test_raw_u64_t v3 = from64[w + 3];
            to64[w + 0] = v0;
            to64[w + 1] = v1;
            to64[w + 2] = v2;
            to64[w + 3] = v3;
        }
        for ( ; w < words; w++ )
        {
            to64[w] = from64[w];
        }
        i = words << 3;
    }
#endif // #if ROCKETNET_RED_ZONES

    volatile uint8_t * to = (volatile uint8_t *) dst;
    const volatile uint8_t * from = (const volatile uint8_t *) src;
    for ( ; i < bytes; i++ )
    {
        to[i] = from[i];
    }
}

// set bytes to value; the span may hold red zones
ROCKETNET_TEST_RAW_ACCESS static inline void rocketnet_test_raw_fill( void * dst, uint8_t value, size_t bytes )
{
    size_t i = 0;
#if ROCKETNET_RED_ZONES
    if ( ( (uintptr_t) dst & 7u ) == 0 )
    {
        const rocketnet_test_raw_u64_t word = (uint64_t) value * 0x0101010101010101ULL;
        volatile rocketnet_test_raw_u64_t * to64 = (volatile rocketnet_test_raw_u64_t *) dst;
        const size_t words = bytes >> 3;
        for ( size_t w = 0; w < words; w++ )
        {
            to64[w] = word;
        }
        i = words << 3;
    }
#endif // #if ROCKETNET_RED_ZONES

    volatile uint8_t * to = (volatile uint8_t *) dst;
    for ( ; i < bytes; i++ )
    {
        to[i] = value;
    }
}

// 0 when the spans hold the same bytes, as memcmp; either may hold red zones
ROCKETNET_TEST_RAW_ACCESS static inline int rocketnet_test_raw_compare( const void * a, const void * b, size_t bytes )
{
    size_t i = 0;
#if ROCKETNET_RED_ZONES
    if ( ( ( (uintptr_t) a | (uintptr_t) b ) & 7u ) == 0 )
    {
        const volatile rocketnet_test_raw_u64_t * x64 = (const volatile rocketnet_test_raw_u64_t *) a;
        const volatile rocketnet_test_raw_u64_t * y64 = (const volatile rocketnet_test_raw_u64_t *) b;
        const size_t words = bytes >> 3;
        for ( size_t w = 0; w < words; w++ )
        {
            if ( x64[w] != y64[w] )
            {
                i = w << 3;
                break;
            }
            i = ( w + 1 ) << 3;
        }
    }
#endif // #if ROCKETNET_RED_ZONES

    const volatile uint8_t * x = (const volatile uint8_t *) a;
    const volatile uint8_t * y = (const volatile uint8_t *) b;
    for ( ; i < bytes; i++ )
    {
        if ( x[i] != y[i] )
        {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

// one byte; the address may be in a red zone
ROCKETNET_TEST_RAW_ACCESS static inline uint8_t rocketnet_test_raw_byte( const void * p, size_t index )
{
    return ( (const volatile uint8_t *) p )[index];
}

#endif // ROCKETNET_TESTS_RED_ZONES_H
