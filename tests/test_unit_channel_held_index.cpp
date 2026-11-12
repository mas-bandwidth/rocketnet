/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The held index (sdk/rocketnet_channel.cpp rn_held_find, rn_held_insert and
// rn_held_erase): 2W open-addressed cells keyed by (channel, ordering), linear
// probing that wraps at the last cell, and backward-shift erase. These cases
// place code-3 records ahead of their turn at chosen cells and check the
// layout after each erase: a probe chain that wraps past the last cell, an
// erase inside that chain, an entry that sits at its own home after the hole,
// and a table filled to W - 1 and drained. A key's home is read off the index
// itself (the cell it takes in an empty table), so the cases do not copy the
// hash. Records go through layer 3 (rn_window_receive_accept) first, as on the
// live path. No clock, socket or transport is used.

#include <string.h>

#include "test_unit_registry.h"
#include "unit_arena.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_receive.h"

#define check rocketnet_unit_check

#define HELD_ORDERINGS 16
#define HELD_CHANNELS 32

static uint8_t held_payload[16];

struct held_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_channels_t channels;
    rn_window_receive_t receive;
    uint32_t next_number;
    uint32_t padding;
};

struct held_key_t
{
    uint32_t channel;
    uint32_t ordering;
};

static void held_reset( held_t * h )
{
    rn_region_reset( h->region );
    memset( &h->counters, 0, sizeof( h->counters ) );
    check( rn_channels_init( &h->channels, h->region, h->region->num_channels, 0 ) );
    rn_window_receive_init( &h->receive, h->region, 0 );
    h->next_number = 0;
}

