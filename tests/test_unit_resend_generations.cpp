/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A transport ack visits the slot of each number its packet referenced. A
// packet can still reference a number after the window has moved a whole
// turn and the slot holds a later number, written by the same descriptor
// taken again at a later generation. That stale ack must leave the later
// number linked, unacked and holding its descriptor: the number comparison
// is the only test between them, as a resend link carries no generation
// (proofs/tla/RNResendGenerations.tla Ack, NamedGenerationOnly).
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

static const uint32_t resend_generations_linked = 0x80000000u;

struct resend_generations_fixture_t
{
    rocketnet_message_config_t config;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
};

// Numbers one reliable record, parks it at the head of the small sequence and
// sends it in packet_id. Returns its number.
static uint64_t resend_generations_write( resend_generations_fixture_t * f, uint64_t packet_id, double now, uint32_t * out_descriptor )
{
    const uint32_t d = rn_region_send_alloc( f->region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &f->ordered.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    rn_window_ack_park( &f->ordered.ack, RN_RESEND_NONE, (uint32_t) ( number % f->region->window ) );
    rn_window_ack_commit_packet( &f->ordered.ack, packet_id, &number, 1, now );
    *out_descriptor = d;
    return number;
}

static void test_resend_stale_ack_leaves_the_later_generation()
{
    resend_generations_fixture_t f;
    memset( &f.config, 0, sizeof( f.config ) );
    rocketnet_message_default_config( &f.config );
    f.config.reliable_messages_in_flight = 64;
    f.config.packets_in_flight = 128;                       // the stale packet stays in the sent map
    f.config.receive_message_buffer_bytes = 4096;
    f.config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    f.config.send_message_buffer_bytes = 12288;
    check( rocketnet_message_validate_config( &f.config ) );
    f.arena = rn_arena_create( NULL, &f.config, 1, NULL );
    check( f.arena != NULL );
    f.region = rn_arena_region( f.arena, 0 );
    rn_region_carve( f.region );
    check( rn_ordered_init( &f.ordered, f.region, &f.config, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, ROCKETNET_MESSAGE_SINGLE_BYTES ) );

    const uint32_t W = f.region->window;
    check( W == 64 );

    // number 0 is sent in packet 1, resent in packet 2, and packet 2 is acked:
    // packet 1 still references number 0

    uint32_t first_descriptor = RN_DESCRIPTOR_NONE;
    double now = 1000.0;
    check( resend_generations_write( &f, 1, now, &first_descriptor ) == 0 );
    const uint32_t first_generation = f.region->send_descriptors[first_descriptor].generation;
    uint64_t zero = 0;
    rn_window_ack_commit_packet( &f.ordered.ack, 2, &zero, 1, now + 0.1 );
    check( rn_window_ack_on_ack( &f.ordered.ack, 2, now + 0.2 ) == 1 );
    check( f.ordered.send.U == 1 );
    check( !f.region->send_descriptors[first_descriptor].in_use );

    // numbers 1 .. W - 1 are written, sent and acked: each takes the
    // descriptor again at its next generation

    uint64_t packet_id = 3;
    for ( uint32_t i = 1; i < W; i++ )
    {
        uint32_t d = RN_DESCRIPTOR_NONE;
        now += 0.01;
        check( resend_generations_write( &f, packet_id, now, &d ) == i );
        check( d == first_descriptor );
        check( rn_window_ack_on_ack( &f.ordered.ack, packet_id, now + 0.001 ) == 1 );
        packet_id++;
    }
    check( f.ordered.send.U == W );

    // number W lands in number 0's slot, written by the same descriptor at a
    // later generation, and is in flight

    uint32_t later_descriptor = RN_DESCRIPTOR_NONE;
    now += 0.01;
    check( resend_generations_write( &f, packet_id, now, &later_descriptor ) == W );
    check( later_descriptor == first_descriptor );
    const uint32_t later_generation = f.region->send_descriptors[later_descriptor].generation;
    check( later_generation != first_generation );
    check( f.region->send_numbers[0].number == W );
    check( f.region->resend_fifo[0].prev & resend_generations_linked );
    const uint32_t resend_count = f.ordered.ack.resend_count;

    // the stale ack of packet 1 names number 0: the later number stays

    check( rn_window_ack_on_ack( &f.ordered.ack, 1, now + 0.001 ) == 1 );

    check( !f.region->send_numbers[0].acked );
    check( f.ordered.send.U == W );
    check( f.region->resend_fifo[0].prev & resend_generations_linked );
    check( f.ordered.ack.resend_count == resend_count );
    check( f.region->send_descriptors[later_descriptor].in_use );
    check( f.region->send_descriptors[later_descriptor].generation == later_generation );
    check( f.region->send_descriptors[later_descriptor].fragments_acked == 0 );

    // its own ack still takes it

    check( rn_window_ack_on_ack( &f.ordered.ack, packet_id, now + 0.002 ) == 1 );
    check( f.region->send_numbers[0].acked );
    check( f.ordered.send.U == W + 1 );
    check( !( f.region->resend_fifo[0].prev & resend_generations_linked ) );
    check( !f.region->send_descriptors[later_descriptor].in_use );

    rn_arena_destroy( f.arena );
}

REGISTER_UNIT_TEST( test_resend_stale_ack_leaves_the_later_generation );

// A resend relinks the number at its class's tail and a failed send at its
// head; the number stays linked through both until its own ack unlinks it
// (proofs/tla/RNResendGenerations.tla Send, LinkNamesOwner).
static void test_resend_requeue_keeps_the_number_linked()
{
    resend_generations_fixture_t f;
    memset( &f.config, 0, sizeof( f.config ) );
    rocketnet_message_default_config( &f.config );
    check( rocketnet_message_validate_config( &f.config ) );
    f.arena = rn_arena_create( NULL, &f.config, 1, NULL );
    check( f.arena != NULL );
    f.region = rn_arena_region( f.arena, 0 );
    rn_region_carve( f.region );
    check( rn_ordered_init( &f.ordered, f.region, &f.config, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, ROCKETNET_MESSAGE_SINGLE_BYTES ) );

    uint32_t d = RN_DESCRIPTOR_NONE;
    const uint64_t number = resend_generations_write( &f, 1, 1000.0, &d );
    const uint32_t slot = (uint32_t) ( number % f.region->window );
    check( f.region->resend_fifo[slot].prev & resend_generations_linked );

    uint64_t n = number;
    rn_window_ack_commit_packet( &f.ordered.ack, 2, &n, 1, 1000.5 );
    check( f.region->resend_fifo[slot].prev & resend_generations_linked );

    rn_window_ack_packet_failed( &f.ordered.ack, &n, 1, 1000.6 );
    check( f.region->resend_fifo[slot].prev & resend_generations_linked );

    check( rn_window_ack_on_ack( &f.ordered.ack, 2, 1000.7 ) == 1 );
    check( f.region->send_numbers[slot].acked );
    check( !( f.region->resend_fifo[slot].prev & resend_generations_linked ) );

    rn_arena_destroy( f.arena );
}

REGISTER_UNIT_TEST( test_resend_requeue_keeps_the_number_linked );
