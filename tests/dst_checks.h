/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST invariant checks of docs/DST.md section 4: each check mirrors an
    invariant or property a TLA+ model in proofs/tla/ already states, named below by
    module and invariant. The checks observe deliveries, receipts, link acks,
    transport states and counters; none changes SDK behaviour. Time is an
    argument starting at 1000.0; nothing here reads a clock, opens a socket or
    starts a thread. A failing check records its line and the core prints:
    `FAIL seed=<n> step=<k> check=<name> what=<detail>`.

    Modules mirrored here: proofs/tla/RNChannel.tla, proofs/tla/RNChannelTypes.tla,
    proofs/tla/RNEndpoint.tla, proofs/tla/RNWindow.tla, proofs/tla/RNReceipts.tla,
    proofs/tla/ReplayWindow.tla, proofs/tla/SessionAcks.tla, proofs/tla/ServerSession.tla,
    proofs/tla/ClientSession.tla, proofs/tla/RNSlots.tla and proofs/tla/RNSimFaults.tla.
*/

#pragma once

#ifndef ROCKETNET_TESTS_DST_CHECKS_H
#define ROCKETNET_TESTS_DST_CHECKS_H

#include "rocketnet_counters.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ROCKETNET_DST_MAX_CLIENTS
#define ROCKETNET_DST_MAX_CLIENTS 16
#endif // #ifndef ROCKETNET_DST_MAX_CLIENTS

// One stream is (receiving session, send channel, type). A server slot s is
// session s; client i is session 16 + i. 32 * 9 * 8 = 2304.
#define ROCKETNET_DST_CHECK_MAX_SESSIONS 32
#define ROCKETNET_DST_CHECK_MAX_STREAMS 2304
#define ROCKETNET_DST_CHECK_MAX_OPENED 64
#define ROCKETNET_DST_CHECK_SEQ_BITS 4096
#define ROCKETNET_DST_CHECK_SEQ_BYTES ( ROCKETNET_DST_CHECK_SEQ_BITS / 8 )
#define ROCKETNET_DST_CHECK_MAX_PACKETS 4096

// The reliable bytes a session may have outstanding before the harness stops
// drawing reliable sends for it (rocketnet_dst_send in dst_harness.h), and
// the largest message it draws (rocketnet_dst_draw_size).
#define ROCKETNET_DST_CHECK_OUTSTANDING_CAP 65536
#define ROCKETNET_DST_CHECK_MAX_MESSAGE_BYTES 16384

// The steps a sequenced message of the quiet window may still be in flight at
// the last step, at 10 ms a step. Two parts:
//
// - the wire: the simulator's longest delay is 150 ms of latency plus half of
//   it in jitter, 225 ms, so 30 steps;
// - the send budget: a reliable item that does not fit the budget waits for a
//   later update and is never dropped (proofs/tla/RNSendBudget.tla), so a new
//   send queues behind every reliable byte its session has outstanding. That
//   is at most the cap plus one more message, drained at the slowest budget,
//   the client's ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS: ( 65536 + 16384 ) x 8
//   bits at 1000 kbps is 656 ms, 66 steps.
//
// A type 4 sent earlier than both together is owed.
#define ROCKETNET_DST_CHECK_WIRE_STEPS 30
#define ROCKETNET_DST_CHECK_BUDGET_STEPS \
    ( ( ( ROCKETNET_DST_CHECK_OUTSTANDING_CAP + ROCKETNET_DST_CHECK_MAX_MESSAGE_BYTES ) * 8 / ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS + 9 ) / 10 )
#define ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS ( ROCKETNET_DST_CHECK_WIRE_STEPS + ROCKETNET_DST_CHECK_BUDGET_STEPS )

// The ghost state one stream keeps. The checks read and update these
// counters; the SDK objects are never touched.
struct rocketnet_dst_check_stream_t
{
    uint32_t generation;            // the session this state belongs to
    uint64_t identity;              // client_id (server) or generation (client)
    int seen;

    // ordered (RNChannel::OrderedExactlyOnceInOrder, RNChannelTypes::
    // OrderedExactlyOnceInOrder, RNEndpoint::OrderedInOrder, RNWindow::
    // OrderedDeliveredInOrder): the k-th delivery has sequence k from 0.
    unsigned ordered_seen;
    uint32_t ordered_expect;
    uint32_t ordered_last;
    int ordered_has_last;

    // ordered_unchanged_by_sequenced (RNChannel::
    // OrderedTraceUnchangedBySequenced, RNChannelTypes::
    // OrderedTraceUnchangedBySequenced): the same rule on channel 8.
    unsigned unchanged_seen;
    uint32_t unchanged_expect;
    uint32_t unchanged_last;

    // sequenced (RNChannelTypes::SequencedNeverBackwards,
    // NoSequencedDuplicate, SequencedNewestWins).
    unsigned sequenced_seen;
    uint32_t sequenced_last;
    uint32_t sequenced_epoch;
    int sequenced_has_epoch;

    // reliable (RNChannelTypes::ReliableExactlyOnce, RNEndpoint::
    // ReliableExactlyOnce).
    uint8_t reliable_delivered[ROCKETNET_DST_CHECK_SEQ_BYTES];
    uint8_t reliable_sent[ROCKETNET_DST_CHECK_SEQ_BYTES];

    // The oldest reliable sequence still unresolved (sent and not delivered).
    // The two tables above are ROCKETNET_DST_CHECK_SEQ_BITS wide, so an index
    // that far past this base would read a newer sequence's bit; the helpers
    // refuse it by name (dst_lap) instead of aliasing.
    uint32_t reliable_base;

    // unreliable (RNChannel::UnreliableAtMostOnce, RNEndpoint::
    // UnreliableAtMostOnce).
    uint8_t unreliable_delivered[ROCKETNET_DST_CHECK_SEQ_BYTES];

    // stale session (ServerSession::SlotReuseObservesNothing, ClientSession::
    // ReconnectObservesNothing, RNSlots::ReusedSlotStartsClean, RNSimFaults::
    // DeliverOnlyToLiveGeneration).
    uint32_t expected_generation;
    int expected_incarnation;

    // receipt_one_outcome (RNReceipts::ExactlyOneOutcomePerReceiptSerial,
    // LossOnlyForUnreliableReceipt).
    uint8_t receipt_seen[ROCKETNET_DST_CHECK_SEQ_BYTES];

