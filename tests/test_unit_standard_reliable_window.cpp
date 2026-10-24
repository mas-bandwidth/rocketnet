/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_reliable_window.cpp -- the vector twins of RN-2 m5
// (standard/MESSAGES.md, Message numbering and the reliable window). Each
// vector is regenerated through the receive window's own reconstruction: a
// pure state over a default message region, no socket, no thread and no
// clock. The vector bytes are one outcome byte, then the reconstructed u64
// little-endian for an accepted record.

#include "test_unit_standard.h"

#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_receive.h"

struct m5_vector_case_t
{
    const char * id;
    uint64_t reference;
    uint32_t wire;
};

static const m5_vector_case_t m5_vector_cases[] =
{
    { "m5.reconstruct-at-wrap", 16777215u, 0u },
    { "m5.reconstruct-at-half-range", 8388608u, 0u },
    { "m5.reconstruct-duplicate", 100u, 99u },
    { "m5.reconstruct-stale-copy-at-depth", 7668421u, 0u },
};

static const rn_standard_vector_t m5_vector_table[] =
{
    RN_STANDARD_VECTOR( "m5.reconstruct-at-wrap", "000000000100000000" ),
    RN_STANDARD_VECTOR( "m5.reconstruct-at-half-range", "01" ),
    RN_STANDARD_VECTOR( "m5.reconstruct-duplicate", "01" ),
    RN_STANDARD_VECTOR( "m5.reconstruct-stale-copy-at-depth", "01" ),
};

static const m5_vector_case_t * m5_vector_case( const char * id )
{
    const int count = (int) ( sizeof( m5_vector_cases ) / sizeof( m5_vector_cases[0] ) );
    for ( int i = 0; i < count; i++ )
        if ( strcmp( m5_vector_cases[i].id, id ) == 0 )
            return &m5_vector_cases[i];
    return 0;
}

static int m5_reconstruct_generate( const char * id, uint8_t * out, int capacity )
{
    const m5_vector_case_t * vector = m5_vector_case( id );
    if ( !vector )
        return -1;

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
    if ( !arena )
        return -1;
    rn_region_t * region = rn_arena_region( arena, 0 );

    rn_window_receive_t receive;
    rn_window_receive_init( &receive, region, vector->reference );

    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE;
    record.number = vector->wire;
    record.bytes = 1;

    uint32_t slot = RN_DESCRIPTOR_NONE;
    const int outcome = rn_window_receive_accept( &receive, &record, &slot );

    int bytes = -1;
    if ( outcome == RN_WINDOW_RECEIVE_ACCEPT && capacity >= 9 )
    {
        const uint64_t number = region->receive_descriptors[slot].number;
        out[0] = 0x00;
        for ( int i = 0; i < 8; i++ )
            out[1 + i] = (uint8_t) ( number >> ( 8 * i ) );
        bytes = 9;
    }
    else if ( outcome == RN_WINDOW_RECEIVE_DUPLICATE && capacity >= 1 )
    {
        out[0] = 0x01;
        bytes = 1;
    }
    else if ( outcome == RN_WINDOW_RECEIVE_VIOLATION && capacity >= 1 )
    {
        out[0] = 0x02;
        bytes = 1;
    }

    rn_arena_destroy( arena );
    return bytes;
}

static void test_standard_reliable_window_vectors()
{
    const int count = (int) ( sizeof( m5_vector_table ) / sizeof( m5_vector_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( m5_vector_table, count, m5_reconstruct_generate ) == count );
    rocketnet_unit_check( m5_reconstruct_generate( "m5.no-such-vector", NULL, 0 ) == -1 );
}

REGISTER_UNIT_TEST( test_standard_reliable_window_vectors );
