/* Tracer harness (scratch): compiles the scratch sodium copy into this unit so
   it can switch the ChaCha20 and Poly1305 implementations, as the repo's
   tests/sodium_dispatch_seam.c does, and checks the arm64 prototype against
   the portable path.

   harness vectors <wycheproof.txt>   RFC 8439 vectors and Wycheproof, both paths
   harness diff <cases> <seed>        differential + forgery rejection
   harness bench <iterations>         seal/open us per packet, both paths
   harness ct                         valgrind secret tracking (build -DRN_CTGRIND) */

#include "sodium.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(RN_CTGRIND)
#include <valgrind/memcheck.h>
#endif

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
extern int rn_arm64_chacha8;
#endif

/* 0: portable; 1: arm64, four ChaCha20 blocks in flight; 2: arm64, eight */
static int use_arm( int on )
{
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
    rn_arm64_chacha8 = on == 2;
    implementation__cc20 = on ? &crypto_stream_chacha20_rn_neon_implementation : &crypto_stream_chacha20_ref_implementation;
    implementation__poly = on ? &crypto_onetimeauth_poly1305_rn_mul64_implementation : &crypto_onetimeauth_poly1305_donna_implementation;
    return 1;
#else
    (void) on;
    return 0;
#endif
}

static const char * picked_chacha20( void )
{
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
    if ( implementation__cc20 == &crypto_stream_chacha20_rn_neon_implementation )
    {
        return rn_arm64_chacha8 ? "neon8" : "neon4";
    }
#endif
    return implementation__cc20 == &crypto_stream_chacha20_ref_implementation ? "ref" : "other";
}

static const char * picked_poly1305( void )
{
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN)
    if ( implementation__poly == &crypto_onetimeauth_poly1305_rn_mul64_implementation )
    {
        return "mul64";
    }
#endif
    return implementation__poly == &crypto_onetimeauth_poly1305_donna_implementation ? "ref" : "other";
}

static size_t unhex( const char * s, unsigned char * out, size_t cap )
{
    if ( strcmp( s, "-" ) == 0 )
    {
        return 0;
    }
    const size_t n = strlen( s ) / 2;
    if ( n > cap )
    {
        fprintf( stderr, "hex too long\n" );
        exit( 2 );
    }
    for ( size_t i = 0; i < n; i++ )
    {
        unsigned int v = 0;
        if ( sscanf( s + 2 * i, "%2x", &v ) != 1 )
        {
            fprintf( stderr, "bad hex\n" );
            exit( 2 );
        }
        out[i] = (unsigned char) v;
    }
    return n;
}

static int failures = 0;

static void expect( int ok, const char * what, const char * path )
{
    if ( !ok )
    {
        failures++;
        printf( "FAIL %s (%s)\n", what, path );
    }
}

