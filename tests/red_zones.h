/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
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
#if defined( _MSC_VER )
#define ROCKETNET_TEST_RAW_ACCESS __declspec( no_sanitize_address )
#else
#define ROCKETNET_TEST_RAW_ACCESS __attribute__( ( no_sanitize( "address" ) ) )
#endif // #if defined( _MSC_VER )
#else
#define ROCKETNET_TEST_RAW_ACCESS
#endif // #if ROCKETNET_RED_ZONES

// copy bytes from src to dst; either may hold red zones
ROCKETNET_TEST_RAW_ACCESS static inline void rocketnet_test_raw_copy( void * dst, const void * src, size_t bytes )
{
    volatile uint8_t * to = (volatile uint8_t *) dst;
    const volatile uint8_t * from = (const volatile uint8_t *) src;
    for ( size_t i = 0; i < bytes; i++ )
    {
        to[i] = from[i];
    }
}

// set bytes to value; the span may hold red zones
ROCKETNET_TEST_RAW_ACCESS static inline void rocketnet_test_raw_fill( void * dst, uint8_t value, size_t bytes )
{
    volatile uint8_t * to = (volatile uint8_t *) dst;
    for ( size_t i = 0; i < bytes; i++ )
    {
        to[i] = value;
    }
}

// 0 when the spans hold the same bytes, as memcmp; either may hold red zones
ROCKETNET_TEST_RAW_ACCESS static inline int rocketnet_test_raw_compare( const void * a, const void * b, size_t bytes )
{
    const volatile uint8_t * x = (const volatile uint8_t *) a;
    const volatile uint8_t * y = (const volatile uint8_t *) b;
    for ( size_t i = 0; i < bytes; i++ )
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