    // whole_message (RNChannel::WholeMessagesOnly): the size recorded when
    // the sequence was sent.
    uint32_t sent_size[ROCKETNET_DST_CHECK_SEQ_BITS];
    uint8_t sent_size_set[ROCKETNET_DST_CHECK_SEQ_BITS];
    uint32_t last_sent_seq;
    uint64_t last_sent_step;
    uint32_t settled_seq;
    uint32_t quiet_start_seq;
    int settled;
    int has_last_sent;

    // acked_means_opened (RNReceipts::AckedMeansPacketOpened,
    // RNChannelTypes::AckedImpliesTransportArrival, ReplayWindow::
    // AckReportsOnlyReceived, SessionAcks::AckedMeansSurfaced).
    uint8_t opened[ROCKETNET_DST_CHECK_SEQ_BYTES];

    // liveness_delivered (RNChannel::EveryOrderedMessageDelivered,
    // RNChannelTypes::EveryReliableAckedAndDelivered,
    // RNReceipts::ReceiptProgress).
    uint32_t reliable_sent_count;
    uint32_t reliable_delivered_count;
    uint64_t outstanding_bytes;
};

// The cross-type view of the sequenced messages on one (session, channel).
// The SDK keeps one sequenced_write counter and one sequenced_watermark per
// channel for types 1 and 4 together (rn_channels_send_sequenced in
// sdk/rocketnet_channel.cpp), and proofs/tla/RNChannelTypes.tla states
// SequencedNewestWins over one SequencedTrace per channel holding both codes.
// So a newer delivery of either type supersedes an older send of the other,
// and the receiver drops what it superseded. send_count mirrors
// sequenced_write: the send index (from 1) of each type 1 or 4 send.
struct rocketnet_dst_check_sequenced_channel_t
{
    int seen;
    uint64_t identity;
    uint32_t send_count;
    uint32_t settle_count;              // send_count when the quiet window began
    int quiet_required_set;             // a type 4 was sent in the quiet window
    uint32_t quiet_required_index;      // the newest such send's send index
    uint32_t quiet_required_epoch;      // that send's ordered epoch
    uint64_t quiet_required_step;       // that send's step
    uint32_t quiet_served_index;        // the highest index delivered in the required epoch
    int quiet_served_set;
    uint32_t delivered_index;           // the highest send index delivered, either type
    uint32_t delivered_epoch;
    int delivered_set;
    uint32_t index_of[2][ROCKETNET_DST_CHECK_SEQ_BITS];     // [type 1, type 4][sequence]
};

struct rocketnet_dst_check_state_t
{
    uint64_t seed;
    uint64_t step;
    int failed;
    char check[64];
    char detail[256];
    struct rocketnet_dst_check_stream_t stream[ROCKETNET_DST_CHECK_MAX_STREAMS];
    struct rocketnet_dst_check_sequenced_channel_t sequenced[ROCKETNET_DST_CHECK_MAX_SESSIONS * 9];

    // one_session_per_slot / token_admits_once (ServerSession::
    // SlotOwnsItsSession, TokenAdmitsOnce; ClientSession::
    // CurrentOnlyWhileLive).
    uint64_t slot_client_id[ROCKETNET_DST_MAX_CLIENTS];
    int slot_connected[ROCKETNET_DST_MAX_CLIENTS];
    int slot_incarnation[ROCKETNET_DST_MAX_CLIENTS];
    uint64_t admitted_client_id[ROCKETNET_DST_CHECK_MAX_PACKETS];
    unsigned admitted_count;

    // client_terminal (ClientSession::ConnectReachesConnectedOrError,
    // safety side).
    int client_state[ROCKETNET_DST_MAX_CLIENTS];

    // unexplained_refusal, clock-back row: set when an endpoint's clock is
    // seen behind the shared step clock. Only the clock-back schedule moves
    // an endpoint's offset, and it only moves it back, so a negative offset
    // observed here is that schedule having fired on this run.
    int clock_back_seen;

    // fresh_generation (RNSimFaults::RestartIsFresh).
    uint64_t generation_high[ROCKETNET_DST_MAX_CLIENTS];

