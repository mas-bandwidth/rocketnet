/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#include "test_unit_registry.h"
#include "rocketnet_ack.h"
#include "rocketnet_internal.h"
#include <math.h>
#include <string.h>

static rocketnet_ack_endpoint_t * stats_endpoint()
{
    auto * endpoint = (rocketnet_ack_endpoint_t *) rocketnet_malloc( NULL, sizeof(rocketnet_ack_endpoint_t) );
    rocketnet_unit_check( endpoint );
    rocketnet_ack_endpoint_reset( endpoint );
    return endpoint;
}

static void stats_sent( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time = 1000.0 )
{
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( endpoint, sequence, 100, 0, 0, 0, time ) != ROCKETNET_ACK_SENT_REFUSED_STALE );
}

static void stats_ack( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence, double time )
{
    rocketnet_ack_block_t block;
    memset( &block, 0, sizeof(block) );
    block.largest = sequence;
    block.first_range_length = 1;
    uint64_t ids[1], unknown = 0, full = 0;
    int count = 0;
    rocketnet_ack_endpoint_process_block( endpoint, &block, time, ids, 1, &count, &unknown, &full );
    rocketnet_unit_check( unknown == 0 && full == 0 && count == 0 );
}

static void test_ack_stats_jitter_positive_samples()
{
    auto * endpoint = stats_endpoint();
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->rtt == 0 && endpoint->jitter == 0 );
    stats_sent( endpoint, 1 );
    stats_ack( endpoint, 1, 1000.0 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->rtt == 0 && endpoint->jitter == 0 );
    stats_sent( endpoint, 2 );
    stats_ack( endpoint, 2, 1000.020 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( fabsf( endpoint->rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( endpoint->jitter == 0 );
    stats_sent( endpoint, 3, 1000.020 );
    stats_ack( endpoint, 3, 1000.060 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( fabsf( endpoint->jitter - 10.0f ) < 0.001f );
    stats_ack( endpoint, 2, 1100.0 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( fabsf( endpoint->rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( endpoint->jitter - 10.0f ) < 0.001f );
    rocketnet_ack_endpoint_reset( endpoint );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->rtt == 0 && endpoint->jitter == 0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_jitter_positive_samples );
