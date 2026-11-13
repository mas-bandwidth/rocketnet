/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Mutation gaps, group r2-ch: the send side of the channel layer and the ack
// window (sdk/rocketnet_channel.cpp, sdk/rocketnet_window_ack.cpp). Each case
// names the mutants it kills. Time is an argument starting at 1000; nothing
// reads a clock, opens a socket or sleeps.

#include <stdio.h>
#include <stdlib.h>
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

struct snd_node_t
{
    rocketnet_message_config_t config;
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
};

static void snd_init( snd_node_t * node, int packets_per_update = ROCKETNET_PACKET_BUILDER_MAX_PACKETS )
{
    memset( node, 0, sizeof( *node ) );
    rocketnet_message_default_config( &node->config );
    check( rocketnet_message_validate_config( &node->config ) == 1 );
    node->arena = rn_unit_arena_create( &node->config, 1, &node->counters );
    check( node->arena != NULL );
    node->region = rn_arena_region( node->arena, 0 );
    rn_region_carve( node->region );
    check( rn_ordered_init( &node->ordered, node->region, &node->config, NULL, packets_per_update, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
}

static void snd_free( snd_node_t * node )
{
    rn_unit_arena_destroy( node->arena );
}

static uint64_t snd_counter( const snd_node_t * node, int which )
{
    return node->counters.values[which];
}

static void snd_send( snd_node_t * node, uint32_t channel, int type, uint32_t start_bytes, uint32_t finish_bytes, uint8_t fill )
{
    uint8_t * pointer = rn_channels_start( &node->ordered.channels, channel, type, start_bytes );
    check( pointer != NULL );
    if ( pointer )
    {
        for ( uint32_t i = 0; i < start_bytes; i++ )
        {
            pointer[i] = (uint8_t) ( fill + i );
        }
    }
    check( rn_channels_finish( &node->ordered.channels, finish_bytes ) );
}

// Numbers one reliable record and parks it at the tail of the small sequence
// (as tests/test_unit_resend_fifo_bounds.cpp does), sent at `now` when
// packet_id is not zero.

static uint32_t snd_number( snd_node_t * node, uint32_t after, uint64_t packet_id, double now )
{
    const uint32_t d = rn_region_send_alloc( node->region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &node->ordered.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    if ( packet_id != 0 )
    {
        rn_window_ack_commit_packet( &node->ordered.ack, packet_id, &number, 1, now );
    }
    const uint32_t slot = (uint32_t) ( number % node->region->window );
    rn_window_ack_park( &node->ordered.ack, after, slot );
    return slot;
}

static int snd_asserts = 0;

static void snd_assert_record( const char *, const char *, const char *, int )
{
    snd_asserts++;
}

// ---------------------------------------------------------------------------
// the resend FIFO's step and the ack window
// ---------------------------------------------------------------------------

// fifo[W] lies inside the region's block (the next carved part), so the case
// plants a linked entry there: a bound that let W through would read it and
// return its next.
// mutation gap: hunt 1/24 (resend_next slot >= W to >)

static void test_gaps_r2_ch_resend_next_at_the_window_reads_nothing()
{
    snd_node_t node;
    snd_init( &node );
    snd_number( &node, RN_RESEND_NONE, 1, 1000.0 );

    const uint32_t W = node.region->window;
    rn_resend_link_t * past = node.region->resend_fifo + W;
    check( (const uint8_t *) ( past + 1 ) <= node.region->base + node.region->bytes );
    const rn_resend_link_t saved = *past;

    uint32_t answers[2];
    for ( int chunk = 0; chunk < 2; chunk++ )
    {
        past->prev = 0x80000000u | 2u;                                      // linked
        past->next = ( chunk ? 0x80000000u : 0u ) | 1u;                     // its class, next slot 1
        answers[chunk] = rn_window_ack_resend_next( &node.ordered.ack, chunk == 1, W );
    }
    *past = saved;

    if ( answers[0] != RN_RESEND_NONE || answers[1] != RN_RESEND_NONE )
    {
        printf( "the resend FIFO step at slot W answered %u and %u: fifo[W], one past the array, was read\n", answers[0], answers[1] );
    }
    check( answers[0] == RN_RESEND_NONE );
    check( answers[1] == RN_RESEND_NONE );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_resend_next_at_the_window_reads_nothing );

// mutation gap: hunt 1/30 (resend_next's NONE for an unlinked or other-class slot deleted)

static void test_gaps_r2_ch_resend_next_of_an_unlinked_or_other_class_slot()
{
    snd_node_t node;
    snd_init( &node );
    const uint32_t a = snd_number( &node, RN_RESEND_NONE, 1, 1000.0 );
    const uint32_t b = snd_number( &node, a, 2, 1000.0 );
    check( rn_window_ack_resend_next( &node.ordered.ack, false, a ) == b );

    uint32_t unlinked = 0;
    while ( unlinked == a || unlinked == b )
    {
        unlinked++;
    }
    check( node.region->resend_fifo[unlinked].prev == 0 );

    const uint32_t other_class = rn_window_ack_resend_next( &node.ordered.ack, true, a );
    const uint32_t not_linked = rn_window_ack_resend_next( &node.ordered.ack, false, unlinked );
    if ( other_class != RN_RESEND_NONE || not_linked != RN_RESEND_NONE )
    {
        printf( "the resend FIFO step followed a stale link: chunk class from small slot %u gave %u, unlinked slot %u gave %u\n", a, other_class,
                unlinked, not_linked );
    }
    check( other_class == RN_RESEND_NONE );
    check( not_linked == RN_RESEND_NONE );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_resend_next_of_an_unlinked_or_other_class_slot );

// mutation gap: hunt 1/317 (commit's return for packet id 0 deleted)

static void test_gaps_r2_ch_commit_of_packet_id_zero_records_nothing()
{
    snd_node_t node;
    snd_init( &node );
    uint64_t number = 0;
    uint64_t ring_start = 0;
    const uint32_t d = rn_region_send_alloc( node.region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    check( rn_window_send_first_write( &node.ordered.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );

    const rn_window_ack_t * ack = &node.ordered.ack;
    const uint64_t tail = ack->pkt_tail;
    const uint32_t count = ack->resend_count;
    const uint32_t slot = (uint32_t) ( number % node.region->window );
    const double last_send = node.region->send_numbers[slot].last_send;

    rn_window_ack_commit_packet( &node.ordered.ack, 0, &number, 1, 1000.0 );

    if ( ack->pkt_tail != tail || ack->resend_count != count || node.region->send_numbers[slot].last_send != last_send )
    {
        printf( "a packet committed with id 0 was recorded: sent ring %llu -> %llu, resend count %u -> %u\n", (unsigned long long) tail,
                (unsigned long long) ack->pkt_tail, count, ack->resend_count );
    }
    check( ack->pkt_tail == tail );
    check( ack->resend_count == count );
    check( node.region->send_numbers[slot].last_send == last_send );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_commit_of_packet_id_zero_records_nothing );

// Acks move the sent ring's head past the receipt sweep's cursor between two
// sweeps; the ring then fills again, so the cursor's index aliases a newer
// entry. The sweep starts at the head: the older new entries are past the
// receipt deadline and are reported LOSS, while the aliased, newer ones are
// not yet due.
// mutation gap: hunt 1/445 (the clamp's test negated), hunt 1/446 (the clamp deleted)

static void test_gaps_r2_ch_receipt_sweep_starts_at_the_head()
{
    snd_node_t node;
    snd_init( &node );
    rn_window_ack_t * ack = &node.ordered.ack;
    const uint32_t P = node.region->packets;
    const uint32_t k = 4;
    check( P > 2 * k );

    uint32_t serial = 1;
    uint64_t id = 1;
    for ( uint32_t i = 0; i < k; i++ )
    {
        rn_window_ack_commit_receipt_packet( ack, id++, &serial, 1, 1000.0 );
        serial++;
    }
    for ( uint64_t p = 1; p <= k; p++ )
    {
        rn_window_ack_on_ack( ack, p, 1000.05 );
    }

    const double early = 1000.1;
    const double late = 1000.6;
    for ( uint32_t i = 0; i < P; i++ )
    {
        rn_window_ack_commit_receipt_packet( ack, id++, &serial, 1, i < P - k ? early : late );
        serial++;
    }
    check( ack->pkt_tail - ack->pkt_head == P );
    check( ack->receipt_sweep < ack->pkt_head );

    const uint64_t loss = snd_counter( &node, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
    rn_window_ack_receipt_sweep( ack, early + ack->resend_delay_max + 0.01 );
    const uint64_t reported = snd_counter( &node, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ) - loss;
    if ( reported != P - k )
    {
        printf( "the receipt sweep reported %llu LOSS, not %u: its cursor behind the sent ring's head read an aliased newer entry\n",
                (unsigned long long) reported, P - k );
    }
    check( reported == P - k );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_receipt_sweep_starts_at_the_head );

// mutation gap: hunt 1/546 (check_timeout's return 1 on a set error deleted)

static void test_gaps_r2_ch_timeout_keeps_an_earlier_error()
{
    snd_node_t node;
    snd_init( &node );
    rn_window_ack_t * ack = &node.ordered.ack;

    // no unacked number: the set error alone ends the connection

    ack->error = ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION;
    const int idle = rn_window_ack_check_timeout( ack, 1000.0 );

    // an unacked number past an opted-in reliable timeout: the earlier reason stays

    snd_number( &node, RN_RESEND_NONE, 1, 1000.0 );
    ack->reliable_timeout = 5.0;
    const int timed = rn_window_ack_check_timeout( ack, 1010.0 );

    if ( idle != 1 || timed != 1 || ack->error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "the reliable timeout check did not keep an earlier PROTOCOL_VIOLATION (returned %d and %d, error %d)\n", idle, timed, ack->error );
    }
    check( idle == 1 );
    check( timed == 1 );
    check( ack->error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_timeout_keeps_an_earlier_error );

// ---------------------------------------------------------------------------
// argument refusals of the send half
// ---------------------------------------------------------------------------

// mutation gap: hunt 1/3191 (set_payload_capacity's refusal deleted)

static void test_gaps_r2_ch_payload_capacity_out_of_range_is_refused()
{
    snd_node_t node;
    snd_init( &node );
    rn_channels_t * channels = &node.ordered.channels;
    const uint32_t capacity = channels->payload_capacity;
    const uint32_t least = ROCKETNET_MESSAGE_FRAGMENT_BYTES + ROCKETNET_MESSAGE_RECORD_MAX_HEADER_BYTES + 1;
    const uint32_t bad[3] = { 0, least - 1, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 };
    for ( int i = 0; i < 3; i++ )
    {
        const bool accepted = rn_channels_set_payload_capacity( channels, bad[i] );
        if ( accepted || channels->payload_capacity != capacity )
        {
            printf( "a payload capacity of %u (outside %u to %u) was accepted and stored as %u\n", bad[i], least,
                    (uint32_t) ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, channels->payload_capacity );
        }
        check( !accepted );
        check( channels->payload_capacity == capacity );
    }
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_payload_capacity_out_of_range_is_refused );

// mutation gap: hunt 1/3206 (type >= NUM_TYPES to >), hunt 1/3211 (BAD_ARGUMENT return deleted)

static void test_gaps_r2_ch_send_check_refuses_a_bad_type_or_channel()
{
    snd_node_t node;
    snd_init( &node );
    rn_channels_t * channels = &node.ordered.channels;
    const uint64_t not_enabled = snd_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED );

    const int past_types = rn_channels_send_check( channels, ROCKETNET_CHANNEL_NUM_TYPES, 0 );
    const int past_channels = rn_channels_send_allowed( channels, ROCKETNET_CHANNEL_TYPE_RELIABLE, channels->num_channels );
    if ( past_types != ROCKETNET_CHANNEL_SEND_BAD_ARGUMENT || past_channels != ROCKETNET_CHANNEL_SEND_BAD_ARGUMENT ||
         snd_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED ) != not_enabled )
    {
        printf( "a send of type %d or on channel %u was not refused as BAD_ARGUMENT (%d, %d)\n", ROCKETNET_CHANNEL_NUM_TYPES,
                channels->num_channels, past_types, past_channels );
    }
    check( past_types == ROCKETNET_CHANNEL_SEND_BAD_ARGUMENT );
    check( past_channels == ROCKETNET_CHANNEL_SEND_BAD_ARGUMENT );
    check( snd_counter( &node, ROCKETNET_COUNTER_MESSAGE_SEND_TYPE_NOT_ENABLED ) == not_enabled );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_send_check_refuses_a_bad_type_or_channel );

// A binding of fewer channels than the region carves: channel k is outside it
// though inside the region's arrays.
// mutation gap: hunt 1/3221, 1/3224, 1/3225 (send_ordered), hunt 1/3231, 1/3234, 1/3235 (send_sequenced)

static void test_gaps_r2_ch_send_counters_refuse_a_channel_outside_the_binding()
{
    snd_node_t node;
    snd_init( &node );
    const uint32_t k = 2;
    check( node.region->num_channels > k );

    rn_channels_t narrow;
    check( rn_channels_init( &narrow, node.region, k, 0 ) );
    node.region->channel_send[k].ordered_write = 7;
    node.region->channel_send[k].sequenced_write = 9;

    uint32_t ordering = 0xABCDu;
    const uint32_t ordered = rn_channels_send_ordered( &narrow, k );
    const uint32_t sequenced = rn_channels_send_sequenced( &narrow, k, &ordering );
    const rn_channel_send_t * send = &node.region->channel_send[k];
    if ( ordered != 0 || sequenced != 0 || send->ordered_write != 7 || send->sequenced_write != 9 || ordering != 0xABCDu )
    {
        printf( "a send counter on channel %u outside the binding was used: returned %u and %u, writes %u and %u\n", k, ordered, sequenced,
                send->ordered_write, send->sequenced_write );
    }
    check( ordered == 0 );
    check( sequenced == 0 );
    check( send->ordered_write == 7 );
    check( send->sequenced_write == 9 );
    check( ordering == 0xABCDu );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_send_counters_refuse_a_channel_outside_the_binding );

// The region's descriptors have all been taken once since the carve
// (descriptors_fresh == Q): a binding resets exactly those, and the bytes just
// past the array (the next carved part, where AddressSanitizer sees no
// overflow) are left alone.
// mutation gap: hunt 1/3165 (d < descriptors_fresh to <=)

static void test_gaps_r2_ch_binding_resets_only_the_taken_descriptors()
{
    snd_node_t node;
    snd_init( &node );
    rn_region_t * region = node.region;
    region->descriptors_fresh = region->queue;

    uint8_t * past = (uint8_t *) ( region->send_descriptors + region->queue );
    check( past + sizeof( rn_send_descriptor_t ) <= region->base + region->bytes );
    uint8_t saved[sizeof( rn_send_descriptor_t )];
    memcpy( saved, past, sizeof( saved ) );
    memset( past, 0xA5, sizeof( rn_send_descriptor_t ) );

    rn_channels_t channels;
    check( rn_channels_init( &channels, region, region->num_channels, 0 ) );

    bool intact = true;
    for ( size_t i = 0; i < sizeof( rn_send_descriptor_t ); i++ )
    {
        if ( past[i] != 0xA5 )
        {
            intact = false;
        }
    }
    memcpy( past, saved, sizeof( saved ) );
    if ( !intact )
    {
        printf( "a channel binding reset a descriptor past send_descriptors[Q - 1] (into the next carved part)\n" );
    }
    check( intact );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_binding_resets_only_the_taken_descriptors );

// ---------------------------------------------------------------------------
// the ready list
// ---------------------------------------------------------------------------

// mutation gap: hunt 1/3596, 1/3597, 1/3598 (the ticket counter's wrap to 1)

static void test_gaps_r2_ch_ready_ticket_wraps_to_one()
{
    snd_node_t node;
    snd_init( &node );
    rn_channels_t * channels = &node.ordered.channels;
    channels->ready_ticket_next = 0xFFFFFFFFu;

    snd_send( &node, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 4, 4, 1 );
    snd_send( &node, 1, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 4, 4, 2 );

    const uint32_t first = node.region->channel_send[0].ready_ticket;
    const uint32_t second = node.region->channel_send[1].ready_ticket;
    if ( first != 0xFFFFFFFFu || second != 1 || channels->ready_ticket_next != 2 )
    {
        printf( "the ready ticket did not wrap past zero to 1: tickets %u and %u, next %u\n", first, second, channels->ready_ticket_next );
    }
    check( first == 0xFFFFFFFFu );
    check( second == 1 );
    check( channels->ready_ticket_next == 2 );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_ready_ticket_wraps_to_one );

// CONFIG puts channel 0 at the ready head, before a ready channel 1.
// mutation gap: hunt 1/3606 (the old head's prev not pointed at channel 0)

static void test_gaps_r2_ch_config_links_in_front_of_a_ready_channel()
{
    snd_node_t node;
    snd_init( &node );
    rn_channels_t * channels = &node.ordered.channels;
    snd_send( &node, 1, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 4, 4, 3 );
    check( channels->ready_head == 1 );

    check( rn_ordered_announce_config( &node.ordered, &node.config ) );
    const rn_channel_send_t * send = node.region->channel_send;
    if ( channels->ready_head != 0 || send[0].ready_next != 1 || send[1].ready_prev != 0 || channels->ready_tail != 1 )
    {
        printf( "CONFIG's channel joined the ready head with a broken link: head %u, 0.next %u, 1.prev %u, tail %u\n", channels->ready_head,
                send[0].ready_next, send[1].ready_prev, channels->ready_tail );
    }
    check( channels->ready_head == 0 );
    check( send[0].ready_next == 1 );
    check( send[1].ready_prev == 0 );
    check( channels->ready_tail == 1 );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_config_links_in_front_of_a_ready_channel );

// Channel 0 ready behind channel 1 when CONFIG is announced: channel 0 leaves
// the middle of the list and rejoins at the head.
// mutation gap: hunt 1/3627 (the previous channel's next not relinked on removal)

static void test_gaps_r2_ch_config_moves_channel_zero_from_behind()
{
    snd_node_t node;
    snd_init( &node );
    rn_channels_t * channels = &node.ordered.channels;
    snd_send( &node, 1, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 4, 4, 4 );
    snd_send( &node, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 4, 4, 5 );
    check( channels->ready_head == 1 && channels->ready_tail == 0 );

    check( rn_ordered_announce_config( &node.ordered, &node.config ) );
    const rn_channel_send_t * send = node.region->channel_send;
    if ( channels->ready_head != 0 || channels->ready_tail != 1 || send[1].ready_next != RN_DESCRIPTOR_NONE || send[0].ready_next != 1 )
    {
        printf( "moving channel 0 from behind channel 1 to the ready head left a stale link: head %u, tail %u, 1.next %08x, 0.next %08x\n",
                channels->ready_head, channels->ready_tail, send[1].ready_next, send[0].ready_next );
    }
    check( channels->ready_head == 0 );
    check( channels->ready_tail == 1 );
    check( send[0].ready_next == 1 );
    check( send[1].ready_next == RN_DESCRIPTOR_NONE );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_config_moves_channel_zero_from_behind );

// ---------------------------------------------------------------------------
// CONFIG and the ordered binding's refusals
// ---------------------------------------------------------------------------

static uint32_t snd_queued( const snd_node_t * node, uint32_t channel )
{
    uint32_t count = 0;
    for ( uint32_t d = node->region->channel_send[channel].queue_head; d != RN_DESCRIPTOR_NONE && count <= node->region->queue;
          d = node->region->send_descriptors[d].queue_next )
    {
        count++;
    }
    return count;
}

// mutation gap: hunt 1/3735 (a second announce's refusal deleted)

static void test_gaps_r2_ch_config_is_announced_once()
{
    snd_node_t node;
    snd_init( &node );
    check( rn_ordered_announce_config( &node.ordered, &node.config ) );
    check( snd_queued( &node, 0 ) == 1 );

    const bool again = rn_ordered_announce_config( &node.ordered, &node.config );
    if ( again || snd_queued( &node, 0 ) != 1 )
    {
        printf( "a second CONFIG was announced (returned %d, %u queued on channel 0)\n", again ? 1 : 0, snd_queued( &node, 0 ) );
    }
    check( !again );
    check( snd_queued( &node, 0 ) == 1 );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_config_is_announced_once );

// mutation gap: hunt 1/3751 (the exhausted region's refusal deleted)

static void test_gaps_r2_ch_config_on_an_exhausted_region_is_refused()
{
    snd_node_t node;
    snd_init( &node );
    rn_region_exhaust( node.region );

    const bool announced = rn_ordered_announce_config( &node.ordered, &node.config );
    if ( announced || snd_queued( &node, 0 ) != 0 || node.ordered.config_announced )
    {
        printf( "CONFIG was announced on an exhausted region (returned %d, %u queued)\n", announced ? 1 : 0, snd_queued( &node, 0 ) );
    }
    check( !announced );
    check( snd_queued( &node, 0 ) == 0 );
    check( !node.ordered.config_announced );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_config_on_an_exhausted_region_is_refused );

static bool snd_all_zero( const void * memory, size_t bytes )
{
    const uint8_t * p = (const uint8_t *) memory;
    for ( size_t i = 0; i < bytes; i++ )
    {
        if ( p[i] != 0 )
        {
            return false;
        }
    }
    return true;
}

// A refused init touches nothing: no region (no dereference), a payload
// capacity below 1 (the region's channel state kept), a capacity the
// channels refuse, and one the packet builder refuses (the binding cleared
// both times).
// mutation gap: hunt 1/3812 (the region/capacity refusal deleted), hunt 1/3815, 1/3816 (the
// channels' refusal leaves ordered filled or bound), hunt 1/3821, 1/3822 (the builder's)

static void test_gaps_r2_ch_refused_ordered_init_changes_nothing()
{
    snd_node_t node;
    snd_init( &node );
    rn_ordered_t ordered;

    memset( &ordered, 0x5A, sizeof( ordered ) );
    const bool no_region = rn_ordered_init( &ordered, NULL, &node.config, NULL, 1, ROCKETNET_MESSAGE_SINGLE_BYTES );
    check( !no_region );

    node.region->channel_send[0].ordered_write = 11;
    const bool zero_capacity = rn_ordered_init( &ordered, node.region, &node.config, NULL, 1, 0 );
    if ( zero_capacity || node.region->channel_send[0].ordered_write != 11 )
    {
        printf( "a refused rn_ordered_init (payload capacity 0) reset the region's channel send state (ordered write %u)\n",
                node.region->channel_send[0].ordered_write );
    }
    check( !zero_capacity );
    check( node.region->channel_send[0].ordered_write == 11 );

    memset( &ordered, 0x5A, sizeof( ordered ) );
    const bool small_capacity = rn_ordered_init( &ordered, node.region, &node.config, NULL, 1, 2 );
    if ( small_capacity || !snd_all_zero( &ordered, sizeof( ordered ) ) )
    {
        printf( "an rn_ordered_init the channels refused (payload capacity 2) left the binding set (region %p)\n", (void *) ordered.channels.region );
    }
    check( !small_capacity );
    check( snd_all_zero( &ordered, sizeof( ordered ) ) );

    // the channels take a capacity up to the largest payload; the builder, one packet's record bytes at most

    check( ROCKETNET_MESSAGE_SINGLE_BYTES + 1 <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    memset( &ordered, 0x5A, sizeof( ordered ) );
    const bool builder_refused = rn_ordered_init( &ordered, node.region, &node.config, NULL, 1, ROCKETNET_MESSAGE_SINGLE_BYTES + 1 );
    if ( builder_refused || !snd_all_zero( &ordered, sizeof( ordered ) ) )
    {
        printf( "an rn_ordered_init the packet builder refused (payload capacity %d) left the binding set (region %p)\n",
                ROCKETNET_MESSAGE_SINGLE_BYTES + 1, (void *) ordered.channels.region );
    }
    check( !builder_refused );
    check( snd_all_zero( &ordered, sizeof( ordered ) ) );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_refused_ordered_init_changes_nothing );

// ---------------------------------------------------------------------------
// the write
// ---------------------------------------------------------------------------

// A message finished shorter than it was started is sent and delivered at
// the finished length, for each reliable type, small and block-sized.
// mutation gap: hunt 1/3692 (finish's descriptor->bytes = bytes deleted)

static int snd_loopback( snd_node_t * sender, snd_node_t * receiver, double now, uint64_t first_id )
{
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int i = 0; i < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; i++ )
    {
        pointers[i] = frames[i];
    }
    const int count = rn_ordered_write( &sender->ordered, now, pointers, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    for ( int i = 0; i < count; i++ )
    {
        const int bytes = (int) sender->ordered.builder.packet[i].payload_bytes;
        rn_ordered_commit( &sender->ordered, i, first_id + (uint64_t) i, now );
        check( rn_ordered_receive( &receiver->ordered, frames[i], bytes ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    }
    return count;
}

static void test_gaps_r2_ch_finished_length_is_sent()
{
    const int types[3] = { ROCKETNET_CHANNEL_TYPE_RELIABLE, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED };
    const uint32_t starts[2] = { 100, 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES };
    for ( int t = 0; t < 3; t++ )
    {
        for ( int s = 0; s < 2; s++ )
        {
            snd_node_t sender;
            snd_node_t receiver;
            snd_init( &sender );
            snd_init( &receiver );
            const uint32_t start = starts[s];
            const uint32_t finish = start / 2;
            snd_send( &sender, 1, types[t], start, finish, 0x10 );

            check( snd_loopback( &sender, &receiver, 1000.0, 1 ) > 0 );
            const uint32_t delivered = rn_ordered_deliveries( &receiver.ordered );
            const int bytes = delivered == 1 ? receiver.region->deliveries[0].bytes : -1;
            bool content = bytes == (int) finish;
            for ( int i = 0; content && i < bytes; i++ )
            {
                content = receiver.region->deliveries[0].data[i] == (uint8_t) ( 0x10 + i );
            }
            if ( delivered != 1 || bytes != (int) finish || !content )
            {
                printf( "a type %d message started at %u bytes and finished at %u was delivered as %d bytes (%u deliveries, content %s)\n", types[t], start,
                        finish, bytes, delivered, content ? "matches" : "differs" );
            }
            check( delivered == 1 );
            check( bytes == (int) finish );
            check( content );
            snd_free( &sender );
            snd_free( &receiver );
        }
    }
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_finished_length_is_sent );

// mutation gap: hunt 1/3913 (num_payloads < 1 to < 0), hunt 1/3915 (the refusal returns 1),
// hunt 1/3916 (the refusal deleted)

static void test_gaps_r2_ch_write_of_no_payloads_does_nothing()
{
    snd_node_t node;
    snd_init( &node );
    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[1] = { frame };

    snd_send( &node, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 8, 8, 0x20 );
    check( rn_ordered_write( &node.ordered, 1000.0, pointers, 1 ) == 1 );
    rn_ordered_commit( &node.ordered, 0, 1, 1000.0 );
    snd_send( &node, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 8, 8, 0x30 );
    node.ordered.ack.reliable_timeout = 5.0;

    const uint64_t next = node.ordered.send.next;
    const int none = rn_ordered_write( &node.ordered, 1010.0, pointers, 0 );
    const int null_payloads = rn_ordered_write( &node.ordered, 1010.0, NULL, 1 );
    if ( none != 0 || null_payloads != 0 || node.ordered.ack.error != ROCKETNET_DISCONNECT_REASON_NONE || node.ordered.send.next != next )
    {
        printf( "a write with no payloads ran the update: returned %d and %d, error %d, numbered %llu more\n", none, null_payloads,
                node.ordered.ack.error, (unsigned long long) ( node.ordered.send.next - next ) );
    }
    check( none == 0 );
    check( null_payloads == 0 );
    check( node.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( node.ordered.send.next == next );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_write_of_no_payloads_does_nothing );

// mutation gap: hunt 1/3917 (the write's builder->packets = 0 to 1)

static void test_gaps_r2_ch_write_that_plans_nothing_has_no_packets()
{
    snd_node_t node;
    snd_init( &node );
    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[1] = { frame };

    snd_send( &node, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 8, 8, 0x40 );
    check( rn_ordered_write( &node.ordered, 1000.0, pointers, 1 ) == 1 );
    rn_ordered_commit( &node.ordered, 0, 1, 1000.0 );
    const uint64_t tail = node.ordered.ack.pkt_tail;

    const int written = rn_ordered_write( &node.ordered, 1000.001, pointers, 1 );
    rn_ordered_commit( &node.ordered, 0, 2, 1000.001 );
    if ( written != 0 || node.ordered.builder.packets != 0 || node.ordered.ack.pkt_tail != tail )
    {
        printf( "a write that planned nothing left %u packets in the builder, and a commit of packet 0 recorded %llu\n",
                node.ordered.builder.packets, (unsigned long long) ( node.ordered.ack.pkt_tail - tail ) );
    }
    check( written == 0 );
    check( node.ordered.builder.packets == 0 );
    check( node.ordered.ack.pkt_tail == tail );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_write_that_plans_nothing_has_no_packets );

// 95 one-byte messages and a three-chunk block in one write: the plan holds
// ROCKETNET_ORDERED_MAX_ITEMS items and stops there, never writing past its
// stack array.
// mutation gap: hunt 1/4000 (plan.count < MAX_ITEMS to <=)

static void test_gaps_r2_ch_plan_stops_at_its_item_bound()
{
    snd_node_t node;
    snd_init( &node );
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int i = 0; i < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; i++ )
    {
        pointers[i] = frames[i];
    }

    for ( int i = 0; i < ROCKETNET_ORDERED_MAX_ITEMS - 1; i++ )
    {
        snd_send( &node, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 1, 1, (uint8_t) i );
    }
    snd_send( &node, 0, ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED, 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 100, 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 100, 0x50 );

    const rocketnet_assert_function_t saved = rn_assert_function;
    snd_asserts = 0;
    rocketnet_assert_function( snd_assert_record );
    const int count = rn_ordered_write( &node.ordered, 1000.0, pointers, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    rocketnet_assert_function( saved );

    uint32_t records = 0;
    for ( int i = 0; i < count; i++ )
    {
        records += node.ordered.builder.packet[i].records;
    }
    if ( records != ROCKETNET_ORDERED_MAX_ITEMS || snd_asserts != 0 )
    {
        printf( "one write planned %u records, not the bound %d (%d asserts): the plan went past its item array\n", records, ROCKETNET_ORDERED_MAX_ITEMS,
                snd_asserts );
    }
    check( records == ROCKETNET_ORDERED_MAX_ITEMS );
    check( snd_asserts == 0 );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_plan_stops_at_its_item_bound );

// The walk of the numbers not on the wire meets a link naming slot W
// exactly. send_numbers[W] lies inside the region's block, so the case plants
// an acked copy of a number there: a walk that read it trips the walk's
// not-acked assert (recorded, not fatal).
// mutation gap: hunt 1/3879 (cursor >= W to >)

static void test_gaps_r2_ch_walk_stops_at_a_link_to_the_window()
{
    snd_node_t node;
    snd_init( &node );
    const uint32_t W = node.region->window;

    const uint32_t a = snd_number( &node, RN_RESEND_NONE, 0, 1000.0 );
    uint64_t a_number = node.region->send_numbers[a].number;
    rn_window_ack_commit_packet( &node.ordered.ack, 1, &a_number, 1, 1000.0 );    // a on the wire: the mark
    const uint32_t b = snd_number( &node, RN_RESEND_NONE, 0, 1000.0 );
    const uint32_t c = snd_number( &node, b, 0, 1000.0 );
    (void) c;
    check( rn_window_ack_resend_first( &node.ordered.ack, false ) == b );
    check( rn_window_ack_resend_sent( &node.ordered.ack, false ) == a );

    rn_send_number_t * past = node.region->send_numbers + W;
    check( (const uint8_t *) ( past + 1 ) <= node.region->base + node.region->bytes );
    const rn_send_number_t saved_past = *past;
    *past = node.region->send_numbers[b];
    past->acked = 1;

    node.region->resend_fifo[b].next = W;                               // small class, link to slot W

    alignas( 16 ) static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * pointers[1] = { frame };
    const rocketnet_assert_function_t saved = rn_assert_function;
    snd_asserts = 0;
    rocketnet_assert_function( snd_assert_record );
    rn_ordered_write( &node.ordered, 1000.0 + 1.0 / 60.0, pointers, 1 );
    rocketnet_assert_function( saved );
    *past = saved_past;

    if ( snd_asserts != 0 || node.ordered.ack.error != ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION )
    {
        printf( "the resend walk followed a link to slot W: send_numbers[W] was read (%d asserts, error %d)\n", snd_asserts, node.ordered.ack.error );
    }
    check( snd_asserts == 0 );
    check( node.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_PROTOCOL_VIOLATION );
    snd_free( &node );
}

REGISTER_UNIT_TEST( test_gaps_r2_ch_walk_stops_at_a_link_to_the_window );