    // replay_monotone (ReplayWindow::Monotone, HighestIsReceived).
    uint64_t replay_high[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint64_t replay_generation[ROCKETNET_DST_CHECK_MAX_SESSIONS];

    // replay_acked_delivered and liveness use the stream tables above.

    // the seam's packet id to (session, channel, type, sequence) join key.
    uint64_t issued_session[ROCKETNET_DST_CHECK_MAX_PACKETS];
    uint32_t issued_seq[ROCKETNET_DST_CHECK_MAX_PACKETS];
    uint16_t issued_channel[ROCKETNET_DST_CHECK_MAX_PACKETS];
    uint16_t issued_type[ROCKETNET_DST_CHECK_MAX_PACKETS];
    uint8_t issued_set[ROCKETNET_DST_CHECK_MAX_PACKETS];

    unsigned allocation_balance;
};

static inline void rocketnet_dst_check_init( struct rocketnet_dst_check_state_t * st,
                                             uint64_t seed, uint64_t step )
{
    memset( st, 0, sizeof( *st ) );
    st->seed = seed;
    st->step = step;
}

static inline void rocketnet_dst_check_fail( struct rocketnet_dst_check_state_t * st,
                                             const char * name, const char * what )
{
    if ( st->failed )
        return;
    st->failed = 1;
    snprintf( st->check, sizeof( st->check ), "%s", name );
    snprintf( st->detail, sizeof( st->detail ), "%s", what );
}

// The core prints the line; a unit case that wants the fire stays silent.

static inline void rocketnet_dst_check_report( struct rocketnet_dst_check_state_t * st )
{
    if ( !st->failed )
        return;
    printf( "FAIL seed=%llu step=%llu check=%s what=%s\n",
            (unsigned long long) st->seed, (unsigned long long) st->step, st->check, st->detail );
    fflush( stdout );
}

static inline int rocketnet_dst_check_stream_index( int session, int channel, int type )
{
    if ( session < 0 || session >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
        return 0;
    if ( channel < 0 || channel >= 9 )
        return 0;
    if ( type < 0 || type >= 8 )
        return 0;

    return ( session * 9 + channel ) * 8 + type;
}

// One sequence table holds ROCKETNET_DST_CHECK_SEQ_BITS bits. base is the
// oldest index the table still answers for: every bit below it is resolved.
// An index at or past base + the width would read a newer sequence's bit, so
// the checks fail naming the table, index, base and lap instead of aliasing.
// The tables that never resolve an entry (opened, unreliable_delivered,
// receipt_seen) pass base 0.

static inline int rocketnet_dst_check_lap( struct rocketnet_dst_check_state_t * st,
                                           const char * table, uint32_t base, uint32_t i )
{
    if ( i < base || i - base < ROCKETNET_DST_CHECK_SEQ_BITS )
        return 0;

    char what[192];
    snprintf( what, sizeof( what ), "table=%s seq=%u base=%u lap=%u",
              table, i, base, ( i - base ) / ROCKETNET_DST_CHECK_SEQ_BITS );
    rocketnet_dst_check_fail( st, "dst_lap", what );

    return 1;
}

static inline int rocketnet_dst_check_bit( struct rocketnet_dst_check_state_t * st,
                                           const uint8_t * bits, uint32_t base, uint32_t i,
                                           const char * table )
{
    if ( i < base )
        return 1;                       // resolved below the oldest unresolved index
    if ( rocketnet_dst_check_lap( st, table, base, i ) )
        return 0;

    return ( bits[( i % ROCKETNET_DST_CHECK_SEQ_BITS ) / 8] >> ( i % 8 ) ) & 1;
}

static inline void rocketnet_dst_check_set_bit( struct rocketnet_dst_check_state_t * st,
                                                uint8_t * bits, uint32_t base, uint32_t i,
                                                const char * table )
{
    if ( i < base )
        return;                         // resolved below the oldest unresolved index
    if ( rocketnet_dst_check_lap( st, table, base, i ) )
        return;
    bits[( i % ROCKETNET_DST_CHECK_SEQ_BITS ) / 8] |= (uint8_t) ( 1u << ( i % 8 ) );
}

// Moves the reliable base past the contiguous delivered sequences and clears
// the slot each one vacates, so the index 4096 later starts clean instead of
// inheriting the resolved sequence's bit.

static inline void rocketnet_dst_check_reliable_resolve( struct rocketnet_dst_check_stream_t * s )
{
    while ( s->has_last_sent && s->reliable_base <= s->last_sent_seq )
    {
        const uint32_t at = s->reliable_base % ROCKETNET_DST_CHECK_SEQ_BITS;
        if ( !( s->reliable_delivered[at / 8] & (uint8_t) ( 1u << ( at % 8 ) ) ) )
            break;
        s->reliable_sent[at / 8] &= ( uint8_t ) ~( 1u << ( at % 8 ) );
        s->reliable_delivered[at / 8] &= ( uint8_t ) ~( 1u << ( at % 8 ) );
        s->sent_size_set[at] = 0;
        s->sent_size[at] = 0;
        s->reliable_base++;
    }
}

static inline struct rocketnet_dst_check_stream_t * rocketnet_dst_check_stream(
    struct rocketnet_dst_check_state_t * st, int session, int channel, int type,
    uint32_t generation, uint64_t identity )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[rocketnet_dst_check_stream_index( session, channel, type )];
    if ( s->seen && s->identity != identity )
        memset( s, 0, sizeof( *s ) );
    if ( !s->seen )
    {
        s->seen = 1;
        s->generation = generation;
        s->identity = identity;
    }
    return s;
}

// The cross-type sequenced view of (session, channel), reset when the session
// changes identity like the stream tables are. NULL outside the tables.
static inline struct rocketnet_dst_check_sequenced_channel_t * rocketnet_dst_check_sequenced_channel(
    struct rocketnet_dst_check_state_t * st, int session, int channel, uint64_t identity )
{
    if ( session < 0 || session >= ROCKETNET_DST_CHECK_MAX_SESSIONS || channel < 0 || channel >= 9 )
        return NULL;
    struct rocketnet_dst_check_sequenced_channel_t * c = &st->sequenced[session * 9 + channel];
    if ( c->seen && c->identity != identity )
        memset( c, 0, sizeof( *c ) );
    if ( !c->seen )
    {
        c->seen = 1;
        c->identity = identity;
    }
    return c;
}

// Records the size the sender drew for a sequence. Called where the harness
// builds a payload, before the message object sees it.

static inline void rocketnet_dst_check_sent( struct rocketnet_dst_check_state_t * st,
                                             double time, int session, int channel, int type,
                                             uint32_t sequence, uint32_t generation, uint64_t identity,
                                             uint32_t epoch, int bytes )
{
    (void) time;

    struct rocketnet_dst_check_stream_t * s =
        rocketnet_dst_check_stream( st, session, channel, type, generation, identity );
    const int reliable = ( type == 2 || type == 3 || type == 4 || type == 6 || type == 7 );
    const int lap = rocketnet_dst_check_lap( st, "sent_size", s->reliable_base, sequence );
    if ( reliable && !lap )
    {
        if ( !rocketnet_dst_check_bit( st, s->reliable_sent, s->reliable_base, sequence, "reliable_sent" ) )
        {
            rocketnet_dst_check_set_bit( st, s->reliable_sent, s->reliable_base, sequence, "reliable_sent" );
            s->reliable_sent_count++;
        }
    }

    if ( !lap )
    {
        // exact at every legal size: a non-negative int fits uint32_t, no clamp
        s->sent_size[sequence % ROCKETNET_DST_CHECK_SEQ_BITS] = bytes > 0 ? (uint32_t) bytes : 0;
        s->sent_size_set[sequence % ROCKETNET_DST_CHECK_SEQ_BITS] = 1;
    }

    s->last_sent_seq = sequence;
    s->last_sent_step = st->step;
    s->has_last_sent = 1;
    rocketnet_dst_check_reliable_resolve( s );
    if ( reliable && !lap )
    {
        s->outstanding_bytes += (uint32_t) bytes;
    }

    if ( channel == 8 && ( type == 1 || type == 4 ) )
    {
        s->sequenced_epoch = epoch;
        s->sequenced_has_epoch = 1;
    }

    if ( type == 1 || type == 4 )
    {
        struct rocketnet_dst_check_sequenced_channel_t * c =
            rocketnet_dst_check_sequenced_channel( st, session, channel, identity );
        if ( c )
        {
            c->send_count++;
            c->index_of[type == 4][sequence % ROCKETNET_DST_CHECK_SEQ_BITS] = c->send_count;
            if ( type == 4 && c->send_count > c->settle_count )
            {
                // The newest type 4 of the quiet window is the one the
                // channel owes, so every later type 4 raises the index a
                // delivery has to reach. The send index grows with the send
                // order, so a delivery seen before this send has a lower
                // index and cannot serve it: a delivery of another epoch
                // does not serve this send either, so the served index
                // starts empty.
                if ( c->quiet_required_set && c->quiet_required_epoch != epoch )
                {
                    c->quiet_served_set = 0;
                }
                c->quiet_required_set = 1;
                c->quiet_required_index = c->send_count;
                c->quiet_required_epoch = epoch;
                c->quiet_required_step = st->step;
            }
        }
    }
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
        snprintf( what, sizeof( what ), "stream=%d expected=%u got=%u",
                  stream, s->ordered_expect, seq );
        rocketnet_dst_check_fail( st, "ordered_in_order", what );
        return 1;
    }
    s->ordered_expect++;
    return 0;
}

