/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Receive-descriptor ownership and the held-ordered index's bound
// (proofs/tla/RNHeldOwnership.tla). A code-4 block that is cancelled releases only
// the descriptors it owns; a held entry names its descriptor's full identity
// and is checked before its ring span is delivered; the held index holds at
// most W entries, and its erase ends on any table. A breach is a protocol
// violation of the one connection that caused it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_unit_registry.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_message_object.h"

#define check rocketnet_unit_check

struct ho_node_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
};

// W = 64, the smallest window, so the held index is 128 cells

static void ho_init( ho_node_t * node )
{
    memset( node, 0, sizeof( *node ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.packets_in_flight = 64;
    config.max_message_bytes = 32768;       // a block is at most W fragments
    check( rocketnet_message_validate_config( &config ) == 1 );

    node->arena = rn_arena_create( NULL, &config, 1, &node->counters );
    check( node->arena != NULL );
    node->region = rn_arena_region( node->arena, 0 );
    check( rn_ordered_init( &node->ordered, node->region, &config, NULL, 8, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    check( node->region->window == 64 && node->region->held_entries == 128 );
    check( node->ordered.channels.num_channels >= 32 );
}

static rocketnet_message_record_t ho_record( int code, uint32_t number, uint32_t channel, uint32_t ordering, uint32_t sequencing,
                                             const uint8_t * data, int bytes, uint32_t ring )
{
    rocketnet_message_record_t record = {};
    record.code = code;
    record.number = number;
    record.channel = channel;
    record.ordering = ordering;
    record.sequencing = sequencing;
    record.ring = ring;
    record.data = data;
    record.bytes = bytes;
    return record;
}

static rocketnet_message_record_t ho_fragment( uint32_t number, uint32_t ordering, uint32_t sequencing, const uint8_t * data )
{
    // fragment 0 of a two-fragment code-4 block on channel 0, its base at ring 0
    rocketnet_message_record_t record = ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, number, 0, ordering, sequencing, data,
                                                   ROCKETNET_MESSAGE_FRAGMENT_BYTES, 0 );
    record.block = 1;
    record.fragment = 0;
    record.fragments = 1;
    return record;
}

// layer 3 then layer 4, as rn_ordered_receive runs them: -1 when the window refuses the number

static int ho_receive( ho_node_t * node, const rocketnet_message_record_t & record )
{
    uint32_t slot = RN_DESCRIPTOR_NONE;
    if ( rn_window_receive_accept( &node->ordered.receive, &record, &slot ) != RN_WINDOW_RECEIVE_ACCEPT )
        return -1;

    rn_delivery_t out;
    return rn_channels_dispatch( &node->ordered.channels, &record, &out );
}

static uint32_t ho_held_used( const ho_node_t * node )
{
    uint32_t used = 0;
    for ( uint32_t i = 0; i < node->region->held_entries; i++ )
        used += node->region->held_index[i].used ? 1 : 0;
    return used;
}

// every channel-1 ordered delivery but the in-turn trigger is the held message, byte for byte

static void ho_check_channel_one( const ho_node_t * node, const uint8_t * trigger, const uint8_t * held, int held_bytes )
{
    for ( uint32_t i = 0; i < node->ordered.channels.deliveries; i++ )
    {
        const rn_delivery_t & delivery = node->region->deliveries[i];
        if ( delivery.channel != 1 || delivery.code != ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED || delivery.data == trigger )
            continue;

        if ( delivery.bytes != held_bytes || memcmp( delivery.data, held, (size_t) held_bytes ) != 0 )
            printf( "channel 1 delivered %d bytes that are not the held message's %d\n", delivery.bytes, held_bytes );

        check( delivery.bytes == held_bytes );
        check( memcmp( delivery.data, held, (size_t) held_bytes ) == 0 );
    }
}

// A code-4 block on channel 0 claims numbers 0 and 1; number 1 is a held
// code-3 message on channel 1. A newer channel-0 state supersedes the
// incomplete block. The release spares number 1, and the claim is a protocol
// violation (BAD_BLOCK).

static void test_sequenced_block_release_spares_a_foreign_hold()
{
    ho_node_t node;
    ho_init( &node );

    uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES] = {};
    const uint8_t held[4] = { 0xA1, 0xA2, 0xA3, 0xA4 };
    const uint8_t newest = 9;

    check( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 1, 0, held, 4, 100 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( ho_receive( &node, ho_fragment( 0, 0, 0, chunk ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( node.region->receive_descriptors[1].live != 0 );

    const uint64_t bad_block = node.counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK];
    const uint64_t violations = node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION];

    ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 2, 0, 0, 1, &newest, 1, 200 ) );

    if ( !node.region->receive_descriptors[1].live )
        printf( "the cancelled channel-0 block released channel 1's held descriptor\n" );

    check( node.region->receive_descriptors[1].live != 0 );
    check( node.region->receive_descriptors[1].channel == 1 );
    check( node.region->receive_descriptors[1].code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
    check( node.region->receive_descriptors[0].live == 0 );     // the block's own fragment is released
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK] == bad_block + 1 );
    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == violations + 1 );

    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_sequenced_block_release_spares_a_foreign_hold );

