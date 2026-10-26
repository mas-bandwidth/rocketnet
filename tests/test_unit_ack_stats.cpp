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
    // the rings zeroed and every record marked written: the tests write
    // records straight into the rings, and the projection below reads all of
    // them, so each starts as the empty record (sequence 0) that a reset's
    // written bits stand for
    memset( (void *) endpoint, 0, sizeof(rocketnet_ack_endpoint_t) );
    rocketnet_ack_endpoint_reset( endpoint );
    memset( endpoint->sent_written, 0xFF, sizeof(endpoint->sent_written) );
    memset( endpoint->received_written, 0xFF, sizeof(endpoint->received_written) );
    memset( endpoint->aggregates_written, 0xFF, sizeof(endpoint->aggregates_written) );
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

#if ROCKETNET_DEV

// Project an endpoint into the independent history scan. Projection happens
// only in this unit oracle; the worker uses its history directly and performs
// no endpoint reconstruction.
static void stats_history_result( const rocketnet_ack_endpoint_t * endpoint,
                                  rocketnet_ack_stats_result_t * result )
{
    auto * history = new rocketnet_ack_stats_history_t;
    memset(history, 0, sizeof(*history));
    history->highest_sent = endpoint->highest_sequence;
    history->highest_received = endpoint->highest_received_sequence;
    memcpy(history->rtt_history, endpoint->rtt_history, sizeof(history->rtt_history));
    // a record the endpoint has not written since its reset is the empty
    // record, as its written bits say: the projection leaves it zero
    for ( int i = 0; i < ROCKETNET_SENT_PACKET_BUFFER_SIZE; ++i )
    {
        if ( !( ( endpoint->sent_written[i / 64] >> ( i % 64 ) ) & 1 ) )
            continue;
        history->sent[i].sequence = endpoint->sent[i].sequence;
        history->sent[i].send_time = endpoint->sent[i].send_time;
        history->sent[i].wire_bytes = endpoint->sent[i].wire_bytes;
        history->sent[i].acked = endpoint->sent[i].acked;
    }
    for ( int i = 0; i < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE; ++i )
    {
        if ( !( ( endpoint->received_written[i / 64] >> ( i % 64 ) ) & 1 ) )
            continue;
        history->received[i].sequence = endpoint->received[i].sequence;
        history->received[i].receive_time = endpoint->received[i].receive_time;
        history->received[i].wire_bytes = endpoint->received[i].wire_bytes;
    }
    memset( result, 0, sizeof(*result) );
    rocketnet_ack_stats_history_compute(history, result);
    delete history;
}

#endif // ROCKETNET_DEV

// Compare both typed recompute entry points at every existing manual numeric
// edge assertion in DEV=1, while keeping the production recompute and numeric
// assertions active in DEV=0. This intentionally updates endpoint; the received
// live aggregate cases below use a separate non-mutating oracle.
static void stats_update_and_compare_history( rocketnet_ack_endpoint_t * endpoint )
{
#if ROCKETNET_DEV
    rocketnet_ack_stats_result_t result;
    stats_history_result( endpoint, &result );
#endif
    rocketnet_ack_endpoint_update_stats(endpoint);
#if ROCKETNET_DEV
    // Equal inputs must retain the exact float operation order, not merely
    // approximate a six-field result after the scratch removal.
    rocketnet_unit_check(result.rtt == endpoint->rtt);
    rocketnet_unit_check(result.jitter == endpoint->jitter);
    rocketnet_unit_check(result.packet_loss == endpoint->packet_loss);
    rocketnet_unit_check(result.sent_kbps == endpoint->bandwidth_sent_kbps);
    rocketnet_unit_check(result.received_kbps == endpoint->bandwidth_received_kbps);
    rocketnet_unit_check(result.acked_kbps == endpoint->bandwidth_acked_kbps);
#endif
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
extern struct rocketnet_client_t * rocketnet_client_ack_stats_public_client_for_test( const rocketnet_ack_endpoint_t * );
extern void rocketnet_client_ack_stats_public_client_for_test_free( struct rocketnet_client_t * );

// rocketnet_client_stats is the public assignment. A hardcoded 0 there fails
// these checks. The seam result is the same ACK history, not a second formula.
static void stats_public_matches_ack_history( const rocketnet_ack_endpoint_t * endpoint, rocketnet_client_stats_t * pub )
{
    rocketnet_client_transport_stats_t seam;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &seam );
    struct rocketnet_client_t * client = rocketnet_client_ack_stats_public_client_for_test( endpoint );
    rocketnet_unit_check( client );
    rocketnet_client_stats( client, pub );
    rocketnet_client_ack_stats_public_client_for_test_free( client );
    rocketnet_unit_check( fabs( pub->packet_loss_percent - (double) seam.packet_loss ) < 0.001 );
    rocketnet_unit_check( fabs( pub->rtt_ms - (double) seam.rtt ) < 0.001 );
    rocketnet_unit_check( fabs( pub->jitter_ms - (double) seam.jitter ) < 0.001 );
}

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
    // A later ACK lowers the client's outbound loss estimate.
    stats_ack( endpoint, 3, 1000.050 );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( stats.packet_loss == 0 );
    rocketnet_ack_endpoint_reset( endpoint );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( stats.rtt == 0 && stats.jitter == 0 && stats.packet_loss == 0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_client_uses_outbound_history );

