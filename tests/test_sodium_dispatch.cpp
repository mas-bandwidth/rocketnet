/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The vendored sodium pair's runtime dispatch, judged from outside it.
//
// 1. Detection: the implementation each primitive picked (ChaCha20, Poly1305,
//    BLAKE2b) is the best this build contains that the CPU and OS can run,
//    with the CPU judged independently of sodium (the compiler's
//    __builtin_cpu_supports, which requires the OS to save YMM state for
//    AVX2; __cpuid and _xgetbv on MSVC).
// 2. Known answers: the seven sealed packet vectors of
//    sdk/rocketnet_test_vectors.h reproduce under the detected picks, with
//    AVX and AVX2 detection forced off (as on a CPU without them), and under
//    every ChaCha20 and Poly1305 pair the CPU can run.
// 3. Agreement: every implementation this build contains that the CPU can run
//    gives the same bytes as the portable reference on the same inputs:
//    the IETF AEAD (seal and open, every ChaCha20 x Poly1305 pair) and
//    BLAKE2b (keyed and unkeyed, 32 and 64 byte outputs), every length 0 to
//    2048, from and into unaligned buffers.
//
// The pair is compiled into this binary (tests/sodium_dispatch_seam.c); it
// does not link the library. No time, socket or network.

#include "sodium.h"
#include "sodium_dispatch_seam.h"
#include "../sdk/rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_AMD64 ) || defined( _M_IX86 ) )
#include <intrin.h>
#include <immintrin.h>
#endif

