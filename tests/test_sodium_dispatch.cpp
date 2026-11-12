/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// The vendored sodium pair's runtime dispatch, judged from outside it.
//
// 1. Detection: the implementation each pick chose (ChaCha20, Poly1305,
//    BLAKE2b, the IETF AEAD) is the best this build contains that the CPU
//    and OS can run, with the CPU judged independently of sodium (the
//    compiler's __builtin_cpu_supports, which requires the OS to save YMM
//    state for AVX2; __cpuid and _xgetbv on MSVC; on AArch64 the kernel's
//    HWCAP_ASIMD on Linux and hw.optional.arm.AdvSIMD on Apple). The selection
//    report (sodium_runtime_*_implementation) names the same picks.
// 2. Known answers: the seven sealed packet vectors of
//    sdk/rocketnet_test_vectors.h, the RFC 8439 vectors (2.4.2, 2.5.2, the
//    Poly1305 edge cases of A.3, 2.8.2) and every Wycheproof
//    ChaCha20-Poly1305 vector with a 96-bit nonce reproduce under the
//    detected picks, with AVX and AVX2 detection forced off, under the forced
//    fallback (ROCKETNET_SODIUM_PORTABLE), and under every implementation
//    set the CPU can run.
// 3. Agreement: every implementation set this build contains that the CPU
//    can run gives the same bytes as the portable reference (the composed
//    AEAD over the ref ChaCha20 and donna Poly1305): the IETF AEAD seal and
//    open for every length 0 to 2048 from and into unaligned buffers, and
//    BLAKE2b (keyed and unkeyed, 32 and 64 byte outputs).
// 4. Forgery: a forged packet (one bit of the tag, the ciphertext or the
//    associated data changed) is refused by every implementation set at every
//    length 0 to 2048 that is 0, 1 or 15 mod 16, with the reported length 0,
//    every output byte zero and no byte past the output written, into a
//    separate buffer and in place.
// 5. Sweep: every message length 0 to 2000 with associated data and
//    alignments against the reference, seal and open, in place and out of
//    place. Per commit a subset (associated data lengths 0, 1, 15, 16, 17,
//    21, 53, 64; three alignment pairs); --sweep-full (the nightly row) every
//    associated data length 0 to 64 and every alignment 0 to 15 of the
//    message and of the associated data: 66,593,280 cases per set.
// 6. --diff <cases> <seed> (the nightly row): random lengths, keys, nonces,
//    associated data, alignments and in place or not, against the
//    reference; each case also forged and refused.
//
// The pair is compiled into this binary (tests/sodium_dispatch_seam.c); it
// does not link the library. No time, socket or network.

#include "sodium.h"
#include "sodium_dispatch_seam.h"
#include "wycheproof.h"
#include "../sdk/rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_AMD64 ) || defined( _M_IX86 ) )
#include <intrin.h>
#include <immintrin.h>
#endif

#if defined( __aarch64__ ) && defined( __linux__ )
#include <sys/auxv.h>
#ifndef HWCAP_ASIMD
#define HWCAP_ASIMD ( 1 << 1 )
#endif
#endif

#if defined( __aarch64__ ) && defined( __APPLE__ )
#include <sys/sysctl.h>
#include <sys/types.h>
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
    bool neon;
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
#elif defined( __aarch64__ ) && defined( __linux__ )
    cpu.neon = ( getauxval( AT_HWCAP ) & HWCAP_ASIMD ) != 0;
#elif defined( __aarch64__ ) && defined( __APPLE__ )
    int value = 0;
    size_t value_bytes = sizeof( value );
    cpu.neon = sysctlbyname( "hw.optional.arm.AdvSIMD", &value, &value_bytes, NULL, 0 ) == 0 && value != 0;
#endif
    return cpu;
}

static bool forced_portable = false;

