/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The sent-packet map shared by reliable packets and receipt-only packets
// (unreliable records with receipts and no reliable number,
// rn_window_ack_commit_receipt_packet). The documented contract
// (packets_in_flight in sdk/rocketnet.h, standard/MESSAGES.md m9.1,
// docs/CHANNELS.md): no entry is evicted before its ack lands while the map
// holds W plus the receipt-only packets written in one round trip. Past that
// the oldest entry is evicted with its ack on the way, the late ack is
// ignored and the numbers it carried are resent. These cases pin that
// overflow behaviour by value at P = W = 64 (proofs/tla/RNSentMap.tla,
// MCRNSentMapReceiptEviction.cfg), and show that a map sized by the rule
// evicts nothing. The repair is docs/roadmap.sexp sent-map-receipts-give-way:
// when it lands, the overflow cases change with it.
//
// Time is an argument and the clock starts at 1000.0; nothing here reads a
// real clock, opens a socket or sleeps.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"
#include "unit_arena.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_send.h"
#include "../sdk/rocketnet_window_ack.h"

#define check rocketnet_unit_check

struct receipt_eviction_window_t
{
    rocketnet_message_config_t config;
    struct rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_window_send_t send;
    rn_window_ack_t ack;
};

// W = 64, the smallest window the config accepts, with a map of `packets`
// entries: 64 is the tightest the config allows, 160 is sized by the rule for
// the receipt traffic below.
static void receipt_eviction_open( receipt_eviction_window_t * w, int packets, int reliable_timeout_ms )
{
    rocketnet_message_default_config( &w->config );
    w->config.reliable_messages_in_flight = 64;
    w->config.packets_in_flight = packets;
    w->config.max_message_bytes = 4096;
    w->config.receive_message_buffer_bytes = 4096;
    w->config.reliable_message_timeout_ms = reliable_timeout_ms;
    w->config.message_resend_delay_initial_ms = 500;                       // the validator needs a 2 s timeout at least 4x the initial delay
    check( rocketnet_message_validate_config( &w->config ) );
    memset( &w->counters, 0, sizeof( w->counters ) );
    w->arena = rn_unit_arena_create( &w->config, 1, &w->counters );
    check( w->arena != NULL );
    w->region = rn_arena_region( w->arena, 0 );
    rn_region_carve( w->region );
    rn_window_send_init( &w->send, w->region, 0 );
    rn_window_ack_init( &w->ack, &w->send, w->region, &w->config );
    check( w->region->packets == (uint32_t) packets && w->region->window == 64 );
}

static void receipt_eviction_close( receipt_eviction_window_t * w )
{
    rn_unit_arena_destroy( w->arena );
    w->arena = NULL;
}

// Numbers one code-2 reliable record (no ring bytes, no descriptor).
static uint64_t receipt_eviction_number( receipt_eviction_window_t * w )
{
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &w->send, ROCKETNET_MESSAGE_CODE_RELIABLE, 16, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    return number;
}

static uint64_t receipt_eviction_counter( const receipt_eviction_window_t * w, int counter )
{
    return w->counters.values[counter];
}

// One update writes W packets of one reliable number each, then a receipt-only
// packet; every ack lands one round trip later. Returns the reliable acks the
// map found.
static uint32_t receipt_eviction_full_window_then_receipt( receipt_eviction_window_t * w )
{
    rn_window_ack_receipt_begin_update( &w->ack );

    const uint32_t W = w->region->window;
    const double now = 1000.0;
    for ( uint32_t k = 0; k < W; k++ )
    {
        const uint64_t number = receipt_eviction_number( w );
        rn_window_ack_commit_packet( &w->ack, (uint64_t) k + 1, &number, 1, now );
    }
    check( w->send.next - w->send.U == W );                                // the window is full

    const uint32_t serial = 1;
    rn_window_ack_commit_receipt_packet( &w->ack, W + 1, &serial, 1, now );

    const double later = now + 0.1;
    uint32_t found = 0;
    for ( uint32_t k = 0; k < W; k++ )
    {
        found += (uint32_t) rn_window_ack_on_ack( &w->ack, (uint64_t) k + 1, later );
    }
    check( rn_window_ack_on_ack( &w->ack, W + 1, later ) == 1 );           // the receipt-only entry stayed
    rn_window_ack_receipt_end_update( &w->ack );
    return found;
}

