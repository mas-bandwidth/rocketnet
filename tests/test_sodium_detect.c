/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The vendored pair's x86 AVX and AVX2 detection
// (_sodium_runtime_intel_cpu_features), fed chosen CPUID and XCR0 values
// through its test seam (SODIUM_RUNTIME_TEST_CPUID, SODIUM_RUNTIME_TEST_XCR0;
// sodium/README.md, "CPU feature detection"):
//
// - AVX only with AVX, XSAVE and OSXSAVE in CPUID leaf 1 and both SSE and YMM
//   state in XCR0; XCR0 is not read without OSXSAVE;
// - AVX2 only with AVX and the AVX2 bit of leaf 7, and leaf 7 is read only
//   when leaf 0 reports it.
//
// A detection that drops the OSXSAVE check, the XCR0 check or the highest
// leaf check fails here by name. The pair is compiled into this binary; it
// does not link the library. No time, socket or network.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int fed_max_leaf;
static unsigned int fed_leaf1_ecx;
static unsigned int fed_leaf7_ebx;
static uint32_t fed_xcr0;
static int leaf7_reads;
static int xcr0_reads;

static void fed_cpuid( unsigned int cpu_info[4], unsigned int cpu_info_type )
{
    memset( cpu_info, 0, 4 * sizeof( cpu_info[0] ) );
    if ( cpu_info_type == 0U )
    {
        cpu_info[0] = fed_max_leaf;
    }
    else if ( cpu_info_type == 1U )
    {
        cpu_info[2] = fed_leaf1_ecx;
        cpu_info[3] = 0x04000000U; // SSE2
    }
    else if ( cpu_info_type == 7U )
    {
        leaf7_reads++;
        cpu_info[1] = fed_leaf7_ebx;
    }
}

static uint32_t fed_xgetbv( void )
{
    xcr0_reads++;
    return fed_xcr0;
}

#define SODIUM_RUNTIME_TEST_CPUID( info, type ) fed_cpuid( ( info ), ( type ) )
#define SODIUM_RUNTIME_TEST_XCR0() fed_xgetbv()

#include "sodium.c"

#define LEAF1_AVX 0x10000000U
#define LEAF1_XSAVE 0x04000000U
#define LEAF1_OSXSAVE 0x08000000U
#define LEAF7_AVX2 0x00000020U

static int cases = 0;

static void detect( const char * name, unsigned int max_leaf, unsigned int leaf1_ecx, uint32_t xcr0, unsigned int leaf7_ebx, int want_avx, int want_avx2, int want_xcr0_reads, int want_leaf7_reads )
{
    CPUFeatures features;
    memset( &features, 0, sizeof( features ) );
    fed_max_leaf = max_leaf;
    fed_leaf1_ecx = leaf1_ecx;
    fed_xcr0 = xcr0;
    fed_leaf7_ebx = leaf7_ebx;
    leaf7_reads = 0;
    xcr0_reads = 0;
    (void) _sodium_runtime_intel_cpu_features( &features );
    if ( features.has_avx != want_avx || features.has_avx2 != want_avx2 || xcr0_reads != want_xcr0_reads || leaf7_reads != want_leaf7_reads )
    {
        fprintf( stderr, "sodium detect check failed: %s: avx=%d avx2=%d xcr0 reads=%d leaf 7 reads=%d, want %d %d %d %d\n", name, features.has_avx, features.has_avx2, xcr0_reads, leaf7_reads, want_avx,
                 want_avx2, want_xcr0_reads, want_leaf7_reads );
        exit( 1 );
    }
    cases++;
}

int main( void )
{
    (void) _cpuid;
#if defined( HAVE_AVXINTRIN_H ) && defined( HAVE_AVX2INTRIN_H ) && !defined( SODIUM_RUNTIME_NO_XCR0_READ )
    const unsigned int all = LEAF1_AVX | LEAF1_XSAVE | LEAF1_OSXSAVE;
    detect( "everything present", 7, all, 6, LEAF7_AVX2, 1, 1, 1, 1 );
    detect( "no AVX2 bit", 7, all, 6, 0, 1, 0, 1, 1 );
    detect( "no OSXSAVE: XCR0 not read", 7, LEAF1_AVX | LEAF1_XSAVE, 6, LEAF7_AVX2, 0, 0, 0, 0 );
    detect( "no XSAVE", 7, LEAF1_AVX | LEAF1_OSXSAVE, 6, LEAF7_AVX2, 0, 0, 0, 0 );
    detect( "no AVX bit", 7, LEAF1_XSAVE | LEAF1_OSXSAVE, 6, LEAF7_AVX2, 0, 0, 0, 0 );
    detect( "XCR0 without YMM state", 7, all, 2, LEAF7_AVX2, 0, 0, 1, 0 );
    detect( "XCR0 without SSE state", 7, all, 4, LEAF7_AVX2, 0, 0, 1, 0 );
    detect( "XCR0 zero", 7, all, 0, LEAF7_AVX2, 0, 0, 1, 0 );
    detect( "highest leaf 6: leaf 7 not read", 6, all, 6, LEAF7_AVX2, 1, 0, 1, 0 );
    detect( "highest leaf 1", 1, all, 6, LEAF7_AVX2, 1, 0, 1, 0 );
    detect( "XCR0 with more state", 7, all, 0xE7, LEAF7_AVX2, 1, 1, 1, 1 );
    printf( "test_sodium_detect: %d cases, AVX and AVX2 detected exactly when CPUID and XCR0 allow\n", cases );
#else
    printf( "test_sodium_detect: this build has no x86 AVX detection\n" );
#endif
    return 0;
}
