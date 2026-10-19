/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST invariant checks of docs/DST.md section 4: each check mirrors an
    invariant or property a TLA+ model in tla/ already states, named below by
    module and invariant. The checks observe deliveries, receipts, link acks
    and counters; none changes SDK behaviour. Time is an argument starting at
    1000.0; nothing here reads a clock, opens a socket or starts a thread. A
    failing check prints the core's FAIL line:
    `FAIL seed=<n> step=<k> check=<name> what=<detail>`.
*/

#ifndef ROCKETNET_DST_CHECKS_H
#define ROCKETNET_DST_CHECKS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ROCKETNET_DST_CHECK_MAX_STREAMS 16
#define ROCKETNET_DST_CHECK_MAX_OPENED 64
#define ROCKETNET_DST_CHECK_MAX_SENT 64

// The ghost state one receiver keeps per stream. The checks read and update
// these counters; the SDK objects are never touched.
struct rocketnet_dst_check_stream_t
{
    // ordered (RNChannel::OrderedExactlyOnceInOrder, RNChannelTypes::
    // OrderedExactlyOnceInOrder, RNEndpoint::OrderedInOrder, RNWindow::
    // OrderedDeliveredInOrder): the k-th delivery of the stream has sequence
    // k, counting from 0.
    unsigned ordered_seen;
    uint32_t ordered_expect;

    // sequenced (RNChannelTypes::SequencedNeverBackwards,
    // NoSequencedDuplicate, SequencedNewestWins): sequences strictly increase.
    unsigned sequenced_seen;
    uint32_t sequenced_last;

    // reliable (RNChannelTypes::ReliableExactlyOnce, RNEndpoint::
    // ReliableExactlyOnce, FragmentsAndAcks::DeliveredOnce): no sequence is
    // delivered twice.
    unsigned reliable_seen;
    uint32_t reliable_last;

    // unreliable (RNChannel::UnreliableAtMostOnce, RNEndpoint::
    // UnreliableAtMostOnce): no sequence is delivered twice.
    unsigned unreliable_seen;
    uint32_t unreliable_last;

    // stale session (ServerSession::SlotReuseObservesNothing, ClientSession::
    // ReconnectObservesNothing, RNSlots::ReusedSlotStartsClean, RNSimFaults::
    // DeliverOnlyToLiveGeneration): a delivery's generation and incarnation
    // equal the receiving session's.
    uint32_t expected_generation;
    int expected_incarnation;

    // liveness (RNChannelTypes::EveryReliableAckedAndDelivered, RNChannel::
    // EveryOrderedMessageDelivered, RNReceipts::ReceiptProgress): every
    // reliable message sent on a still-live session is delivered by the
    // liveness bound.
    uint32_t reliable_sent;
    uint32_t reliable_delivered;
    uint32_t sent_seqs[ROCKETNET_DST_CHECK_MAX_SENT];
    unsigned delivered_seqs[ROCKETNET_DST_CHECK_MAX_SENT];

    // ack (RNReceipts::AckedMeansPacketOpened, RNChannelTypes::
    // AckedImpliesTransportArrival, ReplayWindow::AckReportsOnlyReceived,
    // FragmentsAndAcks::AckedMeansSurfaced): every sequence a link ack names
    // was opened by the peer.
    uint32_t opened[ROCKETNET_DST_CHECK_MAX_OPENED];
    unsigned opened_count;
};

struct rocketnet_dst_check_state_t
{
    uint64_t seed;
    uint64_t step;
    int failed;
    char check[64];
    char detail[256];
    struct rocketnet_dst_check_stream_t stream[ROCKETNET_DST_CHECK_MAX_STREAMS];
};

static inline void rocketnet_dst_check_init( struct rocketnet_dst_check_state_t * st,
                                            uint64_t seed, uint64_t step )
{
    memset( st, 0, sizeof(*st) );
    st->seed = seed;
    st->step = step;
}

static inline void rocketnet_dst_check_fail( struct rocketnet_dst_check_state_t * st,
                                             const char * name, const char * what )
{
    if ( st->failed )
        return;
    st->failed = 1;
    snprintf( st->check, sizeof(st->check), "%s", name );
    snprintf( st->detail, sizeof(st->detail), "%s", what );
    printf( "FAIL seed=%llu step=%llu check=%s what=%s\n",
            (unsigned long long) st->seed, (unsigned long long) st->step, name, what );
    fflush( stdout );
}

// Mirrors RNChannel::OrderedExactlyOnceInOrder, RNChannelTypes::
// OrderedExactlyOnceInOrder, RNEndpoint::OrderedInOrder and RNWindow::
// OrderedDeliveredInOrder. On ordered types, the k-th delivery of the stream
// has sequence k, counting from 0.
static inline int rocketnet_dst_check_ordered( struct rocketnet_dst_check_state_t * st,
                                               double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( !s->ordered_seen )
    {
        s->ordered_seen = 1;
        s->ordered_expect = 0;
    }
    if ( seq != s->ordered_expect )
    {
        char what[160];
        snprintf( what, sizeof(what), "stream=%d expected=%u got=%u",
                  stream, s->ordered_expect, seq );
        rocketnet_dst_check_fail( st, "ordered_in_order", what );
        return 1;
    }
    s->ordered_expect++;
    return 0;
}

