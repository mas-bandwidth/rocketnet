/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A start_packet is refused after the session sequence was spent (the send
// arena, the packet lease or the payload ring is full), so a refused start
// burns a sequence that is never sent. These pins show the burn is harmless,
// matching tla/FragmentsAndAcks.tla's BurnOne / BurnFragmented (lines 300-334)
// and the BurnedNeverNamed invariant (line 707): a burned sequence is never
// opened by the receiver, never named in an ack block, and never resends.

#include "test_unit_registry.h"

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"
#include "rocketnet_replay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern unsigned long long g_ack_visit_count;

static void rec_sent( struct rocketnet_ack_endpoint_t * ep, uint64_t sequence )
{
    enum rocketnet_ack_sent_result_t r = rocketnet_ack_endpoint_record_sent_classified(
        ep, sequence, 100, 1000000 + sequence, 1, 0, 1000.0 + (double) sequence * 0.001 );
    rocketnet_unit_check( r != ROCKETNET_ACK_SENT_REFUSED_STALE );
}

static void check_bits_match_ring( struct rocketnet_ack_endpoint_t * ep )
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    uint64_t window_low = 1;
    if ( ep->highest_sequence >= N )
        window_low = ep->highest_sequence - N + 1;
    for ( uint64_t i = 0; i < N; i++ )
    {
        uint64_t held_bit = ( ep->sent_held[i >> 6] >> ( i & 63 ) ) & 1;
        uint64_t unacked_bit = ( ep->sent_unacked[i >> 6] >> ( i & 63 ) ) & 1;
        int expect_held = ep->sent[i].sequence != 0 && ep->sent[i].sequence >= window_low;
        int expect_unacked = expect_held && !ep->sent[i].acked;
        rocketnet_unit_check( ( held_bit != 0 ) == ( expect_held != 0 ) );
        rocketnet_unit_check( ( unacked_bit != 0 ) == ( expect_unacked != 0 ) );
    }
}

// sequence_burn_sender_walk_is_harmless: a refused start burns sequence 6
// (spent, never recorded). A block naming the opened span around it acks
// exactly the real records, the walk visits only them, no sequence is
// unknown (none is counted lost), and the shadow bitmaps equal the ring.
// A second identical block visits nothing: the burn resends nothing.

static void sequence_burn_sender_walk_is_harmless()
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    struct rocketnet_ack_endpoint_t * ep = (struct rocketnet_ack_endpoint_t*)
        rocketnet_malloc( NULL, sizeof(struct rocketnet_ack_endpoint_t) );
    rocketnet_unit_check( ep );
    rocketnet_ack_endpoint_reset( ep );

    uint64_t * acked = (uint64_t*) rocketnet_malloc( NULL, N * sizeof(uint64_t) );
    rocketnet_unit_check( acked );

    for ( uint64_t s = 1; s <= 5; s++ )
        rec_sent( ep, s );

    // burn sequence 6: start_packet spent it, the frame/lease/ring refusal
    // returned before record_sent, so no record exists
    for ( uint64_t s = 7; s <= 12; s++ )
        rec_sent( ep, s );

    // the receiver opened 1..5 and 7..12 (6 never arrived), so its block has
    // a one-sequence gap at 6: a run 7..12, then a run 1..5
    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = 12;
    block.first_range_length = 6;
    block.num_ranges = 1;
    block.range_gap[0] = 1;
    block.range_length[0] = 5;

    int acked_count = 0;
    uint64_t unknown = 0, batch_full = 0;

    g_ack_visit_count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( ep, &block, 3000.0, acked, (int) N, &acked_count, &unknown, &batch_full ) == 11 );
    rocketnet_unit_check( acked_count == 11 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( batch_full == 0 );
    rocketnet_unit_check( g_ack_visit_count == 11 );
    check_bits_match_ring( ep );

    // a second identical block visits nothing: nothing is resent or re-walked
    g_ack_visit_count = 0;
    acked_count = 0; unknown = 0; batch_full = 0;
    rocketnet_ack_endpoint_process_block( ep, &block, 3100.0, acked, (int) N, &acked_count, &unknown, &batch_full );
    rocketnet_unit_check( acked_count == 0 );
    rocketnet_unit_check( unknown == 0 );
    rocketnet_unit_check( g_ack_visit_count == 0 );
    check_bits_match_ring( ep );

    rocketnet_free( NULL, acked );
    rocketnet_free( NULL, ep );
}
REGISTER_UNIT_TEST( sequence_burn_sender_walk_is_harmless );

// the ack block a decoder expands

static int seq_claimed( const struct rocketnet_ack_block_t * b, uint64_t seq )
{
    if ( seq > b->largest )
        return 0;
    {
        uint64_t lo = b->largest - (uint64_t) b->first_range_length + 1;
        if ( seq >= lo )
            return 1;
    }
    uint64_t cur = b->largest - (uint64_t) b->first_range_length + 1;
    for ( int i = 0; i < (int) b->num_ranges; i++ )
    {
        uint64_t high = cur - 1 - (uint64_t) b->range_gap[i];
        uint64_t lo = high - (uint64_t) b->range_length[i] + 1;
        if ( seq >= lo && seq <= high )
            return 1;
        cur = lo;
    }
    return 0;
}

// sequence_burn_receiver_block_skips_unopened: the receiver opened 1..3 and
// 5..6; sequence 4 is burned (never sent, never opened). The ack block it
// builds names only what it opened, so the burned sequence never appears in a
// block and never skews the sender's walk.

static void sequence_burn_receiver_block_skips_unopened()
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof(replay) );
    for ( uint64_t s = 1; s <= 3; s++ )
        rocketnet_replay_advance( &replay, s );
    // burned sequence 4: start_packet spent it, the packet never arrived
    for ( uint64_t s = 5; s <= 6; s++ )
        rocketnet_replay_advance( &replay, s );

    uint8_t wire[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    const int bytes = rocketnet_ack_block_build( wire, ROCKETNET_ACK_BLOCK_MAX_BYTES, &replay );
    rocketnet_unit_check( bytes > 0 );

    struct rocketnet_ack_block_t block;
    const int parsed = rocketnet_ack_block_read( &block, wire, bytes, 6 );
    rocketnet_unit_check( parsed == bytes );
    rocketnet_unit_check( block.largest == 6 );
    rocketnet_unit_check( block.first_range_length == 2 );
    rocketnet_unit_check( block.num_ranges == 1 );
    rocketnet_unit_check( block.range_gap[0] == 1 );
    rocketnet_unit_check( block.range_length[0] == 3 );

    rocketnet_unit_check( seq_claimed( &block, 1 ) );
    rocketnet_unit_check( seq_claimed( &block, 2 ) );
    rocketnet_unit_check( seq_claimed( &block, 3 ) );
    rocketnet_unit_check( !seq_claimed( &block, 4 ) );
    rocketnet_unit_check( seq_claimed( &block, 5 ) );
    rocketnet_unit_check( seq_claimed( &block, 6 ) );
    rocketnet_unit_check( !seq_claimed( &block, 7 ) );
}
REGISTER_UNIT_TEST( sequence_burn_receiver_block_skips_unopened );