static const char * best( const char * primitive, const cpu_t & cpu )
{
    if ( strcmp( primitive, "aead" ) == 0 )
    {
        return !forced_portable && cpu.neon && sodium_dispatch_built( primitive, "neon-fused" ) ? "neon-fused" : "composed";
    }
    if ( strcmp( primitive, "chacha20" ) == 0 )
    {
        if ( forced_portable )
        {
            return "ref";
        }
        if ( cpu.neon && sodium_dispatch_built( primitive, "neon" ) )
        {
            return "neon";
        }
        if ( cpu.avx2 && sodium_dispatch_built( "cpu", "avx2" ) && sodium_dispatch_built( primitive, "avx2" ) )
        {
            return "avx2";
        }
        return cpu.ssse3 && sodium_dispatch_built( primitive, "ssse3" ) ? "ssse3" : "ref";
    }
    if ( strcmp( primitive, "poly1305" ) == 0 )
    {
        if ( forced_portable )
        {
            return "ref";
        }
        if ( cpu.neon && sodium_dispatch_built( primitive, "mul64" ) )
        {
            return "mul64";
        }
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

// the bundle's own selection report names what the seam reads; the seam
// calls the donna Poly1305 "ref"

static void report_matches()
{
    CHECK( strcmp( sodium_runtime_chacha20_implementation(), sodium_dispatch_chacha20() ) == 0 );
    const char * poly = sodium_dispatch_poly1305();
    CHECK( strcmp( sodium_runtime_poly1305_implementation(), strcmp( poly, "ref" ) == 0 ? "donna" : poly ) == 0 );
    CHECK( strcmp( sodium_runtime_aead_chacha20poly1305_ietf_implementation(), sodium_dispatch_aead() ) == 0 );
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

static int nibble( char c )
{
    if ( c >= '0' && c <= '9' )
    {
        return c - '0';
    }
    if ( c >= 'a' && c <= 'f' )
    {
        return c - 'a' + 10;
    }
    if ( c >= 'A' && c <= 'F' )
    {
        return c - 'A' + 10;
    }
    return -1;
}

static size_t unhex( const char * text, uint8_t * out, size_t capacity )
{
    const size_t length = strlen( text );
    CHECK( length % 2 == 0 );
    const size_t bytes = length / 2;
    CHECK( bytes <= capacity );
    for ( size_t i = 0; i < bytes; i++ )
    {
        const int high = nibble( text[2 * i] );
        const int low = nibble( text[2 * i + 1] );
        CHECK( high >= 0 && low >= 0 );
        out[i] = (uint8_t) ( high * 16 + low );
    }
    return bytes;
}

static const char sunscreen[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";

// RFC 8439 2.4.2 (ChaCha20, counter 1), 2.5.2 (Poly1305), the Poly1305 edge
// vectors of A.3 (#5 to #11, the carries and the final reduction) and 2.8.2
// (the AEAD), under whatever is selected

static void rfc8439_vectors()
{
    uint8_t key[32];
    uint8_t nonce[12];
    uint8_t out[256];
    uint8_t expected[256];
    uint8_t tag[16];

    for ( int i = 0; i < 32; i++ )
    {
        key[i] = (uint8_t) i;
    }
    unhex( "000000000000004a00000000", nonce, sizeof( nonce ) );
    size_t bytes = unhex( "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
                          "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d",
                          expected, sizeof( expected ) );
    CHECK( bytes == strlen( sunscreen ) );
    CHECK( crypto_stream_chacha20_ietf_xor_ic( out, (const uint8_t *) sunscreen, bytes, nonce, 1, key ) == 0 );
    CHECK( memcmp( out, expected, bytes ) == 0 );

    unhex( "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, sizeof( key ) );
    const char * forum = "Cryptographic Forum Research Group";
    CHECK( crypto_onetimeauth_poly1305( tag, (const uint8_t *) forum, strlen( forum ), key ) == 0 );
    unhex( "a8061dc1305136c6c22b8baf0c0127a9", expected, sizeof( expected ) );
    CHECK( memcmp( tag, expected, 16 ) == 0 );

    static const char * const edges[][3] = {
        { "0200000000000000000000000000000000000000000000000000000000000000", "ffffffffffffffffffffffffffffffff", "03000000000000000000000000000000" },
        { "02000000000000000000000000000000ffffffffffffffffffffffffffffffff", "02000000000000000000000000000000", "03000000000000000000000000000000" },
        { "0100000000000000000000000000000000000000000000000000000000000000",
          "fffffffffffffffffffffffffffffffff0ffffffffffffffffffffffffffffff11000000000000000000000000000000", "05000000000000000000000000000000" },
        { "0100000000000000000000000000000000000000000000000000000000000000",
          "fffffffffffffffffffffffffffffffffbfefefefefefefefefefefefefefefe01010101010101010101010101010101", "00000000000000000000000000000000" },
        { "0200000000000000000000000000000000000000000000000000000000000000", "fdffffffffffffffffffffffffffffff", "faffffffffffffffffffffffffffffff" },
        { "0100000000000000040000000000000000000000000000000000000000000000",
          "e33594d7505e43b900000000000000003394d7505e4379cd01000000000000000000000000000000000000000000000001000000000000000000000000000000",
          "14000000000000005500000000000000" },
        { "0100000000000000040000000000000000000000000000000000000000000000",
          "e33594d7505e43b900000000000000003394d7505e4379cd010000000000000000000000000000000000000000000000", "13000000000000000000000000000000" },
    };
    for ( size_t i = 0; i < sizeof( edges ) / sizeof( edges[0] ); i++ )
    {
        uint8_t message[128];
        unhex( edges[i][0], key, sizeof( key ) );
        const size_t message_bytes = unhex( edges[i][1], message, sizeof( message ) );
        unhex( edges[i][2], expected, sizeof( expected ) );
        CHECK( crypto_onetimeauth_poly1305( tag, message, message_bytes, key ) == 0 );
        CHECK( memcmp( tag, expected, 16 ) == 0 );
    }

    for ( int i = 0; i < 32; i++ )
    {
        key[i] = (uint8_t) ( 0x80 + i );
    }
    unhex( "070000004041424344454647", nonce, sizeof( nonce ) );
    uint8_t ad[12];
    unhex( "50515253c0c1c2c3c4c5c6c7", ad, sizeof( ad ) );
    bytes = unhex( "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
                   "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"
                   "1ae10b594f09e26a7e902ecbd0600691",
                   expected, sizeof( expected ) );
    unsigned long long sealed_bytes = 0;
    CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( out, &sealed_bytes, (const uint8_t *) sunscreen, strlen( sunscreen ), ad, sizeof( ad ), NULL, nonce, key ) == 0 );
    CHECK( sealed_bytes == bytes && memcmp( out, expected, bytes ) == 0 );
    uint8_t opened[256];
    unsigned long long opened_bytes = 0;
    CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_bytes, NULL, out, sealed_bytes, ad, sizeof( ad ), nonce, key ) == 0 );
    CHECK( opened_bytes == strlen( sunscreen ) && memcmp( opened, sunscreen, opened_bytes ) == 0 );
}

// every Wycheproof ChaCha20-Poly1305 vector with a 96-bit nonce (the IETF
// AEAD's): a valid vector seals to its ciphertext and tag and opens; an
// invalid one is refused with the reported length 0. Returns the count.

static std::vector<wycheproof_test_t> wycheproof_tests;

static bool wycheproof_field( const wycheproof_test_t & test, const char * name, std::vector<uint8_t> & out )
{
    std::map<std::string, std::string>::const_iterator it = test.fields.find( name );
    return it != test.fields.end() && wycheproof_hex( it->second, out );
}

static int wycheproof_vectors()
{
    if ( wycheproof_tests.empty() )
    {
        int tests = 0;
        int groups = 0;
        CHECK( wycheproof_load( "chacha20_poly1305_test.json", wycheproof_tests, &tests, &groups ) );
        CHECK( tests == 325 );
    }

    int run = 0;
    for ( size_t i = 0; i < wycheproof_tests.size(); i++ )
    {
        const wycheproof_test_t & test = wycheproof_tests[i];
        std::vector<uint8_t> key, iv, ad, msg, ct, tag;
        CHECK( wycheproof_field( test, "key", key ) && wycheproof_field( test, "iv", iv ) && wycheproof_field( test, "aad", ad ) );
        CHECK( wycheproof_field( test, "msg", msg ) && wycheproof_field( test, "ct", ct ) && wycheproof_field( test, "tag", tag ) );
        if ( iv.size() != 12 || key.size() != 32 || tag.size() != 16 )
        {
            continue;
        }
        const bool valid = test.result != "invalid";
        std::vector<uint8_t> sealed( ct );
        sealed.insert( sealed.end(), tag.begin(), tag.end() );
        std::vector<uint8_t> opened( ( msg.size() > ct.size() ? msg.size() : ct.size() ) + 1, 0xA5 );
        unsigned long long opened_bytes = 1;
        const int rc = crypto_aead_chacha20poly1305_ietf_decrypt( opened.data(), &opened_bytes, NULL, sealed.data(), (unsigned long long) sealed.size(),
                                                                  ad.data(), (unsigned long long) ad.size(), iv.data(), key.data() );
        if ( valid )
        {
            CHECK( rc == 0 );
            CHECK( opened_bytes == msg.size() && ( msg.empty() || memcmp( opened.data(), msg.data(), msg.size() ) == 0 ) );
            std::vector<uint8_t> resealed( msg.size() + 16 );
            unsigned long long resealed_bytes = 0;
            CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( resealed.data(), &resealed_bytes, msg.data(), (unsigned long long) msg.size(),
                                                              ad.data(), (unsigned long long) ad.size(), NULL, iv.data(), key.data() ) == 0 );
            CHECK( resealed_bytes == sealed.size() && memcmp( resealed.data(), sealed.data(), sealed.size() ) == 0 );
        }
        else
        {
            CHECK( rc != 0 );
            CHECK( opened_bytes == 0 );
            for ( size_t b = 0; b < ct.size(); b++ )
            {
                CHECK( opened[b] == 0 );
            }
        }
        run++;
    }
    CHECK( run == 316 );
    return run;
}

static void every_vector()
{
    known_answers();
    rfc8439_vectors();
    wycheproof_vectors();
}

// Poly1305 carries that random data almost never reaches: with r = 1 the
// accumulator is the plain sum, so all-ones blocks drive every limb to its
// top and fold at once (h0 + 5 carrying through h1 into h2). Every message
// of one to five blocks drawn from four patterns, then the same with a
// partial last block, under r = 1, r = 2 and the largest clamped r, pad 0
// and all ones: the named Poly1305 against donna. Returns 0 when this build
// or CPU does not have it.

static int poly1305_edges( const char * name )
{
    if ( !sodium_dispatch_select_poly1305( name ) )
    {
        return 0;
    }
    static const char * const rs[] = { "01000000000000000000000000000000", "02000000000000000000000000000000", "ffffff0ffcffff0ffcffff0ffcffff0f" };
    static const uint8_t patterns[4] = { 0x00, 0xff, 0xfe, 0x5a };
    int macs = 0;
    for ( int r = 0; r < 3; r++ )
    {
        for ( int s = 0; s < 2; s++ )
        {
            uint8_t key[32];
            unhex( rs[r], key, 16 );
            memset( key + 16, s ? 0xff : 0x00, 16 );
            for ( int blocks = 1; blocks <= 5; blocks++ )
            {
                int combinations = 1;
                for ( int b = 0; b < blocks; b++ )
                {
                    combinations *= 4;
                }
                for ( int c = 0; c < combinations; c++ )
                {
                    uint8_t message[80];
                    int pick = c;
                    for ( int b = 0; b < blocks; b++ )
                    {
                        memset( message + 16 * b, patterns[pick % 4], 16 );
                        pick /= 4;
                    }
                    for ( int cut = 0; cut <= 1; cut++ )
                    {
                        const size_t bytes = (size_t) ( 16 * blocks - cut * 7 );
                        uint8_t want[16];
                        uint8_t got[16];
                        sodium_dispatch_select_poly1305( "ref" );
                        CHECK( crypto_onetimeauth_poly1305( want, message, bytes, key ) == 0 );
                        sodium_dispatch_select_poly1305( name );
                        CHECK( crypto_onetimeauth_poly1305( got, message, bytes, key ) == 0 );
                        CHECK( memcmp( got, want, 16 ) == 0 );
                        macs++;
                    }
                }
            }
        }
    }
    CHECK( macs == 6 * 2 * ( 4 + 16 + 64 + 256 + 1024 ) );
    return 1;
}

// An implementation set: the AEAD, and the ChaCha20 and Poly1305 primitives
// (the composed AEAD runs through them; the one pass runs neither).

struct set_t
{
    const char * aead;
    const char * chacha;
    const char * poly;
};

static const set_t reference_set = { "composed", "ref", "ref" };

static bool select_set( const set_t & set )
{
    return sodium_dispatch_select_aead( set.aead ) && sodium_dispatch_select_chacha20( set.chacha ) && sodium_dispatch_select_poly1305( set.poly );
}

static void select_or_fail( const set_t & set )
{
    CHECK( select_set( set ) );
}

#define MAX_BYTES 2048

static uint8_t input[MAX_BYTES + 64];
static uint8_t associated[64 + 16 + 64];
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

static int aead_agrees( const set_t & set, const uint8_t * key, const uint8_t * nonce )
{
    if ( !select_set( set ) )
    {
        return 0;
    }

    every_vector();

    for ( int length = 0; length <= MAX_BYTES; length++ )
    {
        select_or_fail( reference_set );
        uint8_t * want = reference + 5 + length % 9;
        aead_seal( length, want, key, nonce );

        select_or_fail( set );
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

// Forgery: refused, the reported length 0, every output byte zero, nothing
// past the output written; into a separate buffer and in place.

static void forged_open_is_zero( const uint8_t * sealed, int length, const uint8_t * ad, unsigned long long ad_bytes, const uint8_t * nonce, const uint8_t * key )
{
    static uint8_t out[MAX_BYTES + 16 + 32];
    memset( out, 0xA5, sizeof( out ) );
    unsigned long long out_length = 12345;
    CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( out + 8, &out_length, NULL, sealed, (unsigned long long) length + 16, ad, ad_bytes, nonce, key ) == -1 );
    CHECK( out_length == 0 );
    for ( int i = 0; i < 8; i++ )
    {
        CHECK( out[i] == 0xA5 );
    }
    for ( int i = 0; i < length; i++ )
    {
        CHECK( out[8 + i] == 0 );
    }
    for ( size_t i = 8 + (size_t) length; i < sizeof( out ); i++ )
    {
        CHECK( out[i] == 0xA5 );
    }

    // in place: the ciphertext buffer is the output
    static uint8_t in_place[MAX_BYTES + 16 + 32];
    memset( in_place, 0xA5, sizeof( in_place ) );
    memcpy( in_place + 3, sealed, (size_t) length + 16 );
    out_length = 12345;
    CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( in_place + 3, &out_length, NULL, in_place + 3, (unsigned long long) length + 16, ad, ad_bytes, nonce, key ) == -1 );
    CHECK( out_length == 0 );
    for ( int i = 0; i < length; i++ )
    {
        CHECK( in_place[3 + i] == 0 );
    }
    CHECK( memcmp( in_place + 3 + length, sealed + length, 16 ) == 0 );

    // detached: the same contract
    memset( out, 0xA5, sizeof( out ) );
    CHECK( crypto_aead_chacha20poly1305_ietf_decrypt_detached( out + 8, NULL, sealed, (unsigned long long) length, sealed + length, ad, ad_bytes, nonce, key ) == -1 );
    for ( int i = 0; i < length; i++ )
    {
        CHECK( out[8 + i] == 0 );
    }
    CHECK( out[8 + length] == 0xA5 );
}

static int forgery_refused( const set_t & set, const uint8_t * key, const uint8_t * nonce )
{
    if ( !select_set( set ) )
    {
        return 0;
    }
    int lengths = 0;
    static uint8_t sealed[MAX_BYTES + 16];
    for ( int length = 0; length <= MAX_BYTES; length++ )
    {
        if ( length % 16 != 0 && length % 16 != 1 && length % 16 != 15 )
        {
            continue;
        }
        const uint8_t * ad = associated + 1 + length % 5;
        const unsigned long long ad_bytes = (unsigned long long) ( length % 23 );
        unsigned long long sealed_length = 0;
        CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( sealed, &sealed_length, input, (unsigned long long) length, ad, ad_bytes, NULL, nonce, key ) == 0 );

        // a tag bit
        sealed[length + ( length % 16 )] ^= 0x80;
        forged_open_is_zero( sealed, length, ad, ad_bytes, nonce, key );
        sealed[length + ( length % 16 )] ^= 0x80;

        // a ciphertext bit
        if ( length > 0 )
        {
            sealed[length - 1] ^= 0x01;
            forged_open_is_zero( sealed, length, ad, ad_bytes, nonce, key );
            sealed[length - 1] ^= 0x01;
        }

        // an associated data bit
        static uint8_t forged_ad[64];
        memcpy( forged_ad, ad, (size_t) ad_bytes );
        if ( ad_bytes > 0 )
        {
            forged_ad[ad_bytes - 1] ^= 0x40;
            forged_open_is_zero( sealed, length, forged_ad, ad_bytes, nonce, key );
        }

        // and the untouched packet still opens
        unsigned long long plain_length = 0;
        CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &plain_length, NULL, sealed, sealed_length, ad, ad_bytes, nonce, key ) == 0 );
        CHECK( plain_length == (unsigned long long) length && memcmp( opened, input, (size_t) length ) == 0 );
        lengths++;
    }
    return lengths;
}

