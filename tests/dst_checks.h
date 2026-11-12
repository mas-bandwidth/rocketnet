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

// receipt_outcome_due: past the receipt deadline (the packet's send time plus
// message_resend_delay_max_ms), the slack for the update that writes the
// packet and the one that surfaces the outcome, in seconds. Not a measured
// bound: generous against the DST step, and the only number the check adds.
#define ROCKETNET_DST_RECEIPT_DUE_SLACK 0.5
#define ROCKETNET_DST_CHECK_SEQ_BYTES ( ROCKETNET_DST_CHECK_SEQ_BITS / 8 )
#define ROCKETNET_DST_CHECK_MAX_PACKETS 4096

// The reliable bytes a session may have outstanding before the harness stops
// drawing reliable sends for it (rocketnet_dst_send in dst_harness.h), and
// the largest message it draws (rocketnet_dst_draw_size).
#define ROCKETNET_DST_CHECK_OUTSTANDING_CAP 65536
#define ROCKETNET_DST_CHECK_MAX_MESSAGE_BYTES 16384

// acked_means_opened's ledger: the server is endpoint 0 and client i is
// endpoint i + 1. Each endpoint numbers its packets from 1 in its own counter,
// so the ledger is per endpoint, and it holds the newest LEDGER_PACKETS ids,
// more than the largest sent-packet history (ROCKETNET_HISTORY_PACKETS_MAX)
// an ack can name.
#define ROCKETNET_DST_CHECK_ENDPOINTS ( ROCKETNET_DST_MAX_CLIENTS + 1 )
#define ROCKETNET_DST_CHECK_LEDGER_PACKETS 65536

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

// The newest reliable sends of one stream, each with the step it was sent
// at, so the end-of-run check excuses only what was sent inside the run's
// last in-flight slack (ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS, or the outage
// schedules' longer one). The harness sends one message per client per step
// on one of the session's 72 streams (rocketnet_dst_send_one), and more only
// to a dark session in an outage window, which ends long before the run, so
// the ring reaches back past the slack at the end of the run. A ring that
// did not would only owe more, never less (rocketnet_dst_check_owed_through).
#define ROCKETNET_DST_CHECK_RECENT_SENDS 128

// The ghost state one stream keeps. The checks read and update these
// counters; the SDK objects are never touched.
struct rocketnet_dst_check_stream_t
{
    uint32_t generation;            // the session this state belongs to
    uint64_t identity;              // client_id (server) or generation (client)
    int seen;

    // ordered (RNChannel::OrderedExactlyOnceInOrder, RNChannelTypes::
    // OrderedExactlyOnceInOrder, RNEndpoint::OrderedInOrder, RNWindow::
    // OrderedDeliveredInOrder): the newest ordered delivery.
    uint32_t ordered_last;
    int ordered_has_last;

    // ordered_unchanged_by_sequenced (RNChannel::
    // OrderedTraceUnchangedBySequenced, RNChannelTypes::
    // OrderedTraceUnchangedBySequenced): the same rule on channel 8.
    unsigned unchanged_seen;
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

    // liveness_delivered (RNChannel::EveryOrderedMessageDelivered,
    // RNChannelTypes::EveryReliableAckedAndDelivered,
    // RNReceipts::ReceiptProgress).
    uint32_t reliable_sent_count;
    uint32_t reliable_delivered_count;
    uint64_t outstanding_bytes;

    // the newest reliable sends and their steps (ROCKETNET_DST_CHECK_RECENT_SENDS)
    uint32_t recent_seq[ROCKETNET_DST_CHECK_RECENT_SENDS];
    uint64_t recent_step[ROCKETNET_DST_CHECK_RECENT_SENDS];
    uint32_t recent_count;
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

// One type 5, 6 or 7 send, found again by the serial its receipt carries.
struct rocketnet_dst_check_receipt_t
{
    uint64_t identity;
    uint32_t serial;
    uint32_t seq;
    int stream;
    int type;
    int outcomes;
    double sent_time;       // receipt_outcome_due: when the send returned (type 5)
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

    // unexplained_refusal, outage row: set when the outage schedules drop a
    // dark client's packet while that client is not yet connected.
    int outage_handshake_seen;

    // unexplained_refusal, send-fail row: set when the send-fail schedule's
    // transport refused a frame at finish.
    int send_fail_seen;

    // liveness_delivered, the by-step rule: the steps a reliable send may
    // still be in flight at the end. 0 is ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS;
    // a run whose send side differs from the defaults sets its own.
    uint64_t in_flight_steps;

    // fresh_generation (RNSimFaults::RestartIsFresh).
    uint64_t generation_high[ROCKETNET_DST_MAX_CLIENTS];