// The getter copies the ACK recompute. The history scan is the oracle the
// other tests in this file already compare against. No second loss formula.
static void stats_client_getter_matches_ack_history( const rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_ack_stats_result_t result;
    stats_history_result( endpoint, &result );
    rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( stats.rtt == result.rtt );
    rocketnet_unit_check( stats.jitter == result.jitter );
    rocketnet_unit_check( stats.packet_loss == result.packet_loss );
}

// First-ACK samples are keyed by sequence. Acknowledging 3, then 1, then 2
// is the same sample set as acknowledging them in send order at those times.
static void test_ack_stats_client_getter_reorder()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1, 1000.0 );
    stats_sent( endpoint, 2, 1000.0 );
    stats_sent( endpoint, 3, 1000.020 );
    stats_ack( endpoint, 3, 1000.060 );
    stats_ack( endpoint, 1, 1000.020 );
    stats_sent( endpoint, half + 3, 1000.060 );
    stats_client_getter_matches_ack_history( endpoint );
    rocketnet_client_stats_t pub;
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( fabsf( stats.rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( stats.jitter - 10.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( stats.packet_loss - 100.0f / 3.0f ) < 0.001f );
    stats_ack( endpoint, 2, 1000.050 );
    stats_client_getter_matches_ack_history( endpoint );
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( fabsf( stats.rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( stats.jitter - 50.0f / 3.0f ) < 0.001f );
    rocketnet_unit_check( stats.packet_loss == 0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_client_getter_reorder );

// A second block naming an already acknowledged sequence adds no sample.
static void test_ack_stats_client_getter_duplicate_ack()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 );
    stats_sent( endpoint, 3 );
    stats_ack( endpoint, 1, 1000.020 );
    stats_ack( endpoint, 2, 1000.040 );
    stats_sent( endpoint, half + 3, 1000.040 );
    stats_client_getter_matches_ack_history( endpoint );
    rocketnet_client_stats_t pub;
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_stats_t before;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &before );
    rocketnet_unit_check( fabsf( before.rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( before.jitter - 10.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( before.packet_loss - 100.0f / 3.0f ) < 0.001f );
    stats_ack( endpoint, 1, 1100.0 );
    stats_ack( endpoint, 2, 1100.0 );
    stats_client_getter_matches_ack_history( endpoint );
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_stats_t after;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &after );
    rocketnet_unit_check( after.rtt == before.rtt );
    rocketnet_unit_check( after.jitter == before.jitter );
    rocketnet_unit_check( after.packet_loss == before.packet_loss );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_client_getter_duplicate_ack );

// Sequence holes are not sends. A jump that evicts the ring is not a loss.
static void test_ack_stats_client_getter_sparse_jump()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1, 1000.0 );
    stats_sent( endpoint, 50, 1000.010 );
    stats_ack( endpoint, 1, 1000.020 );
    stats_sent( endpoint, half + 50, 1000.040 );
    stats_client_getter_matches_ack_history( endpoint );
    rocketnet_client_stats_t pub;
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( fabsf( stats.rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( stats.jitter == 0 );
    rocketnet_unit_check( fabsf( stats.packet_loss - 50.0f ) < 0.001f );

    rocketnet_ack_endpoint_reset( endpoint );
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 * half + 1, 1000.1 );
    stats_ack( endpoint, 2 * half + 1, 1000.2 );
    stats_sent( endpoint, 3 * half + 1, 1000.2 );
    stats_client_getter_matches_ack_history( endpoint );
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( stats.packet_loss == 0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_client_getter_sparse_jump );

// Reset drops the old history. The next session's samples are the only ones.
static void test_ack_stats_client_getter_reconnect()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 );
    stats_sent( endpoint, 3 );
    stats_ack( endpoint, 1, 1000.020 );
    stats_ack( endpoint, 2, 1000.040 );
    stats_sent( endpoint, half + 3, 1000.040 );
    stats_client_getter_matches_ack_history( endpoint );
    rocketnet_client_stats_t pub;
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_stats_t stats;
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( fabsf( stats.packet_loss - 100.0f / 3.0f ) < 0.001f );

    rocketnet_ack_endpoint_reset( endpoint );
    stats_client_getter_matches_ack_history( endpoint );
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( stats.rtt == 0 && stats.jitter == 0 && stats.packet_loss == 0 );

    stats_sent( endpoint, 4, 2000.0 );
    stats_sent( endpoint, 8, 2000.010 );
    stats_ack( endpoint, 4, 2000.050 );
    stats_sent( endpoint, half + 8, 2000.050 );
    stats_client_getter_matches_ack_history( endpoint );
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_client_transport_ack_stats_for_test( endpoint, &stats );
    rocketnet_unit_check( fabsf( stats.rtt - 50.0f ) < 0.001f );
    rocketnet_unit_check( stats.jitter == 0 );
    rocketnet_unit_check( fabsf( stats.packet_loss - 50.0f ) < 0.001f );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_client_getter_reconnect );