// Mirrors RNChannelTypes::ReliableExactlyOnce and
// RNEndpoint::ReliableExactlyOnce. No sequence of a reliable stream is
// delivered twice, and no sequence that was never sent is delivered.
static inline int rocketnet_dst_check_reliable_once( struct rocketnet_dst_check_state_t * st,
                                                     double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( rocketnet_dst_check_bit( st, s->reliable_delivered, s->reliable_base, seq, "reliable_delivered" ) )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d seq=%u generation=%u delivered twice",
                  stream, seq, s->generation );
        rocketnet_dst_check_fail( st, "reliable_once", what );
        return 1;
    }
    if ( !rocketnet_dst_check_bit( st, s->reliable_sent, s->reliable_base, seq, "reliable_sent" ) )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d seq=%u was never sent", stream, seq );
        rocketnet_dst_check_fail( st, "reliable_once", what );
        return 1;
    }
    rocketnet_dst_check_set_bit( st, s->reliable_delivered, s->reliable_base, seq, "reliable_delivered" );
    s->reliable_delivered_count++;
    rocketnet_dst_check_reliable_resolve( s );
    return 0;
}

// Mirrors RNChannelTypes::SequencedNeverBackwards and NoSequencedDuplicate.
// Sequenced sequences strictly increase on the stream; on channel 8 the
// epoch increases, or is equal with a larger sequence.
static inline int rocketnet_dst_check_sequenced( struct rocketnet_dst_check_state_t * st,
                                                 double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->sequenced_seen && seq <= s->sequenced_last )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d last=%u got=%u",
                  stream, s->sequenced_last, seq );
        rocketnet_dst_check_fail( st, "sequenced_forward", what );
        return 1;
    }
    s->sequenced_seen = 1;
    s->sequenced_last = seq;
    return 0;
}

// The channel-8 form of sequenced_forward, where the epoch orders types 1 and
// 4 ahead of their sequence.
static inline int rocketnet_dst_check_sequenced_epoch( struct rocketnet_dst_check_state_t * st,
                                                       double time, int stream, uint32_t seq, uint32_t epoch )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->sequenced_seen && !( epoch > s->sequenced_epoch ||
                                 ( epoch == s->sequenced_epoch && seq > s->sequenced_last ) ) )
    {
        char what[200];
        snprintf( what, sizeof( what ), "stream=%d epoch=%u/%u seq=%u/%u",
                  stream, s->sequenced_epoch, epoch, s->sequenced_last, seq );
        rocketnet_dst_check_fail( st, "sequenced_forward", what );
        return 1;
    }
    s->sequenced_seen = 1;
    s->sequenced_last = seq;
    s->sequenced_epoch = epoch;
    return 0;
}

// Mirrors RNChannel::UnreliableAtMostOnce and RNEndpoint::UnreliableAtMostOnce.
// No sequence of an unreliable stream is delivered twice.
static inline int rocketnet_dst_check_unreliable_once( struct rocketnet_dst_check_state_t * st,
                                                       double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( rocketnet_dst_check_bit( st, s->unreliable_delivered, 0, seq, "unreliable_delivered" ) )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d seq=%u delivered twice", stream, seq );
        rocketnet_dst_check_fail( st, "unreliable_once", what );
        return 1;
    }
    rocketnet_dst_check_set_bit( st, s->unreliable_delivered, 0, seq, "unreliable_delivered" );
    return 0;
}

// Records a sequence the receiving session opened (the opened set of an ack
// check).

static inline void rocketnet_dst_check_opened( struct rocketnet_dst_check_state_t * st,
                                               int stream, uint32_t seq )
{
    rocketnet_dst_check_set_bit( st, st->stream[stream].opened, 0, seq, "opened" );
}

// Mirrors RNReceipts::AckedMeansPacketOpened, RNChannelTypes::
// AckedImpliesTransportArrival, ReplayWindow::AckReportsOnlyReceived and
// SessionAcks::AckedMeansSurfaced. Every sequence a link ack names was
// opened by the receiver.
static inline int rocketnet_dst_check_acked( struct rocketnet_dst_check_state_t * st,
                                             double time, int stream, uint32_t seq )
{
    (void) time;
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( !rocketnet_dst_check_bit( st, s->opened, 0, seq, "opened" ) )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d seq=%u acked but not opened", stream, seq );
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
        snprintf( what, sizeof( what ), "stream=%d generation=%u/%u incarnation=%d/%d",
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
    const uint32_t lo = s->reliable_base;
    const uint32_t hi = lo + ROCKETNET_DST_CHECK_SEQ_BITS;
    for ( uint32_t seq = lo; seq < hi; seq++ )
    {
        if ( !rocketnet_dst_check_bit( st, s->reliable_sent, s->reliable_base, seq, "reliable_sent" ) )
            continue;
        if ( !rocketnet_dst_check_bit( st, s->reliable_delivered, s->reliable_base, seq, "reliable_delivered" ) )
        {
            char what[160];
            snprintf( what, sizeof( what ), "stream=%d seq=%u undelivered at the liveness bound",
                      stream, seq );
            rocketnet_dst_check_fail( st, "liveness_delivered", what );
            return 1;
        }
    }
    return 0;
}

// Mirrors RNChannel::WholeMessagesOnly: the delivery's length matches the size recorded when its sequence was sent.

static inline int rocketnet_dst_check_whole_message( struct rocketnet_dst_check_state_t * st,
                                                     int stream, uint32_t seq, int bytes )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( seq < s->reliable_base )
        return 0;
    if ( rocketnet_dst_check_lap( st, "sent_size", s->reliable_base, seq ) )
        return 1;

    const uint32_t at = seq % ROCKETNET_DST_CHECK_SEQ_BITS;
    if ( s->sent_size_set[at] && ( bytes < 0 || s->sent_size[at] != (uint32_t) bytes ) )  // a negative delivery is itself a mismatch
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d seq=%u sent=%u got=%d bytes",
                  stream, seq, (unsigned) s->sent_size[at], bytes );
        rocketnet_dst_check_fail( st, "whole_message", what );
        return 1;
    }

    return 0;
}