static const char sunscreen[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";

/* RFC 8439 2.4.2, 2.5.2, 2.8.2 and the Poly1305 edge vectors of A.3 (#5-#11). */
static void rfc_vectors( const char * path )
{
    unsigned char key[32], nonce[12], out[512], expect_buf[512], tag[16];
    size_t n;

    /* 2.4.2 ChaCha20 encryption, counter 1 */
    for ( int i = 0; i < 32; i++ )
    {
        key[i] = (unsigned char) i;
    }
    unhex( "000000000000004a00000000", nonce, sizeof( nonce ) );
    n = unhex( "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
               "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d",
               expect_buf, sizeof( expect_buf ) );
    crypto_stream_chacha20_ietf_xor_ic( out, (const unsigned char *) sunscreen, n, nonce, 1, key );
    expect( n == strlen( sunscreen ) && memcmp( out, expect_buf, n ) == 0, "RFC 8439 2.4.2 chacha20", path );

    /* 2.5.2 Poly1305 */
    unhex( "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, sizeof( key ) );
    const char * cfrg = "Cryptographic Forum Research Group";
    crypto_onetimeauth_poly1305( tag, (const unsigned char *) cfrg, strlen( cfrg ), key );
    unhex( "a8061dc1305136c6c22b8baf0c0127a9", expect_buf, sizeof( expect_buf ) );
    expect( memcmp( tag, expect_buf, 16 ) == 0, "RFC 8439 2.5.2 poly1305", path );

    /* A.3 edge vectors: key (r||s), message, tag */
    static const char * const a3[][3] = {
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
    for ( size_t i = 0; i < sizeof( a3 ) / sizeof( a3[0] ); i++ )
    {
        unsigned char msg[128];
        unhex( a3[i][0], key, sizeof( key ) );
        const size_t mlen = unhex( a3[i][1], msg, sizeof( msg ) );
        unhex( a3[i][2], expect_buf, sizeof( expect_buf ) );
        crypto_onetimeauth_poly1305( tag, msg, mlen, key );
        char what[64];
        snprintf( what, sizeof( what ), "RFC 8439 A.3 poly1305 #%zu", i + 5 );
        expect( memcmp( tag, expect_buf, 16 ) == 0, what, path );
    }

    /* 2.8.2 AEAD */
    for ( int i = 0; i < 32; i++ )
    {
        key[i] = (unsigned char) ( 0x80 + i );
    }
    unhex( "070000004041424344454647", nonce, sizeof( nonce ) );
    unsigned char aad[12];
    unhex( "50515253c0c1c2c3c4c5c6c7", aad, sizeof( aad ) );
    n = unhex( "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
               "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"
               "1ae10b594f09e26a7e902ecbd0600691",
               expect_buf, sizeof( expect_buf ) );
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt( out, &clen, (const unsigned char *) sunscreen, strlen( sunscreen ), aad, sizeof( aad ), NULL, nonce, key );
    expect( clen == n && memcmp( out, expect_buf, n ) == 0, "RFC 8439 2.8.2 aead", path );
}

static void wycheproof( const char * file, const char * path )
{
    FILE * f = fopen( file, "r" );
    if ( f == NULL )
    {
        fprintf( stderr, "cannot open %s\n", file );
        exit( 2 );
    }
    static char line[1 << 16];
    int count = 0;
    while ( fgets( line, sizeof( line ), f ) != NULL )
    {
        int id = 0, valid = 0;
        static char k[256], iv[256], aad[1 << 13], msg[1 << 14], ct[1 << 14], tg[256];
        if ( sscanf( line, "%d %d %255s %255s %8191s %16383s %16383s %255s", &id, &valid, k, iv, aad, msg, ct, tg ) != 8 )
        {
            continue;
        }
        static unsigned char K[32], IV[12], A[4096], M[8192], C[8192 + 16], T[16], out[8192 + 16];
        if ( unhex( k, K, sizeof( K ) ) != 32 || unhex( iv, IV, sizeof( IV ) ) != 12 )
        {
            continue;
        }
        const size_t alen = unhex( aad, A, sizeof( A ) );
        const size_t mlen = unhex( msg, M, sizeof( M ) );
        const size_t ctlen = unhex( ct, C, sizeof( C ) - 16 );
        const size_t tlen = unhex( tg, T, sizeof( T ) );
        char what[64];
        snprintf( what, sizeof( what ), "wycheproof tcId %d", id );
        if ( valid )
        {
            unsigned long long clen = 0;
            crypto_aead_chacha20poly1305_ietf_encrypt( out, &clen, M, mlen, A, alen, NULL, IV, K );
            expect( clen == ctlen + 16 && memcmp( out, C, ctlen ) == 0 && memcmp( out + ctlen, T, 16 ) == 0, what, path );
        }
        memcpy( C + ctlen, T, tlen );
        unsigned long long plen = 0;
        const int ret = crypto_aead_chacha20poly1305_ietf_decrypt( out, &plen, NULL, C, ctlen + tlen, A, alen, IV, K );
        if ( valid )
        {
            expect( ret == 0 && plen == mlen && memcmp( out, M, mlen ) == 0, what, path );
        }
        else
        {
            expect( ret != 0, what, path );
        }
        count++;
    }
    fclose( f );
    printf( "wycheproof %s: %d vectors run\n", path, count );
}

/* xoshiro256** for test data (not secret) */
static uint64_t rs[4];

static uint64_t xrotl64( uint64_t x, int k )
{
    return ( x << k ) | ( x >> ( 64 - k ) );
}

static uint64_t next_u64( void )
{
    const uint64_t result = xrotl64( rs[1] * 5, 7 ) * 9;
    const uint64_t t = rs[1] << 17;
    rs[2] ^= rs[0];
    rs[3] ^= rs[1];
    rs[1] ^= rs[2];
    rs[0] ^= rs[3];
    rs[2] ^= t;
    rs[3] = xrotl64( rs[3], 45 );
    return result;
}

static void fill( unsigned char * p, size_t n )
{
    while ( n >= 8 )
    {
        const uint64_t v = next_u64();
        memcpy( p, &v, 8 );
        p += 8;
        n -= 8;
    }
    const uint64_t v = next_u64();
    memcpy( p, &v, n );
}

static void seed_rng( uint64_t seed )
{
    for ( int i = 0; i < 4; i++ )
    {
        seed += 0x9e3779b97f4a7c15ULL;
        uint64_t z = seed;
        z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
        z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
        rs[i] = z ^ ( z >> 31 );
    }
}

static int diff( long cases, uint64_t seed )
{
    static unsigned char key[32], nonce[12], aad[256], m[2001], c_ref[2001 + 16], c_arm[2001 + 16], p[2001], s_ref[2001], s_arm[2001];
    long mismatch = 0, forged_accepted = 0, open_fail = 0, stream_mismatch = 0;
    seed_rng( seed );
    for ( long i = 0; i < cases; i++ )
    {
        fill( key, sizeof( key ) );
        fill( nonce, sizeof( nonce ) );
        const size_t alen = (size_t) ( next_u64() % 65 );
        const size_t mlen = (size_t) ( next_u64() % 2001 );
        fill( aad, alen );
        fill( m, mlen );
        unsigned long long clen = 0, plen = 0;
        use_arm( 0 );
        crypto_aead_chacha20poly1305_ietf_encrypt( c_ref, &clen, m, mlen, aad, alen, NULL, nonce, key );
        use_arm( 1 + (int) ( i & 1 ) );
        crypto_aead_chacha20poly1305_ietf_encrypt( c_arm, &clen, m, mlen, aad, alen, NULL, nonce, key );
        if ( memcmp( c_ref, c_arm, mlen + 16 ) != 0 )
        {
            mismatch++;
            if ( mismatch < 5 )
            {
                printf( "MISMATCH case %ld mlen %zu alen %zu\n", i, mlen, alen );
            }
        }
        if ( crypto_aead_chacha20poly1305_ietf_decrypt( p, &plen, NULL, c_arm, clen, aad, alen, nonce, key ) != 0 || plen != mlen ||
             memcmp( p, m, mlen ) != 0 )
        {
            open_fail++;
        }
        /* forgery: flip one bit of ciphertext, tag or aad; both paths must refuse */
        const uint64_t r = next_u64();
        const size_t where = (size_t) ( r % ( clen + alen ) );
        const unsigned char bit = (unsigned char) ( 1u << ( ( r >> 32 ) & 7 ) );
        if ( where < clen )
        {
            c_arm[where] ^= bit;
        }
        else
        {
            aad[where - clen] ^= bit;
        }
        for ( int path = 0; path < 3; path++ )
        {
            use_arm( path == 0 ? 0 : 1 + (int) ( i & 1 ) );
            if ( crypto_aead_chacha20poly1305_ietf_decrypt( p, &plen, NULL, c_arm, clen, aad, alen, nonce, key ) == 0 )
            {
                forged_accepted++;
            }
        }
        /* every 1000th case: the raw streams across the 32-bit counter
           boundary, both layouts, so the carry into word 13 matches */
        if ( i % 1000 <= 1 )
        {
            const uint32_t ic32 = 0xFFFFFFFFu - (uint32_t) ( next_u64() % 8 );
            const uint64_t ic64 = 0xFFFFFFFFull - ( next_u64() % 8 );
            crypto_stream_chacha20_ref_implementation.stream_ietf_ext_xor_ic( s_ref, m, mlen, nonce, ic32, key );
            crypto_stream_chacha20_rn_neon_implementation.stream_ietf_ext_xor_ic( s_arm, m, mlen, nonce, ic32, key );
            stream_mismatch += mlen > 0 && memcmp( s_ref, s_arm, mlen ) != 0;
            crypto_stream_chacha20_ref_implementation.stream_xor_ic( s_ref, m, mlen, nonce, ic64, key );
            crypto_stream_chacha20_rn_neon_implementation.stream_xor_ic( s_arm, m, mlen, nonce, ic64, key );
            stream_mismatch += mlen > 0 && memcmp( s_ref, s_arm, mlen ) != 0;
            crypto_stream_chacha20_ref_implementation.stream( s_ref, mlen, nonce, key );
            crypto_stream_chacha20_rn_neon_implementation.stream( s_arm, mlen, nonce, key );
            stream_mismatch += mlen > 0 && memcmp( s_ref, s_arm, mlen ) != 0;
            /* the original (non-IETF) AEAD too */
            use_arm( 0 );
            crypto_aead_chacha20poly1305_encrypt( c_ref, &clen, m, mlen, aad, alen, NULL, nonce, key );
            use_arm( 1 + (int) ( i & 1 ) );
            crypto_aead_chacha20poly1305_encrypt( c_arm, &clen, m, mlen, aad, alen, NULL, nonce, key );
            stream_mismatch += memcmp( c_ref, c_arm, mlen + 16 ) != 0;
        }
    }
    printf( "diff cases=%ld seed=%llu mismatch=%ld open_fail=%ld forged_accepted=%ld stream_and_original_aead_mismatch=%ld\n", cases,
            (unsigned long long) seed, mismatch, open_fail, forged_accepted, stream_mismatch );
    return mismatch + open_fail + forged_accepted + stream_mismatch == 0 ? 0 : 1;
}

static double now_seconds( void )
{
    struct timespec t;
    clock_gettime( CLOCK_MONOTONIC, &t );
    return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}

static void bench( int iterations )
{
    static const int sizes[4] = { 64, 256, 1200, 1400 };
    unsigned char key[32], nonce[12], ad[16];
    static unsigned char m[1400], c[1416], p[1400];
    memset( key, 7, sizeof( key ) );
    memset( nonce, 0, sizeof( nonce ) );
    memset( ad, 3, sizeof( ad ) );
    for ( int i = 0; i < 1400; i++ )
    {
        m[i] = (unsigned char) i;
    }
    for ( int path = 0; path < 3; path++ )
    {
        use_arm( path );
        for ( int s = 0; s < 4; s++ )
        {
            const int bytes = sizes[s];
            unsigned long long clen = 0, mlen = 0;
            for ( int i = 0; i < 2000; i++ )
            {
                crypto_aead_chacha20poly1305_ietf_encrypt( c, &clen, m, (unsigned long long) bytes, ad, sizeof( ad ), NULL, nonce, key );
            }
            double best_seal = 1e9, best_open = 1e9;
            int bad = 0;
            for ( int rep = 0; rep < 5; rep++ )
            {
                double t0 = now_seconds();
                for ( int i = 0; i < iterations; i++ )
                {
                    nonce[0] = (unsigned char) i;
                    crypto_aead_chacha20poly1305_ietf_encrypt( c, &clen, m, (unsigned long long) bytes, ad, sizeof( ad ), NULL, nonce, key );
                }
                const double seal = ( now_seconds() - t0 ) / iterations;
                t0 = now_seconds();
                for ( int i = 0; i < iterations; i++ )
                {
                    if ( crypto_aead_chacha20poly1305_ietf_decrypt( p, &mlen, NULL, c, clen, ad, sizeof( ad ), nonce, key ) != 0 )
                    {
                        bad++;
                    }
                }
                const double open = ( now_seconds() - t0 ) / iterations;
                best_seal = seal < best_seal ? seal : best_seal;
                best_open = open < best_open ? open : best_open;
            }
            printf( "bench path=%s/%s bytes=%d seal_us=%.3f open_us=%.3f bad=%d\n", picked_chacha20(), picked_poly1305(), bytes, best_seal * 1e6,
                    best_open * 1e6, bad );
        }
    }
}

/* The two primitives alone: ns per call for ChaCha20 (IETF xor, counter 1)
   and Poly1305 (one-shot) at each size, both paths. */
static void parts( int iterations )
{
    static const int sizes[5] = { 64, 128, 256, 1200, 1400 };
    unsigned char key[32], nonce[12], tag[16];
    static unsigned char m[1400], c[1400];
    memset( key, 7, sizeof( key ) );
    memset( nonce, 1, sizeof( nonce ) );
    for ( int path = 0; path < 3; path++ )
    {
        use_arm( path );
        for ( int s = 0; s < 5; s++ )
        {
            const unsigned long long n = (unsigned long long) sizes[s];
            double t0 = now_seconds();
            for ( int i = 0; i < iterations; i++ )
            {
                nonce[0] = (unsigned char) i;
                crypto_stream_chacha20_ietf_xor_ic( c, m, n, nonce, 1, key );
            }
            const double chacha = ( now_seconds() - t0 ) / iterations;
            t0 = now_seconds();
            for ( int i = 0; i < iterations; i++ )
            {
                m[0] = (unsigned char) i;
                crypto_onetimeauth_poly1305( tag, m, n, key );
            }
            const double poly = ( now_seconds() - t0 ) / iterations;
            printf( "parts path=%s/%s bytes=%d chacha20_ns=%.1f poly1305_ns=%.1f\n", picked_chacha20(), picked_poly1305(), sizes[s], chacha * 1e9,
                    poly * 1e9 );
        }
    }
}

#if defined(RN_CTGRIND)
/* Mark key, nonce-derived state and message undefined: memcheck then reports
   any branch or address that depends on them. The tag compare result is
   declassified, as its 0/-1 is public. */
static void ct( void )
{
    unsigned char key[32], nonce[12], ad[16], m[1400], c[1416], p[1400];
    memset( key, 1, sizeof( key ) );
    memset( nonce, 2, sizeof( nonce ) );
    memset( ad, 3, sizeof( ad ) );
    memset( m, 4, sizeof( m ) );
    static const int sizes[6] = { 0, 1, 63, 200, 1200, 1400 };
    for ( int path = 0; path < 3; path++ )
    {
        use_arm( path );
        for ( int s = 0; s < 6; s++ )
        {
            const unsigned long long n = (unsigned long long) sizes[s];
            unsigned long long clen = 0, plen = 0;
            VALGRIND_MAKE_MEM_UNDEFINED( key, sizeof( key ) );
            VALGRIND_MAKE_MEM_UNDEFINED( m, sizeof( m ) );
            VALGRIND_MAKE_MEM_UNDEFINED( ad, sizeof( ad ) );
            crypto_aead_chacha20poly1305_ietf_encrypt( c, &clen, m, n, ad, sizeof( ad ), NULL, nonce, key );
            VALGRIND_MAKE_MEM_UNDEFINED( c, sizeof( c ) );
            int ret = crypto_aead_chacha20poly1305_ietf_decrypt_detached( p, NULL, c, n, c + n, ad, sizeof( ad ), nonce, key );
            VALGRIND_MAKE_MEM_DEFINED( &ret, sizeof( ret ) );
            (void) plen;
            printf( "ct path=%s/%s bytes=%llu ret=%d\n", picked_chacha20(), picked_poly1305(), n, ret );
        }
    }
}
#endif

int main( int argc, char ** argv )
{
    if ( sodium_init() < 0 )
    {
        return 1;
    }
    printf( "sodium_init picked chacha20=%s poly1305=%s\n", picked_chacha20(), picked_poly1305() );
    if ( argc < 2 )
    {
        return 2;
    }
    if ( strcmp( argv[1], "vectors" ) == 0 && argc > 2 )
    {
        for ( int path = 0; path < 3; path++ )
        {
            if ( !use_arm( path ) && path >= 1 )
            {
                printf( "no arm64 path in this build\n" );
                break;
            }
            const char * name = path == 0 ? "portable" : path == 1 ? "arm64-4" : "arm64-8";
            rfc_vectors( name );
            wycheproof( argv[2], name );
        }
        printf( "vectors failures=%d\n", failures );
        return failures == 0 ? 0 : 1;
    }
    if ( strcmp( argv[1], "diff" ) == 0 && argc > 3 )
    {
        return diff( atol( argv[2] ), (uint64_t) strtoull( argv[3], NULL, 10 ) );
    }
    if ( strcmp( argv[1], "bench" ) == 0 )
    {
        bench( argc > 2 ? atoi( argv[2] ) : 100000 );
        return 0;
    }
    if ( strcmp( argv[1], "parts" ) == 0 )
    {
        parts( argc > 2 ? atoi( argv[2] ) : 100000 );
        return 0;
    }
#if defined(RN_CTGRIND)
    if ( strcmp( argv[1], "ct" ) == 0 )
    {
        ct();
        return 0;
    }
#endif
    return 2;
}