#define CHECK( x )                                                                  \
    do                                                                              \
    {                                                                               \
        if ( !( x ) )                                                               \
        {                                                                           \
            fprintf( stderr, "sodium dispatch check failed: %s at %d\n", #x, __LINE__ ); \
            exit( 1 );                                                              \
        }                                                                           \
    } while ( 0 )

struct cpu_t
{
    bool sse2;
    bool ssse3;
    bool sse41;
    bool avx2;
};

#if defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_AMD64 ) || defined( _M_IX86 ) )
// clang-cl's _xgetbv needs the xsave target feature on the calling function
#if defined( __clang__ )
__attribute__( ( target( "xsave" ) ) )
#endif
static unsigned long long
read_xcr0()
{
    return _xgetbv( 0 );
}
#endif

static cpu_t cpu_oracle()
{
    cpu_t cpu;
    memset( &cpu, 0, sizeof( cpu ) );
    // MSVC and clang-cl read CPUID directly: clang-cl links no compiler-rt for __builtin_cpu_supports
#if ( defined( __GNUC__ ) || defined( __clang__ ) ) && !defined( _MSC_VER ) && ( defined( __x86_64__ ) || defined( __i386__ ) )
    __builtin_cpu_init();
    cpu.sse2 = __builtin_cpu_supports( "sse2" ) != 0;
    cpu.ssse3 = __builtin_cpu_supports( "ssse3" ) != 0;
    cpu.sse41 = __builtin_cpu_supports( "sse4.1" ) != 0;
    cpu.avx2 = __builtin_cpu_supports( "avx2" ) != 0;
#elif defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_AMD64 ) || defined( _M_IX86 ) )
    int info[4];
    __cpuid( info, 1 );
    const unsigned int ecx = (unsigned int) info[2];
    const unsigned int edx = (unsigned int) info[3];
    cpu.sse2 = ( edx >> 26 ) & 1u;
    cpu.ssse3 = ( ecx >> 9 ) & 1u;
    cpu.sse41 = ( ecx >> 19 ) & 1u;
    const bool avx = ( ( ecx >> 28 ) & 1u ) && ( ( ecx >> 27 ) & 1u ) && ( ( ecx >> 26 ) & 1u ) && ( read_xcr0() & 6u ) == 6u;
    int info7[4];
    __cpuidex( info7, 7, 0 );
    cpu.avx2 = avx && ( ( (unsigned int) info7[1] >> 5 ) & 1u );
#endif
    return cpu;
}

static const char * best( const char * primitive, const cpu_t & cpu )
{
    if ( strcmp( primitive, "chacha20" ) == 0 )
    {
        if ( cpu.avx2 && sodium_dispatch_built( "cpu", "avx2" ) && sodium_dispatch_built( primitive, "avx2" ) )
        {
            return "avx2";
        }
        return cpu.ssse3 && sodium_dispatch_built( primitive, "ssse3" ) ? "ssse3" : "ref";
    }
    if ( strcmp( primitive, "poly1305" ) == 0 )
    {
        return cpu.sse2 && sodium_dispatch_built( primitive, "sse2" ) ? "sse2" : "ref";
    }
    if ( cpu.avx2 && sodium_dispatch_built( "cpu", "avx2" ) && sodium_dispatch_built( primitive, "avx2" ) )
    {
        return "avx2";
    }
    if ( cpu.sse41 && sodium_dispatch_built( primitive, "sse41" ) )
    {
        return "sse41";
    }
    return cpu.ssse3 && sodium_dispatch_built( primitive, "ssse3" ) ? "ssse3" : "ref";
}

// the seven sealed packet vectors, as sdk/rocketnet_tests.cpp checks them:
// nonce is four zero bytes then the sequence little endian

static void known_answers()
{
    struct
    {
        const uint8_t * key;
        uint64_t sequence;
        const uint8_t * ad;
        int ad_bytes;
        const uint8_t * plaintext;
        int plaintext_bytes;
        const uint8_t * expected;
        int expected_bytes;
    } vectors[7];

    static const uint8_t response_plaintext[24] = { 0 };

    vectors[0] = { rocketnet_test_handshake_s2c_key, 1, rocketnet_test_challenge_ad, 53, rocketnet_test_server_eph_public, 32, rocketnet_test_challenge_sealed, 48 };
    vectors[1] = { rocketnet_test_handshake_s2c_key, 2, rocketnet_test_denied_ad, 21, NULL, 0, rocketnet_test_denied_sealed, 16 };
    vectors[2] = { rocketnet_test_handshake_c2s_key, 1, rocketnet_test_response_ad, 53, response_plaintext, 24, rocketnet_test_response_sealed, 40 };
    vectors[3] = { rocketnet_test_session_s2c_key, 1, rocketnet_test_keepalive_ad, 21, rocketnet_test_keepalive_plaintext, 15, rocketnet_test_keepalive_sealed, 31 };
    vectors[4] = { rocketnet_test_session_c2s_key, 3, rocketnet_test_payload_ad, 21, rocketnet_test_payload_plaintext, 12, rocketnet_test_payload_sealed, 28 };
    vectors[5] = { rocketnet_test_session_c2s_key, 4, rocketnet_test_disconnect_ad, 21, rocketnet_test_disconnect_plaintext, 7, rocketnet_test_disconnect_sealed, 23 };
    vectors[6] = { rocketnet_test_session_c2s_key, 5, rocketnet_test_ack_ad, 21, rocketnet_test_ack_plaintext, 11, rocketnet_test_ack_sealed, 27 };

    for ( int i = 0; i < 7; i++ )
    {
        uint8_t nonce[12];
        memset( nonce, 0, sizeof( nonce ) );
        for ( int b = 0; b < 8; b++ )
        {
            nonce[4 + b] = (uint8_t) ( vectors[i].sequence >> ( 8 * b ) );
        }

        uint8_t sealed[64];
        unsigned long long sealed_length = 0;
        CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( sealed, &sealed_length, vectors[i].plaintext, (unsigned long long) vectors[i].plaintext_bytes, vectors[i].ad, (unsigned long long) vectors[i].ad_bytes, NULL, nonce, vectors[i].key ) == 0 );
        CHECK( (int) sealed_length == vectors[i].expected_bytes );
        CHECK( memcmp( sealed, vectors[i].expected, sealed_length ) == 0 );

        uint8_t opened[64];
        unsigned long long opened_length = 0;
        CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_length, NULL, sealed, sealed_length, vectors[i].ad, (unsigned long long) vectors[i].ad_bytes, nonce, vectors[i].key ) == 0 );
        CHECK( (int) opened_length == vectors[i].plaintext_bytes );
        CHECK( vectors[i].plaintext_bytes == 0 || memcmp( opened, vectors[i].plaintext, opened_length ) == 0 );
    }
}