static void held_create( held_t * h )
{
    memset( h, 0, sizeof( *h ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = 64;
    config.max_message_bytes = 4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    config.receive_message_buffer_bytes = 8 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    check( rocketnet_message_validate_config( &config ) );

    h->arena = rn_unit_arena_create( &config, 1, &h->counters );
    check( h->arena != NULL );
    h->region = rn_arena_region( h->arena, 0 );
    check( h->region->window == 64 );
    check( h->region->held_entries == 2 * h->region->window );
    held_reset( h );
}

// one code-3 record of one byte: delivered at its turn, held ahead of it
static int held_send( held_t * h, uint32_t channel, uint32_t ordering )
{
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.number = h->next_number++;
    record.channel = channel;
    record.ordering = ordering;
    record.data = held_payload;
    record.bytes = 1;

    uint32_t slot = 0;
    check( rn_window_receive_accept( &h->receive, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );

    rn_delivery_t out;
    return rn_channels_dispatch( &h->channels, &record, &out );
}

// held ahead of its turn; its number names its entry
static uint32_t held_hold( held_t * h, uint32_t channel, uint32_t ordering )
{
    const uint32_t number = h->next_number;
    check( held_send( h, channel, ordering ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    return number;
}

// the cell whose entry names `number`, or RN_DESCRIPTOR_NONE
static uint32_t held_cell( const held_t * h, uint32_t number )
{
    for ( uint32_t i = 0; i < h->region->held_entries; i++ )
    {
        if ( h->region->held_index[i].used && h->region->held_index[i].number == number )
        {
            return i;
        }
    }

    return RN_DESCRIPTOR_NONE;
}

static uint32_t held_used_cells( const held_t * h )
{
    uint32_t used = 0;
    for ( uint32_t i = 0; i < h->region->held_entries; i++ )
    {
        if ( h->region->held_index[i].used )
        {
            used++;
        }
    }

    return used;
}

// delivers the channel's orderings before the held one at their turn; the
// drain then delivers the held one and erases its entry
static void held_release( held_t * h, held_key_t key )
{
    rn_channel_receive_t * rx = h->region->channel_receive + key.channel;
    const uint32_t first = rx->ordered_read;
    for ( uint32_t ordering = first; ordering < key.ordering; ordering++ )
    {
        check( held_send( h, key.channel, ordering ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    }

    check( rx->ordered_read == key.ordering + 1 );
    check( h->channels.violations == 0 );
    rn_channels_end_update( &h->channels );
}

// homes[o][c]: the cell (channel c, ordering o) takes in an empty index
static void held_map( held_t * h, uint32_t homes[HELD_ORDERINGS][HELD_CHANNELS] )
{
    check( h->region->num_channels >= HELD_CHANNELS );
    for ( uint32_t ordering = 1; ordering < HELD_ORDERINGS; ordering++ )
    {
        for ( uint32_t channel = 0; channel < HELD_CHANNELS; channel++ )
        {
            held_reset( h );
            homes[ordering][channel] = held_cell( h, held_hold( h, channel, ordering ) );
        }
    }

    held_reset( h );
}

// a key homed at `home` on a channel no key in `taken` uses, smallest ordering first
static bool held_pick( uint32_t homes[HELD_ORDERINGS][HELD_CHANNELS], uint32_t home, const held_key_t * taken, int num_taken, held_key_t * out )
{
    for ( uint32_t ordering = 1; ordering < HELD_ORDERINGS; ordering++ )
    {
        for ( uint32_t channel = 0; channel < HELD_CHANNELS; channel++ )
        {
            bool free = homes[ordering][channel] == home;
            for ( int i = 0; i < num_taken; i++ )
            {
                if ( taken[i].channel == channel )
                {
                    free = false;
                }
            }

            if ( free )
            {
                out->channel = channel;
                out->ordering = ordering;
                return true;
            }
        }
    }

    return false;
}

static uint32_t held_homes[HELD_ORDERINGS][HELD_CHANNELS];

// RNOpenAddress Insert and EraseStep across the wrap: two keys homed at the
// last cell and two at cell 0 fill last, 0, 1, 2. Erasing at the last cell
// moves each one back across the wrap; erasing inside the wrapped chain moves
// the one after it back to its home; erasing the chain's head leaves the entry
// past the wrap at its home.

static void test_held_index_chain_wraps_past_the_last_cell()
{
    held_t h;
    held_create( &h );
    held_map( &h, held_homes );
    const uint32_t last = h.region->held_entries - 1;

    held_key_t key[4];
    check( held_pick( held_homes, last, key, 0, &key[0] ) );
    check( held_pick( held_homes, last, key, 1, &key[1] ) );
    check( held_pick( held_homes, 0, key, 2, &key[2] ) );
    check( held_pick( held_homes, 0, key, 3, &key[3] ) );

    uint32_t number[4];
    for ( int i = 0; i < 4; i++ )
    {
        number[i] = held_hold( &h, key[i].channel, key[i].ordering );
    }

    check( held_cell( &h, number[0] ) == last );
    check( held_cell( &h, number[1] ) == 0 );
    check( held_cell( &h, number[2] ) == 1 );
    check( held_cell( &h, number[3] ) == 2 );
    check( h.region->held_count == 4 );

    held_release( &h, key[0] );
    check( held_cell( &h, number[1] ) == last );
    check( held_cell( &h, number[2] ) == 0 );
    check( held_cell( &h, number[3] ) == 1 );
    check( !h.region->held_index[2].used );
    check( held_used_cells( &h ) == 3 );
    check( h.region->held_count == 3 );

    held_release( &h, key[2] );
    check( held_cell( &h, number[1] ) == last );
    check( held_cell( &h, number[3] ) == 0 );
    check( !h.region->held_index[1].used );
    check( held_used_cells( &h ) == 2 );
    check( h.region->held_count == 2 );

    held_release( &h, key[1] );
    check( held_cell( &h, number[3] ) == 0 );
    check( !h.region->held_index[last].used );
    check( held_used_cells( &h ) == 1 );
    check( h.region->held_count == 1 );

    held_release( &h, key[3] );
    check( held_used_cells( &h ) == 0 );
    check( h.region->held_count == 0 );
    check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );
    check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE_ORDERING] == 0 );

    rn_unit_arena_destroy( h.arena );
}

// RNOpenAddress Find past the home: the same four keys, released so that each
// lookup probes past its home. The second key, homed at the last cell, is
// found across the wrap at cell 0; the fourth, homed at 0, at cell 1 behind
// the third.

static void test_held_index_finds_entries_past_their_home()
{
    held_t h;
    held_create( &h );
    held_map( &h, held_homes );
    const uint32_t last = h.region->held_entries - 1;

    held_key_t key[4];
    check( held_pick( held_homes, last, key, 0, &key[0] ) );
    check( held_pick( held_homes, last, key, 1, &key[1] ) );
    check( held_pick( held_homes, 0, key, 2, &key[2] ) );
    check( held_pick( held_homes, 0, key, 3, &key[3] ) );

    uint32_t number[4];
    for ( int i = 0; i < 4; i++ )
    {
        number[i] = held_hold( &h, key[i].channel, key[i].ordering );
    }

    held_release( &h, key[1] );
    check( held_cell( &h, number[0] ) == last );
    check( held_cell( &h, number[2] ) == 0 );
    check( held_cell( &h, number[3] ) == 1 );
    check( !h.region->held_index[2].used );

    held_release( &h, key[3] );
    check( held_cell( &h, number[0] ) == last );
    check( held_cell( &h, number[2] ) == 0 );
    check( !h.region->held_index[1].used );

    held_release( &h, key[0] );
    check( held_cell( &h, number[2] ) == 0 );

    held_release( &h, key[2] );
    check( held_used_cells( &h ) == 0 );
    check( h.region->held_count == 0 );

    rn_unit_arena_destroy( h.arena );
}

// RNOpenAddress EraseStep without the wrap: two keys homed at H and one at
// H + 1 fill H, H + 1, H + 2. Erasing at H moves both back one (each home is
// the hole or before it); erasing at H again leaves the entry at H + 1, its
// home, where it is.

static void test_held_index_entry_at_its_home_stays()
{
    held_t h;
    held_create( &h );
    held_map( &h, held_homes );

    held_key_t key[3];
    uint32_t home = 0;
    bool found = false;
    for ( ; home + 2 < h.region->held_entries && !found; home++ )
    {
        found = held_pick( held_homes, home, key, 0, &key[0] ) && held_pick( held_homes, home, key, 1, &key[1] ) &&
                held_pick( held_homes, home + 1, key, 2, &key[2] );
    }
    check( found );
    home--;

    uint32_t number[3];
    for ( int i = 0; i < 3; i++ )
    {
        number[i] = held_hold( &h, key[i].channel, key[i].ordering );
    }

    check( held_cell( &h, number[0] ) == home );
    check( held_cell( &h, number[1] ) == home + 1 );
    check( held_cell( &h, number[2] ) == home + 2 );

    held_release( &h, key[0] );
    check( held_cell( &h, number[1] ) == home );
    check( held_cell( &h, number[2] ) == home + 1 );
    check( !h.region->held_index[home + 2].used );
    check( h.region->held_count == 2 );

    held_release( &h, key[1] );
    check( held_cell( &h, number[2] ) == home + 1 );
    check( !h.region->held_index[home].used );
    check( h.region->held_count == 1 );

    held_release( &h, key[2] );
    check( held_used_cells( &h ) == 0 );
    check( h.region->held_count == 0 );

    rn_unit_arena_destroy( h.arena );
}

// One channel's orderings 1 .. W - 1 held (numbers 1 .. W - 1, ordering 0's
// number lost), the most a sender with W numbers in flight can leave held:
// W - 1 entries in 2W cells. Ordering 0 arrives and the drain finds and erases
// every entry in turn.

static void test_held_index_holds_w_minus_one_and_drains_in_order()
{
    held_t h;
    held_create( &h );
    const uint32_t W = h.region->window;

    h.next_number = 1;
    for ( uint32_t ordering = 1; ordering < W; ordering++ )
    {
        held_hold( &h, 0, ordering );
    }
    check( h.region->held_count == W - 1 );
    check( held_used_cells( &h ) == W - 1 );

    h.next_number = 0;
    check( held_send( &h, 0, 0 ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );

    rn_channel_receive_t * rx = h.region->channel_receive + 0;
    for ( uint32_t round = 0; round < W && rx->ordered_read < W; round++ )
    {
        rn_channels_end_update( &h.channels );
        rn_channels_drain( &h.channels, 0 );
    }

    check( rx->ordered_read == W );
    check( h.region->held_count == 0 );
    check( held_used_cells( &h ) == 0 );
    check( h.channels.violations == 0 );
    check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 0 );

    rn_unit_arena_destroy( h.arena );
}

// By fault injection, the guards on states the model rules out
// (proofs/tla/RNHeldOwnership.tla HeldNamesLive and HeldBound): an entry
// whose descriptor is no longer live, or is live under another number, is
// refused at its turn and never delivered; an index already at W entries takes
// no further one. Each is a protocol violation.

static void test_held_index_guards_refuse_a_dead_entry_and_a_full_index()
{
    held_t h;
    held_create( &h );
    const uint32_t W = h.region->window;

    for ( int variant = 0; variant < 2; variant++ )
    {
        held_reset( &h );
        const uint32_t number = held_hold( &h, 1, 1 );
        rn_receive_descriptor_t * descriptor = &h.region->receive_descriptors[number & ( W - 1 )];
        if ( variant == 0 )
        {
            descriptor->live = 0;
        }
        else
        {
            descriptor->number += W;
        }

        check( held_send( &h, 1, 0 ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        check( h.channels.deliveries == 1 );
        check( h.region->channel_receive[1].ordered_read == 1 );
        check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
        check( h.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    }

    held_reset( &h );
    h.region->held_count = W;
    check( held_send( &h, 1, 1 ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION] == 1 );
    check( held_used_cells( &h ) == 0 );

    held_reset( &h );
    h.region->held_count = W - 1;
    held_hold( &h, 1, 1 );
    check( h.region->held_count == W );
    check( h.channels.violations == 0 );

    rn_unit_arena_destroy( h.arena );
}

REGISTER_UNIT_TEST( test_held_index_chain_wraps_past_the_last_cell );
REGISTER_UNIT_TEST( test_held_index_finds_entries_past_their_home );
REGISTER_UNIT_TEST( test_held_index_entry_at_its_home_stays );
REGISTER_UNIT_TEST( test_held_index_holds_w_minus_one_and_drains_in_order );
REGISTER_UNIT_TEST( test_held_index_guards_refuse_a_dead_entry_and_a_full_index );