// A steady stream at 60 Hz with a 100 ms round trip: each update writes 11
// receipt-only packets (66 a round trip) and one reliable message is
// outstanding, resent whenever its resend delay passes; the reliable timeout
// is 2 s. Every ack lands exactly six updates after its packet (counted in
// updates, so no float comparison decides which update it lands at). Returns
// the reliable message's sends; *timed_out is the update the timeout ended
// the connection at, or -1.
static int receipt_eviction_stream( receipt_eviction_window_t * w, int * timed_out )
{
    check( rn_window_ack_seed_rtt( &w->ack, 0.1 ) == 1 );

    const double dt = 1.0 / 60.0;
    const int rtt_updates = 6;
    const int receipts_per_update = 11;

    struct sent_t
    {
        uint64_t packet_id;
        int land;
    };
    static sent_t wire[4096];
    int wire_count = 0;

    uint64_t packet_id = 1;
    uint32_t serial = 1;
    int reliable_sends = 0;
    *timed_out = -1;

    const uint64_t number = receipt_eviction_number( w );
    const uint32_t slot = (uint32_t) ( number & ( w->region->window - 1 ) );

    for ( int u = 0; u < 180 && *timed_out < 0; u++ )
    {
        const double now = 1000.0 + u * dt;
        rn_window_ack_receipt_begin_update( &w->ack );

        // the acks landing by this update

        int kept = 0;
        for ( int i = 0; i < wire_count; i++ )
        {
            if ( wire[i].land <= u )
            {
                rn_window_ack_on_ack( &w->ack, wire[i].packet_id, now );
            }
            else
            {
                wire[kept++] = wire[i];
            }
        }
        wire_count = kept;

        if ( rn_window_ack_check_timeout( &w->ack, now ) )
        {
            *timed_out = u;
        }

        // the reliable number: first send, or a resend once its delay has passed

        const rn_send_number_t * s = &w->region->send_numbers[slot];
        if ( *timed_out < 0 && !s->acked && s->number == number &&
             ( reliable_sends == 0 || now - s->last_send >= rn_window_ack_resend_delay( &w->ack ) ) )
        {
            rn_window_ack_commit_packet( &w->ack, packet_id, &number, 1, now );
            check( wire_count < 4096 );
            wire[wire_count++] = { packet_id, u + rtt_updates };
            packet_id++;
            reliable_sends++;
        }

        for ( int r = 0; r < receipts_per_update; r++ )
        {
            rn_window_ack_commit_receipt_packet( &w->ack, packet_id, &serial, 1, now );
            check( wire_count < 4096 );
            wire[wire_count++] = { packet_id, u + rtt_updates };
            packet_id++;
            serial++;
        }

        rn_window_ack_receipt_end_update( &w->ack );
    }

    return reliable_sends;
}

// Overflow at P = W: the receipt-only packet written after a full window of
// one-number reliable packets evicts reliable packet 1 with its ack on the
// way. The ack is ignored (63 of 64 found), U stays at 0 so the full window
// refuses a new number, and number 0 stays first in the resend sequence: it
// is resent although it was delivered.
static void test_sent_map_receipt_overflow_evicts_reliable_entry_in_flight()
{
    receipt_eviction_window_t w;
    receipt_eviction_open( &w, 64, 0 );

    check( receipt_eviction_full_window_then_receipt( &w ) == 63 );
    check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 1 );
    check( w.send.U == 0 && w.send.next == 64 );
    check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 16, 1 ) == 0 );
    check( rn_window_ack_resend_first( &w.ack, false ) == 0 );             // number 0, slot 0
    check( rn_window_ack_resend_next( &w.ack, false, 0 ) == RN_RESEND_NONE );
    check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ) == 1 );
    check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ) == 0 );

    receipt_eviction_close( &w );
}