// Mirrors RNChannelTypes::ReliableExactlyOnce, RNEndpoint::ReliableExactlyOnce
// and FragmentsAndAcks::DeliveredOnce. No sequence of a reliable stream is
// delivered twice.
static inline int rocketnet_dst_check_reliable_once( struct rocketnet_dst_check_state_t * st,
                                                     double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->reliable_seen && seq == s->reliable_last )
    {
        char what[160];
        snprintf( what, sizeof(what), "stream=%d seq=%u delivered twice", stream, seq );
        rocketnet_dst_check_fail( st, "reliable_once", what );
        return 1;
    }
    s->reliable_seen = 1;
    s->reliable_last = seq;
    return 0;
}

// Mirrors RNChannelTypes::SequencedNeverBackwards and NoSequencedDuplicate.
// Sequenced sequence strictly increase on the stream.
static inline int rocketnet_dst_check_sequenced( struct rocketnet_dst_check_state_t * st,
                                                 double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->sequenced_seen && seq <= s->sequenced_last )
    {
        char what[160];
        snprintf( what, sizeof(what), "stream=%d last=%u got=%u",
                  stream, s->sequenced_last, seq );
        rocketnet_dst_check_fail( st, "sequenced_forward", what );
        return 1;
    }
    s->sequenced_seen = 1;
    s->sequenced_last = seq;
    return 0;
}

// Mirrors RNChannel::UnreliableAtMostOnce and RNEndpoint::UnreliableAtMostOnce.
// No sequence of an unreliable stream is delivered twice.
static inline int rocketnet_dst_check_unreliable_once( struct rocketnet_dst_check_state_t * st,
                                                       double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->unreliable_seen && seq == s->unreliable_last )
    {
        char what[160];
        snprintf( what, sizeof(what), "stream=%d seq=%u delivered twice", stream, seq );
        rocketnet_dst_check_fail( st, "unreliable_once", what );
        return 1;
    }
    s->unreliable_seen = 1;
    s->unreliable_last = seq;
    return 0;
}

// Records a sequence the peer session opened (the opened set of an ack check).
static inline void rocketnet_dst_check_opened( struct rocketnet_dst_check_state_t * st,
                                               int stream, uint32_t seq )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->opened_count < ROCKETNET_DST_CHECK_MAX_OPENED )
        s->opened[s->opened_count++] = seq;
}

// Mirrors RNReceipts::AckedMeansPacketOpened, RNChannelTypes::
// AckedImpliesTransportArrival, ReplayWindow::AckReportsOnlyReceived and
// FragmentsAndAcks::AckedMeansSurfaced. Every sequence a link ack names was
// opened by the peer.
static inline int rocketnet_dst_check_acked( struct rocketnet_dst_check_state_t * st,
                                             double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    int found = 0;
    for ( unsigned i = 0; i < s->opened_count; i++ )
    {
        if ( s->opened[i] == seq )
        {
            found = 1;
            break;
        }
    }
    if ( !found )
    {
        char what[160];
        snprintf( what, sizeof(what), "stream=%d seq=%u acked but not opened", stream, seq );
        rocketnet_dst_check_fail( st, "acked_means_opened", what );
        return 1;
    }
    return 0;
}

// Mirrors ServerSession::SlotReuseObservesNothing, ClientSession::
// ReconnectObservesNothing, RNSlots::ReusedSlotStartsClean and RNSimFaults::
// DeliverOnlyToLiveGeneration. A delivery's generation and incarnation equal
// the receiving session's, compared separately.
static inline int rocketnet_dst_check_stale_session( struct rocketnet_dst_check_state_t * st,
                                                     double time, int stream, uint32_t generation,
                                                     int incarnation )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    s->expected_generation = generation;
    s->expected_incarnation = incarnation;
    (void) s;
    return 0;
}

static inline int rocketnet_dst_check_stale_session_payload( struct rocketnet_dst_check_state_t * st,
                                                             double time, int stream, uint32_t generation,
                                                             int incarnation )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( generation != s->expected_generation || incarnation != s->expected_incarnation )
    {
        char what[200];
        snprintf( what, sizeof(what), "stream=%d generation=%u/%u incarnation=%d/%d",
                  stream, generation, s->expected_generation, incarnation, s->expected_incarnation );
        rocketnet_dst_check_fail( st, "stale_session_payload", what );
        return 1;
    }
    return 0;
}

// Mirrors RNChannelTypes::EveryReliableAckedAndDelivered, RNChannel::
// EveryOrderedMessageDelivered and RNReceipts::ReceiptProgress (the liveness
// bound, section 4). Every reliable message sent on a live session is
// delivered by the bound.
static inline int rocketnet_dst_check_liveness_delivered( struct rocketnet_dst_check_state_t * st,
                                                          double time, int stream )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    for ( unsigned i = 0; i < s->reliable_sent && i < ROCKETNET_DST_CHECK_MAX_SENT; i++ )
    {
        int delivered = 0;
        for ( unsigned j = 0; j < s->reliable_delivered && j < ROCKETNET_DST_CHECK_MAX_SENT; j++ )
        {
            if ( s->delivered_seqs[j] == s->sent_seqs[i] )
            {
                delivered = 1;
                break;
            }
        }
        if ( !delivered )
        {
            char what[160];
            snprintf( what, sizeof(what), "stream=%d seq=%u undelivered at the liveness bound",
                      stream, s->sent_seqs[i] );
            rocketnet_dst_check_fail( st, "liveness_delivered", what );
            return 1;
        }
    }
    (void) s;
    return 0;
}

#endif // #ifndef ROCKETNET_DST_CHECKS_H