// Public packet_loss_percent for a partial ACK. Hardcoding it to 0 fails.
static void test_ack_stats_public_getter_positive_loss()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 );
    stats_sent( endpoint, 3 );
    stats_ack( endpoint, 1, 1000.020 );
    stats_ack( endpoint, 2, 1000.040 );
    stats_sent( endpoint, half + 3, 1000.040 );
    rocketnet_client_stats_t pub;
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_unit_check( fabs( pub.packet_loss_percent - 100.0 / 3.0 ) < 0.001 );
    rocketnet_unit_check( fabs( pub.rtt_ms - 20.0 ) < 0.001 );
    rocketnet_unit_check( fabs( pub.jitter_ms - 10.0 ) < 0.001 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_public_getter_positive_loss );

// Reset clears the public getter. The next session's samples are the only ones.
static void test_ack_stats_public_getter_reset_new_session()
{
    auto * endpoint = stats_endpoint();
    const uint64_t half = ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2;
    stats_sent( endpoint, 1 );
    stats_sent( endpoint, 2 );
    stats_sent( endpoint, 3 );
    stats_ack( endpoint, 1, 1000.020 );
    stats_ack( endpoint, 2, 1000.040 );
    stats_sent( endpoint, half + 3, 1000.040 );
    rocketnet_client_stats_t pub;
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_unit_check( pub.packet_loss_percent > 0.0 );

    rocketnet_ack_endpoint_reset( endpoint );
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_unit_check( pub.rtt_ms == 0.0 && pub.jitter_ms == 0.0 && pub.packet_loss_percent == 0.0 );

    stats_sent( endpoint, 4, 2000.0 );
    stats_sent( endpoint, 8, 2000.010 );
    stats_ack( endpoint, 4, 2000.050 );
    stats_sent( endpoint, half + 8, 2000.050 );
    stats_public_matches_ack_history( endpoint, &pub );
    rocketnet_unit_check( fabs( pub.packet_loss_percent - 50.0 ) < 0.001 );
    rocketnet_unit_check( fabs( pub.rtt_ms - 50.0 ) < 0.001 );
    rocketnet_unit_check( pub.jitter_ms == 0.0 );
    rocketnet_free( NULL, endpoint );
}

REGISTER_UNIT_TEST( test_ack_stats_public_getter_reset_new_session );

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

#include "rocketnet_replay.h"

// one accepted session packet as both production receive paths take it:
// replay admits it and advances, then the received statistics record it
static void stats_accept( rocketnet_ack_endpoint_t * endpoint, struct rocketnet_replay_protection_t * replay,
                          uint64_t sequence, int wire_bytes, double time )
{
    rocketnet_unit_check( !rocketnet_replay_already_received( replay, sequence ) );
    rocketnet_replay_advance( replay, sequence );
    rocketnet_ack_endpoint_record_received( endpoint, sequence, wire_bytes, time );
}

// a previously unseen sequence that replay admits more than one received
// ring behind the high-water mark shares its slot with a newer counted
// record: the received statistics refuse it and keep the newer record
// counted (tla/RNServerStats.tla Write, invariant RetainedIsNewest)
static void test_ack_received_late_alias_keeps_counted_newer()
{
    auto * endpoint = stats_endpoint();
    struct rocketnet_replay_protection_t replay;
    memset( &replay, 0, sizeof(replay) );

    stats_accept( endpoint, &replay, 3000, 300, 1000.0 );
    stats_accept( endpoint, &replay, 3001, 400, 1001.0 );
    stats_accept( endpoint, &replay, 5000, 500, 1002.0 );

    // the counted window is the older half of the ring behind 5000, and
    // holds 3000 and 3001
    const uint64_t base = 5000 - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 1;
    const uint64_t top = base + ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2 - 1;
    rocketnet_unit_check( base == 2953 && top == 3976 );
    rocketnet_unit_check( 3000 >= base && 3001 <= top );

    // replay admits 952, which shares its received slot with 3000
    const uint64_t late = 952;
    rocketnet_unit_check( !rocketnet_replay_already_received( &replay, late ) );
    rocketnet_unit_check( late % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE == 3000 % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE );
    rocketnet_unit_check( late < base );

    const uint32_t count = endpoint->stats_received_count;
    const uint64_t bytes = endpoint->stats_received_bytes;
    const float kbps = endpoint->bandwidth_received_kbps;
    rocketnet_unit_check( count == 2 && bytes == 700 && kbps > 0.0f );

    stats_accept( endpoint, &replay, late, 100, 1003.0 );

    // refused: the newer record stays in its slot and counted
    rocketnet_unit_check( endpoint->stats_received_count == count );
    rocketnet_unit_check( endpoint->stats_received_bytes == bytes );
    rocketnet_unit_check( endpoint->bandwidth_received_kbps == kbps );
    rocketnet_unit_check( endpoint->received[3000 % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE].sequence == 3000 );
    rocketnet_unit_check( endpoint->highest_received_sequence == 5000 );
    stats_update_and_compare_history( endpoint );
    rocketnet_unit_check( endpoint->bandwidth_received_kbps == kbps );

    // the refusal leaves later writes counted
    stats_accept( endpoint, &replay, 3002, 200, 1004.0 );
    rocketnet_unit_check( endpoint->stats_received_count == 3 );
    rocketnet_unit_check( endpoint->stats_received_bytes == 900 );
    stats_update_and_compare_history( endpoint );

    rocketnet_free( NULL, endpoint );
}
REGISTER_UNIT_TEST( test_ack_received_late_alias_keeps_counted_newer );

