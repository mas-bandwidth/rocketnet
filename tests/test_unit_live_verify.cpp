/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Unit tests for the live-message verify scheme and size schedule
    (tests/live_verify.h, docs/LIVE-XDP.md sections 3 and 4). Logic only: no
    socket, no clock, no network. Known-answer vectors pin the length and
    byte rules, the boundary list, the tiny-prefix rules and each named
    failure (short, bad length, bad header, wrong channel, wrong cycle, bad
    byte at its offset).
*/

#include "test_unit_registry.h"
#include "live_verify.h"

#include <stdint.h>
#include <string.h>

static rocketnet_live_config_t live_verify_default_config( uint32_t seed )
{
    rocketnet_live_config_t config;
    config.seed = seed;
    config.max_message_bytes = 262144;
    return config;
}

static int live_verify_in_list( const uint32_t * list, int n, uint32_t value )
{
    for ( int i = 0; i < n; i++ )
    {
        if ( list[i] == value )
            return 1;
    }
    return 0;
}

// section 4, "The modes' maxima" at the default config
static void live_verify_mode_maxima()
{
    const rocketnet_live_config_t config = live_verify_default_config( 0 );
    rocketnet_unit_check( rocketnet_live_mode_max( &config, 0 ) == 1411 );
    rocketnet_unit_check( rocketnet_live_mode_max( &config, 1 ) == 16373 );
    for ( int mode = 2; mode < 8; mode++ )
        rocketnet_unit_check( rocketnet_live_mode_max( &config, mode ) == 262144 );

    rocketnet_unit_check( rocketnet_live_edge( &config, 0 ) == 1411 );
    rocketnet_unit_check( rocketnet_live_edge( &config, 5 ) == 1411 );
    rocketnet_unit_check( rocketnet_live_edge( &config, 3 ) == 1401 );
    rocketnet_unit_check( rocketnet_live_edge( &config, 7 ) == 1401 );
    rocketnet_unit_check( rocketnet_live_edge( &config, 2 ) == 1408 );
    rocketnet_unit_check( rocketnet_live_edge( &config, 6 ) == 1408 );
    rocketnet_unit_check( rocketnet_live_edge( &config, 4 ) == 1399 );
    rocketnet_unit_check( rocketnet_live_edge( &config, 1 ) == 1404 );

    // a smaller message config clips code 1 and the reliable codes
    rocketnet_live_config_t small = config;
    small.max_message_bytes = 4096;
    rocketnet_unit_check( rocketnet_live_mode_max( &small, 1 ) == 4096 );
    rocketnet_unit_check( rocketnet_live_mode_max( &small, 3 ) == 4096 );
    rocketnet_unit_check( rocketnet_live_mode_max( &small, 0 ) == 1411 );
}

// section 4, the boundary list B
static void live_verify_boundary_list()
{
    const rocketnet_live_config_t config = live_verify_default_config( 0 );
    uint32_t list[ROCKETNET_LIVE_MAX_BOUNDARIES];

    // a reliable mode: the prefix, the edge, the stride multiples, the
    // near-maximum values, each once
    const int n3 = rocketnet_live_boundaries( &config, 3, list );
    const uint32_t expect3[] = { 1, 2, 3, 4, 5, 6, 7, 1400, 1401, 1402, 1151, 1152, 1153, 2303, 2304, 2305, 3455, 3456, 3457,
                                 16383, 16384, 16385, 262144 - 1152 - 1, 262144 - 1152, 262143, 262144 };
    rocketnet_unit_check( n3 == (int) ( sizeof( expect3 ) / sizeof( expect3[0] ) ) );
    for ( int i = 0; i < n3; i++ )
        rocketnet_unit_check( list[i] == expect3[i] );

    // code 1: its transport payload edges and fragment fills, clipped to 16,373
    const int n1 = rocketnet_live_boundaries( &config, 1, list );
    const uint32_t expect1[] = { 1404, 1405, 1406, 2814, 2815, 2816, 4227, 4228, 4229, 16372, 16373, 1403, 16373 - 1153, 16373 - 1152 };
    for ( size_t i = 0; i < sizeof( expect1 ) / sizeof( expect1[0] ); i++ )
        rocketnet_unit_check( live_verify_in_list( list, n1, expect1[i] ) );
    for ( int i = 0; i < n1; i++ )
        rocketnet_unit_check( list[i] >= 1 && list[i] <= 16373 );

    // code 0: everything above 1,411 clips to the maximum and appears once
    const int n0 = rocketnet_live_boundaries( &config, 0, list );
    const uint32_t expect0[] = { 1, 2, 3, 4, 5, 6, 7, 1410, 1411, 1151, 1152, 1153, 258, 259 };
    rocketnet_unit_check( n0 == (int) ( sizeof( expect0 ) / sizeof( expect0[0] ) ) );
    for ( int i = 0; i < n0; i++ )
        rocketnet_unit_check( list[i] == expect0[i] );

    // every list fits and holds no duplicate
    for ( int mode = 0; mode < 8; mode++ )
    {
        const int n = rocketnet_live_boundaries( &config, mode, list );
        rocketnet_unit_check( n > 0 && n <= ROCKETNET_LIVE_MAX_BOUNDARIES );
        for ( int i = 0; i < n; i++ )
            for ( int j = i + 1; j < n; j++ )
                rocketnet_unit_check( list[i] != list[j] );
        rocketnet_unit_check( rocketnet_live_sequences_per_stream( &config, mode ) == 32u * ( ( (uint32_t) n + 31u ) / 32u ) );
    }
}

