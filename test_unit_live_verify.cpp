/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Unit tests for the live-message verify scheme (test/live_verify.h,
    docs/LIVE-XDP.md sections 3 and 4). Logic only: no socket, no clock, no
    network. The generator writes a message and the checker recomputes its
    length from the sequence it reads, so a round trip must be OK and every
    corruption must be a named result.
*/

#include "test_unit_registry.h"
#include "test/live_verify.h"

#include <stdint.h>
#include <string.h>

static void round_trip_all_modes()
{
    static uint8_t buf[32768];
    const uint32_t max_m = 4096;
    for ( int dir = 0; dir < 2; dir++ )
        for ( int mode = 0; mode < 8; mode++ )
            for ( int ch = 0; ch < 32; ch++ )
                for ( uint32_t s = 0; s < 64; s++ )
                {
                    const int len = rocketnet_live_generate( dir, mode, ch, 7, s, 0x5EED5EE1u, max_m, buf, (int) sizeof(buf) );
                    rocketnet_unit_check( len >= 1 && len <= (int) max_m );
                    int off = -1;
                    rocketnet_unit_check( rocketnet_live_check( buf, len, dir, mode, ch, 7, 0x5EED5EE1u, max_m, &off ) == ROCKETNET_LIVE_OK );
                }
}

static void round_trip_tiny_prefix()
{
    static uint8_t buf[32768];
    const uint32_t max_m = 4096;
    for ( int dir = 0; dir < 2; dir++ )
        for ( int mode = 0; mode < 8; mode++ )
        {
            // sequences that land on the boundary list give the 1..6 prefix
            // lengths; each must round trip
            for ( uint32_t s = 0; s < 2048; s++ )
            {
                const uint32_t len = rocketnet_live_stream_len( mode, 0, 0xABCDu, s, max_m );
                if ( len >= 1 && len <= 6 )
                {
                    const int L = rocketnet_live_generate( dir, mode, 0, 3, s, 0xABCDu, max_m, buf, (int) sizeof(buf) );
                    rocketnet_unit_check( L == (int) len );
                    int off = -1;
                    rocketnet_unit_check( rocketnet_live_check( buf, L, dir, mode, 0, 3, 0xABCDu, max_m, &off ) == ROCKETNET_LIVE_OK );
                }
            }
        }
}

static void tamper_detection()
{
    static uint8_t buf[32768];
    static uint8_t copy[32768];
    const uint32_t max_m = 4096;
    const int len = rocketnet_live_generate( 0, 3, 4, 9, 100, 1, max_m, buf, (int) sizeof(buf) );
    rocketnet_unit_check( len >= 7 );
    int off = -1;
    rocketnet_unit_check( rocketnet_live_check( buf, len, 0, 3, 4, 9, 1, max_m, &off ) == ROCKETNET_LIVE_OK );

    // wrong tag
    memcpy( copy, buf, (size_t) len );
    copy[0] ^= 0x01;
    rocketnet_unit_check( rocketnet_live_check( copy, len, 0, 3, 4, 9, 1, max_m, &off ) == ROCKETNET_LIVE_BAD_HEADER );

    // wrong channel byte
    memcpy( copy, buf, (size_t) len );
    copy[1] ^= 0xFF;
    rocketnet_unit_check( rocketnet_live_check( copy, len, 0, 3, 4, 9, 1, max_m, &off ) == ROCKETNET_LIVE_BAD_HEADER );

    // wrong cycle byte
    memcpy( copy, buf, (size_t) len );
    copy[2] ^= 0x01;
    rocketnet_unit_check( rocketnet_live_check( copy, len, 0, 3, 4, 9, 1, max_m, &off ) == ROCKETNET_LIVE_BAD_HEADER );

    // a body byte out of place
    memcpy( copy, buf, (size_t) len );
    copy[len - 1] ^= 0x40;
    rocketnet_unit_check( rocketnet_live_check( copy, len, 0, 3, 4, 9, 1, max_m, &off ) == ROCKETNET_LIVE_BAD_BYTE );
    rocketnet_unit_check( off == len - 1 );

    // short
    rocketnet_unit_check( rocketnet_live_check( buf, 0, 0, 3, 4, 9, 1, max_m, &off ) == ROCKETNET_LIVE_SHORT );

    // a sequence byte changed: the recomputed length differs, so check
    // reports a bad length (or a bad body byte when the new length still
    // happens to match)
    memcpy( copy, buf, (size_t) len );
    copy[3] ^= 0x01;
    {
        const int res = rocketnet_live_check( copy, len, 0, 3, 4, 9, 1, max_m, &off );
        rocketnet_unit_check( res == ROCKETNET_LIVE_BAD_LENGTH || res == ROCKETNET_LIVE_BAD_BYTE );
    }
}

static void schedule_max_reached_and_bounded()
{
    const uint32_t max_m = 4096;
    int saw_max = 0;
    for ( int mode = 0; mode < 8; mode++ )
    {
        const uint32_t eff = rocketnet_live_mode_max( mode, max_m );
        for ( uint32_t s = 0; s < 4096; s++ )
        {
            const uint32_t l = rocketnet_live_stream_len( mode, 0, 7, s, max_m );
            rocketnet_unit_check( l >= 1 && l <= eff );
            if ( l == eff ) saw_max = 1;
        }
    }
    rocketnet_unit_check( saw_max );
}

void live_verify_round_trip()
{
    round_trip_all_modes();
    round_trip_tiny_prefix();
    tamper_detection();
    schedule_max_reached_and_bounded();
}

REGISTER_UNIT_TEST( live_verify_round_trip );