    // replay_monotone (ReplayWindow::Monotone, HighestIsReceived).
    uint64_t replay_high[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint64_t replay_generation[ROCKETNET_DST_CHECK_MAX_SESSIONS];

    // replay_acked_delivered and liveness use the stream tables above.

    // acked_means_opened (RNReceipts::AckedMeansPacketOpened): per
    // endpoint, the newest packet id it issued, and which of the newest
    // LEDGER_PACKETS ids it issued and has seen acked.
    uint64_t issued_high[ROCKETNET_DST_CHECK_ENDPOINTS];
    uint8_t issued_bits[ROCKETNET_DST_CHECK_ENDPOINTS][ROCKETNET_DST_CHECK_LEDGER_PACKETS / 8];
    uint8_t acked_bits[ROCKETNET_DST_CHECK_ENDPOINTS][ROCKETNET_DST_CHECK_LEDGER_PACKETS / 8];

    unsigned allocation_balance;

    // receipt_one_outcome and receipt_acked_delivered: each type 5, 6 or 7
    // send by its sender (server slot s is s, client i is 16 + i) and serial.
    struct rocketnet_dst_check_receipt_t receipt[ROCKETNET_DST_CHECK_MAX_SESSIONS][ROCKETNET_DST_CHECK_SEQ_BITS];

    // receipt_outcome_due: each sender's type 5 serials in send order, the
    // oldest at receipt_due_head (positions modulo SEQ_BITS)
    uint32_t receipt_due[ROCKETNET_DST_CHECK_MAX_SESSIONS][ROCKETNET_DST_CHECK_SEQ_BITS];
    uint32_t receipt_due_head[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint32_t receipt_due_tail[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint32_t receipt_due_serial[ROCKETNET_DST_CHECK_MAX_SESSIONS];  // the serial the endpoint had issued at the last step
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
    {
        return;
    }
    st->failed = 1;
    snprintf( st->check, sizeof( st->check ), "%s", name );
    snprintf( st->detail, sizeof( st->detail ), "%s", what );
}

static inline int rocketnet_dst_check_stream_index( int session, int channel, int type )
{
    if ( session < 0 || session >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
    {
        return 0;
    }
    if ( channel < 0 || channel >= 9 )
    {
        return 0;
    }
    if ( type < 0 || type >= 8 )
    {
        return 0;
    }

    return ( session * 9 + channel ) * 8 + type;
}

// One sequence table holds ROCKETNET_DST_CHECK_SEQ_BITS bits. base is the
// oldest index the table still answers for: every bit below it is resolved.
// An index at or past base + the width would read a newer sequence's bit, so
// the checks fail naming the table, index, base and lap instead of aliasing.
// The table that never resolves an entry (unreliable_delivered) passes base 0.

static inline int rocketnet_dst_check_lap( struct rocketnet_dst_check_state_t * st,
                                           const char * table, uint32_t base, uint32_t i )
{
    if ( i < base || i - base < ROCKETNET_DST_CHECK_SEQ_BITS )
    {
        return 0;
    }

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
    {
        return 1;                       // resolved below the oldest unresolved index
    }
    if ( rocketnet_dst_check_lap( st, table, base, i ) )
    {
        return 0;
    }

    return ( bits[( i % ROCKETNET_DST_CHECK_SEQ_BITS ) / 8] >> ( i % 8 ) ) & 1;
}

static inline void rocketnet_dst_check_set_bit( struct rocketnet_dst_check_state_t * st,
                                                uint8_t * bits, uint32_t base, uint32_t i,
                                                const char * table )
{
    if ( i < base )
    {
        return;                         // resolved below the oldest unresolved index
    }
    if ( rocketnet_dst_check_lap( st, table, base, i ) )
    {
        return;
    }
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
        {
            break;
        }
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
    {
        memset( s, 0, sizeof( *s ) );
    }
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
    {
        return NULL;
    }
    struct rocketnet_dst_check_sequenced_channel_t * c = &st->sequenced[session * 9 + channel];
    if ( c->seen && c->identity != identity )
    {
        memset( c, 0, sizeof( *c ) );
    }
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
        const uint32_t at = s->recent_count % ROCKETNET_DST_CHECK_RECENT_SENDS;
        s->recent_seq[at] = sequence;
        s->recent_step[at] = st->step;
        s->recent_count++;
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

// Mirrors RNChannel::WholeMessagesOnly: the delivery's length matches the size recorded when its sequence was sent.

static inline int rocketnet_dst_check_whole_message( struct rocketnet_dst_check_state_t * st,
                                                     int stream, uint32_t seq, int bytes )
{
    struct rocketnet_dst_check_stream_t * s = &st->stream[stream];
    if ( seq < s->reliable_base )
    {
        return 0;
    }
    if ( rocketnet_dst_check_lap( st, "sent_size", s->reliable_base, seq ) )
    {
        return 1;
    }

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

// Mirrors RNChannel::OrderedExactlyOnceInOrder, RNChannelTypes::
// OrderedExactlyOnceInOrder, RNEndpoint::OrderedInOrder and RNWindow::
// OrderedDeliveredInOrder, over the sender's recorded sequences: an ordered
// stream's deliveries strictly increase, and no earlier sent sequence is
// still undelivered.

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
        {
            continue;
        }

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

// Mirrors RNChannel::OrderedTraceUnchangedBySequenced and RNChannelTypes::
// OrderedTraceUnchangedBySequenced: on channel 8 the ordered deliveries
// strictly increase whatever the sequenced traffic beside them does.

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
        {
            s->outstanding_bytes -= (uint32_t) bytes;
        }
        else
        {
            s->outstanding_bytes = 0;
        }
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
        {
            rocketnet_dst_check_sequenced_epoch( st, time, stream, sequence, epoch );
        }
        else
        {
            rocketnet_dst_check_sequenced( st, time, stream, sequence );
        }

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
}

// Records the serial the sender's send returned for a type 5, 6 or 7, after
// rocketnet_dst_check_sent recorded the sequence.

static inline void rocketnet_dst_check_receipt_sent( struct rocketnet_dst_check_state_t * st, int sender,
                                                     uint64_t identity, uint32_t serial, int session,
                                                     int channel, int type, uint32_t seq )
{
    if ( ( type != 5 && type != 6 && type != 7 ) || sender < 0 || sender >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
    {
        return;
    }
    struct rocketnet_dst_check_receipt_t * r = &st->receipt[sender][serial % ROCKETNET_DST_CHECK_SEQ_BITS];
    r->identity = identity;
    r->serial = serial;
    r->seq = seq;
    r->stream = rocketnet_dst_check_stream_index( session, channel, type );
    r->type = type;
    r->outcomes = 0;
}

// Mirrors RNReceipts::BlockAckedOnlyWhenAllFragmentsAcked and
// RNChannelTypes::EveryReliableAckedAndDelivered (safety side), for one
// receipt rocketnet_dst_check_receipt_one_outcome accepted: an ACKED receipt
// names a message the receiver delivered, or for type 7 one it holds behind
// an earlier sequence still undelivered. A receipt whose receiving stream has
// moved to a new session is not judged.

static inline int rocketnet_dst_check_receipt_acked_delivered( struct rocketnet_dst_check_state_t * st,
                                                               int sender, uint32_t serial, int32_t status )
{
    const struct rocketnet_dst_check_receipt_t * r = &st->receipt[sender][serial % ROCKETNET_DST_CHECK_SEQ_BITS];
    const struct rocketnet_dst_check_stream_t * s = &st->stream[r->stream];
    if ( status != ROCKETNET_RECEIPT_STATUS_ACKED || !s->seen || s->identity != r->identity )
    {
        return 0;
    }

    const int delivered = r->type == 5 ? rocketnet_dst_check_bit( st, s->unreliable_delivered, 0, r->seq, "unreliable_delivered" )
                                       : rocketnet_dst_check_bit( st, s->reliable_delivered, s->reliable_base, r->seq, "reliable_delivered" );
    if ( delivered || ( r->type == 7 && s->reliable_base < r->seq ) )
    {
        return 0;
    }

    char what[160];
    snprintf( what, sizeof( what ), "sender=%d serial=%u stream=%d seq=%u acked but not delivered", sender, serial, r->stream, r->seq );
    rocketnet_dst_check_fail( st, "receipt_acked_delivered", what );
    return 1;
}

// Mirrors RNReceipts::ExactlyOneOutcomePerReceiptSerial and
// LossOnlyForUnreliableReceipt. Called for each receipt a sender reads: it
// names a type 5, 6 or 7 this sender sent, it is the serial's only outcome,
// and LOSS is for type 5 alone. Then the receipt is checked against delivery.

static inline int rocketnet_dst_check_receipt_one_outcome( struct rocketnet_dst_check_state_t * st, int sender,
                                                           uint64_t identity, uint32_t serial, int32_t status )
{
    if ( sender < 0 || sender >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
    {
        return 0;
    }
    struct rocketnet_dst_check_receipt_t * r = &st->receipt[sender][serial % ROCKETNET_DST_CHECK_SEQ_BITS];
    const char * fault = NULL;
    if ( r->serial != serial || r->identity != identity || r->type == 0 )
    {
        fault = "names no receipt send";
    }
    else if ( r->outcomes++ > 0 )
    {
        fault = "a second outcome";
    }
    else if ( status == ROCKETNET_RECEIPT_STATUS_LOSS && r->type != 5 )
    {
        fault = "LOSS for a reliable type";
    }

    if ( fault )
    {
        char what[160];
        snprintf( what, sizeof( what ), "sender=%d serial=%u status=%d %s", sender, serial, (int) status, fault );
        rocketnet_dst_check_fail( st, "receipt_one_outcome", what );
        return 1;
    }

    return rocketnet_dst_check_receipt_acked_delivered( st, sender, serial, status );
}

// receipt_outcome_due, the type 5 side of RNReceipts::ReceiptProgress
// (standard/MESSAGES.md m7.4.3): a type 5 send gets its outcome at once when
// the budget or the transport refuses it, and otherwise by its packet's send
// time plus message_resend_delay_max_ms. Recorded after
// rocketnet_dst_check_receipt_sent, with the time the send returned. Only a
// type 5 send of this session counts: the table entry at its serial can be an
// earlier session's type 5 send when this send is of another type, since
// rocketnet_dst_check_receipt_sent records receipt types alone.

static inline void rocketnet_dst_check_receipt_due_from( struct rocketnet_dst_check_state_t * st, int sender,
                                                         uint64_t identity, uint32_t serial, int type, double now )
{
    if ( type != 5 || sender < 0 || sender >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
    {
        return;
    }
    struct rocketnet_dst_check_receipt_t * r = &st->receipt[sender][serial % ROCKETNET_DST_CHECK_SEQ_BITS];
    if ( r->serial != serial || r->type != 5 || r->identity != identity )
    {
        return;
    }
    r->sent_time = now;
    if ( st->receipt_due_tail[sender] - st->receipt_due_head[sender] == ROCKETNET_DST_CHECK_SEQ_BITS )
    {
        st->receipt_due_head[sender]++;     // the oldest's table entry is already reused by this serial's lap
    }
    st->receipt_due[sender][st->receipt_due_tail[sender] % ROCKETNET_DST_CHECK_SEQ_BITS] = serial;
    st->receipt_due_tail[sender]++;
}

// Called each step, after the receipts are read, for every sender with the
// serial its endpoint last issued (rn_endpoint_t serial, 0 again at each
// reset): while its session identity is live, its oldest type 5 send of that
// session with no outcome is not past the deadline (the receipt deadline
// plus slack for the update that writes its packet and the one that surfaces
// the outcome). A send of an earlier session or of a connection the endpoint
// has reset since (its serial went back: a session can keep its identity
// across a reset, and a reset's sends owe nothing), one with its outcome, and
// one whose table entry a later lap reused are done with.

static inline int rocketnet_dst_check_receipt_outcome_due( struct rocketnet_dst_check_state_t * st, int sender,
                                                           uint64_t identity, int live, uint32_t serial_now,
                                                           double now, double deadline )
{
    if ( sender < 0 || sender >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
    {
        return 0;
    }
    if ( serial_now < st->receipt_due_serial[sender] )
    {
        st->receipt_due_head[sender] = st->receipt_due_tail[sender];
    }
    st->receipt_due_serial[sender] = serial_now;
    if ( !live )
    {
        return 0;
    }
    while ( st->receipt_due_head[sender] != st->receipt_due_tail[sender] )
    {
        const uint32_t serial = st->receipt_due[sender][st->receipt_due_head[sender] % ROCKETNET_DST_CHECK_SEQ_BITS];
        const struct rocketnet_dst_check_receipt_t * r = &st->receipt[sender][serial % ROCKETNET_DST_CHECK_SEQ_BITS];
        if ( r->serial != serial || r->type != 5 || r->outcomes > 0 || r->identity != identity || serial > serial_now )
        {
            st->receipt_due_head[sender]++;
            continue;
        }
        if ( now <= r->sent_time + deadline )
        {
            return 0;
        }
        char what[160];
        snprintf( what, sizeof( what ), "sender=%d serial=%u sent_ms=%lld now_ms=%lld no outcome", sender, serial,
                  (long long) ( r->sent_time * 1000.0 ), (long long) ( now * 1000.0 ) );
        rocketnet_dst_check_fail( st, "receipt_outcome_due", what );
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
    {
        return;
    }

    if ( connected )
    {
        for ( int other = 0; other < ROCKETNET_DST_MAX_CLIENTS; other++ )
        {
            if ( other == slot || !st->slot_connected[other] )
            {
                continue;
            }

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
    {
        return;
    }

    const int terminal = state < 0;
    if ( terminal && !terminal_explained )
    {
        char what[160];
        snprintf( what, sizeof( what ), "client=%d terminal state=%d", client, state );
        rocketnet_dst_check_fail( st, "client_terminal", what );
    }

    st->client_state[client] = state;
}

// no_unscheduled_end (proofs/tla/ClientSession.tla and ServerSession.tla:
// a session ends by the silence timeout, the remote endpoint's goodbye or a cause; the
// DST wire's loss and duplication leave no cause): a client session ends only
// where a script ends it, a drawn churn, an opted-in reliable timeout or the
// outage-past schedule, each of which names the generation it ends
// (explained). Generation 0 is no session. `at_end` says the end is seen
// now; otherwise the client is about to start a new session over this one.

static inline void rocketnet_dst_check_session_end( struct rocketnet_dst_check_state_t * st, int client,
                                                    uint64_t generation, uint64_t explained, const char * how,
                                                    int at_end )
{
    if ( generation == 0 || generation == explained )
    {
        return;
    }

    char what[200];
    snprintf( what, sizeof( what ), "client=%d generation=%llu %s%s", client, (unsigned long long) generation, how,
              at_end ? "" : " after an end no script explains" );
    rocketnet_dst_check_fail( st, "no_unscheduled_end", what );
}

// no_unscheduled_end, the message layer's own kick: one the run did not opt
// in to (anything but RELIABLE_TIMED_OUT on a reliable-timeout run).

static inline void rocketnet_dst_check_unscheduled_kick( struct rocketnet_dst_check_state_t * st, const char * what )
{
    rocketnet_dst_check_fail( st, "no_unscheduled_end", what );
}

// Mirrors RNSimFaults::RestartIsFresh: a restarted client connects only under
// a generation never used before.

static inline void rocketnet_dst_check_generation( struct rocketnet_dst_check_state_t * st, int client,
                                                   uint64_t generation )
{
    if ( client < 0 || client >= ROCKETNET_DST_MAX_CLIENTS )
    {
        return;
    }

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
    {
        return;
    }

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
    {
        st->clock_back_seen = 1;
    }
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
// Only the streams of the session's current identity count: an ended
// connection's undelivered bytes went with its message object, as they do in
// the product, so they hold no new session back.

static inline uint64_t rocketnet_dst_check_outstanding( struct rocketnet_dst_check_state_t * st, int session,
                                                        uint64_t identity )
{
    uint64_t total = 0;
    for ( int channel = 0; channel < 9; channel++ )
    {
        for ( int type = 0; type < 8; type++ )
        {
            const struct rocketnet_dst_check_stream_t * s =
                &st->stream[rocketnet_dst_check_stream_index( session, channel, type )];
            if ( s->seen && s->identity == identity )
            {
                total += s->outstanding_bytes;
            }
        }
    }

    return total;
}

// The newest type 4 sequence this stream delivered (every sequence below the
// reliable base is delivered), for the failure line.

static inline uint32_t rocketnet_dst_check_type4_newest( const struct rocketnet_dst_check_stream_t * s )
{
    uint32_t newest = s->reliable_base > 0 ? s->reliable_base - 1 : 0;
    for ( uint32_t seq = s->reliable_base; seq < s->reliable_base + ROCKETNET_DST_CHECK_SEQ_BITS; seq++ )
    {
        if ( ( s->reliable_delivered[( seq % ROCKETNET_DST_CHECK_SEQ_BITS ) / 8] >> ( seq % 8 ) ) & 1 )
        {
            newest = seq;
        }
    }

    return newest;
}

// The newest reliable sequence of a stream that was sent at least the run's
// in-flight slack (in_flight_steps, or ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS
// when the run sets none) before the check's step, so is owed by now;
// UINT32_MAX when every send the ring holds is younger. A stream sends
// its sequences in increasing order, so every older sequence is owed too.

static inline uint32_t rocketnet_dst_check_owed_through( const struct rocketnet_dst_check_state_t * st,
                                                         const struct rocketnet_dst_check_stream_t * s )
{
    const uint32_t held = s->recent_count < ROCKETNET_DST_CHECK_RECENT_SENDS ? s->recent_count : ROCKETNET_DST_CHECK_RECENT_SENDS;
    const uint64_t in_flight = st->in_flight_steps ? st->in_flight_steps : ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS;
    uint32_t owed = UINT32_MAX;
    uint64_t owed_step = 0;
    for ( uint32_t i = 0; i < held; i++ )
    {
        if ( s->recent_step[i] + in_flight <= st->step &&
             ( owed == UINT32_MAX || s->recent_step[i] > owed_step ||
               ( s->recent_step[i] == owed_step && s->recent_seq[i] > owed ) ) )
        {
            owed = s->recent_seq[i];
            owed_step = s->recent_step[i];
        }
    }

    // a ring that wrapped holds only the newest sends: everything sent
    // before its oldest entry is older still
    const uint32_t oldest = s->recent_seq[s->recent_count % ROCKETNET_DST_CHECK_RECENT_SENDS];
    if ( owed == UINT32_MAX && s->recent_count > ROCKETNET_DST_CHECK_RECENT_SENDS && oldest > 0 )
    {
        owed = oldest - 1;
    }

    return owed;
}

// Marks where the quiet window begins on each stream. The reliable types owe
// every sequence sent before the last in-flight slack at the last step, the
// fault window's included (rocketnet_dst_check_end); the sequenced type 4
// owes the newest send of the quiet window.

static inline void rocketnet_dst_check_settle( struct rocketnet_dst_check_state_t * st )
{
    for ( int i = 0; i < ROCKETNET_DST_CHECK_MAX_STREAMS; i++ )
    {
        struct rocketnet_dst_check_stream_t * s = &st->stream[i];
        if ( !s->seen )
        {
            continue;
        }
        s->settled_seq = s->last_sent_seq;
        s->quiet_start_seq = s->last_sent_seq;
        s->settled = 1;
    }

    for ( int i = 0; i < ROCKETNET_DST_CHECK_MAX_SESSIONS * 9; i++ )
    {
        struct rocketnet_dst_check_sequenced_channel_t * c = &st->sequenced[i];
        if ( !c->seen )
        {
            continue;
        }
        c->settle_count = c->send_count;
        c->quiet_required_set = 0;
        c->quiet_served_set = 0;
    }
}

// Mirrors RNChannelTypes::EveryReliableAckedAndDelivered, RNChannel::
// EveryOrderedMessageDelivered and RNReceipts::ReceiptProgress (the liveness
// bound, section 4). Called at the last step: every reliable message sent on
// a stream whose session is still live is delivered. The harness passes the live flag per
// session (0 the server, i + 1 client i).

static inline void rocketnet_dst_check_end( struct rocketnet_dst_check_state_t * st, double /* time */,
                                            const int * session_live, const uint64_t * live_identity )
{
    for ( int session = 0; session < ROCKETNET_DST_CHECK_MAX_SESSIONS; session++ )
    {
        if ( !session_live[session] )
        {
            continue;
        }

        for ( int channel = 0; channel < 9; channel++ )
        {
            for ( int type = 0; type < 8; type++ )
            {
                struct rocketnet_dst_check_stream_t * s =
                    &st->stream[rocketnet_dst_check_stream_index( session, channel, type )];
                if ( !s->seen || s->identity != live_identity[session] )
                {
                    continue;
                }

                if ( type == 2 || type == 3 || type == 6 || type == 7 )
                {
                    // every message still unresolved is due once the quiet
                    // window has healed the wire, those sent in the fault
                    // window included; only a send of the last
                    // ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS may still be in flight
                    // (by step; never fewer than the earlier rule owed, every
                    // sequence but the newest 20)
                    const uint32_t by_count = s->last_sent_seq > 20 ? s->last_sent_seq - 20 : 0;
                    const uint32_t by_step = rocketnet_dst_check_owed_through( st, s );
                    const uint32_t settled = by_step == UINT32_MAX || by_step < by_count ? by_count : by_step;
                    for ( uint32_t seq = s->reliable_base;
                          seq <= settled && seq < s->reliable_base + ROCKETNET_DST_CHECK_SEQ_BITS; seq++ )
                    {
                        if ( rocketnet_dst_check_bit( st, s->reliable_sent, s->reliable_base, seq, "reliable_sent" ) &&
                             !rocketnet_dst_check_bit( st, s->reliable_delivered, s->reliable_base, seq, "reliable_delivered" ) )
                        {
                            uint64_t sent_step = UINT64_MAX;
                            for ( uint32_t r = 0; r < ROCKETNET_DST_CHECK_RECENT_SENDS && r < s->recent_count; r++ )
                            {
                                if ( s->recent_seq[r] == seq )
                                {
                                    sent_step = s->recent_step[r];
                                }
                            }
                            char what[192];
                            snprintf( what, sizeof( what ), "stream=%d seq=%u owed_through=%u sent_step=%lld last=%u at=%llu id=%llu",
                                      rocketnet_dst_check_stream_index( session, channel, type ), seq, settled,
                                      sent_step == UINT64_MAX ? -1LL : (long long) sent_step, s->last_sent_seq,
                                      (unsigned long long) s->last_sent_step, (unsigned long long) s->identity );
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
    // end is "server" or "clients" (dst_harness.h:1330-1331), and the report
    // is one counter name and two u64 (sdk/rocketnet_counters.cpp:151). The
    // precisions bound both pieces (16 + 2 + 237 + NUL = 256), so the
    // compiler can prove the line fits at every optimisation level.

    char report[240];
    if ( !rocketnet_counters_conserved( counters, report, sizeof( report ) ) )
    {
        char what[256];
        snprintf( what, sizeof( what ), "%.16s: %.237s", end, report );
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
                                                            int duplicates_enabled, int window_bounded )
{
    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        const uint64_t value = rocketnet_counter_value( counters, i );
        if ( value == 0 || rocketnet_counter_class( i ) != ROCKETNET_COUNTER_CLASS_REFUSAL )
        {
            continue;
        }

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
            // The outage schedules drop a dark client's handshake packets
            // in the window: a client that took its cookie before the window
            // and whose request reaches the server only after it answers late
            // in the same way. Explained only when the wire dropped a
            // handshake packet of a dark client on this run
            // (outage_handshake_seen, set by the harness).
            ( st->outage_handshake_seen && i == ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED ) ||
            // The same dropped handshake times the dark client's connect out;
            // it reconnects with a fresh cookie salt while the server's
            // challenge to the old request is still on the wire.
            ( st->outage_handshake_seen && i == ROCKETNET_COUNTER_CLIENT_COOKIE_SALT_MISMATCH ) ||
            i == ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_ALREADY_RECEIVED ||
            i == ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING ||
            // The message objects' rows (measured, seeds 1-2050 and every
            // schedule over 1-50): a full sent-packet map evicts its oldest
            // entry and keeps that entry's numbers queued for resend, so
            // nothing is lost; an unreliable message the send budget cannot
            // pay is dropped, which unreliable delivery allows. A no-room
            // send refusal is judged by refusals_agree, against the sends
            // the harness saw refused.
            i == ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ||
            i == ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ||
            i == ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ||
            i == ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ||
            // The outage schedules run the smallest numbering window (64)
            // and hold a dark sender's numbers unacknowledged, so the window
            // fills and new numbering waits: only on such a run.
            ( window_bounded && i == ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) ||
            // The send-fail schedule's transport refuses frames at finish:
            // an unreliable packet so refused is counted failed. Explained
            // only when the schedule refused a frame on this run.
            ( st->send_fail_seen && i == ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
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

// refusals_agree (standard/MESSAGES.md m1.7, proofs/tla/RNConnectionMemory.tla:
// a send with no room is refused and counted, never a kick): every send the
// harness offered and the library refused is one the message object counted
// as a no-room refusal (MESSAGE_SEND_QUEUE_FULL or MESSAGE_SEND_STORE_FULL)
// or as a send on a connection not yet CONNECTED (MESSAGE_SEND_NOT_CONNECTED:
// the burst schedule's batch can open in the step the client's transport
// connects, before its message connection has), and the object counted none
// the harness did not see refused.

static inline void rocketnet_dst_check_refusals_agree( struct rocketnet_dst_check_state_t * st, const char * end,
                                                       uint64_t refused, const struct rocketnet_counters_t * counters )
{
    const uint64_t counted = rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_SEND_QUEUE_FULL ) +
                             rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL ) +
                             rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_SEND_NOT_CONNECTED );
    if ( counted != refused )
    {
        char what[160];
        snprintf( what, sizeof( what ), "%s: the harness saw %llu sends refused, the library counted %llu", end,
                  (unsigned long long) refused, (unsigned long long) counted );
        rocketnet_dst_check_fail( st, "refusals_agree", what );
    }
}

// send_floor: a library that refused every send would pass every check that
// judges what was sent, so at least half of the reliable sends the harness
// offered each way are accepted. Only the outage schedules' dark sessions
// are refused at all, and only in their window.

static inline void rocketnet_dst_check_send_floor( struct rocketnet_dst_check_state_t * st, const char * end,
                                                   uint64_t offered, uint64_t accepted )
{
    if ( accepted * 2 < offered || ( offered > 0 && accepted == 0 ) )
    {
        char what[160];
        snprintf( what, sizeof( what ), "%s: %llu of %llu reliable sends accepted", end, (unsigned long long) accepted,
                  (unsigned long long) offered );
        rocketnet_dst_check_fail( st, "send_floor", what );
    }
}

// probe_paced (standard/MESSAGES.md m9.3; proofs/tla/RNResendTiming.tla
// ReportedDue, DueMeansWritten): a sender that has had no reliable number
// acknowledged for RN_PROBE_SECONDS resends only its oldest, once per resend
// delay, and asks its transport for a frame only for that probe or for a
// number it never sent (which m9.3 still allows, within the window). Over
// each 100 steps (one second) in which a dark session's sender (side 0 the
// client, 1 the server's slot) was held, the frames it started are at most
// one per its shortest resend delay in them, plus one for alignment, plus
// one per number first sent in them. A sender whose remote endpoint's packets still
// reach it (one-way outage) also answers each of the remote endpoint's packets, so the
// remote endpoint's shortest resend delay adds its own term, and first_sends counts the
// remote endpoint's first sends too; remote_delay 0 means nothing reaches the sender.

static inline void rocketnet_dst_check_probe_paced( struct rocketnet_dst_check_state_t * st, int client, int side,
                                                    uint64_t frames, double resend_delay, double remote_delay,
                                                    uint64_t first_sends )
{
    const double delay = resend_delay > 0.010 ? resend_delay : 0.010;
    uint64_t bound = (uint64_t) ( 1.0 / delay ) + 1 + first_sends;
    if ( remote_delay > 0.0 )
    {
        const double remote = remote_delay > 0.010 ? remote_delay : 0.010;
        bound += (uint64_t) ( 1.0 / remote ) + 1;
    }
    if ( frames > bound )
    {
        char what[224];
        snprintf( what, sizeof( what ),
                  "client=%d %s started %llu frames in a held second, resend delay %.3f s (remote %.3f s) and %llu first sends allow %llu",
                  client, side ? "server" : "client", (unsigned long long) frames, resend_delay, remote_delay,
                  (unsigned long long) first_sends, (unsigned long long) bound );
        rocketnet_dst_check_fail( st, "probe_paced", what );
    }
}

// bright_unaffected (the per-client isolation of the bad-network design, P):
// a client the outage leaves bright sees no longer gap between two
// deliveries of its session inside the window, in either direction
// (0 to the server, 1 to the client), than the same seed's control run
// (the same draws with no client dark) saw in the same window, plus a slack
// of `slack` steps. The two runs share every fault draw but not every packet
// (the dark clients' traffic moves sequence numbers and send order), so a
// bright gap moves a little either way; the slack is one longest resend
// delay, the most one lost and resent packet adds to a gap.

static inline void rocketnet_dst_check_bright_unaffected( struct rocketnet_dst_check_state_t * st, int client,
                                                          int direction, uint64_t control, uint64_t inside,
                                                          uint64_t slack )
{
    if ( inside > control + slack )
    {
        char what[192];
        snprintf( what, sizeof( what ), "client=%d %s: a gap of %llu steps in the window, %llu without the outage (slack %llu)",
                  client, direction ? "to the client" : "to the server", (unsigned long long) inside,
                  (unsigned long long) control, (unsigned long long) slack );
        rocketnet_dst_check_fail( st, "bright_unaffected", what );
    }
}

// outage_end (proofs/tla/ClientSession.tla and ServerSession.tla: the
// silence timeout): a session dark both ways for longer than T ends on each
// end by the timeout reason, no earlier than T after that end last heard
// the other (side 0 the client's state, 1 the server slot's end reason).

static inline void rocketnet_dst_check_outage_end( struct rocketnet_dst_check_state_t * st, int client, int side,
                                                   int reason, int timeout_reason, double silent, double timeout )
{
    if ( reason != timeout_reason || silent < timeout - 1.0e-6 )
    {
        char what[160];
        snprintf( what, sizeof( what ), "client=%d %s ended reason=%d after %.3f s silent (timeout reason %d at %.0f s)",
                  client, side ? "server" : "client", reason, silent, timeout_reason, timeout );
        rocketnet_dst_check_fail( st, "outage_end", what );
    }
}

// acked_means_opened's ledger. Packet ids an endpoint issues strictly
// increase (an aborted packet's id is skipped, never reused); an endpoint
// whose counter restarts (a client's new session) clears its ledger first.

static inline int rocketnet_dst_check_ledger_bit( const uint8_t * bits, uint64_t packet_id )
{
    const uint32_t at = (uint32_t) ( packet_id % ROCKETNET_DST_CHECK_LEDGER_PACKETS );
    return ( bits[at >> 3] >> ( at & 7 ) ) & 1;
}

static inline void rocketnet_dst_check_ledger_write( uint8_t * bits, uint64_t packet_id, int value )
{
    const uint32_t at = (uint32_t) ( packet_id % ROCKETNET_DST_CHECK_LEDGER_PACKETS );
    if ( value )
    {
        bits[at >> 3] |= (uint8_t) ( 1u << ( at & 7 ) );
    }
    else
    {
        bits[at >> 3] &= ( uint8_t ) ~( 1u << ( at & 7 ) );
    }
}

static inline void rocketnet_dst_check_endpoint_reset( struct rocketnet_dst_check_state_t * st, int endpoint )
{
    st->issued_high[endpoint] = 0;
    memset( st->issued_bits[endpoint], 0, sizeof( st->issued_bits[endpoint] ) );
    memset( st->acked_bits[endpoint], 0, sizeof( st->acked_bits[endpoint] ) );
}

// Records a packet id an endpoint sealed. Every id between the endpoint's
// newest and this one was never issued, so its slots are cleared.

static inline void rocketnet_dst_check_issued( struct rocketnet_dst_check_state_t * st, int endpoint, uint64_t packet_id )
{
    const uint64_t high = st->issued_high[endpoint];
    if ( packet_id <= high )
    {
        char what[160];
        snprintf( what, sizeof( what ), "endpoint=%d packet_id=%llu issued at or below %llu",
                  endpoint, (unsigned long long) packet_id, (unsigned long long) high );
        rocketnet_dst_check_fail( st, "acked_means_opened", what );
        return;
    }

    if ( packet_id - high > ROCKETNET_DST_CHECK_LEDGER_PACKETS )
    {
        memset( st->issued_bits[endpoint], 0, sizeof( st->issued_bits[endpoint] ) );
        memset( st->acked_bits[endpoint], 0, sizeof( st->acked_bits[endpoint] ) );
    }
    else
    {
        for ( uint64_t id = high + 1; id < packet_id; id++ )
        {
            rocketnet_dst_check_ledger_write( st->issued_bits[endpoint], id, 0 );
            rocketnet_dst_check_ledger_write( st->acked_bits[endpoint], id, 0 );
        }
    }

    rocketnet_dst_check_ledger_write( st->issued_bits[endpoint], packet_id, 1 );
    rocketnet_dst_check_ledger_write( st->acked_bits[endpoint], packet_id, 0 );
    st->issued_high[endpoint] = packet_id;
}

// Mirrors RNReceipts::AckedMeansPacketOpened: a link ack names a packet the
// acked endpoint itself issued, and names it once. The join key the seam
// surfaces is the packet id recorded when the frame was sealed.

static inline void rocketnet_dst_check_ack_id( struct rocketnet_dst_check_state_t * st, int endpoint, uint64_t packet_id )
{
    const uint64_t high = st->issued_high[endpoint];
    const char * fault = NULL;
    if ( packet_id == 0 || packet_id > high )
    {
        fault = "acked but never issued";
    }
    else if ( high - packet_id >= ROCKETNET_DST_CHECK_LEDGER_PACKETS )
    {
        fault = "acked older than the ledger";
    }
    else if ( !rocketnet_dst_check_ledger_bit( st->issued_bits[endpoint], packet_id ) )
    {
        fault = "acked but never issued";
    }
    else if ( rocketnet_dst_check_ledger_bit( st->acked_bits[endpoint], packet_id ) )
    {
        fault = "acked twice";
    }

    if ( fault )
    {
        char what[160];
        snprintf( what, sizeof( what ), "endpoint=%d packet_id=%llu %s (newest issued %llu)",
                  endpoint, (unsigned long long) packet_id, fault, (unsigned long long) high );
        rocketnet_dst_check_fail( st, "acked_means_opened", what );
        return;
    }

    rocketnet_dst_check_ledger_write( st->acked_bits[endpoint], packet_id, 1 );
}

#endif // ROCKETNET_TESTS_DST_CHECKS_H