#define MAX_BYTES 2048

static uint8_t input[MAX_BYTES + 64];
static uint8_t associated[64 + 16];
static uint8_t reference[MAX_BYTES + 16 + 64];
static uint8_t output[MAX_BYTES + 16 + 64];
static uint8_t opened[MAX_BYTES + 64];

static void fill( uint8_t * p, size_t bytes, uint32_t seed )
{
    uint32_t x = seed * 2654435761u + 1u;
    for ( size_t i = 0; i < bytes; i++ )
    {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t) ( x >> 24 );
    }
}

// the AEAD over every length, unaligned: the message, the associated data and
// the output each start an odd number of bytes into their buffers

static void aead_seal( int length, uint8_t * out, const uint8_t * key, const uint8_t * nonce )
{
    const uint8_t * m = input + 1 + length % 15;
    const uint8_t * ad = associated + 3 + length % 11;
    const unsigned long long ad_bytes = (unsigned long long) ( length % 61 );
    unsigned long long out_length = 0;
    CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( out, &out_length, m, (unsigned long long) length, ad, ad_bytes, NULL, nonce, key ) == 0 );
    CHECK( out_length == (unsigned long long) length + 16 );
}

static int aead_combinations( const char * chacha, const char * poly, const uint8_t * key, const uint8_t * nonce )
{
    if ( !sodium_dispatch_select_chacha20( chacha ) || !sodium_dispatch_select_poly1305( poly ) )
    {
        return 0;
    }

    known_answers();

    for ( int length = 0; length <= MAX_BYTES; length++ )
    {
        sodium_dispatch_select_chacha20( "ref" );
        sodium_dispatch_select_poly1305( "ref" );
        uint8_t * want = reference + 5 + length % 9;
        aead_seal( length, want, key, nonce );

        sodium_dispatch_select_chacha20( chacha );
        sodium_dispatch_select_poly1305( poly );
        uint8_t * got = output + 7 + length % 13;
        aead_seal( length, got, key, nonce );
        CHECK( memcmp( got, want, (size_t) length + 16 ) == 0 );

        // open the reference's ciphertext, and refuse it with one bit changed

        const uint8_t * ad = associated + 3 + length % 11;
        const unsigned long long ad_bytes = (unsigned long long) ( length % 61 );
        uint8_t * plain = opened + 1 + length % 7;
        unsigned long long plain_length = 0;
        CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( plain, &plain_length, NULL, want, (unsigned long long) length + 16, ad, ad_bytes, nonce, key ) == 0 );
        CHECK( plain_length == (unsigned long long) length );
        CHECK( memcmp( plain, input + 1 + length % 15, (size_t) length ) == 0 );
        want[length % ( length + 16 )] ^= 0x01;
        CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( plain, &plain_length, NULL, want, (unsigned long long) length + 16, ad, ad_bytes, nonce, key ) != 0 );
    }

    return 1;
}

static int blake2b_agrees( const char * name, const uint8_t * key )
{
    if ( !sodium_dispatch_select_blake2b( name ) )
    {
        return 0;
    }

    for ( int length = 0; length <= MAX_BYTES; length++ )
    {
        const uint8_t * m = input + 1 + length % 15;
        for ( int keyed = 0; keyed < 2; keyed++ )
        {
            for ( size_t out_bytes = 32; out_bytes <= 64; out_bytes += 32 )
            {
                uint8_t want[64];
                uint8_t got[64];
                sodium_dispatch_select_blake2b( "ref" );
                CHECK( crypto_generichash( want, out_bytes, m, (unsigned long long) length, keyed ? key : NULL, keyed ? 32 : 0 ) == 0 );
                sodium_dispatch_select_blake2b( name );
                CHECK( crypto_generichash( got, out_bytes, m, (unsigned long long) length, keyed ? key : NULL, keyed ? 32 : 0 ) == 0 );
                CHECK( memcmp( got, want, out_bytes ) == 0 );
            }
        }
    }

    return 1;
}

