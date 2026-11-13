/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Mutation gaps (round 2, group r2-me) in the acknowledgement layer
// (sdk/rocketnet_ack.cpp, sdk/rocketnet_ack.h): the rtt minimum's zero sample,
// a recycled slot's keep-alive mark, the state comparison's first received
// slot, the received statistics' cached pieces after a reset, the sent window's
// high edge after its first clear, the epoch bitmap's rollover, and the ack
// block builder's word mask at a word's top bit. Single-threaded, no sockets,
// no real time; clocks start at 1000.

#include "test_unit_registry.h"

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_replay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static rocketnet_ack_endpoint_t * gaps_ack_endpoint()
{
    auto * endpoint = (rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof( rocketnet_ack_endpoint_t ) );
    check( endpoint );
    memset( (void *) endpoint, 0, sizeof( rocketnet_ack_endpoint_t ) );
    rocketnet_ack_endpoint_reset( endpoint );
    return endpoint;
}

static void gaps_ack_send( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    check( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, 100, 0, 0, time ) != ROCKETNET_ACK_SENT_REFUSED_STALE );
}

static int gaps_ack_one( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = sequence;
    block.first_range_length = 1;

    uint64_t ids[1];
    uint64_t unknown = 0;
    uint64_t full = 0;
    int count = 0;
    const int acked = rocketnet_ack_endpoint_process_block( endpoint, &block, time, ids, 1, &count, &unknown, &full );
    check( unknown == 0 && full == 0 );
    return acked;
}

// mutation gap: me rocketnet_ack.cpp:1193 sample > 0.0f -> sample >= 0.0f
//
// An ack in the same instant as the send is a zero rtt sample. The reference
// (rocketnet_ack_endpoint_update_stats) takes the minimum over positive
// samples only, so the zero is no sample at all; the running min tree must
// read it the same way. A leaf that admits zero publishes rtt 0 for the whole
// connection while the real round trip is 50 ms.

static void test_gaps_r2_me_ack_zero_rtt_sample_is_not_the_minimum()
{
    rocketnet_ack_endpoint_t * endpoint = gaps_ack_endpoint();

    gaps_ack_send( endpoint, 1, 1000.0 );
    gaps_ack_send( endpoint, 2, 1000.0 );
    check( gaps_ack_one( endpoint, 1, 1000.0 ) == 1 );          // a zero sample
    check( gaps_ack_one( endpoint, 2, 1000.05 ) == 1 );         // 50 ms

    const float running = endpoint->rtt;
    rocketnet_ack_endpoint_update_stats( endpoint );

    if ( running != endpoint->rtt || running < 49.0f || running > 51.0f )
    {
        printf( "fault: published rtt is %.3f ms, the reference over positive samples is %.3f ms: a zero rtt sample entered the min tree\n",
                (double) running, (double) endpoint->rtt );
    }
    check( running == endpoint->rtt );
    check( running > 49.0f && running < 51.0f );

    rocketnet_free( NULL, endpoint );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_ack_zero_rtt_sample_is_not_the_minimum );

// mutation gap: me rocketnet_ack.cpp:1324 record->was_keep_alive = 0 deleted
//
// A keep-alive's record is replaced by a later sequence in the same slot.
// The new record is not a keep-alive, so acknowledging it must not latch
// keep_alive_acked: that latch is the server's evidence the client holds its
// index and turns keep-alive suppression on.

static void test_gaps_r2_me_ack_recycled_slot_forgets_keep_alive()
{
    rocketnet_ack_endpoint_t * endpoint = gaps_ack_endpoint();

    const uint64_t s = 5;
    const uint64_t reused = s + ROCKETNET_SENT_PACKET_BUFFER_SIZE;

    gaps_ack_send( endpoint, s, 1000.0 );
    rocketnet_ack_endpoint_mark_keep_alive( endpoint, s );
    check( endpoint->sent[s % ROCKETNET_SENT_PACKET_BUFFER_SIZE].was_keep_alive == 1 );

    check( rocketnet_ack_endpoint_record_sent_classified( endpoint, reused, 100, 0, 0, 1001.0 ) == ROCKETNET_ACK_SENT_EVICTED_UNKNOWN );
    check( gaps_ack_one( endpoint, reused, 1001.1 ) == 1 );

    if ( endpoint->keep_alive_acked != 0 )
    {
        printf( "fault: acking sequence %llu (not a keep-alive) latched keep_alive_acked: the slot kept the keep-alive mark of sequence %llu\n",
                (unsigned long long) reused, (unsigned long long) s );
    }
    check( endpoint->keep_alive_acked == 0 );

    rocketnet_free( NULL, endpoint );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_ack_recycled_slot_forgets_keep_alive );

