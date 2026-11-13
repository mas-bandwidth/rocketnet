/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// Secret tracking for the IETF AEAD (sodium/ARM64-CONSTANT-TIME.md, "By tool:
// secret tracking"). Run under valgrind memcheck with --error-exitcode=1:
// the key, the message and the associated data are marked undefined, and the
// ciphertext and tag again before each open, so a branch or a memory index
// that depends on any of them is a memcheck error. The tag comparison's
// result is declassified inside the bundle (SODIUM_DECLASSIFY); this program
// declassifies only what it compares itself, after each call returns.
//
// Every size below through every implementation set the CPU runs: seal, open,
// and open of a forgery. Outside valgrind the client requests do nothing and
// the program checks only the results. The pair is compiled into this binary
// with ROCKETNET_SODIUM_SECRET_TRACKING (tests/sodium_dispatch_seam.c). No
// time, socket or network.

#include "sodium.h"
#include "sodium_dispatch_seam.h"

#include <valgrind/memcheck.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( x )                                                                  \
    do                                                                              \
    {                                                                               \
        if ( !( x ) )                                                               \
        {                                                                           \
            fprintf( stderr, "sodium secrets check failed: %s at %d\n", #x, __LINE__ ); \
            exit( 1 );                                                              \
        }                                                                           \
    } while ( 0 )

#define SECRET( p, n ) ( (void) VALGRIND_MAKE_MEM_UNDEFINED( ( p ), ( n ) ) )
#define PUBLIC( p, n ) ( (void) VALGRIND_MAKE_MEM_DEFINED( ( p ), ( n ) ) )

struct set_t
{
    const char * aead;
    const char * chacha;
    const char * poly;
};

static const struct set_t sets[] = {
    { "composed", "ref", "ref" },
    { "composed", "ref", "sse2" },
    { "composed", "ssse3", "ref" },
    { "composed", "avx2", "sse2" },
    { "composed", "neon", "ref" },
    { "composed", "ref", "mul64" },
    { "composed", "neon", "mul64" },
    { "neon-fused", "neon", "mul64" }, // the production pick; the fused pass calls neither pointer
};

static const int sizes[] = { 0, 1, 15, 16, 63, 64, 65, 191, 192, 193, 200, 256, 257, 1200, 1400 };

#define MAX_BYTES 1400

static void fill( uint8_t * p, size_t bytes, uint32_t seed )
{
    uint32_t x = seed * 2654435761u + 1u;
    for ( size_t i = 0; i < bytes; i++ )
    {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t) ( x >> 24 );
    }
}

static int run_set( const struct set_t * set )
{
    if ( !sodium_dispatch_select_aead( set->aead ) || !sodium_dispatch_select_chacha20( set->chacha ) || !sodium_dispatch_select_poly1305( set->poly ) )
    {
        return 0;
    }

    static uint8_t key[32];
    static uint8_t nonce[12];
    static uint8_t message[MAX_BYTES];
    static uint8_t ad[64];
    static uint8_t sealed[MAX_BYTES + 16];
    static uint8_t opened[MAX_BYTES];

    for ( size_t s = 0; s < sizeof( sizes ) / sizeof( sizes[0] ); s++ )
    {
        const int length = sizes[s];
        const unsigned long long ad_bytes = (unsigned long long) ( length % 65 );
        fill( key, sizeof( key ), (uint32_t) length + 1u );
        fill( nonce, sizeof( nonce ), (uint32_t) length + 2u );
        fill( message, sizeof( message ), (uint32_t) length + 3u );
        fill( ad, sizeof( ad ), (uint32_t) length + 4u );
        SECRET( key, sizeof( key ) );
        SECRET( message, sizeof( message ) );
        SECRET( ad, sizeof( ad ) );

        unsigned long long sealed_bytes = 0;
        CHECK( crypto_aead_chacha20poly1305_ietf_encrypt( sealed, &sealed_bytes, message, (unsigned long long) length, ad, ad_bytes, NULL, nonce, key ) == 0 );
        CHECK( sealed_bytes == (unsigned long long) length + 16 );

        SECRET( sealed, sizeof( sealed ) );
        unsigned long long opened_bytes = 0;
        CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_bytes, NULL, sealed, sealed_bytes, ad, ad_bytes, nonce, key ) == 0 );
        CHECK( opened_bytes == (unsigned long long) length );
        PUBLIC( opened, (size_t) length );
        PUBLIC( message, (size_t) length );
        CHECK( memcmp( opened, message, (size_t) length ) == 0 );

        // a forgery: one tag bit; refused, no plaintext written (the
        // composed AEAD zeroes the output, the one pass leaves the poison)
        SECRET( message, sizeof( message ) );
        sealed[length] ^= 0x01;
        SECRET( sealed, sizeof( sealed ) );
        memset( opened, 0xA5, (size_t) length );
        opened_bytes = 1;
        CHECK( crypto_aead_chacha20poly1305_ietf_decrypt( opened, &opened_bytes, NULL, sealed, sealed_bytes, ad, ad_bytes, nonce, key ) == -1 );
        CHECK( opened_bytes == 0 );
        PUBLIC( opened, (size_t) length );
        const unsigned char left = strcmp( sodium_dispatch_aead(), "composed" ) == 0 ? 0 : 0xA5;
        for ( int i = 0; i < length; i++ )
        {
            CHECK( opened[i] == left );
        }

        PUBLIC( key, sizeof( key ) );
        PUBLIC( message, sizeof( message ) );
        PUBLIC( ad, sizeof( ad ) );
        PUBLIC( sealed, sizeof( sealed ) );
    }
    return 1;
}

int main( void )
{
    CHECK( sodium_dispatch_init() >= 0 );
    int ran = 0;
    for ( size_t i = 0; i < sizeof( sets ) / sizeof( sets[0] ); i++ )
    {
        if ( run_set( &sets[i] ) )
        {
            printf( "secrets: aead=%s chacha20=%s poly1305=%s sizes=%d\n", sets[i].aead, sets[i].chacha, sets[i].poly, (int) ( sizeof( sizes ) / sizeof( sizes[0] ) ) );
            ran++;
        }
    }
    CHECK( ran >= 1 );
    printf( "test_sodium_secrets: %d sets; under valgrind %s\n", ran, RUNNING_ON_VALGRIND ? "yes" : "no" );
    return 0;
}
