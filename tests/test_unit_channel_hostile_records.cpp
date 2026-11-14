/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Hostile records at each receive bound of layer 4 (rn_channels_dispatch and
// the per-code paths in sdk/rocketnet_channel*.cpp): for every check, the
// record exactly at the limit is taken and the one just past it is refused
// and counted by its own counter. The layer 2 reader checks the block shape
// and span against the same limits before dispatch; these cases pin layer 4's
// own copy, which is what a region with tighter limits than the wire relies
// on. A numbered record is accepted by layer 3 (rn_window_receive_accept)
// first, as on the live path. No clock, socket or transport is used.

#include <string.h>

#include "test_unit_registry.h"
#include "unit_arena.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_receive.h"

#define check rocketnet_unit_check

#define HOSTILE_F ROCKETNET_MESSAGE_FRAGMENT_BYTES
#define HOSTILE_UNITS ( ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16 )
#define HOSTILE_HALF ROCKETNET_MESSAGE_HALF_RANGE

static uint8_t hostile_payload[2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES];

struct hostile_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_channels_t channels;
    rn_window_receive_t receive;
};

static void hostile_reset( hostile_t * h, uint32_t enabled_types )
{
    rn_region_reset( h->region );
    memset( &h->counters, 0, sizeof( h->counters ) );
    check( rn_channels_init( &h->channels, h->region, h->region->num_channels, enabled_types ) );
    rn_window_receive_init( &h->receive, h->region, 0 );
}

static void hostile_create( hostile_t * h, int window, int max_message_bytes, int ring_bytes )
{
    memset( h, 0, sizeof( *h ) );

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = window;
    config.max_message_bytes = max_message_bytes;
    config.receive_message_buffer_bytes = ring_bytes;
    check( rocketnet_message_validate_config( &config ) );

    h->arena = rn_unit_arena_create( &config, 1, &h->counters );
    check( h->arena != NULL );
    h->region = rn_arena_region( h->arena, 0 );
    check( h->region->window == (uint32_t) window );
    check( h->region->ring_bytes == (uint32_t) ring_bytes );
    hostile_reset( h, 0 );
}

static void hostile_record( rocketnet_message_record_t * record, int code, uint32_t number, uint32_t ordering, uint32_t sequencing, int bytes )
{
    memset( record, 0, sizeof( *record ) );
    record->code = code;
    record->number = number;
    record->ordering = ordering;
    record->sequencing = sequencing;
    record->data = hostile_payload;
    record->bytes = bytes;
}

// fragment `index` of a `count`-fragment block numbered from `first`, placed at
// ring unit `ring`
static void hostile_chunk( rocketnet_message_record_t * record, int code, uint32_t first, uint32_t index, uint32_t count, uint32_t ring, int bytes )
{
    hostile_record( record, code, first + index, 0, 0, bytes );
    record->block = 1;
    record->fragment = index;
    record->fragments = count - 1;
    record->ring = ring;
}

