/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The vendored sodium pair compiled into this translation unit, so the test
// (tests/test_sodium_dispatch.cpp) can read and set the implementation each
// runtime dispatch picked: ChaCha20 (ref, SSSE3, AVX2, NEON), Poly1305
// (donna, SSE2, mul64), BLAKE2b (ref, SSSE3, SSE4.1, AVX2) and the IETF AEAD
// (composed, neon-fused). The library's own copy is a separate object; this
// binary does not link it. Test code only.

#include "sodium.c"

#include "sodium_dispatch_seam.h"

#include <string.h>

int sodium_dispatch_init( void )
{
    return sodium_init();
}

// The CPU features sodium detected, and the picks they made.

int sodium_dispatch_has( const char * feature )
{
    if ( strcmp( feature, "sse2" ) == 0 )
    {
        return _cpu_features.has_sse2;
    }
    if ( strcmp( feature, "ssse3" ) == 0 )
    {
        return _cpu_features.has_ssse3;
    }
    if ( strcmp( feature, "sse41" ) == 0 )
    {
        return _cpu_features.has_sse41;
    }
    if ( strcmp( feature, "avx" ) == 0 )
    {
        return _cpu_features.has_avx;
    }
    if ( strcmp( feature, "avx2" ) == 0 )
    {
        return _cpu_features.has_avx2;
    }
    if ( strcmp( feature, "neon" ) == 0 )
    {
        return _cpu_features.has_neon;
    }
    return -1;
}

const char * sodium_dispatch_aead( void )
{
    return implementation__aead->name;
}

const char * sodium_dispatch_chacha20( void )
{
#if defined( SODIUM_ROCKETNET_ARM64 )
    if ( implementation__cc20 == &crypto_stream_chacha20_neon_implementation )
    {
        return "neon";
    }
#endif
#if defined( HAVE_AVX2INTRIN_H ) && defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
    if ( implementation__cc20 == &crypto_stream_chacha20_dolbeau_avx2_implementation )
    {
        return "avx2";
    }
#endif
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H )
    if ( implementation__cc20 == &crypto_stream_chacha20_dolbeau_ssse3_implementation )
    {
        return "ssse3";
    }
#endif
    if ( implementation__cc20 == &crypto_stream_chacha20_ref_implementation )
    {
        return "ref";
    }
    return "?";
}

const char * sodium_dispatch_poly1305( void )
{
#if defined( SODIUM_ROCKETNET_ARM64 )
    if ( implementation__poly == &crypto_onetimeauth_poly1305_mul64_implementation )
    {
        return "mul64";
    }
#endif
#if defined( HAVE_TI_MODE ) && defined( HAVE_EMMINTRIN_H )
    if ( implementation__poly == &crypto_onetimeauth_poly1305_sse2_implementation )
    {
        return "sse2";
    }
#endif
    if ( implementation__poly == &crypto_onetimeauth_poly1305_donna_implementation )
    {
        return "ref";
    }
    return "?";
}

const char * sodium_dispatch_blake2b( void )
{
#if defined( HAVE_AVX2INTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
    if ( blake2b_compress == blake2b_compress_avx2 )
    {
        return "avx2";
    }
#endif
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
    if ( blake2b_compress == blake2b_compress_sse41 )
    {
        return "sse41";
    }
#endif
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H )
    if ( blake2b_compress == blake2b_compress_ssse3 )
    {
        return "ssse3";
    }
#endif
    if ( blake2b_compress == blake2b_compress_ref )
    {
        return "ref";
    }
    return "?";
}

// Whether this build contains a detection or an implementation: "cpu" with
// "avx" or "avx2" (the AVX detection itself), or a primitive and a name.

// The baseline reads XCR0, without which AVX is never detected, under exactly
// these conditions (_sodium_runtime_intel_cpu_features in sodium.c; sodium/
// README.md, "CPU feature detection").
#if defined( HAVE__XGETBV ) || ( defined( _MSC_VER ) && defined( _XCR_XFEATURE_ENABLED_MASK ) && _MSC_FULL_VER >= 160040219 ) || \
    ( defined( _MSC_VER ) && defined( _M_IX86 ) ) || defined( HAVE_AVX_ASM )
#define SODIUM_DISPATCH_SEAM_READS_XCR0 1
#endif

int sodium_dispatch_built( const char * primitive, const char * name )
{
    const int ref = strcmp( name, "ref" ) == 0;
    if ( strcmp( primitive, "cpu" ) == 0 )
    {
#if defined( HAVE_AVXINTRIN_H ) && defined( SODIUM_DISPATCH_SEAM_READS_XCR0 )
        if ( strcmp( name, "avx" ) == 0 )
        {
            return 1;
        }
#endif
#if defined( HAVE_AVX2INTRIN_H ) && defined( SODIUM_DISPATCH_SEAM_READS_XCR0 )
        if ( strcmp( name, "avx2" ) == 0 )
        {
            return 1;
        }
#endif
        return 0;
    }
    if ( strcmp( primitive, "aead" ) == 0 )
    {
#if defined( SODIUM_ROCKETNET_ARM64 )
        if ( strcmp( name, "neon-fused" ) == 0 )
        {
            return 1;
        }
#endif
        return strcmp( name, "composed" ) == 0;
    }
    if ( strcmp( primitive, "chacha20" ) == 0 )
    {
#if defined( SODIUM_ROCKETNET_ARM64 )
        if ( strcmp( name, "neon" ) == 0 )
        {
            return 1;
        }
#endif
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H )
        if ( strcmp( name, "ssse3" ) == 0 )
        {
            return 1;
        }
#endif
#if defined( HAVE_AVX2INTRIN_H ) && defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
        if ( strcmp( name, "avx2" ) == 0 )
        {
            return 1;
        }
#endif
        return ref;
    }
    if ( strcmp( primitive, "poly1305" ) == 0 )
    {
#if defined( SODIUM_ROCKETNET_ARM64 )
        if ( strcmp( name, "mul64" ) == 0 )
        {
            return 1;
        }
#endif
#if defined( HAVE_TI_MODE ) && defined( HAVE_EMMINTRIN_H )
        if ( strcmp( name, "sse2" ) == 0 )
        {
            return 1;
        }
#endif
        return ref;
    }
    if ( strcmp( primitive, "blake2b" ) == 0 )
    {
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H )
        if ( strcmp( name, "ssse3" ) == 0 )
        {
            return 1;
        }
