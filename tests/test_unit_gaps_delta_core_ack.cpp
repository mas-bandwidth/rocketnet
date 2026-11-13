/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Mutation gaps (delta, core): the bounds of the first-ack round-trip sample
// in rocketnet_ack_endpoint_process_block (sdk/rocketnet_ack.cpp). An elapsed
// time of exactly 0 ms is a sample (an ack read in the same update its packet
// was sent in); one of exactly ROCKETNET_ACK_RTT_NONE (1e9 ms) is no sample.
// No socket, no thread, no real time: the clock is an argument from 1000.0.

#include "test_unit_registry.h"
#include "rocketnet_ack.h"
#include "rocketnet_internal.h"

#include <string.h>

static rocketnet_ack_endpoint_t * gaps_ack_endpoint()
{
    auto * endpoint = (rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof( rocketnet_ack_endpoint_t ) );
    rocketnet_unit_check( endpoint );
    memset( (void *) endpoint, 0, sizeof( rocketnet_ack_endpoint_t ) );
    rocketnet_ack_endpoint_reset( endpoint );
    return endpoint;
}

static void gaps_ack_sent( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, 100, 0, 0, time ) != ROCKETNET_ACK_SENT_REFUSED_STALE );
}

static void gaps_ack_acked( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = sequence;
    block.first_range_length = 1;

    uint64_t ids[1];
    uint64_t unknown = 0;
    uint64_t full = 0;
    int count = 0;
    rocketnet_unit_check( rocketnet_ack_endpoint_process_block( endpoint, &block, time, ids, 1, &count, &unknown, &full ) == 1 );
    rocketnet_unit_check( unknown == 0 && full == 0 && count == 0 );
}

// mutation gap: delta sdk/rocketnet_ack.cpp:1703:28:relational:> (sample_ms > 0.0)
// A packet acknowledged at the time it was sent is a 0 ms sample: it lands,
// and the minimum round trip is 0, not the later 20 ms sample.

static void test_gaps_delta_core_ack_zero_ms_ack_is_an_rtt_sample()
{
    auto * endpoint = gaps_ack_endpoint();

    gaps_ack_sent( endpoint, 1, 1000.0 );
    gaps_ack_acked( endpoint, 1, 1000.0 );
    const bool zero_ms_ack_landed_as_a_sample = endpoint->has_rtt_sample == 1;
    rocketnet_unit_check( zero_ms_ack_landed_as_a_sample );

    gaps_ack_sent( endpoint, 2, 1000.0 );
    gaps_ack_acked( endpoint, 2, 1000.020 );
    const bool rtt_minimum_is_the_zero_ms_sample = endpoint->rtt == 0.0f;
    rocketnet_unit_check( rtt_minimum_is_the_zero_ms_sample );

    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_ack_zero_ms_ack_is_an_rtt_sample );

// mutation gap: delta sdk/rocketnet_ack.cpp:1703:48:relational:<= (sample_ms <= ROCKETNET_ACK_RTT_NONE)
// An elapsed time of exactly 1e6 s is 1e9 ms, exactly ROCKETNET_ACK_RTT_NONE
// (both exact in double and float): no sample. A later 20 ms sample is the
// first, and the round trip.

static void test_gaps_delta_core_ack_rtt_none_elapsed_is_no_sample()
{
    auto * endpoint = gaps_ack_endpoint();

    gaps_ack_sent( endpoint, 1, 1000.0 );
    gaps_ack_acked( endpoint, 1, 1000.0 + 1000000.0 );
    const bool elapsed_of_exactly_rtt_none_is_no_sample = endpoint->has_rtt_sample == 0;
    rocketnet_unit_check( elapsed_of_exactly_rtt_none_is_no_sample );

    gaps_ack_sent( endpoint, 2, 1001000.0 );
    gaps_ack_acked( endpoint, 2, 1001000.020 );
    const bool rtt_is_the_later_20_ms_sample = endpoint->has_rtt_sample == 1 && endpoint->rtt > 19.99f && endpoint->rtt < 20.01f;
    rocketnet_unit_check( rtt_is_the_later_20_ms_sample );

    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_ack_rtt_none_elapsed_is_no_sample );
