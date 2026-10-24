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

// Compare both typed entry points at every existing numeric edge assertion.
// Projection happens only in this unit oracle; the worker uses its history
// directly and performs no endpoint reconstruction.
static void stats_update_and_compare_history( rocketnet_ack_endpoint_t * endpoint )
{
    auto * history = new rocketnet_ack_stats_history_t;
    memset(history, 0, sizeof(*history));
    history->highest_sent = endpoint->highest_sequence;
    history->highest_received = endpoint->highest_received_sequence;
    memcpy(history->rtt_history, endpoint->rtt_history, sizeof(history->rtt_history));
    for ( int i = 0; i < ROCKETNET_SENT_PACKET_BUFFER_SIZE; ++i )
    {
        history->sent[i].sequence = endpoint->sent[i].sequence;
        history->sent[i].send_time = endpoint->sent[i].send_time;
        history->sent[i].wire_bytes = endpoint->sent[i].wire_bytes;
        history->sent[i].acked = endpoint->sent[i].acked;
    }
    for ( int i = 0; i < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE; ++i )
    {
        history->received[i].sequence = endpoint->received[i].sequence;
        history->received[i].receive_time = endpoint->received[i].receive_time;
        history->received[i].wire_bytes = endpoint->received[i].wire_bytes;
    }
    rocketnet_ack_stats_result_t result = {};
    rocketnet_ack_stats_history_compute(history, &result);
    rocketnet_ack_endpoint_update_stats(endpoint);
    // Equal inputs must retain the exact float operation order, not merely
    // approximate a six-field result after the scratch removal.
    rocketnet_unit_check(result.rtt == endpoint->rtt);
    rocketnet_unit_check(result.jitter == endpoint->jitter);
    rocketnet_unit_check(result.packet_loss == endpoint->packet_loss);
    rocketnet_unit_check(result.sent_kbps == endpoint->bandwidth_sent_kbps);
    rocketnet_unit_check(result.received_kbps == endpoint->bandwidth_received_kbps);
    rocketnet_unit_check(result.acked_kbps == endpoint->bandwidth_acked_kbps);
    delete history;
}

static void test_ack_stats_jitter_positive_samples()
{
    auto * endpoint = stats_endpoint();
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->rtt == 0 && endpoint->jitter == 0 );
    stats_sent( endpoint, 1 );
    stats_ack( endpoint, 1, 1000.0 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->rtt == 0 && endpoint->jitter == 0 );
    stats_sent( endpoint, 2 );
    stats_ack( endpoint, 2, 1000.020 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( fabsf( endpoint->rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( endpoint->jitter == 0 );
    stats_sent( endpoint, 3, 1000.020 );
    stats_ack( endpoint, 3, 1000.060 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( fabsf( endpoint->jitter - 10.0f ) < 0.001f );
    stats_ack( endpoint, 2, 1100.0 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( fabsf( endpoint->rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( endpoint->jitter - 10.0f ) < 0.001f );
    rocketnet_ack_endpoint_reset( endpoint );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->rtt == 0 && endpoint->jitter == 0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_jitter_positive_samples );

static void test_ack_stats_loss_excludes_newest_half()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    stats_sent( endpoint, half );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    // Bandwidth keeps its existing cohort despite the new loss warmup rule.
    stats_sent( endpoint, 2, 1000.1 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->bandwidth_sent_kbps > 0 );
    stats_sent( endpoint, half + 1, 1000.1 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 100 );
    stats_ack( endpoint, 1, 1000.2 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    stats_sent( endpoint, half + 2, 1000.2 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 50 );
    // Sequence 3 was never sent: it must not become a lost record.
    stats_sent( endpoint, 4, 1000.2 );
    stats_ack( endpoint, 4, 1000.3 );
    stats_sent( endpoint, half + 4, 1000.3 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( fabsf( endpoint->packet_loss - 100.0f / 3.0f ) < 0.001f );
    stats_ack( endpoint, 2, 1001.0 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    stats_ack( endpoint, 2, 1002.0 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->packet_loss == 0 );
    // Wrap the ring: an evicted unacked record is not the new held record.
    rocketnet_ack_endpoint_reset( endpoint );
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 * half + 1, 1000.1 );
    stats_ack( endpoint, 2 * half + 1, 1000.2 );
    stats_sent( endpoint, 3 * half + 1, 1000.2 );
    stats_update_and_compare_history( endpoint );
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

static void test_ack_stats_direct_history_uint64_high_water()
{
    auto * endpoint = stats_endpoint();
    // Session sequences never wrap through zero. Exercise the retained
    // window ending exactly at the largest representable sequence instead.
    endpoint->highest_sequence = UINT64_MAX;
    endpoint->highest_received_sequence = UINT64_MAX;
    endpoint->rtt_history[0] = 0.0f;
    endpoint->rtt_history[1] = 20.0f;
    endpoint->rtt_history[2] = 40.0f;
    const uint64_t sent_base = UINT64_MAX - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;
    const uint64_t sent_offsets[] = { 0, 2, ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 - 1 };
    for ( uint32_t i = 0; i < 3; ++i )
    {
        const uint64_t sequence = sent_base + sent_offsets[i];
        auto & record = endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
        record.sequence = sequence;
        record.send_time = 1000.0 + 0.25 * i;
        record.wire_bytes = 100 * (i + 1);
        record.acked = i != 1;
    }
    // An overwritten record at a hole and a newest-half record cannot enter
    // the old cohort's byte sum or loss denominator.
    auto & stale = endpoint->sent[(sent_base + 1) % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    stale.sequence = sent_base + 1 - ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    stale.wire_bytes = 10000;
    const uint64_t newest_sequence = sent_base + ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    auto & newest = endpoint->sent[newest_sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    newest.sequence = newest_sequence;
    newest.send_time = 1001.0;
    newest.wire_bytes = 10000;

    const uint64_t received_base = UINT64_MAX - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 1;
    for ( uint32_t i = 0; i < 2; ++i )
    {
        const uint64_t sequence = received_base + 2 * i;
        auto & record = endpoint->received[sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE];
        record.sequence = sequence;
        record.receive_time = 1000.0 + 0.5 * i;
        record.wire_bytes = 100 + 200 * i;
    }
    stats_update_and_compare_history(endpoint);
    // Independent known sums, in addition to entry-point parity.
    rocketnet_unit_check(endpoint->rtt == 20.0f && endpoint->jitter == 10.0f);
    rocketnet_unit_check(fabsf(endpoint->packet_loss - 100.0f / 3.0f) < 0.001f);
    rocketnet_unit_check(fabsf(endpoint->bandwidth_sent_kbps - 9.6f) < 0.001f);
    rocketnet_unit_check(fabsf(endpoint->bandwidth_acked_kbps - 6.4f) < 0.001f);
    rocketnet_unit_check(fabsf(endpoint->bandwidth_received_kbps - 6.4f) < 0.001f);
    rocketnet_free(NULL, endpoint);
}

REGISTER_UNIT_TEST(test_ack_stats_direct_history_uint64_high_water);