#if ROCKETNET_DEV

// Stale records may look like current sequence numbers after slot reuse.
// A cleared validity mask must hide them without touching the cold ring.
static void test_ack_stats_history_ignores_unwritten_records()
{
    auto * history = new rocketnet_ack_stats_history_t;
    memset(history, 0xA5, sizeof(*history));
    memset(history->rtt_history, 0, sizeof(history->rtt_history));
    history->highest_sent = history->highest_received = 3;
    uint64_t sent_written[ROCKETNET_SENT_BITMAP_WORDS] = {};
    uint64_t received_written[ROCKETNET_RECEIVED_STATS_DIRTY_WORDS] = {};
    for (int i = 1; i <= 3; ++i)
    {
        history->sent[i] = { (uint64_t)i, (double)i, 100, 1, {} };
        history->received[i] = { (uint64_t)i, (double)i, 100, 0 };
    }
    rocketnet_ack_stats_result_t result = {};
    rocketnet_ack_stats_history_compute(history, &result, sent_written, received_written);
    rocketnet_unit_check(result.sent_kbps == 0 && result.received_kbps == 0 && result.acked_kbps == 0);
    sent_written[0] = received_written[0] = (1ull << 1) | (1ull << 3);
    rocketnet_ack_stats_history_compute(history, &result, sent_written, received_written);
    rocketnet_unit_check(fabsf(result.sent_kbps - 0.8f) < 0.0001f);
    rocketnet_unit_check(fabsf(result.received_kbps - 0.8f) < 0.0001f);
    rocketnet_unit_check(fabsf(result.acked_kbps - 0.8f) < 0.0001f);
    // A generation reset only clears validity; the exact old records remain.
    sent_written[0] = received_written[0] = 0;
    rocketnet_ack_stats_history_compute(history, &result, sent_written, received_written);
    rocketnet_unit_check(result.sent_kbps == 0 && result.received_kbps == 0 && result.acked_kbps == 0);
    delete history;
}
REGISTER_UNIT_TEST(test_ack_stats_history_ignores_unwritten_records);

static void test_ack_stats_epoch_invalidation_preserves_storage()
{
    rocketnet_stats_epoch_bits_t<2> bits;
    memset(&bits, 0xA5, sizeof(bits));
    bits.initialize();
    bits.set(7); bits.set(90);
    const uint64_t first = bits.bits[0], second = bits.bits[1];
    const uint64_t old_epoch = bits.epoch;
    bits.invalidate();
    rocketnet_unit_check(bits.epoch == old_epoch + 1 && !bits.test(7) && !bits.test(90));
    rocketnet_unit_check(bits.bits[0] == first && bits.bits[1] == second);
    bits.set(8);
    rocketnet_unit_check(bits.test(8) && !bits.test(7) && !bits.test(90));
    // Explicit rollover: an ancient epoch-1 tag must never become current.
    bits.epoch = UINT64_MAX;
    bits.tags[1] = 1;
    bits.invalidate();
    rocketnet_unit_check(bits.epoch == 1 && !bits.test(90));
    rocketnet_unit_check(bits.bits[1] == second);

    auto * history = new rocketnet_ack_stats_history_t;
    memset(history, 0xA5, sizeof(*history));
    history->highest_sent = history->highest_received = 3;
    history->rtt_history[0] = 20.0f;
    rocketnet_ack_stats_validity_t valid;
    valid.initialize();
    valid.rtt.set(0);
    rocketnet_ack_stats_result_t result;
    rocketnet_ack_stats_history_compute(history, &result, NULL, NULL, &valid);
    rocketnet_unit_check(result.rtt == 20.0f && result.jitter == 0 && result.sent_kbps == 0 && result.received_kbps == 0);
    valid.invalidate();
    rocketnet_ack_stats_history_compute(history, &result, NULL, NULL, &valid);
    rocketnet_unit_check(result.rtt == 0 && result.jitter == 0 && history->rtt_history[0] == 20.0f);
    delete history;
}
REGISTER_UNIT_TEST(test_ack_stats_epoch_invalidation_preserves_storage);