// section 4, the length rule: known answers per slot
static void live_verify_length_rule()
{
    const rocketnet_live_config_t config = live_verify_default_config( 5 );
    uint32_t list[ROCKETNET_LIVE_MAX_BOUNDARIES];
    const int n = rocketnet_live_boundaries( &config, 3, list );       // 26 entries, one per channel step

    // r = 0 on channel 0 at s = 0, 32, 64: B[0], B[1], B[2]
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 0, 0 ) == list[0] );
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 0, 32 ) == list[1] );
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 0, 64 ) == list[2] );
    // r = 0 on channel 3 is s = 29: B[(0 + 3 x 1) % 26]
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 3, 29 ) == list[3 % n] );

    // r = 1 to 23, small: 7 + (((s + seed) * 1023) % (1401 - 7 + 1))
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 0, 1 ) == 7u + ( ( ( 1u + 5u ) * 1023u ) % 1395u ) );
    // r = 24 to 29, medium: 1402 + (((s + seed) * 1023) % (16384 - 1402 + 1))
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 0, 24 ) == 1402u + ( ( ( 24u + 5u ) * 1023u ) % 14983u ) );
    // r = 30 and 31, large: 16385 + (((s + seed) * 1023) % (262144 - 16385 + 1))
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 0, 30 ) == 16385u + ( ( ( 30u + 5u ) * 1023u ) % 245760u ) );

    // the 64-bit rule past s x 1023 = 2^32: s = 5,000,030 is slot 30, large
    rocketnet_unit_check( rocketnet_live_length( &config, 3, 0, 5000030u ) ==
                          (uint32_t) ( 16385u + ( ( ( 5000030ull + 5u ) * 1023u ) % 245760u ) ) );

    // code 0 has no medium or large room: those slots are small
    rocketnet_unit_check( rocketnet_live_length( &config, 0, 0, 24 ) == 7u + ( ( ( 24u + 5u ) * 1023u ) % 1405u ) );
    rocketnet_unit_check( rocketnet_live_length( &config, 0, 0, 31 ) == 7u + ( ( ( 31u + 5u ) * 1023u ) % 1405u ) );

    // every length of every stream is within 1 and the mode's maximum, and
    // across the channels each (mode) walks all of B
    for ( int mode = 0; mode < 8; mode++ )
    {
        const uint32_t m = rocketnet_live_mode_max( &config, mode );
        const uint32_t per = rocketnet_live_sequences_per_stream( &config, mode );
        const int nb = rocketnet_live_boundaries( &config, mode, list );
        int covered[ROCKETNET_LIVE_MAX_BOUNDARIES];
        memset( covered, 0, sizeof( covered ) );
        const int channels = ( mode == 1 || mode == 3 || mode == 4 || mode == 7 ) ? 31 : 32;
        for ( int channel = 0; channel < channels; channel++ )
            for ( uint32_t s = 0; s < per; s++ )
            {
                const uint32_t length = rocketnet_live_length( &config, mode, channel, s );
                rocketnet_unit_check( length >= 1 && length <= m );
                for ( int i = 0; i < nb; i++ )
                {
                    if ( list[i] == length && ( ( s + (uint32_t) channel ) % 32u ) == 0 )
                        covered[i] = 1;
                }
            }
        for ( int i = 0; i < nb; i++ )
            rocketnet_unit_check( covered[i] );
    }
}

