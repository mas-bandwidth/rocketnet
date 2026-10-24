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