// Sweep: lengths 0..2000 against the reference, seal and open, out of place
// and in place, at the given associated data lengths and alignment pairs.

#define SWEEP_BYTES 2000

static long sweep( const set_t & set, const int * ad_lengths, int num_ad_lengths, const int ( *alignments )[2], int num_alignments, const uint8_t * key, const uint8_t * nonce )
{
    static uint8_t want[SWEEP_BYTES + 16];
    static uint8_t message[SWEEP_BYTES + 16 + 16];
    static uint8_t ad_buffer[64 + 16];
    static uint8_t got[SWEEP_BYTES + 16 + 16];
    static uint8_t plain[SWEEP_BYTES + 16];
    long cases = 0;

    for ( int length = 0; length <= SWEEP_BYTES; length++ )
    {
        for ( int a = 0; a < num_ad_lengths; a++ )
        {
            const int ad_bytes = ad_lengths[a];
            unsigned long long n = 0;
            select_or_fail( reference_set );
            CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( want, &n, input, (unsigned long long) length, associated, (unsigned long long) ad_bytes, NULL, nonce, key ) == 0 );
            select_or_fail( set );

            for ( int p = 0; p < num_alignments; p++ )
            {
                const int am = alignments[p][0];
                const int aa = alignments[p][1];
                uint8_t * ad = ad_buffer + aa;
                memcpy( ad, associated, (size_t) ad_bytes );

                // out of place: from message + am into got + (am * 7 + 3) % 16
                uint8_t * m = message + am;
                memcpy( m, input, (size_t) length );
                uint8_t * c = got + ( am * 7 + 3 ) % 16;
                CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( c, &n, m, (unsigned long long) length, ad, (unsigned long long) ad_bytes, NULL, nonce, key ) == 0 );
                CHECK( n == (unsigned long long) length + 16 && memcmp( c, want, (size_t) n ) == 0 );
                CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( plain, &n, NULL, c, (unsigned long long) length + 16, ad, (unsigned long long) ad_bytes, nonce, key ) == 0 );
                CHECK( n == (unsigned long long) length && memcmp( plain, input, (size_t) length ) == 0 );

                // in place: message + am is sealed over itself, then opened over itself
                CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( m, &n, m, (unsigned long long) length, ad, (unsigned long long) ad_bytes, NULL, nonce, key ) == 0 );
                CHECK( memcmp( m, want, (size_t) length + 16 ) == 0 );
                CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( m, &n, NULL, m, (unsigned long long) length + 16, ad, (unsigned long long) ad_bytes, nonce, key ) == 0 );
                CHECK( n == (unsigned long long) length && memcmp( m, input, (size_t) length ) == 0 );
                cases += 2;
            }
        }
    }
    return cases;
}

