/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The floating-point floor canary, shared by the unit suite (tests/test.cpp)
// and test_fp_contract_fma (tests/fp_contract_fma.cpp), which compiles it with
// FMA code generation so the floor is exercised on targets whose baseline has
// no FMA.

#pragma once

#ifndef ROCKETNET_TESTS_FP_CONTRACT_CANARY_H
#define ROCKETNET_TESTS_FP_CONTRACT_CANARY_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined( _MSC_VER )
#define ROCKETNET_TEST_FP_NOINLINE __declspec(noinline)
#elif defined( __GNUC__ ) || defined( __clang__ )
#define ROCKETNET_TEST_FP_NOINLINE __attribute__((noinline))
#else
#define ROCKETNET_TEST_FP_NOINLINE
#endif // #if defined( _MSC_VER )

// Canary: the estate's strict floating-point floor is -ffp-contract=off
// (GCC/Clang), /fp:precise (MSVC) or /fp:precise -Xclang -ffp-contract=off
// (clang-cl). Compiled with contraction enabled, a*b+c
// here rounds once into an FMA and changes the result; under the floor it takes
// two roundings. The inputs ride through volatile so nothing folds at compile
// time, and the arithmetics live in out-of-line helpers so the backend is free
// to contract (and the probe is about the setting, not common-subexpression
// folding). One rounding yields 0x33800000 (float) / 0x3C90000000000000
// (double); the floor yields 0x00000000 / 0x0000000000000000.

ROCKETNET_TEST_FP_NOINLINE static int rocketnet_fp_check_float( float a, float b, float c )
{
    const float r = a * b + c;
    uint32_t bits;
    memcpy( &bits, &r, sizeof( bits ) );

    if ( bits != UINT32_C( 0x00000000 ) )
    {
        printf( "FAIL test_fp_contract_off: float a*b+c bits 0x%08X, expected 0x00000000\n", (unsigned) bits );
        return 1;
    }

    return 0;
}

ROCKETNET_TEST_FP_NOINLINE static int rocketnet_fp_check_double( double a, double b, double c )
{
    const double r = a * b + c;
    uint64_t bits;
    memcpy( &bits, &r, sizeof( bits ) );

    if ( bits != UINT64_C( 0x0000000000000000 ) )
    {
        printf( "FAIL test_fp_contract_off: double a*b+c bits 0x%016llX, expected 0x0000000000000000\n", (unsigned long long) bits );
        return 1;
    }

    return 0;
}

static inline int test_fp_contract_off()
{
    const uint32_t ab_bits = UINT32_C( 0x3F800800 );
    const uint32_t c_bits = UINT32_C( 0xBF801000 );
    volatile float av, bv, cv;
    float a, b, c;
    memcpy( &a, &ab_bits, sizeof( a ) );
    memcpy( &b, &ab_bits, sizeof( b ) );
    memcpy( &c, &c_bits, sizeof( c ) );
    av = a;
    bv = b;
    cv = c;

    const uint64_t dab_bits = UINT64_C( 0x3FF0000002000000 );
    const uint64_t dc_bits = UINT64_C( 0xBFF0000004000000 );
    volatile double dav, dbv, dcv;
    double da, db, dc;
    memcpy( &da, &dab_bits, sizeof( da ) );
    memcpy( &db, &dab_bits, sizeof( db ) );
    memcpy( &dc, &dc_bits, sizeof( dc ) );
    dav = da;
    dbv = db;
    dcv = dc;

    int rc = rocketnet_fp_check_float( av, bv, cv );
    rc |= rocketnet_fp_check_double( dav, dbv, dcv );
    return rc;
}

#endif // ROCKETNET_TESTS_FP_CONTRACT_CANARY_H