// Mirrors RNChannel::OrderedTraceUnchangedBySequenced and RNChannelTypes::
// OrderedTraceUnchangedBySequenced: on channel 8 the ordered deliveries alone
// are the ordered sends, in order.

static inline int rocketnet_dst_check_ordered_unchanged( struct rocketnet_dst_check_state_t * st,
                                                         int stream, uint32_t seq )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( !s->unchanged_seen )
    {
        s->unchanged_seen = 1;
        s->unchanged_expect = 0;
    }

    if ( seq != s->unchanged_expect )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d expected=%u got=%u",
                  stream, s->unchanged_expect, seq );
        rocketnet_dst_check_fail( st, "ordered_unchanged_by_sequenced", what );
        return 1;
    }

    s->unchanged_expect++;

    return 0;
}

static inline int rocketnet_dst_check_receipt_one_outcome( struct rocketnet_dst_check_state_t * st,
                                                           int stream, int type, uint32_t seq );
static inline int rocketnet_dst_check_receipt_acked_delivered( struct rocketnet_dst_check_state_t * st,
                                                               int stream, uint32_t seq );

// The ordered delivery rule over the sender's recorded sequences: strictly
// increasing, and no earlier sent sequence is still undelivered.

static inline int rocketnet_dst_check_ordered_runtime( struct rocketnet_dst_check_state_t * st,
                                                       int stream, uint32_t seq )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->ordered_has_last && seq <= s->ordered_last )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d last=%u got=%u", stream, s->ordered_last, seq );
        rocketnet_dst_check_fail( st, "ordered_in_order", what );
        return 1;
    }

    for ( uint32_t earlier = 0; earlier < seq && earlier < ROCKETNET_DST_CHECK_SEQ_BITS; earlier++ )
    {
        if ( !rocketnet_dst_check_bit( st, s->reliable_sent, s->reliable_base, earlier, "reliable_sent" ) )
            continue;

        if ( !rocketnet_dst_check_bit( st, s->reliable_delivered, s->reliable_base, earlier, "reliable_delivered" ) )
        {
            char what[200];
            snprintf( what, sizeof( what ), "stream=%d seq=%u delivered before %u", stream, seq, earlier );
            rocketnet_dst_check_fail( st, "ordered_in_order", what );
            return 1;
        }
    }

    s->ordered_last = seq;
    s->ordered_has_last = 1;

    return 0;
}

static inline int rocketnet_dst_check_unchanged_runtime( struct rocketnet_dst_check_state_t * st,
                                                         int stream, uint32_t seq )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( s->unchanged_seen && seq <= s->unchanged_last )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d last=%u got=%u", stream, s->unchanged_last, seq );
        rocketnet_dst_check_fail( st, "ordered_unchanged_by_sequenced", what );
        return 1;
    }

    s->unchanged_seen = 1;
    s->unchanged_last = seq;

    return 0;
}

// One delivery, all the per-delivery rows. expected_generation is the
// receiving session's generation (0 when the session is not live), and
// expected_incarnation the session's server incarnation.

static inline void rocketnet_dst_check_delivery( struct rocketnet_dst_check_state_t * st,
                                                 double time, int session, int channel, int type,
                                                 uint32_t sequence, uint32_t generation, int incarnation,
                                                 uint64_t identity, uint32_t epoch, int bytes,
                                                 uint32_t expected_generation, int expected_incarnation )
{
    struct rocketnet_dst_check_stream_t * s =
        rocketnet_dst_check_stream( st, session, channel, type, generation, identity );
    const int stream = rocketnet_dst_check_stream_index( session, channel, type );
    (void) s;

    rocketnet_dst_check_stale_session( st, time, stream, expected_generation, expected_incarnation );
    rocketnet_dst_check_stale_session_payload( st, time, stream, generation, incarnation );
    rocketnet_dst_check_whole_message( st, stream, sequence, bytes );
    rocketnet_dst_check_opened( st, stream, sequence );

    if ( type == 2 || type == 3 || type == 6 || type == 7 )
    {
        rocketnet_dst_check_reliable_once( st, time, stream, sequence );
    }

    if ( type == 4 )
    {
        rocketnet_dst_check_set_bit( st, s->reliable_delivered, s->reliable_base, sequence, "reliable_delivered" );
    }
    rocketnet_dst_check_reliable_resolve( s );

    if ( type == 2 || type == 3 || type == 4 || type == 6 || type == 7 )
    {
        if ( s->outstanding_bytes >= (uint64_t) bytes )
            s->outstanding_bytes -= (uint32_t) bytes;
        else
            s->outstanding_bytes = 0;
    }

    if ( type == 3 || type == 7 || ( channel == 8 && type == 3 ) )
    {
        rocketnet_dst_check_ordered_runtime( st, stream, sequence );
    }

    if ( channel == 8 && type == 3 )
    {
        rocketnet_dst_check_unchanged_runtime( st, stream, sequence );
    }

    if ( type == 1 || type == 4 )
    {
        if ( channel == 8 )
            rocketnet_dst_check_sequenced_epoch( st, time, stream, sequence, epoch );
        else
            rocketnet_dst_check_sequenced( st, time, stream, sequence );

        // the cross-type view: a delivery of either type at or above the
        // newest type 4 quiet-window send, in its epoch, serves that send

        struct rocketnet_dst_check_sequenced_channel_t * c =
            rocketnet_dst_check_sequenced_channel( st, session, channel, identity );
        const uint32_t index = c ? c->index_of[type == 4][sequence % ROCKETNET_DST_CHECK_SEQ_BITS] : 0;
        if ( index > 0 )
        {
            if ( !c->delivered_set || index > c->delivered_index )
            {
                c->delivered_set = 1;
                c->delivered_index = index;
                c->delivered_epoch = epoch;
            }

            if ( c->quiet_required_set && epoch == c->quiet_required_epoch &&
                 ( !c->quiet_served_set || index > c->quiet_served_index ) )
            {
                c->quiet_served_set = 1;
                c->quiet_served_index = index;
            }
        }
    }

    if ( type == 0 || type == 5 )
    {
        rocketnet_dst_check_unreliable_once( st, time, stream, sequence );
    }

    if ( type == 5 || type == 6 || type == 7 )
    {
        rocketnet_dst_check_receipt_one_outcome( st, stream, type, sequence );
    }

    if ( type == 6 || type == 7 )
    {
        rocketnet_dst_check_receipt_acked_delivered( st, stream, sequence );
    }
}