// splitmix64: the differential's generator

static uint64_t next( uint64_t * state )
{
    uint64_t z = ( *state += 0x9e3779b97f4a7c15ull );
    z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
    z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebull;
    return z ^ ( z >> 31 );
}

static void random_bytes( uint64_t * state, uint8_t * p, size_t bytes )
{
    for ( size_t i = 0; i < bytes; i++ )
    {
        p[i] = (uint8_t) next( state );
    }
}

static int differential( const set_t & set, long cases, uint64_t seed )
{
    if ( !select_set( set ) )
    {
        return 0;
    }
    static uint8_t m[MAX_BYTES + 32];
    static uint8_t want[MAX_BYTES + 16];
    static uint8_t got[MAX_BYTES + 16 + 32];
    static uint8_t ad[64 + 16];
    uint64_t state = seed;
    long mismatch = 0;
    long open_fail = 0;
    long forged_accepted = 0;

    for ( long i = 0; i < cases; i++ )
    {
        uint8_t key[32];
        uint8_t nonce[12];
        random_bytes( &state, key, sizeof( key ) );
        random_bytes( &state, nonce, sizeof( nonce ) );
        const uint64_t shape = next( &state );
        // half the cases are packet sized (0..1500), half any length to 2048
        const int length = ( shape & 1 ) ? (int) ( ( shape >> 1 ) % 1501 ) : (int) ( ( shape >> 1 ) % ( MAX_BYTES + 1 ) );
        const int ad_bytes = (int) ( ( shape >> 20 ) % 65 );
        const int am = (int) ( ( shape >> 28 ) % 16 );
        const int aa = (int) ( ( shape >> 32 ) % 16 );
        const bool in_place = ( ( shape >> 36 ) & 1 ) != 0;
        uint8_t * message = m + am;
        uint8_t * associated_data = ad + aa;
        random_bytes( &state, message, (size_t) length );
        random_bytes( &state, associated_data, (size_t) ad_bytes );

        unsigned long long n = 0;
        select_or_fail( reference_set );
        CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( want, &n, message, (unsigned long long) length, associated_data, (unsigned long long) ad_bytes, NULL, nonce, key ) == 0 );
        select_or_fail( set );

        uint8_t * c = in_place ? message : got + ( am * 5 + 1 ) % 16;
        static uint8_t original[MAX_BYTES];
        memcpy( original, message, (size_t) length );
        CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( c, &n, message, (unsigned long long) length, associated_data, (unsigned long long) ad_bytes, NULL, nonce, key ) == 0 );
        if ( memcmp( c, want, (size_t) length + 16 ) != 0 )
        {
            mismatch++;
        }
        uint8_t * plain = in_place ? c : opened;
        if ( crypto_aead_chacha20poly1305_ietf_decrypt( plain, &n, NULL, c, (unsigned long long) length + 16, associated_data, (unsigned long long) ad_bytes, nonce, key ) != 0 ||
             memcmp( plain, original, (size_t) length ) != 0 )
        {
            open_fail++;
        }

        // forge one bit of the ciphertext or the tag, sealed again by the reference
        memcpy( got, want, (size_t) length + 16 );
        const uint64_t bit = next( &state ) % ( (uint64_t) ( length + 16 ) * 8 );
        got[bit / 8] ^= (uint8_t) ( 1u << ( bit % 8 ) );
        if ( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &n, NULL, got, (unsigned long long) length + 16, associated_data, (unsigned long long) ad_bytes, nonce, key ) == 0 )
        {
            forged_accepted++;
        }
    }
    printf( "diff aead=%s chacha20=%s poly1305=%s cases=%ld seed=%llu mismatch=%ld open_fail=%ld forged_accepted=%ld\n", set.aead, set.chacha, set.poly, cases,
            (unsigned long long) seed, mismatch, open_fail, forged_accepted );
    CHECK( mismatch == 0 && open_fail == 0 && forged_accepted == 0 );
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