// mutation gap: me rocketnet_ack.cpp:378 for ( i = 0 ... ) -> for ( i = 1 ... )
//
// Two endpoints with the same history that differ only in received slot 0
// (sequence 2048 lands there) are not the same state.

static void test_gaps_r2_me_ack_state_equal_reads_received_slot_zero()
{
    rocketnet_ack_endpoint_t * a = gaps_ack_endpoint();
    rocketnet_ack_endpoint_t * b = gaps_ack_endpoint();

    const uint64_t sequence = ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE;
    check( sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE == 0 );

    rocketnet_ack_endpoint_record_received( a, sequence, 100, 1000.0 );
    rocketnet_ack_endpoint_record_received( b, sequence, 100, 1000.0 );
    check( rocketnet_ack_endpoint_state_equal( a, b ) );

    b->received[0].receive_time = 1000.5;

    const bool equal = rocketnet_ack_endpoint_state_equal( a, b );
    if ( equal )
    {
        printf( "fault: state_equal reports equal endpoints whose received slot 0 (sequence %llu) differs in receive time\n",
                (unsigned long long) sequence );
    }
    check( !equal );

    rocketnet_free( NULL, a );
    rocketnet_free( NULL, b );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_ack_state_equal_reads_received_slot_zero );

// mutation gap: me rocketnet_ack.cpp:442 cache base word + 1 = 0 -> = 1
//
// The older half's cached pieces carry their base in two reclaimed record
// words. Reset clears both, so the first receive after a reset rebuilds the
// pieces instead of reusing the ones built before it. Received 1..60, reset,
// then 62 alone: one record counted, its bytes alone.

static void test_gaps_r2_me_ack_reset_drops_cached_received_pieces()
{
    rocketnet_ack_endpoint_t * endpoint = gaps_ack_endpoint();

    for ( uint64_t sequence = 1; sequence <= 60; sequence++ )
    {
        rocketnet_ack_endpoint_record_received( endpoint, sequence, 100, 1000.0 + (double) sequence * 0.01 );
    }
    check( endpoint->stats_received_count == 60 );

    rocketnet_ack_endpoint_reset( endpoint );
    rocketnet_ack_endpoint_record_received( endpoint, 62, 77, 1002.0 );

    if ( endpoint->stats_received_count != 1 || endpoint->stats_received_bytes != 77 )
    {
        printf( "fault: after a reset and one receive the received statistics count %u records of %llu bytes, not 1 of 77: cached pieces from before the reset were reused\n",
                (unsigned) endpoint->stats_received_count, (unsigned long long) endpoint->stats_received_bytes );
    }
    check( endpoint->stats_received_count == 1 );
    check( endpoint->stats_received_bytes == 77 );

    const float running = endpoint->bandwidth_received_kbps;
    rocketnet_ack_endpoint_update_stats( endpoint );
    check( running == endpoint->bandwidth_received_kbps );

    rocketnet_free( NULL, endpoint );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_ack_reset_drops_cached_received_pieces );

// mutation gap: me rocketnet_ack.cpp:615 stats_sent_high = 0 -> = 1
//
// The sent window's clear leaves no counted record (high 0), so the first
// record counted after it sets both edges and both edge times. A clear that
// leaves high at 1 makes sequence 1 look already counted: its send time never
// reaches stats_sent_high_time, and with a send time below zero the sent
// bandwidth is published from a span that does not exist.