int main()
{
    CHECK( sodium_dispatch_init() >= 0 );

    const cpu_t cpu = cpu_oracle();

    // 1. detection

    const char * chacha = sodium_dispatch_chacha20();
    const char * poly = sodium_dispatch_poly1305();
    const char * blake = sodium_dispatch_blake2b();
    printf( "cpu: sse2=%d ssse3=%d sse41=%d avx2=%d; sodium: avx=%d avx2=%d; picked chacha20=%s poly1305=%s blake2b=%s\n",
            cpu.sse2, cpu.ssse3, cpu.sse41, cpu.avx2, sodium_dispatch_has( "avx" ), sodium_dispatch_has( "avx2" ), chacha, poly, blake );
    if ( sodium_dispatch_built( "cpu", "avx2" ) )
    {
        CHECK( sodium_dispatch_has( "avx2" ) == ( cpu.avx2 ? 1 : 0 ) );
    }
    CHECK( strcmp( chacha, best( "chacha20", cpu ) ) == 0 );
    CHECK( strcmp( poly, best( "poly1305", cpu ) ) == 0 );
    CHECK( strcmp( blake, best( "blake2b", cpu ) ) == 0 );

    // 2. known answers as detected, then with AVX and AVX2 forced off

    known_answers();

    sodium_dispatch_force_no_avx2( 1 );
    cpu_t without = cpu;
    without.avx2 = false;
    CHECK( sodium_dispatch_has( "avx2" ) == 0 );
    CHECK( strcmp( sodium_dispatch_chacha20(), best( "chacha20", without ) ) == 0 );
    CHECK( strcmp( sodium_dispatch_blake2b(), best( "blake2b", without ) ) == 0 );
    printf( "forced off: chacha20=%s poly1305=%s blake2b=%s\n", sodium_dispatch_chacha20(), sodium_dispatch_poly1305(), sodium_dispatch_blake2b() );
    known_answers();

    sodium_dispatch_force_no_avx2( 0 );
    CHECK( strcmp( sodium_dispatch_chacha20(), chacha ) == 0 );

    // 3. agreement

    uint8_t key[32];
    uint8_t nonce[12];
    fill( key, sizeof( key ), 1 );
    fill( nonce, sizeof( nonce ), 2 );
    fill( input, sizeof( input ), 3 );
    fill( associated, sizeof( associated ), 4 );

    const char * chachas[] = { "ref", "ssse3", "avx2" };
    const char * polys[] = { "ref", "sse2" };
    const char * blakes[] = { "ref", "ssse3", "sse41", "avx2" };

    int aead_pairs = 0;
    for ( int c = 0; c < 3; c++ )
    {
        for ( int p = 0; p < 2; p++ )
        {
            aead_pairs += aead_combinations( chachas[c], polys[p], key, nonce );
        }
    }

    int blake_variants = 0;
    for ( int b = 0; b < 4; b++ )
    {
        blake_variants += blake2b_agrees( blakes[b], key );
    }

    // every pair the picks above say this CPU runs was compared

    CHECK( aead_pairs >= 1 );
    CHECK( blake_variants >= 1 );
    if ( strcmp( chacha, "avx2" ) == 0 && strcmp( poly, "sse2" ) == 0 )
    {
        CHECK( aead_pairs == 6 );
    }

    sodium_dispatch_force_no_avx2( 0 );
    known_answers();

    printf( "test_sodium_dispatch: chacha20=%s poly1305=%s blake2b=%s best for this cpu; %d aead pairs and %d blake2b variants identical, lengths 0-%d\n",
            chacha, poly, blake, aead_pairs, blake_variants, MAX_BYTES );
    return 0;
}