// every set this build might contain: the composed AEAD over every primitive
// pair, and the one pass (which runs neither primitive) once

static const set_t all_sets[] = {
    { "composed", "ref", "ref" },
    { "composed", "ref", "sse2" },
    { "composed", "ssse3", "ref" },
    { "composed", "ssse3", "sse2" },
    { "composed", "avx2", "ref" },
    { "composed", "avx2", "sse2" },
    { "composed", "neon", "ref" },
    { "composed", "ref", "mul64" },
    { "composed", "neon", "mul64" },
    { "neon-fused", "ref", "ref" },
};

static const int num_sets = (int) ( sizeof( all_sets ) / sizeof( all_sets[0] ) );

static void set_environment( const char * name, const char * value )
{
#if defined( _WIN32 )
    CHECK( _putenv_s( name, value ? value : "" ) == 0 );
#else
    if ( value )
    {
        CHECK( setenv( name, value, 1 ) == 0 );
    }
    else
    {
        CHECK( unsetenv( name ) == 0 );
    }
#endif
}

static void prepare( uint8_t * key, uint8_t * nonce )
{
    fill( key, 32, 1 );
    fill( nonce, 12, 2 );
    fill( input, sizeof( input ), 3 );
    fill( associated, sizeof( associated ), 4 );
}