// layer 3 takes a numbered record's number first, as rn_ordered_receive does
static int hostile_take( hostile_t * h, const rocketnet_message_record_t * record )
{
    if ( record->code != ROCKETNET_MESSAGE_CODE_UNRELIABLE && record->code != ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED )
    {
        uint32_t slot = 0;
        check( rn_window_receive_accept( &h->receive, record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    }

    rn_delivery_t out;
    return rn_channels_dispatch( &h->channels, record, &out );
}

// the record is taken (held as the first piece of its block, or as named) with no violation
static void hostile_expect_taken( hostile_t * h, const rocketnet_message_record_t * record, int result )
{
    hostile_reset( h, 0 );
    check( hostile_take( h, record ) == result );
    check( h->channels.violations == 0 );
    check( h->channels.error == ROCKETNET_DISCONNECT_REASON_NONE );
}

// the record is refused, counted once by `counter`, and ends the connection
static void hostile_expect_refused( hostile_t * h, const rocketnet_message_record_t * record, int counter )
{
    hostile_reset( h, 0 );
    check( hostile_take( h, record ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( h->counters.values[counter] == 1 );
    check( h->channels.violations == 1 );
    check( h->channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( h->channels.deliveries == 0 );
}

// rn_block_shape_valid (sdk/rocketnet_channel.cpp): count at least two, at most
// max_message_bytes' fragment count and W, the index inside the count, the last
// fragment 1 to F bytes and every other exactly F. Each bound at its edge.

static void test_hostile_block_shape_at_each_bound()
{
    const int code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    const int F = (int) HOSTILE_F;
    const int bad_block = ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK;
    rocketnet_message_record_t record;

    // W = 64 and max_message_bytes = 64F: the window and the size allow the same count
    {
        hostile_t h;
        hostile_create( &h, 64, 64 * F, 128 * F );

        hostile_chunk( &record, code, 0, 0, 1, 0, F );                  // a block of one fragment
        hostile_expect_refused( &h, &record, bad_block );

        hostile_chunk( &record, code, 0, 0, 2, 0, F );
        hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

        hostile_chunk( &record, code, 0, 0, 64, 0, F );                 // count == W == the size's count
        hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

        hostile_chunk( &record, code, 0, 0, 65, 0, F );
        hostile_expect_refused( &h, &record, bad_block );

        hostile_chunk( &record, code, 0, 2, 2, 2 * HOSTILE_UNITS, F );  // index == count
        hostile_expect_refused( &h, &record, bad_block );

        hostile_chunk( &record, code, 0, 0, 2, 0, F - 1 );              // not the last: exactly F
        hostile_expect_refused( &h, &record, bad_block );

        hostile_chunk( &record, code, 0, 1, 2, HOSTILE_UNITS, 1 );      // the last: 1 to F
        hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

        hostile_chunk( &record, code, 0, 1, 2, HOSTILE_UNITS, F );
        hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

        hostile_chunk( &record, code, 0, 1, 2, HOSTILE_UNITS, F + 1 );
        hostile_expect_refused( &h, &record, bad_block );

        rn_unit_arena_destroy( h.arena );
    }

    // max_message_bytes = 4F below W = 64: four fragments, not five
    {
        hostile_t h;
        hostile_create( &h, 64, 4 * F, 16 * F );

        hostile_chunk( &record, code, 0, 0, 4, 0, F );
        hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

        hostile_chunk( &record, code, 0, 0, 5, 0, F );
        hostile_expect_refused( &h, &record, bad_block );

        rn_unit_arena_destroy( h.arena );
    }

    // max_message_bytes = 4F + 1: the fragment count rounds up to five, not six
    {
        hostile_t h;
        hostile_create( &h, 64, 4 * F + 1, 16 * F );

        hostile_chunk( &record, code, 0, 0, 5, 0, F );
        hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

        hostile_chunk( &record, code, 0, 0, 6, 0, F );
        hostile_expect_refused( &h, &record, bad_block );

        rn_unit_arena_destroy( h.arena );
    }
}

// The block's span check, once per path that has its own: code 3
// (rn_channels_dispatch_chunk), code 2 (rn_channels_dispatch_reliable_chunk)
// and code 4 in turn (rn_sequenced_block). A fragment that is not the last
// counts the last one as 16 bytes; the last one counts its own size rounded
// to 16. A block that ends exactly at the ring's end is taken; one unit later,
// or 17 bytes in the ring's last unit, is BAD_RING.

static void hostile_span_edges( hostile_t * h, int code )
{
    const uint32_t F = HOSTILE_F;
    const uint32_t ring = h->region->ring_bytes;
    rocketnet_message_record_t record;

    // a block at the ring's start, base 0
    hostile_chunk( &record, code, 0, 0, 2, 0, (int) F );
    hostile_expect_taken( h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

    // fragment 0 of 2: the block ends at base * 16 + F + 16
    const uint32_t base = ( ring - F - 16 ) / 16;
    hostile_chunk( &record, code, 0, 0, 2, base, (int) F );
    hostile_expect_taken( h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

    hostile_chunk( &record, code, 0, 0, 2, base + 1, (int) F );
    check( ( base + 1 ) * 16 + F <= ring );                                 // the fragment's own span still fits
    hostile_expect_refused( h, &record, ROCKETNET_COUNTER_MESSAGE_BAD_RING );

    // fragment 1 of 2, 16 and 18 bytes: the block ends at base * 16 + F + round16( bytes );
    // at that base it is taken, and one unit later is BAD_RING
    const int sizes[2] = { 16, 18 };
    for ( int i = 0; i < 2; i++ )
    {
        const uint32_t rounded = ( (uint32_t) sizes[i] + 15 ) & ~15u;
        const uint32_t last_base = ( ring - F - rounded ) / 16;
        hostile_chunk( &record, code, 0, 1, 2, last_base + HOSTILE_UNITS, sizes[i] );
        hostile_expect_taken( h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

        hostile_chunk( &record, code, 0, 1, 2, last_base + 1 + HOSTILE_UNITS, sizes[i] );
        hostile_expect_refused( h, &record, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
    }

    // 17 bytes in the ring's last unit rounds up to 32 and writes past the ring
    hostile_chunk( &record, code, 0, 1, 2, ( ring / 16 ) - 1, 17 );
    hostile_expect_refused( h, &record, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
}

static void test_hostile_block_span_ends_at_the_ring_end()
{
    const int F = (int) HOSTILE_F;

    hostile_t h;
    hostile_create( &h, 64, 4 * F, 8 * F );

    hostile_span_edges( &h, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED );
    hostile_span_edges( &h, ROCKETNET_MESSAGE_CODE_RELIABLE );
    hostile_span_edges( &h, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED );

    rn_unit_arena_destroy( h.arena );
}

// The record fields rn_channels_dispatch and rn_channels_dispatch_ordered
// check before any path: a record of 0 bytes is nothing, a code-0 record on a
// connection that does not enable it is TYPE_NOT_ENABLED, a code-0 record on a
// channel past the count is BAD_CHANNEL, and an ordering exactly half the range
// ahead is behind (ORDERING_BEHIND) while W - 1 ahead is held and W ahead is
// ORDERING_AHEAD.

static void test_hostile_record_fields_at_their_bounds()
{
    hostile_t h;
    hostile_create( &h, 64, 4 * (int) HOSTILE_F, 8 * (int) HOSTILE_F );
    rocketnet_message_record_t record;

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 0 );
    hostile_reset( &h, 0 );
    check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( h.channels.deliveries == 0 && h.channels.unreliable_delivered == 0 );
    check( h.channels.violations == 0 );

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0, 0, 1 );
    hostile_reset( &h, 1u << ROCKETNET_CHANNEL_TYPE_RELIABLE );
    check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_TYPE_NOT_ENABLED] == 1 );
    check( h.channels.deliveries == 0 );

    record.channel = h.region->num_channels - 1;
    hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    record.channel = h.region->num_channels;
    hostile_expect_refused( &h, &record, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, HOSTILE_HALF, 0, 1 );
    hostile_expect_refused( &h, &record, ROCKETNET_COUNTER_MESSAGE_ORDERING_BEHIND );

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, h.region->window - 1, 0, 1 );
    hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, h.region->window, 0, 1 );
    hostile_expect_refused( &h, &record, ROCKETNET_COUNTER_MESSAGE_ORDERING_AHEAD );

    rn_unit_arena_destroy( h.arena );
}

// The sequenced paths' 24-bit gaps (sdk/rocketnet_channel_sequenced.cpp) are
// stale at exactly half the range and live one short of it: the ordering
// against the read index, the sequencing against the watermark in turn, the
// sequencing against a held future state of the same ordering, and the
// sequencing against an in-turn block still collecting. A stale record is
// counted and changes nothing; it is never a violation.

static void hostile_expect_stale( hostile_t * h, const rocketnet_message_record_t * record, int counter )
{
    const uint64_t before = h->counters.values[counter];
    check( hostile_take( h, record ) == ROCKETNET_CHANNEL_DISPATCH_STALE );
    check( h->counters.values[counter] == before + 1 );
    check( h->channels.violations == 0 );
}

static void test_hostile_sequenced_gaps_at_half_range()
{
    hostile_t h;
    hostile_create( &h, 64, 4 * (int) HOSTILE_F, 8 * (int) HOSTILE_F );
    const int sequenced = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
    const int stale = ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE;
    const int superseded = ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED;
    rocketnet_message_record_t record;

    // the ordering against the read index (0)
    hostile_record( &record, sequenced, 0, HOSTILE_HALF, 0, 1 );
    hostile_reset( &h, 0 );
    hostile_expect_stale( &h, &record, stale );
    check( h.region->channel_receive[0].held_sequenced == RN_DESCRIPTOR_NONE );
    hostile_record( &record, sequenced, 0, HOSTILE_HALF - 1, 0, 1 );
    hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

    // the sequencing in turn against the watermark (0)
    hostile_record( &record, sequenced, 0, 0, HOSTILE_HALF, 1 );
    hostile_reset( &h, 0 );
    hostile_expect_stale( &h, &record, stale );
    check( h.channels.deliveries == 0 );
    hostile_record( &record, sequenced, 0, 0, HOSTILE_HALF - 1, 1 );
    hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_DELIVERED );

    // the sequencing against a held future state at the same ordering: the
    // same sequencing is stale and the hold keeps its own bytes, one newer
    // replaces it
    {
        hostile_reset( &h, 0 );
        hostile_payload[0] = 0x11;
        hostile_record( &record, sequenced, 0, 1, 5, 1 );
        check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        const uint32_t held = h.region->channel_receive[0].held_sequenced;

        hostile_payload[0] = 0x22;
        hostile_expect_stale( &h, &record, superseded );
        check( h.region->channel_receive[0].held_sequenced == held );
        check( h.region->sequenced_message_payloads[held].data[0] == 0x11 );

        hostile_record( &record, sequenced, 0, 1, 6, 1 );
        check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        check( h.counters.values[superseded] == 2 );
        const uint32_t newer = h.region->channel_receive[0].held_sequenced;
        check( newer != RN_DESCRIPTOR_NONE );
        check( h.region->sequenced_messages_held[newer].sequencing == 6 );
        check( h.region->sequenced_message_payloads[newer].data[0] == 0x22 );
        hostile_payload[0] = 0;
    }

    // the sequencing against a held future state at the same ordering, half the range away
    {
        hostile_reset( &h, 0 );
        hostile_record( &record, sequenced, 0, 1, 5, 1 );
        check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        const uint32_t held = h.region->channel_receive[0].held_sequenced;

        hostile_record( &record, sequenced, 0, 1, 5 + HOSTILE_HALF, 1 );
        hostile_expect_stale( &h, &record, superseded );
        check( h.region->channel_receive[0].held_sequenced == held );
        check( h.region->sequenced_messages_held[held].sequencing == 5 );

        hostile_record( &record, sequenced, 0, 1, 5 + HOSTILE_HALF - 1, 1 );
        check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        check( h.counters.values[superseded] == 2 );
        const uint32_t now = h.region->channel_receive[0].held_sequenced;
        check( now != RN_DESCRIPTOR_NONE );
        check( h.region->sequenced_messages_held[now].sequencing == 5 + HOSTILE_HALF - 1 );
    }

    // the sequencing against an in-turn code-4 block still collecting
    // (sequencing 5): the same sequencing and half the range away are
    // superseded and the block stays; one short of half the range releases it
    {
        hostile_reset( &h, 0 );
        rocketnet_message_record_t chunk;
        hostile_chunk( &chunk, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 0, 2, 0, (int) HOSTILE_F );
        chunk.sequencing = 5;
        check( hostile_take( &h, &chunk ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
        const uint32_t block = h.region->channel_receive[0].block_in_turn;
        check( block != RN_DESCRIPTOR_NONE );

        hostile_record( &record, sequenced, 0, 0, 5, 1 );
        hostile_expect_stale( &h, &record, superseded );
        check( h.region->channel_receive[0].block_in_turn == block );

        hostile_record( &record, sequenced, 0, 0, 5 + HOSTILE_HALF, 1 );
        hostile_expect_stale( &h, &record, superseded );
        check( h.counters.values[stale] == 0 );
        check( h.region->channel_receive[0].block_in_turn == block );

        hostile_record( &record, sequenced, 0, 0, 6, 1 );
        check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
        check( h.region->channel_receive[0].block_in_turn == RN_DESCRIPTOR_NONE );
        check( h.counters.values[superseded] == 3 );
        check( h.channels.violations == 0 );
    }

    rn_unit_arena_destroy( h.arena );
}

// A code-4 block complete in the future-state slot takes no further chunk: a
// chunk of that block dispatched again (layer 3 refuses the number as a
// duplicate on the live path; this is layer 4's own check) is BAD_BLOCK.

static void test_hostile_chunk_past_a_complete_held_block()
{
    hostile_t h;
    hostile_create( &h, 64, 4 * (int) HOSTILE_F, 8 * (int) HOSTILE_F );

    rocketnet_message_record_t chunk[2];
    rn_delivery_t out;
    for ( uint32_t i = 0; i < 2; i++ )
    {
        hostile_chunk( &chunk[i], ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, i, 2, i * HOSTILE_UNITS, (int) HOSTILE_F );
        chunk[i].ordering = 1;
        check( hostile_take( &h, &chunk[i] ) == ROCKETNET_CHANNEL_DISPATCH_HELD );
    }
    check( h.channels.violations == 0 );
    check( h.region->channel_receive[0].held_sequenced != RN_DESCRIPTOR_NONE );

    check( rn_channels_dispatch( &h.channels, &chunk[1], &out ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK] == 1 );
    check( h.channels.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );

    rn_unit_arena_destroy( h.arena );
}

// A code-4 record held as a future state is copied into the ring at its own
// span, its size rounded up to 16: 1 byte anywhere, 16 bytes in the ring's
// last unit and 18 in its last two are taken; 17 bytes in the last unit would
// write one byte past the ring and are BAD_RING.

static void test_hostile_held_reliable_sequenced_span_at_the_ring_end()
{
    hostile_t h;
    hostile_create( &h, 64, 4 * (int) HOSTILE_F, 8 * (int) HOSTILE_F );
    const uint32_t units = h.region->ring_bytes / 16;
    rocketnet_message_record_t record;

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, 0, 1, 0, 1 );
    hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

    record.ring = units - 1;
    record.bytes = 16;
    hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

    record.ring = units - 2;
    record.bytes = 18;
    hostile_expect_taken( &h, &record, ROCKETNET_CHANNEL_DISPATCH_HELD );

    record.ring = units - 1;
    record.bytes = 17;
    hostile_expect_refused( &h, &record, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
    check( h.region->channel_receive[0].held_sequenced == RN_DESCRIPTOR_NONE );

    rn_unit_arena_destroy( h.arena );
}

// A code-1 future state two orderings ahead waits through the ordered
// delivery that opens the epoch before it, and is delivered by the next one.

static void test_hostile_held_state_waits_for_its_epoch()
{
    hostile_t h;
    hostile_create( &h, 64, 4 * (int) HOSTILE_F, 8 * (int) HOSTILE_F );
    rocketnet_message_record_t record;

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 0, 2, 0, 1 );
    check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_HELD );

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, 1 );
    check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( h.channels.deliveries == 1 );
    check( h.region->channel_receive[0].held_sequenced != RN_DESCRIPTOR_NONE );

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1, 1, 0, 1 );
    check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_DELIVERED );
    check( h.channels.deliveries == 3 );
    check( h.region->channel_receive[0].held_sequenced == RN_DESCRIPTOR_NONE );
    check( h.counters.values[ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE] == 0 );
    check( h.channels.violations == 0 );

    rn_unit_arena_destroy( h.arena );
}

