/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_standard_fragmentation.cpp -- the vector twins of RN-1 t16
// (standard/TRANSPORT.md, Fragmentation and reassembly), regenerated through
// the implementation. The one vector is a reassembly trace: an
// out-of-order final fragment staged and completed, a duplicate fragment
// refused, a fragment-count mismatch refused, and an incomplete payload
// abandoned when a newer payload id claims its slot. The bytes are the
// twelve reassembly counters in the order of the reference table, each as
// one byte.

#include "test_unit_standard.h"

#include "rocketnet_fragment.h"

static void rn_t16_u32( uint8_t * p, uint32_t v )
{
    p[0] = (uint8_t) ( v & 0xffu );
    p[1] = (uint8_t) ( ( v >> 8 ) & 0xffu );
    p[2] = (uint8_t) ( ( v >> 16 ) & 0xffu );
    p[3] = (uint8_t) ( ( v >> 24 ) & 0xffu );
}

struct rn_t16_fragment_t
{
    uint32_t payload_id;
    int index;
    int count;
    int size;
    uint8_t fill;
};

static void rn_t16_feed( struct rocketnet_reassembly_set_t * set, uint64_t * counters, const struct rocketnet_reassembly_counters_t * table, const struct rn_t16_fragment_t * f )
{
    uint8_t buf[64];
    rn_t16_u32( buf, f->payload_id );
    buf[4] = (uint8_t) f->index;
    buf[5] = (uint8_t) ( f->count - 1 );
    memset( buf + 6, f->fill, (size_t) f->size );
    rocketnet_reassembly_process_fragment( set, 0, buf, 6 + f->size, counters, table );
}

static int standard_fragmentation_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "t16.reassembly-trace" ) != 0 )
        return -1;
    if ( capacity < 12 )
        return -1;

    struct rocketnet_reassembly_counters_t table;
    memset( &table, 0, sizeof( table ) );
    table.zero_payload_id = 0;
    table.stale = 1;
    table.count_invalid = 2;
    table.index_out_of_range = 3;
    table.count_mismatch = 4;
    table.size_mismatch = 5;
    table.payload_too_large = 6;
    table.duplicate_index = 7;
    table.accepted = 8;
    table.reassembled = 9;
    table.abandoned = 10;
    table.batch_full = 11;

    uint64_t counters[12];
    memset( counters, 0, sizeof( counters ) );

    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create( NULL, 1, 3, 2000, 4 );
    rocketnet_unit_check( set != NULL );

    // the trace: payload 7 completes with its final fragment arriving first
    // and staged; payload 9 repeats a fragment; payload 10 changes its count;
    // payload 16 takes payload 10's slot unfinished.
    static const struct rn_t16_fragment_t trace[] = {
        { 7, 2, 3, 5, 'A' },
        { 7, 0, 3, 8, 'B' },
        { 7, 1, 3, 8, 'C' },
        { 9, 0, 3, 8, 'D' },
        { 9, 0, 3, 8, 'E' },
        { 10, 0, 4, 8, 'F' },
        { 10, 1, 3, 8, 'G' },
        { 16, 0, 2, 8, 'H' },
    };

    const int n = (int) ( sizeof( trace ) / sizeof( trace[0] ) );
    for ( int i = 0; i < n; i++ )
        rn_t16_feed( set, counters, &table, &trace[i] );

    for ( int i = 0; i < 12; i++ )
        out[i] = (uint8_t) counters[i];

    rocketnet_reassembly_set_destroy( set );
    return 12;
}

static const rn_standard_vector_t standard_fragmentation_table[] = {
    RN_STANDARD_VECTOR( "t16.reassembly-trace", "000000000100000106010100" ),
};

static void test_standard_fragmentation_vectors()
{
    const int count = (int) ( sizeof( standard_fragmentation_table ) / sizeof( standard_fragmentation_table[0] ) );
    rocketnet_unit_check( rn_standard_vectors_walk( standard_fragmentation_table, count, standard_fragmentation_generate ) == count );
}

REGISTER_UNIT_TEST( test_standard_fragmentation_vectors );