// Mirrors RNReceipts::ExactlyOneOutcomePerReceiptSerial and
// LossOnlyForUnreliableReceipt. A receipt serial of types 5, 6, 7 gets one
// outcome; the delivery is the outcome this run observes.

static inline int rocketnet_dst_check_receipt_one_outcome( struct rocketnet_dst_check_state_t * st,
                                                           int stream, int type, uint32_t seq )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    (void) type;

    if ( rocketnet_dst_check_bit( st, s->receipt_seen, 0, seq, "receipt_seen" ) )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d seq=%u two receipt outcomes", stream, seq );
        rocketnet_dst_check_fail( st, "receipt_one_outcome", what );
        return 1;
    }

    rocketnet_dst_check_set_bit( st, s->receipt_seen, 0, seq, "receipt_seen" );

    return 0;
}

// Mirrors RNReceipts::BlockAckedOnlyWhenAllFragmentsAcked and
// RNChannelTypes::EveryReliableAckedAndDelivered (safety side): an ACKED
// receipt for types 6 and 7 names a message the receiver delivered; by the last
// step on a live session it is delivered.

static inline int rocketnet_dst_check_receipt_acked_delivered( struct rocketnet_dst_check_state_t * st,
                                                               int stream, uint32_t seq )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( !rocketnet_dst_check_bit( st, s->reliable_delivered, s->reliable_base, seq, "reliable_delivered" ) )
    {
        char what[160];
        snprintf( what, sizeof( what ), "stream=%d seq=%u a receipt without a delivery", stream, seq );
        rocketnet_dst_check_fail( st, "receipt_acked_delivered", what );
        return 1;
    }

    return 0;
}

// Mirrors ServerSession::SlotOwnsItsSession, ClientSession::
// CurrentOnlyWhileLive and ServerSession::TokenAdmitsOnce. No two connected
// slots hold one client_id; a client_id reaches connected once per server
// incarnation. Called every step, after the server update.

static inline void rocketnet_dst_check_slot( struct rocketnet_dst_check_state_t * st, int slot,
                                             int connected, uint64_t client_id, int incarnation )
{
    if ( slot < 0 || slot >= ROCKETNET_DST_MAX_CLIENTS )
        return;

    if ( connected )
    {
        for ( int other = 0; other < ROCKETNET_DST_MAX_CLIENTS; other++ )
        {
            if ( other == slot || !st->slot_connected[other] )
                continue;

            if ( st->slot_client_id[other] == client_id && client_id != 0 )
            {
                char what[200];
                snprintf( what, sizeof( what ), "slot=%d and slot=%d hold client_id=%llu",
                          slot, other, (unsigned long long) client_id );
                rocketnet_dst_check_fail( st, "one_session_per_slot", what );
                return;
            }
        }

        if ( st->slot_client_id[slot] != client_id )
        {
            for ( unsigned i = 0; i < st->admitted_count; i++ )
            {
                if ( st->admitted_client_id[i] == client_id )
                {
                    char what[200];
                    snprintf( what, sizeof( what ), "client_id=%llu admitted twice",
                              (unsigned long long) client_id );
                    rocketnet_dst_check_fail( st, "token_admits_once", what );
                    return;
                }
            }

            if ( st->admitted_count < ROCKETNET_DST_CHECK_MAX_PACKETS )
            {
                st->admitted_client_id[st->admitted_count++] = client_id;
            }
        }

        st->slot_client_id[slot] = client_id;
        st->slot_connected[slot] = 1;
        st->slot_incarnation[slot] = incarnation;
    }
    else
    {
        st->slot_connected[slot] = 0;
        st->slot_client_id[slot] = 0;
    }
}

// Mirrors ClientSession::ConnectReachesConnectedOrError (safety side): a
// terminal client state outside the explained windows of section 3.

static inline void rocketnet_dst_check_client_state( struct rocketnet_dst_check_state_t * st, int client,
                                                     int state, int terminal_explained )
{
    if ( client < 0 || client >= ROCKETNET_DST_MAX_CLIENTS )
        return;

    const int terminal = state < 0;
    if ( terminal && !terminal_explained )
    {
        char what[160];
        snprintf( what, sizeof( what ), "client=%d terminal state=%d", client, state );
        rocketnet_dst_check_fail( st, "client_terminal", what );
    }

    st->client_state[client] = state;
}

// Mirrors RNSimFaults::RestartIsFresh: a restarted client connects only under
// a generation never used before.

static inline void rocketnet_dst_check_generation( struct rocketnet_dst_check_state_t * st, int client,
                                                   uint64_t generation )
{
    if ( client < 0 || client >= ROCKETNET_DST_MAX_CLIENTS )
        return;

    if ( generation <= st->generation_high[client] )
    {
        char what[160];
        snprintf( what, sizeof( what ), "client=%d generation=%llu after %llu",
                  client, (unsigned long long) generation, (unsigned long long) st->generation_high[client] );
        rocketnet_dst_check_fail( st, "fresh_generation", what );
    }

    if ( generation > st->generation_high[client] )
    {
        st->generation_high[client] = generation;
    }
}

// Mirrors ReplayWindow::Monotone and HighestIsReceived: within a session the
// receiver's replay_session.highest_opened never decreases.

