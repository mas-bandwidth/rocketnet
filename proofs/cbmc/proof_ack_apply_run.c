/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    ESBMC harness for the current production rocketnet_ack_apply_run leaf.
    The SDK translation unit is included below, so the proof executes the
    bitmap popcount and descending set-bit walk in sdk/rocketnet_ack.cpp,
    rather than a copied model of an older linear implementation.

    A symbolic retained run has length 1..ACK_RUN_BOUND. Every sequence in
    the run has a matching live sent record; whether it was already acked is
    symbolic. The sent_held and sent_unacked bitmaps are initialized to the
    exact ring state those records describe. No record carries a packet id or
    aggregate id, and a negative time avoids the independent RTT tree update.

    The assertions prove that the production walk:
      * counts every and only newly acked live record;
      * does not report a live record as unknown or batch-full;
      * visits every and only initially-unacked sequence once;
      * leaves an arbitrary sequence in the run acked, written and held, with
        its unacked bit cleared; and
      * advances largest_acked_sequence to the newest newly acked sequence.

    ACK_RUN_BOUND=8 is the required small run. ACK_RUN_BOUND=4096 is the full
    retained window. The Makefile records timeout, OOM, UNKNOWN or unwind
    failure as HOLD, never as a proof. ACK_NEGATIVE_CONTROL replaces the visit
    equality with its negation after requiring an unacked record; its intended
    assertion failure witnesses satisfiable assumptions and operation reach.
*/

#include <stdint.h>

#ifndef ACK_RUN_BOUND
#define ACK_RUN_BOUND 4096
#endif // #ifndef ACK_RUN_BOUND

#include "rocketnet_ack.cpp"

static void proof_set_bit( uint64_t * words, uint64_t slot )
{
    words[slot >> 6] |= 1ULL << ( slot & 63 );
}

static int proof_bit( const uint64_t * words, uint64_t slot )
{
    return (int) ( ( words[slot >> 6] >> ( slot & 63 ) ) & 1ULL );
}

int harness_ack_apply_run( void )
{
    static struct rocketnet_ack_endpoint_t endpoint;
    uint64_t low = nondet_uint64();
    uint64_t high = nondet_uint64();
    uint64_t highest = nondet_uint64();
    uint64_t len;
    uint64_t expected_new = 0;
    uint64_t expected_largest = 0;
    uint64_t eligible_unacked = 0;
    uint64_t chosen;
    uint64_t acked_packet_ids[1];
    int acked_count = 0;
    uint64_t unknown_sequence_counter = 0;
    uint64_t batch_full_counter = 0;

    __CPROVER_assume( ACK_RUN_BOUND >= 1 );
    __CPROVER_assume( ACK_RUN_BOUND <= ROCKETNET_SENT_PACKET_BUFFER_SIZE );
    __CPROVER_assume( low >= 1 );
    __CPROVER_assume( low <= high );
    __CPROVER_assume( high - low < (uint64_t) ACK_RUN_BOUND );
    __CPROVER_assume( high <= highest );
    __CPROVER_assume( highest - low < ROCKETNET_SENT_PACKET_BUFFER_SIZE );

    endpoint.highest_sequence = highest;
    len = high - low + 1;

    for ( uint64_t i = 0; i < len; ++i )
    {
        const uint64_t sequence = low + i;
        const uint64_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
        struct rocketnet_sent_packet_record_t * record = &endpoint.sent[slot];
        const uint8_t already_acked = nondet_uchar() & 1U;

        record->sequence = sequence;
        record->packet_id = 0;
        record->aggregate_id = 0;
        record->send_time = 0.0;
        record->wire_bytes = 0;
        record->acked = already_acked;
        record->has_packet_id = 0;
        record->was_keep_alive = 0;
        proof_set_bit( endpoint.sent_written, slot );
        proof_set_bit( endpoint.sent_held, slot );

        if ( !already_acked )
        {
            proof_set_bit( endpoint.sent_unacked, slot );
            expected_new++;
            expected_largest = sequence;
            if ( sequence <= rocketnet_ack_stats_eligible_top( highest ) )
            {
                eligible_unacked++;
            }
        }
    }

    endpoint.stats_eligible_unacked = (uint32_t) eligible_unacked;
    g_ack_visit_count = 0;

    const int newly_acked = rocketnet_ack_apply_run(
        &endpoint, low, high, -1.0, acked_packet_ids, 1, &acked_count,
        &unknown_sequence_counter, &batch_full_counter );

    __CPROVER_assert( (uint64_t) newly_acked == expected_new,
                      "the production ACK run neither skips nor double-counts" );
    __CPROVER_assert( unknown_sequence_counter == 0,
                      "every held sequence remains known" );
    __CPROVER_assert( batch_full_counter == 0,
                      "records without packet or aggregate ids never defer" );
    __CPROVER_assert( acked_count == 0,
                      "records without packet or aggregate ids surface no id" );
    __CPROVER_assert( endpoint.stats_eligible_unacked == 0,
                      "every eligible unacked record leaves the running total" );
    __CPROVER_assert( g_ack_visit_count == expected_new,
                      "the production walk visits each initially-unacked sequence once" );
    __CPROVER_assert( endpoint.largest_acked_sequence == expected_largest,
                      "largest_acked_sequence names the newest newly acked sequence" );

    chosen = nondet_uint64();
    __CPROVER_assume( chosen < len );
    {
        const uint64_t sequence = low + chosen;
        const uint64_t slot = sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE;
        __CPROVER_assert( endpoint.sent[slot].sequence == sequence,
                          "the ACK walk does not overwrite a live sent record" );
        __CPROVER_assert( endpoint.sent[slot].acked == 1,
                          "an arbitrary sequence in the run ends acked" );
        __CPROVER_assert( proof_bit( endpoint.sent_written, slot ) == 1,
                          "an ACK does not erase the written identity" );
        __CPROVER_assert( proof_bit( endpoint.sent_held, slot ) == 1,
                          "an ACK does not erase the held identity" );
        __CPROVER_assert( proof_bit( endpoint.sent_unacked, slot ) == 0,
                          "an ACK clears the unacked identity" );
    }

#if defined( ACK_NEGATIVE_CONTROL )
    __CPROVER_assume( expected_new > 0 );
    __CPROVER_assert( g_ack_visit_count == 0,
                      "negative control reaches the production ACK operation" );
#endif // #if defined( ACK_NEGATIVE_CONTROL )

    return 0;
}