// Overflow at P = W with the receipt-only packet in the middle: reliable,
// receipt-only, then 63 more reliable in one update. The 65th write evicts
// the oldest entry, the reliable one, not the receipt-only one.
static void test_sent_map_receipt_overflow_evicts_oldest_not_the_receipt_entry()
{
    receipt_eviction_window_t w;
    receipt_eviction_open( &w, 64, 0 );
    rn_window_ack_receipt_begin_update( &w.ack );

    const uint32_t W = w.region->window;
    const double now = 1000.0;
    uint64_t packet_id = 1;

    const uint64_t first = receipt_eviction_number( &w );
    const uint64_t first_packet = packet_id;
    rn_window_ack_commit_packet( &w.ack, packet_id++, &first, 1, now );

    const uint32_t serial = 7;
    const uint64_t receipt_packet = packet_id;
    rn_window_ack_commit_receipt_packet( &w.ack, packet_id++, &serial, 1, now );

    for ( uint32_t k = 1; k < W; k++ )
    {
        const uint64_t number = receipt_eviction_number( &w );
        rn_window_ack_commit_packet( &w.ack, packet_id++, &number, 1, now );
    }

    check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 1 );
    check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ) == 0 );

    const double later = now + 0.1;
    check( rn_window_ack_on_ack( &w.ack, first_packet, later ) == 0 );    // the oldest reliable entry went
    check( rn_window_ack_on_ack( &w.ack, receipt_packet, later ) == 1 );
    for ( uint64_t id = receipt_packet + 1; id < packet_id; id++ )
    {
        check( rn_window_ack_on_ack( &w.ack, id, later ) == 1 );
    }
    check( w.send.U == 0 && w.send.next == 64 );
    check( rn_window_ack_resend_first( &w.ack, false ) == 0 );
    check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ) == 1 );
    rn_window_ack_receipt_end_update( &w.ack );

    receipt_eviction_close( &w );
}

// Overflow at P = W, steady: 66 receipt-only packets a round trip push every
// send of the one reliable message out before its ack lands. It is never
// acked, it is resent on each resend delay, and the 2 s reliable timeout ends
// the connection.
static void test_sent_map_receipt_stream_past_p_ends_in_reliable_timeout()
{
    receipt_eviction_window_t w;
    receipt_eviction_open( &w, 64, 2000 );

    int timed_out = -1;
    const int sends = receipt_eviction_stream( &w, &timed_out );

    check( timed_out == 120 );                                            // 2 s after the first send
    check( sends == 9 );                                                   // the first send and 8 resends, each evicted
    check( w.send.U == 0 && w.send.next == 1 );
    check( w.ack.error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 281 );  // the in-flight excess past P, about 2.3 an update

    receipt_eviction_close( &w );
}

// The same two traffic shapes with the map sized by the rule: P = 160 holds W
// plus the receipt-only packets of one round trip (1 in the first, 66 in the
// stream: 64 + 66 = 130 <= 160). Nothing is evicted, every ack is found, the
// reliable message is acked on its first send and the connection lives.
static void test_sent_map_sized_for_receipts_evicts_nothing()
{
    {
        receipt_eviction_window_t w;
        receipt_eviction_open( &w, 160, 0 );

        check( receipt_eviction_full_window_then_receipt( &w ) == 64 );
        check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 0 );
        check( w.send.U == 64 && w.send.next == 64 );
        check( rn_window_ack_resend_first( &w.ack, false ) == RN_RESEND_NONE );
        check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED ) == 1 );

        receipt_eviction_close( &w );
    }

    {
        receipt_eviction_window_t w;
        receipt_eviction_open( &w, 160, 2000 );

        int timed_out = -1;
        const int sends = receipt_eviction_stream( &w, &timed_out );

        check( timed_out == -1 );
        check( sends == 1 );
        check( w.send.U == 1 && w.send.next == 1 );
        check( receipt_eviction_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 0 );

        receipt_eviction_close( &w );
    }
}

REGISTER_UNIT_TEST( test_sent_map_receipt_overflow_evicts_reliable_entry_in_flight );
REGISTER_UNIT_TEST( test_sent_map_receipt_overflow_evicts_oldest_not_the_receipt_entry );
REGISTER_UNIT_TEST( test_sent_map_receipt_stream_past_p_ends_in_reliable_timeout );
REGISTER_UNIT_TEST( test_sent_map_sized_for_receipts_evicts_nothing );