static bool stats_received_time_equal( double a, double b )
{
    return a == b || ( isnan( a ) && isnan( b ) );
}

// Compare the publication left by record_received against a fresh scan without
// calling update_stats: that call would replace the values under test. The
// extra received-only scan checks the block aggregate's unpublished count,
// bytes and extrema as well as the public bandwidth field.
static bool stats_received_live_matches_history( const rocketnet_ack_endpoint_t * endpoint )
{
    rocketnet_ack_stats_result_t result;
    stats_history_result( endpoint, &result );

    uint64_t base = 1;
    if ( endpoint->highest_received_sequence >= ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE )
        base = endpoint->highest_received_sequence - ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE + 1;

    uint64_t bytes = 0;
    uint32_t count = 0;
    double low_time = 0.0;
    double high_time = 0.0;
    for ( int i = 0; i < ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE / 2; ++i )
    {
        const uint64_t sequence = base + (uint64_t) i;
        if ( sequence > endpoint->highest_received_sequence )
            break;
        const uint64_t slot = sequence % ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE;
        if ( !( ( endpoint->received_written[slot / 64] >> ( slot % 64 ) ) & 1 ) )
            continue;
        const rocketnet_received_packet_record_t * record = &endpoint->received[slot];
        if ( record->sequence != sequence )
            continue;
        bytes += record->wire_bytes;
        if ( count == 0 )
        {
            low_time = record->receive_time;
            high_time = record->receive_time;
        }
        else
        {
            if ( record->receive_time < low_time )
                low_time = record->receive_time;
            if ( record->receive_time > high_time )
                high_time = record->receive_time;
        }
        ++count;
    }

    return result.rtt == endpoint->rtt &&
           result.jitter == endpoint->jitter &&
           result.packet_loss == endpoint->packet_loss &&
           result.sent_kbps == endpoint->bandwidth_sent_kbps &&
           result.received_kbps == endpoint->bandwidth_received_kbps &&
           result.acked_kbps == endpoint->bandwidth_acked_kbps &&
           count == endpoint->stats_received_count &&
           bytes == endpoint->stats_received_bytes &&
           stats_received_time_equal( low_time, endpoint->stats_received_low_time ) &&
           stats_received_time_equal( high_time, endpoint->stats_received_high_time );
}

static void stats_received_step( rocketnet_ack_endpoint_t * endpoint, uint64_t sequence,
                                 int wire_bytes, double time )
{
    rocketnet_ack_endpoint_record_received( endpoint, sequence, wire_bytes, time );
    rocketnet_unit_check( endpoint->stats_received_operations <= 256 );
    rocketnet_unit_check( stats_received_live_matches_history( endpoint ) );
}

// The priority replacement's exact red: after the descending writes, moving
// the high-water mark to 3071 makes sequences 1025..2047 enter the counted
// cohort. The former time-sorted list walked 1+...+1023 = 523776 predecessor
// links in that one call. The fixed block aggregate visits at most 256
// blocks/records in any receive.
static void test_ack_received_stats_adversarial_sequence_is_bounded()
{
    auto * endpoint = stats_endpoint();
    double time = 1000.0;
    for ( uint64_t sequence = 2048; sequence >= 1024; --sequence )
    {
        stats_received_step( endpoint, sequence, 100, time );
        time += 1.0;
        if ( sequence == 1024 )
            break;
    }
    stats_received_step( endpoint, 3071, 200, time );
    printf( "received ACK stats cost: old_predecessor_visits=523776 block_record_visits=%u\n",
            endpoint->stats_received_operations );
    fflush( stdout );
    rocketnet_unit_check( endpoint->stats_received_operations <= 256 );
    rocketnet_unit_check( endpoint->stats_received_count == 1024 );
    rocketnet_unit_check( endpoint->stats_received_bytes == 102400 );
    rocketnet_unit_check( endpoint->stats_received_low_time == 1001.0 );
    rocketnet_unit_check( endpoint->stats_received_high_time == 2024.0 );
    rocketnet_free( NULL, endpoint );
}
REGISTER_UNIT_TEST( test_ack_received_stats_adversarial_sequence_is_bounded );

