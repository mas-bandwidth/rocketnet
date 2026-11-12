/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The resend FIFO links entries by slot index through region->resend_fifo.
// A link that names a slot at or beyond the window (memory corruption) must
// not be followed, in Release as in Debug: the unlink and insert paths skip the
// access and set ack->error to ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION,
// which rn_ordered_decide reports so the connection ends.
//
// Time is an argument; nothing here reads a clock, opens a socket or sleeps.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"
#include "unit_arena.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_send.h"
#include "../sdk/rocketnet_window_ack.h"
#include "../sdk/rocketnet_channel.h"

#define check rocketnet_unit_check

struct resend_bounds_fixture_t
{
    rocketnet_message_config_t config;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
};

static void resend_bounds_open( resend_bounds_fixture_t * f )
{
    check( rocketnet_message_validate_config( &f->config ) );
    f->arena = rn_unit_arena_create( &f->config, 1, NULL );
    check( f->arena != NULL );
    f->region = rn_arena_region( f->arena, 0 );
    rn_region_carve( f->region );
    check( rn_ordered_init( &f->ordered, f->region, &f->config, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
}

// Numbers one reliable record and parks it at the tail of the small sequence.
static uint32_t resend_bounds_add( resend_bounds_fixture_t * f, uint32_t after, uint64_t packet_id )
{
    const uint32_t d = rn_region_send_alloc( f->region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &f->ordered.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    rn_window_ack_commit_packet( &f->ordered.ack, packet_id, &number, 1, 1000.0 );
    const uint32_t slot = (uint32_t) ( number % f->region->window );
    rn_window_ack_park( &f->ordered.ack, after, slot );
    return slot;
}

static void test_resend_fifo_corrupt_link_is_bounded()
{
    // an unlink whose neighbour link is out of range changes nothing

    {
        resend_bounds_fixture_t f;
        memset( &f.config, 0, sizeof( f.config ) );
        rocketnet_message_default_config( &f.config );
        resend_bounds_open( &f );

        const uint32_t a = resend_bounds_add( &f, RN_RESEND_NONE, 1 );
        const uint32_t b = resend_bounds_add( &f, a, 2 );
        check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
        const uint32_t count = f.ordered.ack.resend_count;

        f.region->resend_fifo[b].prev = 0x80000000u | ( f.region->window + 5 );     // linked, prev out of range
        rn_window_ack_park( &f.ordered.ack, RN_RESEND_NONE, b );                  // unlinks b first

        check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        check( f.ordered.ack.resend_count == count );
        check( rn_ordered_decide( &f.ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        rn_unit_arena_destroy( f.arena );
    }

    // an insert behind a slot whose next link is out of range changes nothing

    {
        resend_bounds_fixture_t f;
        memset( &f.config, 0, sizeof( f.config ) );
        rocketnet_message_default_config( &f.config );
        resend_bounds_open( &f );

        const uint32_t a = resend_bounds_add( &f, RN_RESEND_NONE, 1 );
        const uint32_t count = f.ordered.ack.resend_count;
        f.region->resend_fifo[a].next = f.region->window + 9;
        const uint32_t c = resend_bounds_add( &f, a, 2 );
        (void) c;

        check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
        check( f.ordered.ack.resend_count == count );
        rn_unit_arena_destroy( f.arena );
    }
}

// Numbers one reliable record, never sent, and parks it after `after`.
static uint32_t resend_bounds_number( resend_bounds_fixture_t * f, uint32_t after, uint64_t * number )
{
    const uint32_t d = rn_region_send_alloc( f->region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t ring_start = 0;
    check( rn_window_send_first_write( &f->ordered.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    const uint32_t slot = (uint32_t) ( *number % f->region->window );
    rn_window_ack_park( &f->ordered.ack, after, slot );
    return slot;
}

// The walk of the numbers not on the wire runs from the head to the class's
// sent mark. A sequence cut before the mark (a next link set to the end
// marker) must end the connection, not send the cursor past the window.

static void test_resend_walk_stops_at_a_cut_link()
{
    resend_bounds_fixture_t f;
    memset( &f.config, 0, sizeof( f.config ) );
    rocketnet_message_default_config( &f.config );
    resend_bounds_open( &f );

    uint64_t a_number = 0, b_number = 0, c_number = 0;
    const uint32_t a = resend_bounds_number( &f, RN_RESEND_NONE, &a_number );
    rn_window_ack_commit_packet( &f.ordered.ack, 1, &a_number, 1, 1000.0 );          // a on the wire: the mark
    const uint32_t b = resend_bounds_number( &f, RN_RESEND_NONE, &b_number );
    const uint32_t c = resend_bounds_number( &f, b, &c_number );
    (void) c;

    check( rn_window_ack_resend_first( &f.ordered.ack, false ) == b );
    check( rn_window_ack_resend_sent( &f.ordered.ack, false ) == a );
    check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );

    f.region->resend_fifo[b].next = 0x7FFFFFFFu;                                         // small class, end marker

    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[1] = { frame };
    rn_ordered_write( &f.ordered, 1000.0 + 1.0 / 60.0, pointers, 1 );

    check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( rn_ordered_decide( &f.ordered ) == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    rn_unit_arena_destroy( f.arena );
}

// A refused unlink changes nothing, not only the count: the corrupt slot keeps
// both its link fields, its neighbour keeps its link to it, and the sequence's
// head and tail stay. Each of the three bad links is tried alone (only prev,
// only next, then both), at W itself, the first index out of range, and
// beyond it. An unlink that went on with one bad link would write through
// it, outside the FIFO.
static void test_resend_fifo_refused_unlink_changes_nothing()
{
    for ( int which = 0; which < 3; which++ )
    {
        for ( uint32_t beyond = 0; beyond < 6; beyond += 5 )
        {
            resend_bounds_fixture_t f;
            memset( &f.config, 0, sizeof( f.config ) );
            rocketnet_message_default_config( &f.config );
            resend_bounds_open( &f );

            const uint32_t a = resend_bounds_add( &f, RN_RESEND_NONE, 1 );
            const uint32_t b = resend_bounds_add( &f, a, 2 );
            const uint32_t c = resend_bounds_add( &f, b, 3 );
            check( rn_window_ack_resend_next( &f.ordered.ack, false, a ) == b );
            check( rn_window_ack_resend_next( &f.ordered.ack, false, b ) == c );
            check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );

            const uint32_t bad = f.region->window + beyond;
            rn_resend_link_t * entry = &f.region->resend_fifo[b];
            if ( which != 1 )
            {
                entry->prev = 0x80000000u | bad;                                          // linked, prev out of range
            }
            if ( which != 0 )
            {
                entry->next = ( entry->next & 0x80000000u ) | bad;                          // next out of range, class kept
            }

            const rn_resend_link_t corrupt = *entry;
            const rn_resend_link_t before_a = f.region->resend_fifo[a];
            const rn_resend_link_t before_c = f.region->resend_fifo[c];
            const rn_window_ack_t before = f.ordered.ack;

            rn_window_ack_park( &f.ordered.ack, RN_RESEND_NONE, b );                      // unlinks b first: refused

            check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
            check( entry->prev == corrupt.prev && entry->next == corrupt.next );
            check( f.region->resend_fifo[a].prev == before_a.prev && f.region->resend_fifo[a].next == before_a.next );
            check( f.region->resend_fifo[c].prev == before_c.prev && f.region->resend_fifo[c].next == before_c.next );
            check( f.ordered.ack.resend_head == before.resend_head && f.ordered.ack.resend_tail == before.resend_tail );
            check( f.ordered.ack.resend_sent == before.resend_sent );
            check( f.ordered.ack.resend_count == before.resend_count );
            rn_unit_arena_destroy( f.arena );
        }
    }
}

// A corrupt tail (the class's last slot out of range) is refused before the
// insert behind it follows it: the number committed is not linked, the count
// does not move, and the connection ends.
static void test_resend_fifo_corrupt_tail_is_not_followed()
{
    resend_bounds_fixture_t f;
    memset( &f.config, 0, sizeof( f.config ) );
    rocketnet_message_default_config( &f.config );
    resend_bounds_open( &f );

    const uint32_t a = resend_bounds_add( &f, RN_RESEND_NONE, 1 );
    const uint32_t count = f.ordered.ack.resend_count;
    check( f.ordered.ack.resend_tail == a );
    f.ordered.ack.resend_tail = f.region->window + 3;

    const uint32_t d = rn_region_send_alloc( f.region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &f.ordered.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    const uint32_t slot = (uint32_t) ( number % f.region->window );
    rn_window_ack_commit_packet( &f.ordered.ack, 2, &number, 1, 1000.0 );            // links at the tail: refused

    check( f.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    check( f.region->resend_fifo[slot].prev == 0 && f.region->resend_fifo[slot].next == 0 );
    check( f.ordered.ack.resend_count == count );
    check( f.ordered.ack.resend_head == a );
    rn_unit_arena_destroy( f.arena );
}

// The walkers' step names only slots inside the window: a slot at or past W
// has no next.
static void test_resend_next_of_a_slot_outside_the_window()
{
    resend_bounds_fixture_t f;
    memset( &f.config, 0, sizeof( f.config ) );
    rocketnet_message_default_config( &f.config );
    resend_bounds_open( &f );

    resend_bounds_add( &f, RN_RESEND_NONE, 1 );
    for ( int chunk = 0; chunk < 2; chunk++ )
    {
        check( rn_window_ack_resend_next( &f.ordered.ack, chunk == 1, f.region->window ) == RN_RESEND_NONE );
        check( rn_window_ack_resend_next( &f.ordered.ack, chunk == 1, RN_RESEND_NONE ) == RN_RESEND_NONE );
    }
    rn_unit_arena_destroy( f.arena );
}

REGISTER_UNIT_TEST( test_resend_fifo_corrupt_link_is_bounded );
REGISTER_UNIT_TEST( test_resend_walk_stops_at_a_cut_link );
REGISTER_UNIT_TEST( test_resend_fifo_refused_unlink_changes_nothing );
REGISTER_UNIT_TEST( test_resend_fifo_corrupt_tail_is_not_followed );
REGISTER_UNIT_TEST( test_resend_next_of_a_slot_outside_the_window );