// The same claim, then number 1 + W reuses slot 1 with a different ring span
// whose bytes were never written for it (they stand in for an earlier
// connection's data), then channel 1's in-turn message drains the hold. The
// drain delivers the held message's own bytes or nothing: never the reused
// descriptor's span.

static void test_sequenced_block_release_never_delivers_stale_ring_bytes()
{
    ho_node_t node;
    ho_init( &node );

    const uint32_t W = node.region->window;
    uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES] = {};
    const uint8_t held[4] = { 0xA1, 0xA2, 0xA3, 0xA4 };
    const uint8_t newest = 9;
    const uint8_t trigger = 7;
    uint8_t reuse[32];
    memset( reuse, 0x11, sizeof( reuse ) );

    check( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 1, 0, held, 4, 100 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( ho_receive( &node, ho_fragment( 0, 0, 0, chunk ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 2, 0, 0, 1, &newest, 1, 200 ) );
    rn_ordered_end_update( &node.ordered );

    memset( node.region->receive_ring + 300 * 16, 0x5A, 64 );

    ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 1 + W, 0, 0, 0, reuse, (int) sizeof( reuse ), 300 ) );
    ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 3, 1, 0, 0, &trigger, 1, 400 ) );

    ho_check_channel_one( &node, &trigger, held, (int) sizeof( held ) );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    rn_ordered_end_update( &node.ordered );
    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_sequenced_block_release_never_delivers_stale_ring_bytes );

// The drain's identity check alone: a held entry's descriptor is freed by
// hand (an invariant broken from outside the code path above) and its slot
// reused by number 1 + W. The drain refuses the entry as a protocol
// violation instead of delivering the reused descriptor's ring span.

static void test_held_entry_with_a_reused_descriptor_is_refused()
{
    ho_node_t node;
    ho_init( &node );

    const uint32_t W = node.region->window;
    const uint8_t held[4] = { 0xA1, 0xA2, 0xA3, 0xA4 };
    const uint8_t filler = 3;
    const uint8_t trigger = 7;
    uint8_t reuse[32];
    memset( reuse, 0x11, sizeof( reuse ) );

    check( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 1, 0, held, 4, 100 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 0, 0, 0, 0, &filler, 1, 0 ) ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    rn_ordered_end_update( &node.ordered );

    node.region->receive_descriptors[1].live = 0;
    memset( node.region->receive_ring + 300 * 16, 0x5A, 64 );

    check( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE, 1 + W, 0, 0, 0, reuse, (int) sizeof( reuse ), 300 ) ) ==
           ROCKETNET_CHANNEL_DISPATCH_DELIVERED );

    const uint64_t violations = node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION];

    ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 1, 0, 0, &trigger, 1, 400 ) );

    ho_check_channel_one( &node, &trigger, held, (int) sizeof( held ) );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == violations + 1 );

    rn_ordered_end_update( &node.ordered );
    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_held_entry_with_a_reused_descriptor_is_refused );

// Each round claims one held code-3 number (a distinct (channel, ordering)
// key on channels 1 to 31) inside a cancelled channel-0 block. The held index
// never holds more than W entries. A full index (2W) is checked for before
// the drain that would erase from it: the erase scans for an empty cell, so
// the test fails here instead of hanging.