// R5, by fault injection: a code-3 record at its turn with the delivery
// table already full (rn_ordered_receive defers before that on the live path,
// so the table is filled by hand) is not delivered past the table: the
// region is exhausted, the read index stays, and the count stays at capacity.

static void test_hostile_reliable_delivery_on_a_full_table_exhausts()
{
    hostile_t h;
    hostile_create( &h, 64, 4 * (int) HOSTILE_F, 8 * (int) HOSTILE_F );
    rocketnet_message_record_t record;

    hostile_record( &record, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 0, 0, 0, 1 );
    h.channels.deliveries = h.channels.delivery_capacity;
    check( hostile_take( &h, &record ) == ROCKETNET_CHANNEL_DISPATCH_NONE );
    check( h.channels.deliveries == h.channels.delivery_capacity );
    check( h.region->channel_receive[0].ordered_read == 0 );
    check( rn_region_exhausted( h.region ) );

    rn_unit_arena_destroy( h.arena );
}

// rn_channels_set_payload_capacity at both bounds: a chunk's record (the
// longest header, then F bytes) and the form byte is the least capacity
// taken, one byte less is refused; the largest payload is taken, one byte
// more is refused. A refusal leaves the capacity as it was.

static void test_hostile_payload_capacity_at_its_bounds()
{
    hostile_t h;
    hostile_create( &h, 64, 4 * (int) HOSTILE_F, 8 * (int) HOSTILE_F );
    const uint32_t least = ROCKETNET_MESSAGE_FRAGMENT_BYTES + ROCKETNET_MESSAGE_RECORD_MAX_HEADER_BYTES + 1;

    check( rn_channels_set_payload_capacity( &h.channels, least ) );
    check( h.channels.payload_capacity == least );
    check( !rn_channels_set_payload_capacity( &h.channels, least - 1 ) );
    check( h.channels.payload_capacity == least );
    check( rn_channels_set_payload_capacity( &h.channels, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) );
    check( h.channels.payload_capacity == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    check( !rn_channels_set_payload_capacity( &h.channels, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 ) );
    check( h.channels.payload_capacity == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

    rn_unit_arena_destroy( h.arena );
}

REGISTER_UNIT_TEST( test_hostile_payload_capacity_at_its_bounds );
REGISTER_UNIT_TEST( test_hostile_reliable_delivery_on_a_full_table_exhausts );
REGISTER_UNIT_TEST( test_hostile_held_reliable_sequenced_span_at_the_ring_end );
REGISTER_UNIT_TEST( test_hostile_held_state_waits_for_its_epoch );
REGISTER_UNIT_TEST( test_hostile_block_shape_at_each_bound );
REGISTER_UNIT_TEST( test_hostile_block_span_ends_at_the_ring_end );
REGISTER_UNIT_TEST( test_hostile_record_fields_at_their_bounds );
REGISTER_UNIT_TEST( test_hostile_sequenced_gaps_at_half_range );
REGISTER_UNIT_TEST( test_hostile_chunk_past_a_complete_held_block );