static void test_ack_received_stats_blocks_match_scan_edges()
{
    const uint64_t N = ROCKETNET_RECEIVED_PACKET_BUFFER_SIZE;
    auto * endpoint = stats_endpoint();

    // Equal and decreasing times, duplicate replacement, N/2, N, N+1 and
    // many-N jumps. The scan oracle is checked after every operation.
    stats_received_step( endpoint, 1, 11, 7.0 );
    stats_received_step( endpoint, N / 2, 12, 7.0 );

    // Prove this oracle observes the immediate publication rather than
    // silently repairing it through update_stats before comparison.
    const float published_received_kbps = endpoint->bandwidth_received_kbps;
    endpoint->bandwidth_received_kbps = published_received_kbps + 1.0f;
    rocketnet_unit_check( !stats_received_live_matches_history( endpoint ) );
    endpoint->bandwidth_received_kbps = published_received_kbps;
    rocketnet_unit_check( stats_received_live_matches_history( endpoint ) );

    stats_received_step( endpoint, N, 13, -5.0 );
    stats_received_step( endpoint, N / 2, 14, 9.0 );
    stats_received_step( endpoint, N + 1, 15, 9.0 );
    stats_received_step( endpoint, 4 * N + 19, 16, 8.0 );
    stats_received_step( endpoint, 4 * N + 20, 17, 8.0 );

    // An alias exactly N apart clears the old physical generation before
    // the new leaf is published; a still older alias is refused unchanged.
    const uint64_t live = 5 * N + 19;
    stats_received_step( endpoint, live, 21, 10.0 );
    const uint64_t count = endpoint->stats_received_count;
    const uint64_t bytes = endpoint->stats_received_bytes;
    stats_received_step( endpoint, live - N, 2000, 11.0 );
    rocketnet_unit_check( endpoint->stats_received_count == count );
    rocketnet_unit_check( endpoint->stats_received_bytes == bytes );

    rocketnet_ack_endpoint_reset( endpoint );
    rocketnet_unit_check( stats_received_live_matches_history( endpoint ) );
    rocketnet_unit_check( endpoint->stats_received_count == 0 );
    stats_received_step( endpoint, 17, 31, 4.0 );
    stats_received_step( endpoint, N + 17, 32, 5.0 );
    rocketnet_unit_check( endpoint->received[17].sequence == N + 17 );

    // Crossing the 32-bit boundary must not truncate sequence identity or
    // choose a summary by the low 32 bits alone.
    rocketnet_ack_endpoint_reset( endpoint );
    stats_received_step( endpoint, (uint64_t) UINT32_MAX - N + 1, 33, 1.0 );
    stats_received_step( endpoint, (uint64_t) UINT32_MAX, 34, 2.0 );
    stats_received_step( endpoint, (uint64_t) UINT32_MAX + 1, 35, 3.0 );
    stats_received_step( endpoint, (uint64_t) UINT32_MAX + N, 36, 4.0 );
    rocketnet_unit_check( endpoint->highest_received_sequence == (uint64_t) UINT32_MAX + N );
    rocketnet_unit_check( endpoint->stats_received_count == 1 );
    rocketnet_unit_check( endpoint->stats_received_bytes == 35 );
    rocketnet_unit_check( endpoint->stats_received_low_time == 3.0 );
    rocketnet_unit_check( endpoint->stats_received_high_time == 3.0 );
    rocketnet_unit_check( endpoint->bandwidth_received_kbps == 0.0f );

    // The direct internal entry point historically accepts any double. A
    // first-sequence NaN poisons the reference span; a later NaN is ignored;
    // infinities and negative finite times retain the scan comparisons.
    rocketnet_ack_endpoint_reset( endpoint );
    stats_received_step( endpoint, 1, 40, nan("") );
    stats_received_step( endpoint, 2, 50, -2.0 );
    rocketnet_unit_check( endpoint->stats_received_count == 2 );
    rocketnet_unit_check( endpoint->stats_received_bytes == 90 );
    rocketnet_unit_check( isnan( endpoint->stats_received_low_time ) );
    rocketnet_unit_check( isnan( endpoint->stats_received_high_time ) );
    rocketnet_unit_check( endpoint->bandwidth_received_kbps == 0.0f );
    rocketnet_ack_endpoint_reset( endpoint );
    stats_received_step( endpoint, 1, 40, -2.0 );
    stats_received_step( endpoint, 2, 50, nan("") );
    stats_received_step( endpoint, 3, 60, HUGE_VAL );
    rocketnet_unit_check( endpoint->stats_received_count == 3 );
    rocketnet_unit_check( endpoint->stats_received_bytes == 150 );
    rocketnet_unit_check( endpoint->stats_received_low_time == -2.0 );
    rocketnet_unit_check( isinf( endpoint->stats_received_high_time ) &&
                          endpoint->stats_received_high_time > 0.0 );
    rocketnet_unit_check( endpoint->bandwidth_received_kbps == 0.0f );

    // Session sequences never wrap through zero, but the retained window may
    // end at UINT64_MAX. Exercise the physical ring wrap and full-width tags.
    rocketnet_ack_endpoint_reset( endpoint );
    const uint64_t base = UINT64_MAX - N + 1;
    const uint64_t sequences[] = { base, base + N / 2 - 1, base + 3, UINT64_MAX };
    for ( int i = 0; i < 4; ++i )
        stats_received_step( endpoint, sequences[i], 100 + i, 100.0 + i );
    rocketnet_unit_check( endpoint->highest_received_sequence == UINT64_MAX );
    rocketnet_unit_check( endpoint->stats_received_count == 3 );
    rocketnet_unit_check( endpoint->stats_received_bytes == 303 );
    rocketnet_unit_check( endpoint->stats_received_low_time == 100.0 );
    rocketnet_unit_check( endpoint->stats_received_high_time == 102.0 );
    rocketnet_unit_check( fabsf( endpoint->bandwidth_received_kbps - 1.212f ) < 0.0001f );

    // Dense full-window state followed by a long, deliberately unaligned
    // overlapping jump exercises two partial clear blocks, whole blocks, a
    // point-block rebuild and wrapped query edges in one bounded operation.
    rocketnet_ack_endpoint_reset( endpoint );
    for ( uint64_t sequence = 1; sequence <= N; ++sequence )
        stats_received_step( endpoint, sequence, (int) ( 100 + sequence % 37 ), (double) sequence );
    stats_received_step( endpoint, N + 2000, 777, 5000.0 );
    rocketnet_unit_check( endpoint->stats_received_operations <= 256 );

    // Combined 6-field live aggregate, reference history and independent
    // numeric expectations across reset, unwritten stale ring entries,
    // equal-time history, ring wrap, slot aliases and late out-of-order
    // arrivals without calling update_stats first.
    rocketnet_ack_endpoint_reset( endpoint );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( endpoint, 1, 100, 0, 0, 0, 2000.0 ) );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( endpoint, 2, 200, 0, 0, 0, 2000.0 ) );
    stats_ack( endpoint, 1, 2000.0 );
    stats_ack( endpoint, 2, 2000.0 );
    stats_received_step( endpoint, 1, 150, 2000.0 );
    stats_received_step( endpoint, 2, 250, 2000.0 );
    rocketnet_ack_stats_result_t ref = {};
    stats_history_result( endpoint, &ref );
    rocketnet_unit_check( ref.rtt == 0.0f && ref.jitter == 0.0f && ref.packet_loss == 0.0f );
    rocketnet_unit_check( ref.sent_kbps == 0.0f && ref.acked_kbps == 0.0f && ref.received_kbps == 0.0f );

    // Reset leaves the prior records in the cold rings as unwritten stale
    // entries; wrap both rings, replace slot 1 with an alias N apart, refuse a
    // stale alias, and check all six fields against independent numbers.
    rocketnet_ack_endpoint_reset( endpoint );
    const uint64_t S = ROCKETNET_SENT_PACKET_BUFFER_SIZE;
    const uint64_t sent_half = S / 2;
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( endpoint, 1, 500, 0, 0, 0, 900.0 ) );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( endpoint, S + 1, 100, 0, 0, 0, 1000.00 ) == ROCKETNET_ACK_SENT_EVICTED_UNKNOWN );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( endpoint, S + 2, 200, 0, 0, 0, 1000.25 ) );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( endpoint, S + 3, 300, 0, 0, 0, 1000.50 ) );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent( endpoint, S + sent_half + 3, 400, 0, 0, 0, 1001.00 ) );
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified( endpoint, 1, 999, 0, 0, 0, 1002.00 ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
    stats_ack( endpoint, S + 1, 1000.020 );
    stats_ack( endpoint, S + 3, 1000.540 );

    stats_received_step( endpoint, 1, 900, 900.0 );
    stats_received_step( endpoint, N + 1, 125, 2000.50 );
    stats_received_step( endpoint, N + 3, 275, 2000.00 );
    stats_received_step( endpoint, N + 2, 100, 2000.50 );
    stats_received_step( endpoint, N + N / 2 + 3, 800, 2005.00 );
    stats_received_step( endpoint, 1, 999, 2006.00 );

    stats_history_result( endpoint, &ref );
    rocketnet_unit_check( fabsf( ref.rtt - 20.0f ) < 0.001f && fabsf( endpoint->rtt - 20.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( ref.jitter - 10.0f ) < 0.001f && fabsf( endpoint->jitter - 10.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( ref.packet_loss - 100.0f / 3.0f ) < 0.001f && fabsf( endpoint->packet_loss - 100.0f / 3.0f ) < 0.001f );
    rocketnet_unit_check( fabsf( ref.sent_kbps - 9.6f ) < 0.001f && fabsf( endpoint->bandwidth_sent_kbps - 9.6f ) < 0.001f );
    rocketnet_unit_check( fabsf( ref.acked_kbps - 6.4f ) < 0.001f && fabsf( endpoint->bandwidth_acked_kbps - 6.4f ) < 0.001f );
    rocketnet_unit_check( fabsf( ref.received_kbps - 8.0f ) < 0.001f && fabsf( endpoint->bandwidth_received_kbps - 8.0f ) < 0.001f );

    rocketnet_free( NULL, endpoint );
}
REGISTER_UNIT_TEST( test_ack_received_stats_blocks_match_scan_edges );

#endif // ROCKETNET_DEV

// A send record is written only for a sequence newer than the one its slot
// holds and within one buffer of the highest sent, at a finite time; anything
// else is refused and changes nothing. rocketnet_ack_endpoint_record_sent_
// classified (sdk/rocketnet_ack.cpp) returns ROCKETNET_ACK_SENT_REFUSED_STALE
// and leaves the endpoint byte-identical when the slot already holds this or a
// newer sequence, when the sequence fell more than one buffer behind the
// highest sent, or when the time is not finite. Sequence 0 is an assert, not a
// refusal. The snapshot compares only initialized record fields, not padding
// or unwritten storage.
struct ack_sent_state_t
{
    bool written;
    struct rocketnet_sent_packet_record_t record;
};

static ack_sent_state_t sent_state( const struct rocketnet_ack_endpoint_t * endpoint, uint64_t slot )
{
    ack_sent_state_t state;
    memset( &state, 0, sizeof( state ) );
    state.written = ( endpoint->sent_written[slot >> 6] >> ( slot & 63 ) ) & 1;
    if ( state.written )
    {
        const struct rocketnet_sent_packet_record_t * record = &endpoint->sent[slot];
        state.record.sequence = record->sequence;
        state.record.packet_id = record->packet_id;
        state.record.aggregate_id = record->aggregate_id;
        state.record.send_time = record->send_time;
        state.record.wire_bytes = record->wire_bytes;
        state.record.acked = record->acked;
        state.record.has_packet_id = record->has_packet_id;
        state.record.was_keep_alive = record->was_keep_alive;
    }
    return state;
}

static bool sent_state_equal( const ack_sent_state_t & a, const ack_sent_state_t & b )
{
    return memcmp( &a, &b, sizeof( a ) ) == 0;
}

static void test_ack_record_sent_refuses_stale_and_nonfinite()
{
    const uint64_t N = ROCKETNET_SENT_PACKET_BUFFER_SIZE;

    struct rocketnet_server_acks_t * acks = rocketnet_server_acks_create( NULL, 1 );
    rocketnet_unit_check( acks );
    struct rocketnet_ack_endpoint_t * endpoint = &acks->endpoints[0];

    // (a) the same sequence twice: the first is recorded, the second (the
    // same slot, not newer) is refused and leaves the record and high-water
    // mark untouched.
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
        endpoint, 5, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_RECORDED );

    {
        const uint64_t highest_before = endpoint->highest_sequence;
        const ack_sent_state_t before = sent_state( endpoint, 5 % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            endpoint, 5, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( endpoint->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( endpoint, 5 % N ) ) );
    }

    // (b) a new high is recorded; the older sequence in the same slot is
    // refused.
    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
        endpoint, N + 10, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_RECORDED );

    {
        const uint64_t highest_before = endpoint->highest_sequence;
        const ack_sent_state_t before = sent_state( endpoint, 10 % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            endpoint, 10, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( endpoint->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( endpoint, 10 % N ) ) );
    }

    rocketnet_server_acks_destroy( acks );

    // (c) on a fresh endpoint, a sequence more than one buffer behind the
    // highest sent (in a different slot) is refused.
    struct rocketnet_server_acks_t * acks_fresh = rocketnet_server_acks_create( NULL, 1 );
    rocketnet_unit_check( acks_fresh );
    struct rocketnet_ack_endpoint_t * fresh = &acks_fresh->endpoints[0];

    rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
        fresh, 2 * N, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_RECORDED );

    {
        const uint64_t highest_before = fresh->highest_sequence;
        const ack_sent_state_t before = sent_state( fresh, ( N - 1 ) % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            fresh, N - 1, 100, 0, 0, 0, 1000.0 ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( fresh->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( fresh, ( N - 1 ) % N ) ) );
    }

    // (d) a non-finite time refuses before any state is touched.
    {
        const uint64_t highest_before = fresh->highest_sequence;
        const ack_sent_state_t before = sent_state( fresh, ( 2 * N + 1 ) % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            fresh, 2 * N + 1, 100, 0, 0, 0, (double) NAN ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( fresh->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( fresh, ( 2 * N + 1 ) % N ) ) );
    }
    {
        const uint64_t highest_before = fresh->highest_sequence;
        const ack_sent_state_t before = sent_state( fresh, ( 2 * N + 2 ) % N );
        rocketnet_unit_check( rocketnet_ack_endpoint_record_sent_classified(
            fresh, 2 * N + 2, 100, 0, 0, 0, (double) INFINITY ) == ROCKETNET_ACK_SENT_REFUSED_STALE );
        rocketnet_unit_check( fresh->highest_sequence == highest_before );
        rocketnet_unit_check( sent_state_equal( before, sent_state( fresh, ( 2 * N + 2 ) % N ) ) );
    }

    rocketnet_server_acks_destroy( acks_fresh );
}


REGISTER_UNIT_TEST( test_ack_record_sent_refuses_stale_and_nonfinite );