// section 4, the bucket names
static void live_verify_buckets()
{
    const rocketnet_live_config_t config = live_verify_default_config( 0 );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 1 ) == ROCKETNET_LIVE_BUCKET_TINY );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 6 ) == ROCKETNET_LIVE_BUCKET_TINY );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 7 ) == ROCKETNET_LIVE_BUCKET_SMALL );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 1401 ) == ROCKETNET_LIVE_BUCKET_EDGE );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 1402 ) == ROCKETNET_LIVE_BUCKET_EDGE );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 2304 ) == ROCKETNET_LIVE_BUCKET_FRAGMENT );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 5000 ) == ROCKETNET_LIVE_BUCKET_MEDIUM );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 16385 ) == ROCKETNET_LIVE_BUCKET_LARGE );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 262144 ) == ROCKETNET_LIVE_BUCKET_MAX );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 3, 262145 ) == ROCKETNET_LIVE_BUCKET_OVER );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 1, 2815 ) == ROCKETNET_LIVE_BUCKET_FRAGMENT );
    rocketnet_unit_check( rocketnet_live_bucket( &config, 0, 1411 ) == ROCKETNET_LIVE_BUCKET_MAX );
    rocketnet_unit_check( strcmp( rocketnet_live_bucket_name( ROCKETNET_LIVE_BUCKET_FRAGMENT ), "fragment" ) == 0 );
}

// section 3, known-answer bytes of one message, and every stream round trips
static void live_verify_generate_and_check()
{
    const rocketnet_live_config_t config = live_verify_default_config( 5 );
    static uint8_t buffer[262144];

    // direction 1, mode 3, channel 4, cycle 9, sequence 1: small, 7 + 6138 % 1395
    const int length = rocketnet_live_generate( &config, 1, 3, 4, 9, 1, buffer, (int) sizeof( buffer ) );
    rocketnet_unit_check( length == (int) rocketnet_live_length( &config, 3, 4, 1 ) );
    rocketnet_unit_check( buffer[0] == 0xAB );          // 0xA0 | 1 << 3 | 3
    rocketnet_unit_check( buffer[1] == 4 );
    rocketnet_unit_check( buffer[2] == 9 );
    rocketnet_unit_check( buffer[3] == 1 && buffer[4] == 0 && buffer[5] == 0 && buffer[6] == 0 );
    rocketnet_unit_check( buffer[7] == 8 && buffer[255] == 0 );
    rocketnet_unit_check( rocketnet_live_sequence( buffer, length ) == 1 );

    // a sequence above 2^24 is carried little endian in all four bytes
    const uint32_t s = 0x01020304u;
    const int l2 = rocketnet_live_generate( &config, 0, 2, 0, 0, s, buffer, (int) sizeof( buffer ) );
    if ( l2 >= ROCKETNET_LIVE_HEADER_BYTES )
    {
        rocketnet_unit_check( buffer[3] == 0x04 && buffer[4] == 0x03 && buffer[5] == 0x02 && buffer[6] == 0x01 );
        rocketnet_unit_check( rocketnet_live_sequence( buffer, l2 ) == s );
    }

    // capacity too small: nothing written
    rocketnet_unit_check( rocketnet_live_generate( &config, 0, 3, 0, 0, 30, buffer, 100 ) == 0 );

    for ( int direction = 0; direction < 2; direction++ )
        for ( int mode = 0; mode < 8; mode++ )
            for ( int channel = 0; channel < 32; channel += 7 )
                for ( uint32_t sequence = 0; sequence < 64; sequence++ )
                {
                    const int l = rocketnet_live_generate( &config, direction, mode, channel, 2, sequence, buffer, (int) sizeof( buffer ) );
                    rocketnet_unit_check( l >= 1 );
                    rocketnet_unit_check( rocketnet_live_check( &config, buffer, l, direction, mode, channel, 2, -1, NULL ) == ROCKETNET_LIVE_OK );
                    rocketnet_unit_check( rocketnet_live_check( &config, buffer, l, direction, mode, channel, 2, sequence, NULL ) == ROCKETNET_LIVE_OK );
                }
}

