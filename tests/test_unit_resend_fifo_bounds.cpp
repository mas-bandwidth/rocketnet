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
    f->arena = rn_arena_create( NULL, &f->config, 1, NULL );
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
        rn_arena_destroy( f.arena );
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
        rn_arena_destroy( f.arena );
    }
}

REGISTER_UNIT_TEST( test_resend_fifo_corrupt_link_is_bounded );
