/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_replay.cpp -- the vector twins of RN-1 t14
// (standard/TRANSPORT.md, Replay protection), regenerated through the
// implementation. A vector's bytes are the window depth, the final high-water
// mark, then one verdict byte per probe.
//
// Vectors specify a depth, independently of the library's configured history.
// Compile the real replay source at each vector depth in private namespaces.
// Both copies use the maximum-sized test storage; the minimum copy indexes only
// its first 4096 bits. No differently sized replay object crosses the SDK ABI.
// The build-configured implementation has its own retained-depth boundary test.

#include "test_unit_standard.h"
#include "rocketnet_history.h"
#include "rocketnet_address.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_seams.h"
#include <string.h>

#undef ROCKETNET_HISTORY_PACKETS
#define ROCKETNET_HISTORY_PACKETS 32768
namespace standard_replay_maximum
{
#define rocketnet_replay_already_received rocketnet_replay_max_already_received
#define rocketnet_replay_advance rocketnet_replay_max_advance
#include "rocketnet_replay.cpp"
#undef rocketnet_replay_advance
#undef rocketnet_replay_already_received
}

#undef ROCKETNET_HISTORY_PACKETS
#define ROCKETNET_HISTORY_PACKETS 4096
namespace standard_replay_minimum
{
    using standard_replay_maximum::rocketnet_replay_protection_t;
#define rocketnet_replay_bit rn_min_replay_bit
#define rocketnet_replay_set_bit rn_min_replay_set_bit
#define rocketnet_replay_clear_range rn_min_replay_clear_range
#include "rocketnet_replay.cpp"
#undef rocketnet_replay_clear_range
#undef rocketnet_replay_set_bit
#undef rocketnet_replay_bit
}
#undef ROCKETNET_HISTORY_PACKETS

typedef standard_replay_maximum::rocketnet_replay_protection_t rn_vector_replay_t;
using standard_replay_maximum::rocketnet_replay_max_advance;
using standard_replay_maximum::rocketnet_replay_max_already_received;
using standard_replay_minimum::rocketnet_replay_advance;
using standard_replay_minimum::rocketnet_replay_already_received;

typedef int ( *rn_replay_check_t )( const rn_vector_replay_t *, uint64_t );
typedef void ( *rn_replay_advance_t )( rn_vector_replay_t *, uint64_t );

static void standard_replay_write_u32( uint8_t * out, uint32_t value )
{
    out[0] = (uint8_t) ( value );
    out[1] = (uint8_t) ( value >> 8 );
    out[2] = (uint8_t) ( value >> 16 );
    out[3] = (uint8_t) ( value >> 24 );
}

static void standard_replay_write_u64( uint8_t * out, uint64_t value )
{
    for ( int i = 0; i < 8; i++ )
    {
        out[i] = (uint8_t) ( value >> ( 8 * i ) );
    }
}

static int standard_replay_observe( rn_replay_check_t check, rn_replay_advance_t advance, uint64_t depth,
                                    const uint64_t * marks, int mark_count,
                                    const uint64_t * probes, int probe_count,
                                    uint8_t * out, int capacity )
{
    const int bytes = 4 + 8 + probe_count;
    if ( capacity < bytes )
        return -1;

    rn_vector_replay_t replay;
    memset( &replay, 0, sizeof( replay ) );

    for ( int i = 0; i < mark_count; i++ )
    {
        advance( &replay, marks[i] );
    }

    int at = 0;
    standard_replay_write_u32( out + at, (uint32_t) depth );
    at += 4;
    standard_replay_write_u64( out + at, replay.highest_opened );
    at += 8;

    for ( int i = 0; i < probe_count; i++ )
    {
        out[at++] = (uint8_t) ( check( &replay, probes[i] ) ? 0x00 : 0x01 );
    }

    return at;
}

static const rn_standard_vector_t standard_replay_table[] = {
    RN_STANDARD_VECTOR( "t14.in-window-duplicate", "00100000f20300000000000000010001" ),
    RN_STANDARD_VECTOR( "t14.behind-window", "001000002c10000000000000000100" ),
    RN_STANDARD_VECTOR( "t14.forward-jump-clears", "001000000110000000000000000001" ),
    RN_STANDARD_VECTOR( "t14.window-maximum", "008000000180000000000000000001" ),
};

static int standard_replay_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t14.in-window-duplicate" ) == 0 )
    {
        const uint64_t marks[] = { 1000, 1010 };
        const uint64_t probes[] = { 1000, 1005, 1010, 1011 };
        return standard_replay_observe( rocketnet_replay_already_received, rocketnet_replay_advance,
                                        ROCKETNET_HISTORY_PACKETS_MIN,
                                        marks, 2, probes, 4, out, capacity );
    }

    if ( strcmp( id, "t14.behind-window" ) == 0 )
    {
        const uint64_t marks[] = { ROCKETNET_HISTORY_PACKETS_MIN + 44 };
        const uint64_t probes[] = { 44, 45, ROCKETNET_HISTORY_PACKETS_MIN + 44 };
        return standard_replay_observe( rocketnet_replay_already_received, rocketnet_replay_advance,
                                        ROCKETNET_HISTORY_PACKETS_MIN,
                                        marks, 1, probes, 3, out, capacity );
    }

    if ( strcmp( id, "t14.forward-jump-clears" ) == 0 )
    {
        const uint64_t marks[] = { 1, ROCKETNET_HISTORY_PACKETS_MIN + 1 };
        const uint64_t probes[] = { ROCKETNET_HISTORY_PACKETS_MIN + 1, 1, 2 };
        return standard_replay_observe( rocketnet_replay_already_received, rocketnet_replay_advance,
                                        ROCKETNET_HISTORY_PACKETS_MIN,
                                        marks, 2, probes, 3, out, capacity );
    }

    if ( strcmp( id, "t14.window-maximum" ) == 0 )
    {
        const uint64_t marks[] = { 1, ROCKETNET_HISTORY_PACKETS_MAX + 1 };
        const uint64_t probes[] = { ROCKETNET_HISTORY_PACKETS_MAX + 1, 1, 2 };
        return standard_replay_observe( rocketnet_replay_max_already_received, rocketnet_replay_max_advance,
                                        ROCKETNET_HISTORY_PACKETS_MAX,
                                        marks, 2, probes, 3, out, capacity );
    }

    return -1;
}

static void test_standard_replay_vectors()
{
    const int count = (int) ( sizeof( standard_replay_table ) / sizeof( standard_replay_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_replay_table, count, standard_replay_generate ) == 4 );

    // the maximum-depth copy really is configured at 32768: its ring keeps a
    // sequence one depth behind the high-water mark, while the minimum-depth copy
    // does not

    rn_vector_replay_t replay;
    memset( &replay, 0, sizeof( replay ) );
    rocketnet_replay_max_advance( &replay, ROCKETNET_HISTORY_PACKETS_MAX + 1 );
    rocketnet_unit_check( rocketnet_replay_max_already_received( &replay, 2 ) == 0 );

    memset( &replay, 0, sizeof( replay ) );
    rocketnet_replay_advance( &replay, ROCKETNET_HISTORY_PACKETS_MIN + 1 );
    rocketnet_unit_check( rocketnet_replay_already_received( &replay, 1 ) == 1 );

    uint8_t scratch[1];
    rocketnet_unit_check( standard_replay_generate( "t14.no-such-vector", scratch, 0 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_replay_vectors );