static void test_gaps_r2_me_ack_first_sent_record_sets_the_high_edge()
{
    rocketnet_ack_endpoint_t * endpoint = gaps_ack_endpoint();

    gaps_ack_send( endpoint, 1, 1000.0 );

    if ( endpoint->stats_sent_high != 1 || endpoint->stats_sent_high_time != 1000.0 )
    {
        printf( "fault: after the first send the sent window's high edge is sequence %llu at time %.3f, not sequence 1 at 1000.000: the clear left a counted high edge\n",
                (unsigned long long) endpoint->stats_sent_high, endpoint->stats_sent_high_time );
    }
    check( endpoint->stats_sent_high == 1 );
    check( endpoint->stats_sent_high_time == 1000.0 );
    check( endpoint->stats_sent_low_time == 1000.0 );
    check( endpoint->bandwidth_sent_kbps == 0.0f );

    // the published consequence: one record has no span, whatever its time

    rocketnet_ack_endpoint_t * negative = gaps_ack_endpoint();
    gaps_ack_send( negative, 1, -1.0 );
    const float running = negative->bandwidth_sent_kbps;
    rocketnet_ack_endpoint_update_stats( negative );
    if ( running != 0.0f || running != negative->bandwidth_sent_kbps )
    {
        printf( "fault: one sent record published %.3f kbps (reference %.3f): its span ran from its send time to a high edge time never set\n",
                (double) running, (double) negative->bandwidth_sent_kbps );
    }
    check( running == 0.0f );
    check( running == negative->bandwidth_sent_kbps );

    rocketnet_free( NULL, negative );
    rocketnet_free( NULL, endpoint );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_ack_first_sent_record_sets_the_high_edge );

#if ROCKETNET_DEV

// mutation gap: me rocketnet_ack.h:185 for ( i = 0 ... ) -> for ( i = 1 ... )
//
// At the 2^64 epoch boundary invalidate reinitializes every tag before epoch
// 1 recurs. A word whose tag is left at 1 would come back live with its old
// bits. Word 0 is set here, so a reinitialize that skips it shows.

static void test_gaps_r2_me_ack_epoch_rollover_clears_every_tag()
{
    rocketnet_stats_epoch_bits_t<2> bits;
    memset( &bits, 0, sizeof( bits ) );
    bits.initialize();

    bits.epoch = UINT64_MAX;
    bits.tags[0] = 1;
    bits.bits[0] = 1ull << 7;
    bits.tags[1] = 1;
    bits.bits[1] = 1ull << 3;

    bits.invalidate();
    check( bits.epoch == 1 );

    const bool stale0 = bits.test( 7 );
    const bool stale1 = bits.test( 64 + 3 );
    if ( stale0 || stale1 )
    {
        printf( "fault: after the epoch rollover stale bits read live (word 0: %d, word 1: %d): initialize left a tag at the new epoch\n",
                (int) stale0, (int) stale1 );
    }
    check( !stale0 );
    check( !stale1 );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_ack_epoch_rollover_clears_every_tag );

#endif // #if ROCKETNET_DEV

// mutation gap: me rocketnet_ack.cpp:136 offset == 63 -> offset != 63
//
// The block builder loads the replay window a word at a time. A walk that
// crosses a word boundary loads the next word at its top bit (offset 63),
// where the mask must be the whole word below the cursor: a mask built as
// (1 << 64) - 1 is undefined (the asan + ubsan unit leg stops on it) and on
// x86 and arm64 reads as 0, dropping the word. Received 60..130 is one run of
// 71 ending at 130; the walk loads 128..130, then 64..127 at offset 63, then
// 60..63.

static void test_gaps_r2_me_ack_block_build_crosses_word_tops()
{
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof( replay ) );
    for ( uint64_t sequence = 60; sequence <= 130; sequence++ )
    {
        rocketnet_replay_advance( &replay, sequence );
    }
    check( replay.highest_opened == 130 );

    uint8_t output[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    const int bytes = rocketnet_ack_block_build( output, (int) sizeof( output ), &replay );
    check( bytes >= ROCKETNET_ACK_BLOCK_MIN_BYTES );

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    check( rocketnet_ack_block_read( &block, output, bytes, 130 ) > 0 );

    if ( block.largest != 130 || block.first_range_length != 71 || block.num_ranges != 0 )
    {
        printf( "fault: received 60..130 built a block of largest %llu, first run %u, %d ranges, not 130, 71, 0: a word loaded at its top bit was masked wrong\n",
                (unsigned long long) block.largest, (unsigned) block.first_range_length, block.num_ranges );
    }
    check( block.largest == 130 );
    check( block.first_range_length == 71 );
    check( block.num_ranges == 0 );
}
REGISTER_UNIT_TEST( test_gaps_r2_me_ack_block_build_crosses_word_tops );