static inline void rocketnet_dst_check_replay( struct rocketnet_dst_check_state_t * st, int session,
                                               uint64_t generation, uint64_t highest_opened )
{
    if ( session < 0 || session >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
        return;

    if ( st->replay_generation[session] != generation )
    {
        st->replay_generation[session] = generation;
        st->replay_high[session] = highest_opened;
        return;
    }

    if ( highest_opened < st->replay_high[session] )
    {
        char what[160];
        snprintf( what, sizeof( what ), "session=%d highest=%llu after %llu", session,
                  (unsigned long long) highest_opened, (unsigned long long) st->replay_high[session] );
        rocketnet_dst_check_fail( st, "replay_monotone", what );
    }

    st->replay_high[session] = highest_opened;
}

// Mirrors RNSimFaults::CrashedEndpointSilent: an endpoint (the server or a client) that is not up sends and
// receives nothing. No crash fault is injected (docs/DST.md, "Fault kinds");
// every endpoint is up, so the check runs and holds.

static inline void rocketnet_dst_check_crashed_silent( struct rocketnet_dst_check_state_t * st, int endpoint,
                                                       int live, int sent_or_received )
{
    if ( !live && sent_or_received )
    {
        char what[160];
        snprintf( what, sizeof( what ), "endpoint=%d crashed but sent or received", endpoint );
        rocketnet_dst_check_fail( st, "crashed_silent", what );
    }
}

// Mirrors RNSimFaults::SkewBounded and ClockMonotone: every endpoint's clock is
// within its bound of the true clock. The harness passes each endpoint's own
// clock (docs/DST.md, clock-back) and the shared step clock; clock-back
// steps one endpoint's clock back on purpose, so ClockMonotone is not
// checked here.
static inline void rocketnet_dst_check_clock( struct rocketnet_dst_check_state_t * st, int endpoint,
                                              double clock, double true_clock, double bound )
{
    const double delta = clock - true_clock;
    if ( delta < 0 )
        st->clock_back_seen = 1;
    if ( delta > bound || delta < -bound )
    {
        char what[160];
        snprintf( what, sizeof( what ), "endpoint=%d clock offset=%.6f bound=%.6f", endpoint, delta, bound );
        rocketnet_dst_check_fail( st, "skew_bounded", what );
    }
}

// Mirrors ClientSession::ConnectReachesConnectedOrError, ServerSession::
// HandshakeResolves and RNSimFaults::EveryLiveClientConnects: every live
// client is connected and the server holds its session by the last step.

static inline void rocketnet_dst_check_liveness_connected( struct rocketnet_dst_check_state_t * st,
                                                           int client, int live, int connected )
{
    if ( live && !connected )
    {
        char what[160];
        snprintf( what, sizeof( what ), "client=%d live but not connected", client );
        rocketnet_dst_check_fail( st, "liveness_connected", what );
    }
}

// The reliable bytes sent on a session and not yet delivered at the receiver.

static inline uint64_t rocketnet_dst_check_outstanding( struct rocketnet_dst_check_state_t * st, int session )
{
    uint64_t total = 0;
    for ( int channel = 0; channel < 9; channel++ )
        for ( int type = 0; type < 8; type++ )
            total += st->stream[rocketnet_dst_check_stream_index( session, channel, type )].outstanding_bytes;

    return total;
}

// The newest type 4 sequence this stream delivered (every sequence below the
// reliable base is delivered), for the failure line.

static inline uint32_t rocketnet_dst_check_type4_newest( const struct rocketnet_dst_check_stream_t * s )
{
    uint32_t newest = s->reliable_base > 0 ? s->reliable_base - 1 : 0;
    for ( uint32_t seq = s->reliable_base; seq < s->reliable_base + ROCKETNET_DST_CHECK_SEQ_BITS; seq++ )
        if ( ( s->reliable_delivered[( seq % ROCKETNET_DST_CHECK_SEQ_BITS ) / 8] >> ( seq % 8 ) ) & 1 )
            newest = seq;

    return newest;
}

// Marks the sequences that were sent before the fault window ended: the
// quiet window is the liveness bound, so only those are due at the last step.

static inline void rocketnet_dst_check_settle( struct rocketnet_dst_check_state_t * st )
{
    for ( int i = 0; i < ROCKETNET_DST_CHECK_MAX_STREAMS; i++ )
    {
        struct rocketnet_dst_check_stream_t * s = &st->stream[i];
        if ( !s->seen )
            continue;
        s->settled_seq = s->last_sent_seq;
        s->quiet_start_seq = s->last_sent_seq;
        s->settled = 1;
    }

    for ( int i = 0; i < ROCKETNET_DST_CHECK_MAX_SESSIONS * 9; i++ )
    {
        struct rocketnet_dst_check_sequenced_channel_t * c = &st->sequenced[i];
        if ( !c->seen )
            continue;
        c->settle_count = c->send_count;
        c->quiet_required_set = 0;
        c->quiet_served_set = 0;
    }
}

// Called at the last step: every reliable message sent on a stream whose
// session is still live is delivered. The harness passes the live flag per
// session (0 the server, i + 1 client i).

static inline void rocketnet_dst_check_end( struct rocketnet_dst_check_state_t * st, double /* time */,
                                            const int * session_live, const uint64_t * live_identity )
{
    for ( int session = 0; session < ROCKETNET_DST_CHECK_MAX_SESSIONS; session++ )
    {
        if ( !session_live[session] )
            continue;

        for ( int channel = 0; channel < 9; channel++ )
        {
            for ( int type = 0; type < 8; type++ )
            {
                struct rocketnet_dst_check_stream_t * s =
                    &st->stream[rocketnet_dst_check_stream_index( session, channel, type )];
                if ( !s->seen || s->identity != live_identity[session] )
                    continue;

                if ( type == 2 || type == 3 || type == 6 || type == 7 )
                {
                    // messages sent after the fault window ended, inside the
                    // quiet window, are due; the last 20 may still be in
                    // flight
                    const uint32_t settled = s->last_sent_seq > 20 ? s->last_sent_seq - 20 : 0;
                    for ( uint32_t seq = s->quiet_start_seq + 1;
                          seq <= settled && seq < s->reliable_base + ROCKETNET_DST_CHECK_SEQ_BITS; seq++ )
                    {
                        if ( rocketnet_dst_check_bit( st, s->reliable_sent, s->reliable_base, seq, "reliable_sent" ) &&
                             !rocketnet_dst_check_bit( st, s->reliable_delivered, s->reliable_base, seq, "reliable_delivered" ) )
                        {
                            char what[160];
                            snprintf( what, sizeof( what ), "stream=%d seq=%u settled=%u last=%u at=%llu id=%llu",
                                      rocketnet_dst_check_stream_index( session, channel, type ), seq,
                                      s->settled_seq, s->last_sent_seq, (unsigned long long) s->last_sent_step,
                                      (unsigned long long) s->identity );
                            rocketnet_dst_check_fail( st, "liveness_delivered", what );
                            break;
                        }
                    }
                }

                if ( type == 4 && s->has_last_sent && s->last_sent_seq > s->quiet_start_seq )
                {
                    // Sequenced delivery never goes backwards and drops what a
                    // newer message superseded, and types 1 and 4 share one
                    // sequenced_write and one sequenced_watermark per channel
                    // (rn_channels_send_sequenced in sdk/rocketnet_channel.cpp;
                    // SequencedNewestWins in proofs/tla/RNChannelTypes.tla). So the
                    // quiet window is served when a delivery of type 1 or 4
                    // at or above the newest type 4 quiet-window send was
                    // seen on the channel, in that send's ordered epoch.
                    const struct rocketnet_dst_check_sequenced_channel_t * c =
                        &st->sequenced[session * 9 + channel];
                    const int served = c->quiet_served_set && c->quiet_served_index >= c->quiet_required_index;

                    // a type 4 sent in the last steps of the run may still be
                    // queued behind the send budget or on the wire
                    // (ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS), so it is not owed yet

                    const int in_flight = c->quiet_required_set &&
                                          st->step - c->quiet_required_step < ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS;
                    if ( !served && !in_flight )
                    {
                        char what[220];
                        snprintf( what, sizeof( what ),
                                  "stream=%d quiet_start=%u newest=%u sent_count=%u cross_newest=%u quiet_required=%u",
                                  rocketnet_dst_check_stream_index( session, channel, type ), s->quiet_start_seq,
                                  rocketnet_dst_check_type4_newest( s ), s->reliable_sent_count,
                                  c->delivered_set ? c->delivered_index : 0,
                                  c->quiet_required_set ? c->quiet_required_index : 0 );
                        rocketnet_dst_check_fail( st, "liveness_delivered", what );
                    }
                }
            }
        }
    }
}

// Mirrors the counter law of rocketnet_counters_conserved (no model).

static inline void rocketnet_dst_check_conservation( struct rocketnet_dst_check_state_t * st,
                                                     const struct rocketnet_counters_t * counters,
                                                     const char * end )
{
    // "<end>: <report>" fits the check state's detail[256] (dst_checks.h:120):
    // end is "server" or "clients" (dst_harness.h:1264-1265), and the report
    // is one counter name and two u64 (sdk/rocketnet_counters.cpp:137)

    char report[240];
    if ( !rocketnet_counters_conserved( counters, report, sizeof( report ) ) )
    {
        char what[256];
        snprintf( what, sizeof( what ), "%s: %s", end, report );
        rocketnet_dst_check_fail( st, "conservation", what );
    }
}

// Mirrors soak_sim's explained table (no model). A refusal not explained by
// the faults this run injects is a FAIL. The duplicate fault (or the dup-storm
// schedule) is the one that can replay a packet, so it is the one that
// explains a replayed ACK or a payload taken twice.

static inline void rocketnet_dst_check_unexplained_refusal( struct rocketnet_dst_check_state_t * st,
                                                            const struct rocketnet_counters_t * counters,
                                                            const char * end,
                                                            int duplicates_enabled )
{
    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        const uint64_t value = rocketnet_counter_value( counters, i );
        if ( value == 0 || rocketnet_counter_class( i ) != ROCKETNET_COUNTER_CLASS_REFUSAL )
            continue;

        const int explained =
            i == ROCKETNET_COUNTER_SEND_HISTORY_OVERFLOW ||
            // A replayed standalone ACK is a benign refusal when the
            // duplicate fault can replay next-update ACKs.
            ( duplicates_enabled && i == ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED ) ||
            // The server's standalone ACK replayed at the client: only the
            // duplicate fault moves it (seeds 1-200, one fault at a time),
            // as soak_sim's table explains the same row.
            ( duplicates_enabled && i == ROCKETNET_COUNTER_CLIENT_ACK_ALREADY_RECEIVED ) ||
            // Measured mechanism (seeds 1-1000 under clock-back at base:
            // 11 seeds, first seed 13): clock-back rewinds one endpoint's
            // protocol clock by a drawn 10 ms .. 5 s, past the server's
            // three-second cookie lifetime (cookie_lifetime_seconds = 3 in
            // the harness; the server judges expiry on the shared unix
            // timestamp, cookie_expire <= current_timestamp). The rewound
            // client therefore answers a cookie challenge after the shared
            // clock has passed its expiry, so the server counts
            // CONNECTION_REQUEST_COOKIE_EXPIRED for a request the schedule
            // itself made late. Explained only when a rewound clock was
            // actually observed on this run (clock_back_seen, set by
            // rocketnet_dst_check_clock above); on any other run the same
            // counter stays unexplained.
            ( st->clock_back_seen && i == ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED ) ||
            i == ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING;
        if ( !explained )
        {
            char what[256];
            snprintf( what, sizeof( what ), "%s: %s = %llu", end,
                      rocketnet_counter_name( i ), (unsigned long long) value );
            rocketnet_dst_check_fail( st, "unexplained_refusal", what );
            return;
        }
    }
}