static void test_held_index_stays_within_the_window()
{
    ho_node_t node;
    ho_init( &node );

    const uint32_t W = node.region->window;
    uint8_t chunk[ROCKETNET_MESSAGE_FRAGMENT_BYTES] = {};
    const uint8_t held[4] = { 0xA1, 0xA2, 0xA3, 0xA4 };
    const uint8_t newest = 9;
    uint32_t most = 0;
    uint32_t rounds = 0;

    for ( uint32_t r = 0; r < 2 * W + 8; r++ )
    {
        const uint32_t n = 3 * r;
        const uint32_t channel = 1 + r % 31;
        const uint32_t ordering = 1 + r / 31;

        if ( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, n + 1, channel, ordering, 0, held, 4, 100 ) ) < 0 ||
             ho_receive( &node, ho_fragment( n, 0, 2 * r, chunk ) ) < 0 ||
             ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, n + 2, 0, 0, 2 * r + 1, &newest, 1, 200 ) ) < 0 )
            break;

        rn_ordered_end_update( &node.ordered );
        rounds++;

        const uint32_t used = ho_held_used( &node );
        most = used > most ? used : most;
    }

    if ( most > W )
        printf( "the held index reached %u entries (W = %u, %u cells) in %u rounds\n", most, W, node.region->held_entries, rounds );

    check( most <= W );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    // a full index is the one an erase never leaves: refuse to run it

    check( ho_held_used( &node ) < node.region->held_entries );

    const uint8_t trigger = 7;
    ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 3 * rounds + 3, 1, 0, 0, &trigger, 1, 400 ) );
    ho_check_channel_one( &node, &trigger, held, (int) sizeof( held ) );

    rn_ordered_end_update( &node.ordered );
    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_held_index_stays_within_the_window );

// The erase's own bound: an index filled by hand to all 2W cells (an
// invariant broken from outside; the code's W bound keeps it from filling).
// The drain reaches the one entry with a live descriptor, and its erase finds
// no empty cell. It stops after one pass and the connection ends; before the
// bound this scan never returned.

static void test_held_erase_ends_on_a_full_index()
{
    ho_node_t node;
    ho_init( &node );

    rn_region_t * region = node.region;
    const uint8_t held[4] = { 0xA1, 0xA2, 0xA3, 0xA4 };
    const uint8_t trigger = 7;

    check( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 1, 0, held, 4, 100 ) ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    check( ho_held_used( &node ) == 1 );

    for ( uint32_t i = 0, k = 0; i < region->held_entries; i++ )
    {
        if ( region->held_index[i].used )
            continue;

        region->held_index[i].key = ( (uint64_t) 3 << 32 ) | ( 1 + k++ );
        region->held_index[i].number = 0;
        region->held_index[i].used = 1;
    }

    check( ho_held_used( &node ) == region->held_entries );

    ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2, 1, 0, 0, &trigger, 1, 400 ) );

    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    for ( uint32_t i = 0; i < node.ordered.channels.deliveries; i++ )
        check( region->deliveries[i].data == &trigger );

    rn_ordered_end_update( &node.ordered );
    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_held_erase_ends_on_a_full_index );

// The W + 1st refusal, reached by hand: the held index is filled with W entries
// (the bound the 2W cells are sized for) and held_count says so. A code-3
// message ahead of its turn is then refused by rn_held_insert: one
// PROTOCOL_VIOLATION, the connection ends, the index and its count are
// unchanged, and no entry is made for the message (RNHeldOwnership Hold).

static void test_held_insert_refuses_the_window_plus_first_entry()
{
    ho_node_t node;
    ho_init( &node );

    rn_region_t * region = node.region;
    const uint32_t W = region->window;
    const uint8_t held[4] = { 0xA1, 0xA2, 0xA3, 0xA4 };

    for ( uint32_t k = 0; k < W; k++ )
    {
        rn_held_entry_t & entry = region->held_index[2 * k];
        entry.key = ( (uint64_t) 3 << 32 ) | ( 100 + k );
        entry.number = 0;
        entry.used = 1;
    }

    region->held_count = W;
    check( ho_held_used( &node ) == W );

    const uint64_t before = node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION];

    check( ho_receive( &node, ho_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 1, 0, held, 4, 100 ) ) == ROCKETNET_CHANNEL_DISPATCH_NONE );

    check( node.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == before + 1 );
    check( node.ordered.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( region->held_count == W );
    check( ho_held_used( &node ) == W );

    rn_ordered_end_update( &node.ordered );
    rn_arena_destroy( node.arena );
}

REGISTER_UNIT_TEST( test_held_insert_refuses_the_window_plus_first_entry );