// section 3, messages under 7 bytes carry the first L bytes of the header
static void live_verify_tiny_prefix()
{
    const rocketnet_live_config_t config = live_verify_default_config( 0 );
    uint8_t buffer[16];
    // channel 0, r = 0 at s = 0..6 x 32: B[0..6] = 1..7
    for ( uint32_t k = 0; k < 6; k++ )
    {
        const uint32_t sequence = k * 32u;
        const int l = rocketnet_live_generate( &config, 1, 2, 0, 3, sequence, buffer, (int) sizeof( buffer ) );
        rocketnet_unit_check( l == (int) ( k + 1 ) );
        rocketnet_unit_check( buffer[0] == 0xAA );
        if ( l >= 2 )
            rocketnet_unit_check( buffer[1] == 0 );
        if ( l >= 3 )
            rocketnet_unit_check( buffer[2] == 3 );
        if ( l >= 4 )
            rocketnet_unit_check( buffer[3] == (uint8_t) sequence );
        // without a sequence the check proves the bytes it carries; with the
        // expected one it proves the length too
        rocketnet_unit_check( rocketnet_live_check( &config, buffer, l, 1, 2, 0, 3, -1, NULL ) == ROCKETNET_LIVE_OK );
        rocketnet_unit_check( rocketnet_live_check( &config, buffer, l, 1, 2, 0, 3, sequence, NULL ) == ROCKETNET_LIVE_OK );
        rocketnet_unit_check( rocketnet_live_check( &config, buffer, l, 1, 2, 0, 3, sequence + 1, NULL ) == ROCKETNET_LIVE_BAD_LENGTH );
    }
    // a 3-byte message with the wrong cycle, a 2-byte one on the wrong channel
    const int l3 = rocketnet_live_generate( &config, 0, 3, 0, 1, 64, buffer, (int) sizeof( buffer ) );
    rocketnet_unit_check( l3 == 3 );
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, l3, 0, 3, 0, 2, -1, NULL ) == ROCKETNET_LIVE_BAD_CYCLE );
    const int l2 = rocketnet_live_generate( &config, 0, 3, 0, 1, 32, buffer, (int) sizeof( buffer ) );
    rocketnet_unit_check( l2 == 2 );
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, l2, 0, 3, 1, 1, -1, NULL ) == ROCKETNET_LIVE_BAD_CHANNEL );
}

// section 3, each failure is a named result
static void live_verify_named_failures()
{
    const rocketnet_live_config_t config = live_verify_default_config( 7 );
    static uint8_t buffer[262144];
    static uint8_t copy[262144];
    const uint32_t sequence = 100;      // r = 4 on channel 0: small
    const int length = rocketnet_live_generate( &config, 0, 6, 0, 9, sequence, buffer, (int) sizeof( buffer ) );
    rocketnet_unit_check( length >= ROCKETNET_LIVE_HEADER_BYTES + 2 );
    int offset = 0;
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, length, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_OK );
    rocketnet_unit_check( offset == -1 );

    rocketnet_unit_check( rocketnet_live_check( &config, buffer, 0, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_SHORT );
    rocketnet_unit_check( rocketnet_live_check( &config, NULL, length, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_SHORT );

    // the wrong direction, mode or top nibble is a bad header
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, length, 1, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_HEADER );
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, length, 0, 2, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_HEADER );
    memcpy( copy, buffer, (size_t) length );
    copy[0] = (uint8_t) ( ( copy[0] & 0x0F ) | 0x50 );
    rocketnet_unit_check( rocketnet_live_check( &config, copy, length, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_HEADER );
    rocketnet_unit_check( offset == 0 );

    // the wrong channel and the wrong cycle (a message from an earlier connection)
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, length, 0, 6, 5, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_CHANNEL );
    rocketnet_unit_check( offset == 1 );
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, length, 0, 6, 0, 8, -1, &offset ) == ROCKETNET_LIVE_BAD_CYCLE );
    rocketnet_unit_check( offset == 2 );

    // the right bytes at the wrong length: truncated by one, and one longer
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, length - 1, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_LENGTH );
    buffer[length] = (uint8_t) ( ( (uint32_t) length + sequence ) % 256u );
    rocketnet_unit_check( rocketnet_live_check( &config, buffer, length + 1, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_LENGTH );

    // a duplicate of another sequence's length with this sequence's header:
    // the length rule refuses it
    memcpy( copy, buffer, (size_t) length );
    copy[3] = (uint8_t) ( sequence + 1 );
    rocketnet_unit_check( rocketnet_live_check( &config, copy, length, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_LENGTH );

    // a body byte out of place, named at its offset
    memcpy( copy, buffer, (size_t) length );
    copy[length - 1] ^= 0x40;
    rocketnet_unit_check( rocketnet_live_check( &config, copy, length, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_BYTE );
    rocketnet_unit_check( offset == length - 1 );
    memcpy( copy, buffer, (size_t) length );
    copy[7] ^= 0x01;
    rocketnet_unit_check( rocketnet_live_check( &config, copy, length, 0, 6, 0, 9, -1, &offset ) == ROCKETNET_LIVE_BAD_BYTE );
    rocketnet_unit_check( offset == 7 );

    rocketnet_unit_check( strcmp( rocketnet_live_result_name( ROCKETNET_LIVE_BAD_CYCLE ), "bad-cycle" ) == 0 );
}

void test_live_verify_round_trip()
{
    live_verify_mode_maxima();
    live_verify_boundary_list();
    live_verify_length_rule();
    live_verify_buckets();
    live_verify_generate_and_check();
    live_verify_tiny_prefix();
    live_verify_named_failures();
}

REGISTER_UNIT_TEST( test_live_verify_round_trip );