// No model: the harness's allocations balance across a seed.

static inline void rocketnet_dst_check_leak( struct rocketnet_dst_check_state_t * st )
{
    if ( st->allocation_balance != 0 )
    {
        char what[160];
        snprintf( what, sizeof( what ), "%u harness allocations not freed", st->allocation_balance );
        rocketnet_dst_check_fail( st, "leak", what );
    }
}

// The seam's ack join key: a packet id maps to the stream and sequence its
// frame was sealed under.

static inline void rocketnet_dst_check_issued( struct rocketnet_dst_check_state_t * st, uint64_t packet_id )
{
    const uint32_t at = (uint32_t) ( packet_id % ROCKETNET_DST_CHECK_MAX_PACKETS );
    st->issued_set[at] = 1;
}

// Mirrors RNReceipts::AckedMeansPacketOpened: a link ack names a packet this
// endpoint issued. The join key the seam surfaces is the packet id recorded
// when the frame was sealed.

static inline void rocketnet_dst_check_ack_id( struct rocketnet_dst_check_state_t * st, uint64_t packet_id )
{
    const uint32_t at = (uint32_t) ( packet_id % ROCKETNET_DST_CHECK_MAX_PACKETS );
    if ( packet_id != 0 && !st->issued_set[at] )
    {
        char what[160];
        snprintf( what, sizeof( what ), "packet_id=%llu acked but never issued",
                  (unsigned long long) packet_id );
        rocketnet_dst_check_fail( st, "acked_means_opened", what );
    }
}

#endif // ROCKETNET_TESTS_DST_CHECKS_H