static int sweep_full( const uint8_t * key, const uint8_t * nonce )
{
    int ad_lengths[65];
    for ( int i = 0; i <= 64; i++ )
    {
        ad_lengths[i] = i;
    }
    int alignments[256][2];
    for ( int i = 0; i < 256; i++ )
    {
        alignments[i][0] = i / 16;
        alignments[i][1] = i % 16;
    }
    int sets = 0;
    for ( int s = 1; s < num_sets; s++ )
    {
        if ( !select_set( all_sets[s] ) )
        {
            continue;
        }
        const long cases = sweep( all_sets[s], ad_lengths, 65, alignments, 256, key, nonce );
        printf( "sweep aead=%s chacha20=%s poly1305=%s lengths=0-%d ad=0-64 alignments=16x16 in+out of place cases=%ld mismatch=0\n", all_sets[s].aead, all_sets[s].chacha,
                all_sets[s].poly, SWEEP_BYTES, cases );
        CHECK( cases == 66593280L );
        sets++;
    }
    printf( "test_sodium_dispatch --sweep-full: %d sets\n", sets );
    return 0;
}

int main( int argc, char ** argv )
{
    CHECK( sodium_dispatch_init() >= 0 );

    uint8_t key[32];
    uint8_t nonce[12];
    prepare( key, nonce );

    if ( argc >= 2 && strcmp( argv[1], "--sweep-full" ) == 0 )
    {
        return sweep_full( key, nonce );
    }
    if ( argc >= 4 && strcmp( argv[1], "--diff" ) == 0 )
    {
        const long cases = strtol( argv[2], NULL, 10 );
        const uint64_t seed = strtoull( argv[3], NULL, 10 );
        int sets = 0;
        for ( int s = 1; s < num_sets; s++ )
        {
            sets += differential( all_sets[s], cases, seed );
        }
        printf( "test_sodium_dispatch --diff: %d sets\n", sets );
        return 0;
    }
    CHECK( argc == 1 );

    const cpu_t cpu = cpu_oracle();
    forced_portable = sodium_runtime_forced_portable() != 0;

    // 1. detection, and the selection report

    const char * chacha = sodium_dispatch_chacha20();
    const char * poly = sodium_dispatch_poly1305();
    const char * blake = sodium_dispatch_blake2b();
    const char * aead = sodium_dispatch_aead();
    printf( "cpu: sse2=%d ssse3=%d sse41=%d avx2=%d neon=%d; sodium: avx=%d avx2=%d neon=%d forced_portable=%d; picked chacha20=%s poly1305=%s blake2b=%s aead=%s\n",
            cpu.sse2, cpu.ssse3, cpu.sse41, cpu.avx2, cpu.neon, sodium_dispatch_has( "avx" ), sodium_dispatch_has( "avx2" ), sodium_dispatch_has( "neon" ),
            forced_portable, chacha, poly, blake, aead );
    if ( sodium_dispatch_built( "cpu", "avx2" ) )
    {
        CHECK( sodium_dispatch_has( "avx2" ) == ( cpu.avx2 ? 1 : 0 ) );
    }
    if ( sodium_dispatch_built( "aead", "neon-fused" ) )
    {
        CHECK( sodium_dispatch_has( "neon" ) == ( cpu.neon ? 1 : 0 ) );
    }
    CHECK( strcmp( chacha, best( "chacha20", cpu ) ) == 0 );
    CHECK( strcmp( poly, best( "poly1305", cpu ) ) == 0 );
    CHECK( strcmp( blake, best( "blake2b", cpu ) ) == 0 );
    CHECK( strcmp( aead, best( "aead", cpu ) ) == 0 );
    report_matches();

    // 2. known answers as detected, with AVX and AVX2 forced off, and under
    // the forced fallback

    every_vector();

    sodium_dispatch_force_no_avx2( 1 );
    cpu_t without = cpu;
    without.avx2 = false;
    CHECK( sodium_dispatch_has( "avx2" ) == 0 );
    CHECK( strcmp( sodium_dispatch_chacha20(), best( "chacha20", without ) ) == 0 );
    CHECK( strcmp( sodium_dispatch_blake2b(), best( "blake2b", without ) ) == 0 );
    printf( "forced off: chacha20=%s poly1305=%s blake2b=%s aead=%s\n", sodium_dispatch_chacha20(), sodium_dispatch_poly1305(), sodium_dispatch_blake2b(), sodium_dispatch_aead() );
    every_vector();
    sodium_dispatch_force_no_avx2( 0 );
    CHECK( strcmp( sodium_dispatch_chacha20(), chacha ) == 0 );

    if ( !forced_portable )
    {
        set_environment( "ROCKETNET_SODIUM_PORTABLE", "1" );
        CHECK( sodium_runtime_forced_portable() == 1 );
        sodium_dispatch_repick();
        CHECK( strcmp( sodium_runtime_chacha20_implementation(), "ref" ) == 0 );
        CHECK( strcmp( sodium_runtime_poly1305_implementation(), "donna" ) == 0 );
        CHECK( strcmp( sodium_runtime_aead_chacha20poly1305_ietf_implementation(), "composed" ) == 0 );
        report_matches();
        printf( "forced fallback: chacha20=%s poly1305=%s aead=%s\n", sodium_runtime_chacha20_implementation(), sodium_runtime_poly1305_implementation(),
                sodium_runtime_aead_chacha20poly1305_ietf_implementation() );
        every_vector();
        set_environment( "ROCKETNET_SODIUM_PORTABLE", NULL );
        CHECK( sodium_runtime_forced_portable() == 0 );
        sodium_dispatch_repick();
        CHECK( strcmp( sodium_dispatch_chacha20(), chacha ) == 0 );
        CHECK( strcmp( sodium_dispatch_poly1305(), poly ) == 0 );
        CHECK( strcmp( sodium_dispatch_aead(), aead ) == 0 );
    }

    // 3. agreement and 4. forgery, every set the CPU runs

    int aead_sets = 0;
    int forged_sets = 0;
    for ( int s = 0; s < num_sets; s++ )
    {
        aead_sets += aead_agrees( all_sets[s], key, nonce );
        const int lengths = forgery_refused( all_sets[s], key, nonce );
        if ( lengths > 0 )
        {
            CHECK( lengths == 3 * ( MAX_BYTES / 16 ) + 1 );
            forged_sets++;
        }
    }

    int poly_edges = 0;
    poly_edges += poly1305_edges( "sse2" );
    poly_edges += poly1305_edges( "mul64" );
    sodium_dispatch_repick();
    CHECK( strcmp( sodium_dispatch_poly1305(), poly ) == 0 );
    CHECK( poly_edges == ( ( cpu.sse2 && sodium_dispatch_built( "poly1305", "sse2" ) ) || ( cpu.neon && sodium_dispatch_built( "poly1305", "mul64" ) ) ? 1 : 0 ) );

    const char * blakes[] = { "ref", "ssse3", "sse41", "avx2" };
    int blake_variants = 0;
    for ( int b = 0; b < 4; b++ )
    {
        blake_variants += blake2b_agrees( blakes[b], key );
    }

    // every set the picks above say this CPU runs was compared

    CHECK( aead_sets >= 1 && forged_sets == aead_sets );
    CHECK( blake_variants >= 1 );
    if ( strcmp( chacha, "avx2" ) == 0 && strcmp( poly, "sse2" ) == 0 )
    {
        CHECK( aead_sets == 6 );
    }
    if ( strcmp( aead, "neon-fused" ) == 0 )
    {
        CHECK( aead_sets == 5 );
    }

    // 5. the per-commit sweep

    static const int ad_lengths[] = { 0, 1, 15, 16, 17, 21, 53, 64 };
    static const int alignments[][2] = { { 0, 0 }, { 3, 5 }, { 15, 15 } };
    long swept = 0;
    for ( int s = 1; s < num_sets; s++ )
    {
        if ( select_set( all_sets[s] ) )
        {
            swept += sweep( all_sets[s], ad_lengths, 8, alignments, 3, key, nonce );
        }
    }

    sodium_dispatch_force_no_avx2( 0 );
    CHECK( strcmp( sodium_dispatch_aead(), aead ) == 0 );
    every_vector();

    printf( "test_sodium_dispatch: chacha20=%s poly1305=%s blake2b=%s aead=%s best for this cpu; %d aead sets and %d blake2b variants identical, lengths 0-%d; "
            "forgeries refused and zeroed in %d sets; %ld sweep cases\n",
            chacha, poly, blake, aead, aead_sets, blake_variants, MAX_BYTES, forged_sets, swept );
    return 0;
}
