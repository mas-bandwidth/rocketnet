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

static void test_ack_stats_loss_excludes_newest_half()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    stats_sent( endpoint, half );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    // Bandwidth keeps its existing cohort despite the new loss warmup rule.
    stats_sent( endpoint, 2, 1000.1 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->bandwidth_sent_kbps > 0 );
    stats_sent( endpoint, half + 1, 1000.1 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 100 );
    stats_ack( endpoint, 1, 1000.2 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    stats_sent( endpoint, half + 2, 1000.2 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 50 );
    // Sequence 3 was never sent: it must not become a lost record.
    stats_sent( endpoint, 4, 1000.2 );
    stats_ack( endpoint, 4, 1000.3 );
    stats_sent( endpoint, half + 4, 1000.3 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( fabsf( endpoint->packet_loss - 100.0f / 3.0f ) < 0.001f );
    stats_ack( endpoint, 2, 1001.0 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    stats_ack( endpoint, 2, 1002.0 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    // Wrap the ring: an evicted unacked record is not the new held record.
    rocketnet_ack_endpoint_reset( endpoint );
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 * half + 1, 1000.1 );
    stats_ack( endpoint, 2 * half + 1, 1000.2 );
    stats_sent( endpoint, 3 * half + 1, 1000.2 );
    rocketnet_ack_endpoint_update_stats( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_loss_excludes_newest_half );

#include "rocketnet_layers.h"
extern void rocketnet_client_transport_ack_stats_for_test( const rocketnet_ack_endpoint_t *, rocketnet_client_transport_stats_t * );

static void test_ack_stats_client_uses_outbound_history()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 );
    stats_sent( endpoint, 3 );
    stats_ack( endpoint, 1, 1000.020 );
    stats_ack( endpoint, 2, 1000.040 );
    stats_sent( endpoint, half + 3, 1000.040 );
    rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( fabsf( stats.rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( stats.jitter - 10.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( stats.packet_loss - 100.0f / 3.0f ) < 0.001f );
    // A later ACK lowers outgoing loss without changing the receive trackers.
    stats_ack( endpoint, 3, 1000.050 );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( stats.packet_loss == 0 );
    rocketnet_ack_endpoint_reset( endpoint );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( stats.rtt == 0 && stats.jitter == 0 && stats.packet_loss == 0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_client_uses_outbound_history );