#endif
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
        if ( strcmp( name, "sse41" ) == 0 )
        {
            return 1;
        }
#endif
#if defined( HAVE_AVX2INTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
        if ( strcmp( name, "avx2" ) == 0 )
        {
            return 1;
        }
#endif
        return ref;
    }
    return 0;
}

// Select one implementation by name. Returns 0 when this build does not
// contain it or the CPU sodium detected cannot run it, 1 when selected.

int sodium_dispatch_select_chacha20( const char * name )
{
    if ( strcmp( name, "ref" ) == 0 )
    {
        implementation__cc20 = &crypto_stream_chacha20_ref_implementation;
        return 1;
    }
#if defined( SODIUM_ROCKETNET_ARM64 )
    if ( strcmp( name, "neon" ) == 0 && _cpu_features.has_neon )
    {
        implementation__cc20 = &crypto_stream_chacha20_neon_implementation;
        return 1;
    }
#endif
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H )
    if ( strcmp( name, "ssse3" ) == 0 && _cpu_features.has_ssse3 )
    {
        implementation__cc20 = &crypto_stream_chacha20_dolbeau_ssse3_implementation;
        return 1;
    }
#endif
#if defined( HAVE_AVX2INTRIN_H ) && defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
    if ( strcmp( name, "avx2" ) == 0 && _cpu_features.has_avx2 )
    {
        implementation__cc20 = &crypto_stream_chacha20_dolbeau_avx2_implementation;
        return 1;
    }
#endif
    return 0;
}

int sodium_dispatch_select_poly1305( const char * name )
{
    if ( strcmp( name, "ref" ) == 0 )
    {
        implementation__poly = &crypto_onetimeauth_poly1305_donna_implementation;
        return 1;
    }
#if defined( SODIUM_ROCKETNET_ARM64 )
    if ( strcmp( name, "mul64" ) == 0 && _cpu_features.has_neon )
    {
        implementation__poly = &crypto_onetimeauth_poly1305_mul64_implementation;
        return 1;
    }
#endif
#if defined( HAVE_TI_MODE ) && defined( HAVE_EMMINTRIN_H )
    if ( strcmp( name, "sse2" ) == 0 && _cpu_features.has_sse2 )
    {
        implementation__poly = &crypto_onetimeauth_poly1305_sse2_implementation;
        return 1;
    }
#endif
    return 0;
}

int sodium_dispatch_select_blake2b( const char * name )
{
    if ( strcmp( name, "ref" ) == 0 )
    {
        blake2b_compress = blake2b_compress_ref;
        return 1;
    }
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H )
    if ( strcmp( name, "ssse3" ) == 0 && _cpu_features.has_ssse3 )
    {
        blake2b_compress = blake2b_compress_ssse3;
        return 1;
    }
#endif
#if defined( HAVE_EMMINTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
    if ( strcmp( name, "sse41" ) == 0 && _cpu_features.has_sse41 )
    {
        blake2b_compress = blake2b_compress_sse41;
        return 1;
    }
#endif
#if defined( HAVE_AVX2INTRIN_H ) && defined( HAVE_TMMINTRIN_H ) && defined( HAVE_SMMINTRIN_H )
    if ( strcmp( name, "avx2" ) == 0 && _cpu_features.has_avx2 )
    {
        blake2b_compress = blake2b_compress_avx2;
        return 1;
    }
#endif
    return 0;
}

int sodium_dispatch_select_aead( const char * name )
{
    if ( strcmp( name, "composed" ) == 0 )
    {
        implementation__aead = &crypto_aead_chacha20poly1305_ietf_composed_implementation;
        return 1;
    }
#if defined( SODIUM_ROCKETNET_ARM64 )
    if ( strcmp( name, "neon-fused" ) == 0 && _cpu_features.has_neon )
    {
        implementation__aead = &crypto_aead_chacha20poly1305_ietf_neon_fused_implementation;
        return 1;
    }
#endif
    return 0;
}

// The picks rerun the way sodium_init runs them, which reads the forced
// fallback (ROCKETNET_SODIUM_PORTABLE) again.

void sodium_dispatch_repick( void )
{
    _crypto_onetimeauth_poly1305_pick_best_implementation();
    _crypto_stream_chacha20_pick_best_implementation();
    _crypto_aead_chacha20poly1305_ietf_pick_best_implementation();
    _crypto_generichash_blake2b_pick_best_implementation();
}

// Detection forced: clear AVX and AVX2 (as on a CPU or OS without YMM
// state), or detect again, then rerun the picks.

void sodium_dispatch_force_no_avx2( int no_avx2 )
{
    _sodium_runtime_get_cpu_features();
    if ( no_avx2 )
    {
        _cpu_features.has_avx = 0;
        _cpu_features.has_avx2 = 0;
    }
    sodium_dispatch_repick();
}
