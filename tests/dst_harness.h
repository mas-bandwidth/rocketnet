/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The deterministic simulation harness core (docs/DST.md sections 1, 2 and
    6): the world lifted from soak_sim, the N-client rn_message_link_t seam,
    the named substreams, the payload scheme, the trace hash and the seeded
    single-process run. Time is an argument that starts at 1000.0 and is
    computed from the step; nothing here reads a clock, opens a socket or
    starts a thread.
*/

#pragma once

#ifndef ROCKETNET_TESTS_DST_HARNESS_H
#define ROCKETNET_TESTS_DST_HARNESS_H

#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_message_object.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_ack.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_wire.h"
#if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif // #if defined( ROCKETNET_USE_SYSTEM_SODIUM )
#include "dst_checks.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROCKETNET_DST_MAX_CLIENTS            16
#define ROCKETNET_DST_NUM_CHANNELS           9
// One endpoint's frames for a step: the message layer's packets_per_update
// (256), so the burst schedule can stage and send a whole burst in one update.
#define ROCKETNET_DST_MAX_PACKETS            256
// The delivery ring of one endpoint. It holds what one update delivers, and
// the burst schedule releases its whole window in one update: the ring is as
// wide as the window the schedule can hold (ROCKETNET_DST_BURST_CAPACITY).
#define ROCKETNET_DST_MAX_PAYLOADS           1024
// The receipts the harness reads from one endpoint's list per step; a
// longer list fails the run (receipt_list) rather than going unjudged.
#define ROCKETNET_DST_RECEIPTS               4096
#define ROCKETNET_DST_MAX_STREAMS            8

// The fault kinds. A kind that is off makes no draw. Every name here is
// applied by the world below: a fault the world cannot apply is not named
// (docs/DST.md, "Fault kinds").
#define ROCKETNET_DST_FAULT_LOSS             0x01
#define ROCKETNET_DST_FAULT_DUP              0x02
#define ROCKETNET_DST_FAULT_JITTER           0x04
#define ROCKETNET_DST_FAULT_ALL              0x07

// The fault schedules (docs/DST.md, "Fault schedules"): one per run, chosen
// with --schedule, each drawing only from its own substream
// ("schedule.<name>"), so a run with no schedule draws and traces exactly
// as before and one schedule never moves another fault's draws.
#define ROCKETNET_DST_SCHEDULE_NONE          0
#define ROCKETNET_DST_SCHEDULE_CLOCK_BACK    1
#define ROCKETNET_DST_SCHEDULE_SEQ_GAP       2
#define ROCKETNET_DST_SCHEDULE_SEQ_WRAP_31   3
#define ROCKETNET_DST_SCHEDULE_SEQ_WRAP_32   4
#define ROCKETNET_DST_SCHEDULE_BURST         5
#define ROCKETNET_DST_SCHEDULE_DUP_STORM     6
#define ROCKETNET_DST_SCHEDULE_OUTAGE        7
#define ROCKETNET_DST_SCHEDULE_OUTAGE_PAST   8
#define ROCKETNET_DST_SCHEDULE_SEND_FAIL     9
#define ROCKETNET_DST_NUM_SCHEDULES          10

// The connect token's timeout_seconds, T: a session silent for T ends.
#define ROCKETNET_DST_TOKEN_TIMEOUT_SECONDS  5

// outage and outage-past: a drawn subset of the clients (at least one left
// bright) goes dark in a drawn direction for T - 1 s (outage, survived) or
// T + 1 s (outage-past, both ends time out). The wire drops a dark
// direction's packets. Through the run the dark clients send and are sent
// messages of at most one packet (no blocks, so a held sender's one probe
// is one frame). In the window they are offered only reliable messages,
// OUTAGE_OFFERS draws a step each way, with no outstanding cap.
//
// The schedule's send side is small so that the library's no-room refusal
// is reached in the window: a connection's send memory is the configured
// side (16 messages, 12288 bytes) plus a resend reserve of its send budget
// times message_resend_delay_max_ms (rn_region_reserve_sizes), so the delay
// cap is 250 ms (the server's 10000 kbps then reserves 312500 bytes, not
// 2.5 MB). The numbering window is the smallest (64), so a dark sender has
// numbered all it can within the window's first second and is left with
// only its probe; max_message_bytes is the most 64 fragments carry.
#define ROCKETNET_DST_OUTAGE_STEPS           ( ( ROCKETNET_DST_TOKEN_TIMEOUT_SECONDS - 1 ) * 100 )
#define ROCKETNET_DST_OUTAGE_PAST_STEPS      ( ( ROCKETNET_DST_TOKEN_TIMEOUT_SECONDS + 1 ) * 100 )
#define ROCKETNET_DST_OUTAGE_C2S             1
#define ROCKETNET_DST_OUTAGE_S2C             2
#define ROCKETNET_DST_OUTAGE_BOTH            3
#define ROCKETNET_DST_OUTAGE_SEND_BYTES      12288
#define ROCKETNET_DST_OUTAGE_SEND_MESSAGES   16
#define ROCKETNET_DST_OUTAGE_RESEND_MAX_MS   250
#define ROCKETNET_DST_OUTAGE_WINDOW          64
#define ROCKETNET_DST_OUTAGE_MAX_MESSAGE     ( ROCKETNET_DST_OUTAGE_WINDOW * 1152 )
#define ROCKETNET_DST_OUTAGE_MAX_BYTES       1152
#define ROCKETNET_DST_OUTAGE_OFFERS          16
// liveness_delivered's in-flight slack under this send side: a reliable
// send queues behind at most the outstanding cap plus one message (as in
// ROCKETNET_DST_CHECK_BUDGET_STEPS), 81920 bytes, which is more numbers
// (one per 1152-byte packet, 72) than the window holds, so it waits up to
// two windows' worth of round trips, each two one-way wire bounds
// (ROCKETNET_DST_CHECK_WIRE_STEPS), on top of the default slack. Derived
// from the constants, not measured: 96 + 2 x 60 = 216 steps.
#define ROCKETNET_DST_OUTAGE_WINDOW_ROUNDS \
    ( ( ROCKETNET_DST_CHECK_OUTSTANDING_CAP + ROCKETNET_DST_CHECK_MAX_MESSAGE_BYTES + ROCKETNET_DST_OUTAGE_MAX_MESSAGE - 1 ) / \
      ROCKETNET_DST_OUTAGE_MAX_MESSAGE )
#define ROCKETNET_DST_OUTAGE_IN_FLIGHT_STEPS \
    ( ROCKETNET_DST_CHECK_IN_FLIGHT_STEPS + ROCKETNET_DST_OUTAGE_WINDOW_ROUNDS * 2 * ROCKETNET_DST_CHECK_WIRE_STEPS )
// probe_paced judges a sender over each 100 steps (one second) it was held
#define ROCKETNET_DST_OUTAGE_SECOND_STEPS    100

// clock-back: the step back is a whole number of milliseconds from one
// frame (10 ms) to 5 s; the skew check's bound for the stepped endpoint is
// that maximum plus 1 ms for the rounding of the double sum.
#define ROCKETNET_DST_CLOCK_BACK_MIN_MS      10
#define ROCKETNET_DST_CLOCK_BACK_MAX_MS      5000
#define ROCKETNET_DST_SKEW_BOUND_SECONDS     0.45

// seq-gap: the send sequence skips past the replay history (ROCKETNET_HISTORY_PACKETS).
#define ROCKETNET_DST_SEQ_GAP_MIN            ( ROCKETNET_HISTORY_PACKETS + 1 )
#define ROCKETNET_DST_SEQ_GAP_MAX            ( 2 * ROCKETNET_HISTORY_PACKETS )

// seq-wrap-31 and seq-wrap-32: every session starts this far below 2^31 or
// 2^32, so each session's send sequence crosses the boundary early in it.
#define ROCKETNET_DST_SEQ_WRAP_BELOW         256

// burst: the held window, in steps, and the most packets the harness holds.
#define ROCKETNET_DST_BURST_MIN_STEPS        100
#define ROCKETNET_DST_BURST_MAX_STEPS        300
#define ROCKETNET_DST_BURST_CAPACITY         1024
// burst: the messages one endpoint queues in its one update. Each is
// unreliable and close to one packet's payload, so it takes exactly one
// packet and a drawn 300 fill the update's 256 packets and spill the rest
// into the next step. Unreliable sends are never retransmitted, so the held
// window carries the drawn burst and not a resend storm.
#define ROCKETNET_DST_BURST_MESSAGES         300
#define ROCKETNET_DST_BURST_MIN_BYTES        1250
#define ROCKETNET_DST_BURST_MAX_BYTES        1380

// dup-storm: the window in steps, the copies of each packet, and the most
// extra delay one copy draws (so the copies arrive reordered).
#define ROCKETNET_DST_DUP_STORM_MIN_STEPS    50
#define ROCKETNET_DST_DUP_STORM_MAX_STEPS    200
#define ROCKETNET_DST_DUP_STORM_MIN_COPIES   2
#define ROCKETNET_DST_DUP_STORM_MAX_COPIES   5
#define ROCKETNET_DST_DUP_STORM_MAX_DELAY_MS 50

// send-fail: the window in steps, and one frame in ONE_IN that the drawn
// side's transport refuses at finish (finish_packet returns 0: the frame
// never leaves, and the message layer takes its packet as failed).
#define ROCKETNET_DST_SEND_FAIL_MIN_STEPS    50
#define ROCKETNET_DST_SEND_FAIL_MAX_STEPS    200
#define ROCKETNET_DST_SEND_FAIL_ONE_IN       3

static inline const char * rocketnet_dst_schedule_name( int schedule )
{
    static const char * const names[ROCKETNET_DST_NUM_SCHEDULES] = {
        "none",
        "clock-back",
        "seq-gap",
        "seq-wrap-31",
        "seq-wrap-32",
        "burst",
        "dup-storm",
        "outage",
        "outage-past",
        "send-fail",
    };
    if ( schedule < 0 || schedule >= ROCKETNET_DST_NUM_SCHEDULES )
    {
        return "none";
    }

    return names[schedule];
}

static inline int rocketnet_dst_schedule_parse( const char * name, int * schedule )
{
    for ( int i = 0; i < ROCKETNET_DST_NUM_SCHEDULES; i++ )
    {
        if ( strcmp( name, rocketnet_dst_schedule_name( i ) ) == 0 )
        {
            *schedule = i;
            return 1;
        }
    }

    return 0;
}

// The session sequence start a seq-wrap schedule needs from
// ROCKETNET_DEV_SESSION_SEQUENCE_START, 0 for every other schedule.

static inline uint64_t rocketnet_dst_schedule_sequence_start( int schedule )
{
    if ( schedule == ROCKETNET_DST_SCHEDULE_SEQ_WRAP_31 )
    {
        return ( 1ULL << 31 ) - ROCKETNET_DST_SEQ_WRAP_BELOW;
    }
    if ( schedule == ROCKETNET_DST_SCHEDULE_SEQ_WRAP_32 )
    {
        return ( 1ULL << 32 ) - ROCKETNET_DST_SEQ_WRAP_BELOW;
    }

    return 0;
}

#define ROCKETNET_DST_SHAPE_FULL             0
#define ROCKETNET_DST_SHAPE_PUSH             1

// FNV-1a 64 over a byte range.

static inline uint64_t rocketnet_dst_fnv1a64( const uint8_t * data, size_t bytes )
{
    uint64_t hash = 0xcbf29ce484222325ULL;

    for ( size_t i = 0; i < bytes; i++ )
    {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }

    return hash;
}

// splitmix64's finaliser.

static inline uint64_t rocketnet_dst_splitmix64_finalise( uint64_t z )
{
    z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
    z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
    return z ^ ( z >> 31 );
}

// A substream: its own 64-bit state, drawn from the master seed and the
// substream's name.
struct rocketnet_dst_substream_t
{
    uint64_t state;
};

static inline void rocketnet_dst_substream_init( struct rocketnet_dst_substream_t * stream,
                                                 uint64_t master_seed, const char * name )
{
    const uint64_t name_hash = rocketnet_dst_fnv1a64( (const uint8_t *) name, strlen( name ) );
    stream->state = rocketnet_dst_splitmix64_finalise( master_seed ^ name_hash );
}

static inline uint64_t rocketnet_dst_substream_draw( struct rocketnet_dst_substream_t * stream )
{
    stream->state += 0x9E3779B97F4A7C15ULL;
    return rocketnet_dst_splitmix64_finalise( stream->state );
}

static inline uint64_t rocketnet_dst_substream_uniform( struct rocketnet_dst_substream_t * stream,
                                                        uint64_t lo, uint64_t hi )
{
    const uint64_t draw = rocketnet_dst_substream_draw( stream );
    return lo + draw % ( hi - lo + 1 );
}

static inline double rocketnet_dst_substream_real( struct rocketnet_dst_substream_t * stream )
{
    const uint64_t draw = rocketnet_dst_substream_draw( stream );
    return (double) ( draw >> 11 ) * ( 1.0 / 9007199254740992.0 );   // 2^-53, exact
}

// The trace accumulator: FNV-1a 64 over fixed-width records appended in step
// order.
struct rocketnet_dst_trace_t
{
    uint64_t hash;
};

static inline void rocketnet_dst_trace_init( struct rocketnet_dst_trace_t * trace )
{
    trace->hash = 0xcbf29ce484222325ULL;
}

static inline void rocketnet_dst_trace_u64( struct rocketnet_dst_trace_t * trace, uint64_t v )
{
    for ( int i = 0; i < 8; i++ )
    {
        trace->hash ^= (uint8_t) ( v >> ( 8 * i ) );
        trace->hash *= 0x100000001b3ULL;
    }
}

static inline void rocketnet_dst_trace_record( struct rocketnet_dst_trace_t * trace, uint64_t a, uint64_t b,
                                               uint64_t c, uint64_t d, uint64_t e )
{
    rocketnet_dst_trace_u64( trace, a );
    rocketnet_dst_trace_u64( trace, b );
    rocketnet_dst_trace_u64( trace, c );
    rocketnet_dst_trace_u64( trace, d );
    rocketnet_dst_trace_u64( trace, e );
}

// The one step bound. Every (int) k in this harness (connect, pause, churn and
// quiet-window steps, and k + 50) is in range by construction because k and
// fault_steps never exceed it: INT_MAX less room for the 1000-step quiet slack.
#define ROCKETNET_DST_MAX_STEPS ( (uint64_t) INT_MAX - 1000 )

static inline int rocketnet_dst_steps_valid( uint64_t steps )
{
    return steps >= 1 && steps <= ROCKETNET_DST_MAX_STEPS;
}

struct rocketnet_dst_options_t
{
    uint64_t steps;             // total steps this seed runs
    uint64_t fault_steps;       // the quiet window starts at this step
    int shape;
    int clients;                // 0: draw from the shape's range
    uint32_t faults;
    int force_fail_step;        // a test's forced failure, -1 for none
    int schedule;               // ROCKETNET_DST_SCHEDULE_*, 0 for none
    int witness;                // ROCKETNET_DST_WITNESS_*, 0 for none: a unit case's planted defect
    int reliable_timeout_ms;    // 0: the default reliable_message_timeout_ms; else that accepted value
    int outage_control;         // internal: the outage control run (no client dark) bright_unaffected compares with
};

// Reversed witnesses: a unit case plants one defect in what the checks
// observe and requires the run to fail naming the check that guards it.
// ORDERED_SWAP holds the run's first ordered (type 3) delivery and passes it
// to the checks after the next ordered delivery on the same stream.
// FOREIGN_ACK has a client's ack name the lowest packet id the server issued
// and the client did not.

#define ROCKETNET_DST_WITNESS_NONE          0
#define ROCKETNET_DST_WITNESS_ORDERED_SWAP  1
#define ROCKETNET_DST_WITNESS_FOREIGN_ACK   2
// OUTAGE_BRIGHT (outage schedules): the wire drops one bright client's
// packets in the window too, while the checks still judge it as bright
#define ROCKETNET_DST_WITNESS_OUTAGE_BRIGHT 3

struct rocketnet_dst_result_t
{
    int passed;
    uint64_t trace_hash;
    uint64_t seed;
    uint64_t step;
    char check[64];
    char detail[256];

    // what the schedule did, so a unit case can see that it fired
    uint64_t schedule_fired;        // times the schedule's fault was applied
    uint64_t clock_back_seen;       // endpoint updates given a time below the one before
    uint64_t max_sequence_jump;     // largest one-step rise of a receiver's highest opened sequence
    uint64_t sequence_gap;          // seq-gap: the gap applied to the send sequence, 0 if it never fired
    uint64_t burst_sent;            // packets the burst endpoint put on the wire in its one update
    uint64_t burst_received;        // packets the burst endpoint took in its one release step
    uint64_t duplicate_payloads;    // payloads a receiver took twice (the library's duplicate refusals)
    uint64_t server_reliable_timeouts;  // server connections the message layer ended with RELIABLE_TIMED_OUT
    uint64_t client_reliable_timeouts;  // client connections the message layer ended with RELIABLE_TIMED_OUT
    uint64_t message_kicks;             // message-layer kicks for any reason
    uint64_t budget_aborts;             // frames started and then aborted for a reliable budget wait
    uint64_t bright_gap[ROCKETNET_DST_MAX_CLIENTS * 2]; // outage: each client's longest window gap, [client * 2 + direction]
    uint64_t session_ends;              // client session ends of any kind, explained or not
    uint64_t refused;                   // harness sends the library refused, both directions
    uint64_t accepted_reliable;         // reliable harness sends the library accepted, both directions
    uint64_t frames_max;                // outage: the most frames one held sender started in a judged second
    uint64_t seconds_judged;            // outage: the seconds probe_paced judged
    uint64_t outage_ends;               // outage-past: the dark session ends judged, both ends counted
};

struct rocketnet_dst_world_t;

typedef struct rocketnet_dst_endpoint_t
{
    struct rocketnet_dst_world_t * world;
    int is_server;
    int client_index;
    int pending_frame;
    uint64_t next_packet_id;
    int outgoing_count;
    uint8_t frame[ROCKETNET_DST_MAX_PACKETS][ROCKETNET_FRAME_STRIDE];
    int frame_bytes[ROCKETNET_DST_MAX_PACKETS];
    int frame_receiver[ROCKETNET_DST_MAX_PACKETS];
    uint8_t ( *payload )[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    uint8_t * payload_ptr[ROCKETNET_DST_MAX_PAYLOADS];
    int payload_bytes[ROCKETNET_DST_MAX_PAYLOADS];
    int payload_client_index[ROCKETNET_DST_MAX_PAYLOADS];

    // the session sequence and the connection each payload arrived on, as
    // the real transports keep them beside their batch, for the message
    // layer's accept_payload and payload_connection
    uint64_t payload_sequence[ROCKETNET_DST_MAX_PAYLOADS];
    uint64_t payload_connection_id[ROCKETNET_DST_MAX_PAYLOADS];
    int payload_count;
} rocketnet_dst_endpoint_t;

typedef struct rocketnet_dst_client_t
{
    struct rocketnet_client_protocol_t protocol;
    rocketnet_dst_endpoint_t endpoint;
    struct rocketnet_message_client_t * message;
    struct rocketnet_address_t address;
    struct rocketnet_dst_substream_t schedule;
    struct rocketnet_dst_substream_t traffic;
    uint64_t generation;
    int next_connect_step;
    int churn_step;
    int pause_until;
    int last_state;
    int last_server_slot;
    uint32_t sequence[ROCKETNET_DST_MAX_STREAMS][2];
    uint32_t epoch[2];
    unsigned mixed[2];
    uint64_t wrap_generation;   // seq-wrap: the last session seen crossing the boundary

    // no_unscheduled_end: the session (generation) whose end a script
    // explains (a drawn churn, an opted-in reliable timeout, outage-past),
    // and the transport state the last step ended in
    uint64_t explained_generation;
    uint64_t connect_step;              // the step the current generation's connect began
    int end_state;
} rocketnet_dst_client_t;

// One schedule's drawn plan and what it observed. Endpoint e is 0 for the
// server and i + 1 for client i.
struct rocketnet_dst_schedule_t
{
    int kind;
    struct rocketnet_dst_substream_t draws;
    uint64_t start_step;            // the step the fault fires (or its window opens)
    uint64_t end_step;              // the window's end, exclusive (burst, dup-storm)
    int endpoint;                   // clock-back: the endpoint; burst, seq-gap: the client
    int side;                       // seq-gap, send-fail: 0 the client's sends, 1 the server's
    int pending;                    // seq-gap: waiting for the drawn client's session; burst: waiting for it to connect
    uint64_t amount;                // clock-back: ms; seq-gap: sequences skipped
    int burst_slot;                 // burst: the drawn client's server slot when it fired
    double clock_offset[ROCKETNET_DST_MAX_CLIENTS + 1];
    double clock_last[ROCKETNET_DST_MAX_CLIENTS + 1];
    int clock_seen[ROCKETNET_DST_MAX_CLIENTS + 1];
    uint64_t replay_last[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint64_t replay_identity[ROCKETNET_DST_CHECK_MAX_SESSIONS];
    uint8_t ( *held )[ROCKETNET_RECEIVE_CEILING];    // burst: the client's packets the server has not yet received
    int held_bytes[ROCKETNET_DST_BURST_CAPACITY];
    int held_count;
    uint64_t fired;
    uint64_t clock_back_seen;
    uint64_t max_sequence_jump;
    uint64_t burst_sent;
    uint64_t burst_received;

    // outage, outage-past: the dark clients (bit i, client i), the dark
    // direction (ROCKETNET_DST_OUTAGE_*), and per client the session it held
    // on both ends when the window opened (generation 0: none), its slot,
    // whether each end still holds it, and each end's last receive time
    // while it did
    uint32_t dark_mask;
    int direction;
    int witness_client;                     // OUTAGE_BRIGHT: the bright client also dropped, or -1
    uint64_t dark_generation[ROCKETNET_DST_MAX_CLIENTS];
    int dark_slot[ROCKETNET_DST_MAX_CLIENTS];
    int dark_client_live[ROCKETNET_DST_MAX_CLIENTS];
    int dark_server_live[ROCKETNET_DST_MAX_CLIENTS];
    double dark_client_heard[ROCKETNET_DST_MAX_CLIENTS];
    double dark_server_heard[ROCKETNET_DST_MAX_CLIENTS];
    int dark_slot_reused[ROCKETNET_DST_MAX_CLIENTS];

    // probe_paced: whether each dark session's two senders are held (no ack
    // progress for RN_PROBE_SECONDS, so only the probe may be resent), the
    // held steps counted toward the current judged second, the frames each
    // started in those steps, and the shortest resend delay each had in them
    int probe_held[ROCKETNET_DST_MAX_CLIENTS][2];
    uint64_t held_steps[ROCKETNET_DST_MAX_CLIENTS][2];
    uint64_t frames[ROCKETNET_DST_MAX_CLIENTS][2];
    double resend_delay_low[ROCKETNET_DST_MAX_CLIENTS][2];
    double remote_delay_low[ROCKETNET_DST_MAX_CLIENTS][2];   // the remote endpoint's, while it reaches the sender
    uint64_t first_sends[ROCKETNET_DST_MAX_CLIENTS][2];    // numbers first sent in the counted steps
    uint64_t numbers_sent[ROCKETNET_DST_MAX_CLIENTS][2];   // each side's numbers written at least once, at the last step
    uint64_t frames_max;                    // the most frames one sender started in a judged second
    uint64_t seconds_judged;

    // bright_unaffected: per client and direction ([0] to the server, [1]
    // to the client), the step and session of the last delivery, the
    // longest gap between two deliveries of one session before the window,
    // and the longest inside it
    uint64_t delivery_step[ROCKETNET_DST_MAX_CLIENTS][2];
    uint64_t delivery_generation[ROCKETNET_DST_MAX_CLIENTS][2];
    uint64_t gap_before[ROCKETNET_DST_MAX_CLIENTS][2];
    uint64_t gap_inside[ROCKETNET_DST_MAX_CLIENTS][2];
    uint64_t outage_ends;                   // outage-past: dark session ends judged (both ends counted)
};

// the delivery ROCKETNET_DST_WITNESS_ORDERED_SWAP holds back

struct rocketnet_dst_held_delivery_t
{
    int session, channel, type, incarnation, expected_incarnation;
    uint32_t sequence, generation, epoch, expected_generation;
    uint64_t identity;
    int bytes;
};

typedef struct rocketnet_dst_world_t
{
    struct rocketnet_network_simulator_t * sim;
    struct rocketnet_counters_t server_counters;
    struct rocketnet_counters_t client_counters;
    struct rocketnet_server_protocol_t server;
    uint64_t server_cookie_reply_sources[64];   // a harness-sized bitmap: 4096 bits

    // the server protocol's per-connection storage the server transport
    // allocates (rocketnet_server_transport_create): the id of each slot's
    // connection, the connection of each acked batch entry, and each slot's
    // end reason
    uint64_t server_connection_ids[ROCKETNET_DST_MAX_CLIENTS];
    uint64_t server_acked_connection_ids[ROCKETNET_DST_MAX_CLIENTS * ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    int server_end_reasons[ROCKETNET_DST_MAX_CLIENTS];
    // each slot's receive allowance at the default rate, as a created server
    // holds it: a correct client that it refuses is an unexplained refusal
    struct rocketnet_server_receive_allowance_t server_receive_allowances[ROCKETNET_DST_MAX_CLIENTS];
    rocketnet_dst_endpoint_t server_endpoint;
    struct rocketnet_message_server_t * message_server;
    struct rocketnet_message_config_t message_config;
    struct rocketnet_address_t server_address;
    rocketnet_dst_client_t client[ROCKETNET_DST_MAX_CLIENTS];
    int num_clients;
    uint64_t step;
    double now;
    uint64_t unix_ts;
    struct rocketnet_dst_trace_t trace;
    uint64_t seed;
    struct rocketnet_dst_substream_t traffic_server;
    struct rocketnet_dst_substream_t link;
    struct rocketnet_dst_substream_t fault_loss;
    struct rocketnet_dst_substream_t fault_dup;
    struct rocketnet_dst_substream_t fault_jitter;
    uint32_t faults;
    double base_latency;
    double base_jitter;
    float loss_percent;
    float dup_percent;
    int quiet_applied;
    int server_incarnation;
    uint64_t fault_steps;
    struct rocketnet_dst_schedule_t schedule;
    uint64_t reliable_timeouts[2];  // the message layer's RELIABLE_TIMED_OUT kicks: server, clients
    int reliable_timeout_run;       // the run opted in to reliable_message_timeout_ms

    // the message objects' own counters: [0] the server's, [1] every client
    // object's summed (a client's object is rebuilt per session, so each is
    // added here before it is destroyed); the run's end adds the live ones
    struct rocketnet_counters_t message_counters[2];

    // the harness's own sends, by sender ([0] clients, [1] the server): those
    // offered to the library, those it accepted, those it refused, and the
    // reliable ones offered and accepted (send_floor, refusals_agree)
    uint64_t offered[2];
    uint64_t accepted[2];
    uint64_t refused[2];
    uint64_t offered_reliable[2];
    uint64_t accepted_reliable[2];
    uint64_t message_kicks;         // every message-layer kick, any reason
    uint64_t budget_aborts;         // frames aborted because the send budget could not pay a due reliable packet
    int outage_control;             // the outage control run: the same draws with no client dark
    const uint64_t * bright_control; // bright_unaffected: the control run's longest window gap, [client * 2 + direction]
    uint64_t session_ends;          // every client session end seen, explained or not
    struct rocketnet_dst_check_state_t check;
    int witness;
    int witness_state;                  // 0 armed, 1 holding (ORDERED_SWAP), 2 fired
    struct rocketnet_dst_held_delivery_t witness_held;
    int failed;
    uint64_t fail_step;
    char fail_check[64];
    char fail_detail[256];
} rocketnet_dst_world_t;

static inline void rocketnet_dst_fail( rocketnet_dst_world_t * w, const char * check, const char * detail )
{
    if ( w->failed )
    {
        return;
    }
    w->failed = 1;
    w->fail_step = w->step;
    snprintf( w->fail_check, sizeof( w->fail_check ), "%s", check );
    snprintf( w->fail_detail, sizeof( w->fail_detail ), "%s", detail );
}

static inline void rocketnet_dst_world_trace_send( rocketnet_dst_world_t * w, int sender, int receiver,
                                                   const uint8_t * data, int bytes )
{
    rocketnet_dst_trace_u64( &w->trace, w->step );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) sender );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) receiver );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) bytes );
    rocketnet_dst_trace_u64( &w->trace, bytes > 0 ? data[0] : 0 );
}

// A schedule's event in the trace. Only a run with a schedule appends one,
// so the trace of a run with none is unchanged.

static inline void rocketnet_dst_schedule_trace( rocketnet_dst_world_t * w, uint64_t a, uint64_t b, uint64_t c )
{
    rocketnet_dst_trace_record( &w->trace, w->step, (uint64_t) ( 0x5C4ED00 + w->schedule.kind ), a, b, c );
}

// Every packet the harness puts on the wire. The wire runs on the shared step
// clock; dup-storm adds its copies here, each through deliver_packets with its
// own drawn delay, so the copies take no draw from the simulator's own stream.

static inline void rocketnet_dst_wire_send( rocketnet_dst_world_t * w, const struct rocketnet_address_t * from,
                                            const struct rocketnet_address_t * to, const uint8_t * data, int bytes )
{
    rocketnet_network_simulator_send( w->sim, from, to, data, bytes, w->now );

    struct rocketnet_dst_schedule_t * s = &w->schedule;
    if ( s->kind != ROCKETNET_DST_SCHEDULE_DUP_STORM || w->step < s->start_step || w->step >= s->end_step )
    {
        return;
    }

    const int copies = (int) rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_DUP_STORM_MIN_COPIES,
                                                              ROCKETNET_DST_DUP_STORM_MAX_COPIES );
    for ( int c = 0; c < copies; c++ )
    {
        const uint64_t delay_ms = rocketnet_dst_substream_uniform( &s->draws, 0, ROCKETNET_DST_DUP_STORM_MAX_DELAY_MS );
        const uint8_t * packet = data;
        const int packet_bytes = bytes;
        s->fired += (uint64_t) rocketnet_network_simulator_deliver_packets( w->sim, from, to, &packet, &packet_bytes, 1,
                                                                            w->now + w->base_latency + (double) delay_ms * 0.001 );
    }
}

// The time endpoint e's transport sees: the shared step clock plus the
// endpoint's offset, which only clock-back moves.

static inline double rocketnet_dst_endpoint_clock( rocketnet_dst_world_t * w, int e, double time )
{
    const double clock = time + w->schedule.clock_offset[e];
    if ( w->schedule.clock_seen[e] && clock < w->schedule.clock_last[e] )
    {
        w->schedule.clock_back_seen++;
    }
    w->schedule.clock_seen[e] = 1;
    w->schedule.clock_last[e] = clock;

    return clock;
}

// burst: client i's packets are held in both directions while the window is
// open, and the window's end releases them in one step.

static inline int rocketnet_dst_burst_holding( const rocketnet_dst_world_t * w, int client_index )
{
    const struct rocketnet_dst_schedule_t * s = &w->schedule;
    return s->kind == ROCKETNET_DST_SCHEDULE_BURST && client_index == s->endpoint &&
           w->step >= s->start_step && w->step < s->end_step;
}

// outage: whether client i is one of the run's dark clients (through the
// whole run, so its messages stay within one packet), and whether the window
// is open at this step.

static inline int rocketnet_dst_outage_run( const rocketnet_dst_world_t * w )
{
    return w->schedule.kind == ROCKETNET_DST_SCHEDULE_OUTAGE || w->schedule.kind == ROCKETNET_DST_SCHEDULE_OUTAGE_PAST;
}

static inline int rocketnet_dst_outage_dark( const rocketnet_dst_world_t * w, int client_index )
{
    return rocketnet_dst_outage_run( w ) && client_index >= 0 && client_index < ROCKETNET_DST_MAX_CLIENTS &&
           ( w->schedule.dark_mask >> client_index ) & 1u;
}

static inline int rocketnet_dst_outage_open( const rocketnet_dst_world_t * w )
{
    return rocketnet_dst_outage_run( w ) && w->step >= w->schedule.start_step && w->step < w->schedule.end_step;
}

// outage: whether a dark session's side (0 the client, 1 the server) still
// receives its remote endpoint's packets in the window: a one-way outage leaves the
// other direction bright.

static inline int rocketnet_dst_outage_reaches( const struct rocketnet_dst_schedule_t * s, int side )
{
    return !( s->direction & ( side ? ROCKETNET_DST_OUTAGE_C2S : ROCKETNET_DST_OUTAGE_S2C ) );
}

// outage: how many reliable numbers a connection's sender has written at
// least once: every number it has numbered (below next) but those in the
// window it holds still unsent. Its growth over a step is the step's first
// sends, which probe pacing still allows (m9.3).

static inline uint64_t rocketnet_dst_outage_numbers_sent( const struct rn_message_connection_t * connection )
{
    const rn_region_t * region = connection->region;
    const uint64_t U = connection->ordered.send.U;
    const uint64_t next = connection->ordered.send.next;
    uint64_t unsent = 0;
    for ( uint64_t n = U; n < next; n++ )
    {
        const rn_send_number_t * slot = &region->send_numbers[n & ( region->window - 1 )];
        if ( slot->number == n && !slot->acked && rn_send_number_never_sent( slot->last_send ) )
        {
            unsent++;
        }
    }
    return next - unsent;
}

// outage: the wire drops this packet of client i's, sent to the server
// (to_server) or to the client, while the window is open and that direction
// is dark for the client (or for the OUTAGE_BRIGHT witness's bright client).

static inline int rocketnet_dst_outage_drops( rocketnet_dst_world_t * w, int client_index, int to_server )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    const int dark = rocketnet_dst_outage_dark( w, client_index ) || ( client_index >= 0 && client_index == s->witness_client );
    const int bit = to_server ? ROCKETNET_DST_OUTAGE_C2S : ROCKETNET_DST_OUTAGE_S2C;
    if ( !dark || !rocketnet_dst_outage_open( w ) || !( s->direction & bit ) )
    {
        return 0;
    }
    s->fired++;
    if ( w->client[client_index].protocol.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        w->check.outage_handshake_seen = 1;
    }
    return 1;
}

static inline void rocketnet_dst_endpoint_reset_payloads( rocketnet_dst_endpoint_t * endpoint )
{
    endpoint->payload_count = 0;
}

static inline void rocketnet_dst_endpoint_payload( rocketnet_dst_endpoint_t * endpoint, int client_index,
                                                   const uint8_t * data, int bytes, uint64_t sequence,
                                                   uint64_t connection_id )
{
    if ( endpoint->payload_count >= ROCKETNET_DST_MAX_PAYLOADS || bytes <= 0 ||
         bytes > ROCKETNET_RECEIVE_PAYLOAD_BYTES )
    {
        rocketnet_dst_fail( endpoint->world, "payload_ring", "the delivery ring is full" );
        return;
    }

    const int index = endpoint->payload_count++;
    memcpy( endpoint->payload[index], data, (size_t) bytes );
    endpoint->payload_ptr[index] = endpoint->payload[index];
    endpoint->payload_bytes[index] = bytes;
    endpoint->payload_client_index[index] = client_index;
    endpoint->payload_sequence[index] = sequence;
    endpoint->payload_connection_id[index] = connection_id;
}

static inline int rocketnet_dst_world_client_for_slot( rocketnet_dst_world_t * w, int slot )
{
    if ( !rocketnet_client_slots_connected( w->server.client_slots, slot ) )
    {
        return -1;
    }

    const uint64_t id = rocketnet_client_slots_client_id( w->server.client_slots, slot );
    return (int) ( id & 0xFF );
}

// The slot holding client index's current session (client_id generation *
// 256 + index), or -1. After a client ends its session (a churn whose
// disconnect burst is lost, or its message layer's reliable timeout), the
// server keeps the old session's slot until its own transport ends it, so
// for a while two slots name the same client index: only this one is the
// session the client's message object talks to.

static inline int rocketnet_dst_world_slot_for_client( rocketnet_dst_world_t * w, int index )
{
    const uint64_t client_id = w->client[index].generation * 256 + (uint64_t) index;
    for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
    {
        if ( rocketnet_client_slots_connected( w->server.client_slots, slot ) &&
             rocketnet_client_slots_client_id( w->server.client_slots, slot ) == client_id )
        {
            return slot;
        }
    }

    return -1;
}

static inline int rocketnet_dst_world_client_for_address( rocketnet_dst_world_t * w,
                                                          const struct rocketnet_address_t * address )
{
    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( rocketnet_address_equal( &w->client[i].address, address ) )
        {
            return i;
        }
    }

    return -1;
}

static inline void rocketnet_dst_endpoint_send_control( rocketnet_dst_endpoint_t * endpoint, int receiver,
                                                        const uint8_t * data, int bytes,
                                                        const struct rocketnet_address_t * to )
{
    if ( bytes <= 0 )
    {
        return;
    }

    rocketnet_dst_world_t * w = endpoint->world;
    const struct rocketnet_address_t * from = endpoint->is_server ? &w->server_address
                                                                  : &w->client[endpoint->client_index].address;
    if ( !to )
    {
        to = &w->server_address;
    }
    rocketnet_dst_world_trace_send( w, endpoint->is_server ? 0 : endpoint->client_index + 1, receiver, data, bytes );
    rocketnet_dst_wire_send( w, from, to, data, bytes );
}

static inline void rocketnet_dst_server_process( rocketnet_dst_endpoint_t * endpoint, const struct rocketnet_address_t * from,
                                                 uint8_t * packet, int bytes, double time )
{
    rocketnet_dst_world_t * w = endpoint->world;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];
    int response_bytes = 0;
    int payload_bytes = 0;
    int payload_client_index = -1;
    const int result = rocketnet_server_protocol_process_packet( &w->server, from, packet, bytes,
                                                                 w->unix_ts, time,
                                                                 response, &response_bytes,
                                                                 payload, &payload_bytes,
                                                                 &payload_client_index );
    rocketnet_dst_endpoint_send_control( endpoint, rocketnet_dst_world_client_for_address( w, from ) + 1,
                                         response, response_bytes, from );
    if ( result == ROCKETNET_SERVER_PROCESS_ACCEPTED && payload_bytes > 0 )
    {
        rocketnet_dst_endpoint_payload( endpoint, payload_client_index, payload, payload_bytes,
                                        w->server.payload_sequence, w->server.payload_connection_id );
    }
}

static inline void rocketnet_dst_endpoint_update( void * transport, double time )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    rocketnet_dst_endpoint_reset_payloads( endpoint );

    struct rocketnet_address_t from;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_PAYLOAD_BYTES];

    // the wire is read on the shared clock; the protocol runs on the
    // endpoint's own clock (docs/DST.md, clock-back)

    const double wire_time = time;
    time = rocketnet_dst_endpoint_clock( w, endpoint->is_server ? 0 : endpoint->client_index + 1, time );

    if ( endpoint->is_server )
    {
        w->server.acks->num_acked = 0;

        struct rocketnet_dst_schedule_t * s = &w->schedule;
        if ( s->kind == ROCKETNET_DST_SCHEDULE_BURST && w->step == s->end_step && s->held_count > 0 )
        {
            // the burst client's held packets reach the server in this one step
            for ( int i = 0; i < s->held_count; i++ )
            {
                rocketnet_dst_server_process( endpoint, &w->client[s->endpoint].address, s->held[i], s->held_bytes[i], time );
            }
            rocketnet_dst_schedule_trace( w, 1, (uint64_t) s->held_count, 0 );
            s->held_count = 0;
        }

        int bytes;
        while ( ( bytes = rocketnet_network_simulator_receive( w->sim, &w->server_address, wire_time, &from, packet ) ) > 0 )
        {
            if ( s->held )
            {
                const int sender = rocketnet_dst_world_client_for_address( w, &from );
                if ( sender >= 0 && rocketnet_dst_burst_holding( w, sender ) )
                {
                    // past the capacity the held packet is lost, as a full wire loses it
                    if ( s->held_count < ROCKETNET_DST_BURST_CAPACITY )
                    {
                        memcpy( s->held[s->held_count], packet, (size_t) bytes );
                        s->held_bytes[s->held_count] = bytes;
                        s->held_count++;
                    }

                    continue;
                }
            }

            if ( rocketnet_dst_outage_drops( w, rocketnet_dst_world_client_for_address( w, &from ), 1 ) )
            {
                continue;   // outage: a dark client's packet to the server is lost
            }

            rocketnet_dst_server_process( endpoint, &from, packet, bytes, time );
        }

        rocketnet_server_protocol_update( &w->server, time );

        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            // the server transport's maintenance: a keep-alive when one is
            // due, otherwise a standalone ack while the slot owes one
            int maintenance_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &w->server, slot, time, response );
            if ( maintenance_bytes == 0 )
            {
                maintenance_bytes = rocketnet_server_protocol_write_ack_if_due( &w->server, slot, time, response );
            }

            if ( maintenance_bytes > 0 )
            {
                const int mapping_index = rocketnet_client_slots_mapping_index( w->server.client_slots, slot );
                const struct rocketnet_address_t * to = rocketnet_encryption_map_address( w->server.encryption_map, mapping_index );
                if ( to )
                {
                    rocketnet_dst_endpoint_send_control( endpoint, rocketnet_dst_world_client_for_slot( w, slot ) + 1,
                                                         response, maintenance_bytes, to );
                }
            }
        }

        return;
    }

    w->client[endpoint->client_index].protocol.num_acked = 0;

    // burst: while the window is open the client reads nothing, and the
    // wire keeps what is due for it; the window's end reads it all at once

    const int holding = rocketnet_dst_burst_holding( w, endpoint->client_index );
    const int releasing = w->schedule.kind == ROCKETNET_DST_SCHEDULE_BURST &&
                          endpoint->client_index == w->schedule.endpoint && w->step == w->schedule.end_step;
    uint64_t received = 0;
    int bytes;
    while ( !holding &&
            ( bytes = rocketnet_network_simulator_receive( w->sim, &w->client[endpoint->client_index].address, wire_time, &from, packet ) ) > 0 )
    {
        if ( rocketnet_dst_outage_drops( w, endpoint->client_index, 0 ) )
        {
            continue;   // outage: the server's packet to a dark client is lost
        }

        received++;

        int payload_bytes = 0;
        const int accepted = rocketnet_client_protocol_process_packet( &w->client[endpoint->client_index].protocol,
                                                                       packet, bytes, time,
                                                                       payload, sizeof( payload ), &payload_bytes );
        if ( accepted && payload_bytes > 0 )
        {
            rocketnet_dst_endpoint_payload( endpoint, -1, payload, payload_bytes,
                                            w->client[endpoint->client_index].protocol.received_session_sequence, 1 );
        }
    }

    if ( releasing )
    {
        w->schedule.burst_received = received;
        rocketnet_dst_schedule_trace( w, 2, received, 0 );
    }

    int packet_bytes = 0;
    rocketnet_client_protocol_update( &w->client[endpoint->client_index].protocol, time, packet, &packet_bytes );
    rocketnet_dst_endpoint_send_control( endpoint, 0, packet, packet_bytes, &w->server_address );
}

static inline int rocketnet_dst_endpoint_payloads( void * transport, const int ** client_index,
                                                   uint8_t * const ** payload_data, const int ** payload_bytes )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    *client_index = endpoint->is_server ? endpoint->payload_client_index : NULL;
    *payload_data = endpoint->payload_ptr;
    *payload_bytes = endpoint->payload_bytes;

    return endpoint->payload_count;
}

static inline int rocketnet_dst_endpoint_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;

    if ( endpoint->is_server )
    {
        *client_index = w->server.acks->acked_client_index;
        *packet_ids = w->server.acks->acked_packet_ids;
        for ( int i = 0; i < w->server.acks->num_acked; i++ )
        {
            rocketnet_dst_check_ack_id( &w->check, 0, w->server.acks->acked_packet_ids[i] );
        }
        return w->server.acks->num_acked;
    }

    *client_index = NULL;
    *packet_ids = w->client[endpoint->client_index].protocol.acked_packet_ids;
    if ( w->witness == ROCKETNET_DST_WITNESS_FOREIGN_ACK && w->witness_state == 0 )
    {
        const int self = endpoint->client_index + 1;
        for ( uint64_t foreign = w->check.issued_high[self] + 1; foreign <= w->check.issued_high[0]; foreign++ )
        {
            if ( rocketnet_dst_check_ledger_bit( w->check.issued_bits[0], foreign ) )
            {
                rocketnet_dst_check_ack_id( &w->check, self, foreign );
                w->witness_state = 2;
                break;
            }
        }
    }
    for ( int i = 0; i < w->client[endpoint->client_index].protocol.num_acked; i++ )
    {
        rocketnet_dst_check_ack_id( &w->check, endpoint->client_index + 1, w->client[endpoint->client_index].protocol.acked_packet_ids[i] );
    }

    return w->client[endpoint->client_index].protocol.num_acked;
}

// probe_paced: the dark client whose held session's sender starts (or
// aborts) this frame, or -1. The server's frame for slot client_index is
// the server side of the session in that slot; a client's frame is its own.

static inline int rocketnet_dst_outage_frame_client( rocketnet_dst_world_t * w, const rocketnet_dst_endpoint_t * endpoint,
                                                     int client_index, int receiver )
{
    if ( !rocketnet_dst_outage_open( w ) )
    {
        return -1;
    }
    const int dark = endpoint->is_server ? receiver - 1 : endpoint->client_index;
    if ( !rocketnet_dst_outage_dark( w, dark ) || w->schedule.dark_generation[dark] == 0 ||
         !w->schedule.probe_held[dark][endpoint->is_server ? 1 : 0] )
    {
        return -1;
    }
    const uint64_t session = endpoint->is_server ? rocketnet_client_slots_client_id( w->server.client_slots, client_index )
                                                 : w->client[dark].generation * 256 + (uint64_t) dark;
    return session == w->schedule.dark_generation[dark] * 256 + (uint64_t) dark ? dark : -1;
}

static inline uint8_t * rocketnet_dst_endpoint_start_packet( void * transport, int client_index, int request_bytes,
                                                             struct rocketnet_packet_context_t * context )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    (void) request_bytes;
    if ( endpoint->pending_frame >= 0 || endpoint->outgoing_count >= ROCKETNET_DST_MAX_PACKETS )
    {
        return NULL;
    }

    const int slot = endpoint->outgoing_count;
    uint8_t * wire_start = endpoint->frame[slot] + ROCKETNET_FRAME_WIRE_OFFSET;
    uint64_t sequence = 0;
    int receiver = 0;
    if ( endpoint->is_server )
    {
        struct rocketnet_address_t to;
        if ( !rocketnet_server_protocol_start_payload( &w->server, client_index, &to, &sequence ) )
        {
            return NULL;
        }
        receiver = rocketnet_dst_world_client_for_address( w, &to ) + 1;
    }
    else
    {
        if ( !rocketnet_client_protocol_start_payload( &w->client[endpoint->client_index].protocol, &sequence ) )
        {
            return NULL;
        }
    }

    memset( context, 0, sizeof( *context ) );
    context->frame = wire_start;
    context->payload_index = ROCKETNET_FRAME_PAYLOAD_OFFSET - ROCKETNET_FRAME_WIRE_OFFSET;
    context->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    context->packet_id = ++endpoint->next_packet_id;
    context->sequence = sequence;
    context->client_index = client_index;
    endpoint->pending_frame = slot;
    endpoint->frame_receiver[slot] = receiver;

    // probe_paced: a frame a held dark session's sender starts in the window
    // (each one takes a slot of the transport's send capacity, sent or aborted)
    const int dark = rocketnet_dst_outage_frame_client( w, endpoint, client_index, receiver );
    if ( dark >= 0 )
    {
        w->schedule.frames[dark][endpoint->is_server ? 1 : 0]++;
    }

    return wire_start;
}

static inline int rocketnet_dst_endpoint_finish_packet( void * transport, struct rocketnet_packet_context_t * context,
                                                        int payload_bytes )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( endpoint->pending_frame < 0 ||
         context->frame != endpoint->frame[endpoint->pending_frame] + ROCKETNET_FRAME_WIRE_OFFSET )
    {
        return 0;
    }

    // send-fail: inside the window, the drawn side's transport refuses one
    // frame in ONE_IN at finish, as a transport whose send failed does

    struct rocketnet_dst_schedule_t * s = &w->schedule;
    if ( s->kind == ROCKETNET_DST_SCHEDULE_SEND_FAIL && w->step >= s->start_step && w->step < s->end_step &&
         endpoint->is_server == s->side &&
         rocketnet_dst_substream_uniform( &s->draws, 1, ROCKETNET_DST_SEND_FAIL_ONE_IN ) == 1 )
    {
        endpoint->pending_frame = -1;
        s->fired++;
        w->check.send_fail_seen = 1;
        return 0;
    }

    int wire_bytes;
    if ( endpoint->is_server )
    {
        wire_bytes = rocketnet_server_protocol_seal_payload_frame( &w->server, context->client_index,
                                                                   context->sequence, context->packet_id,
                                                                   context->frame, payload_bytes,
                                                                   w->now + w->schedule.clock_offset[0] );
    }
    else
    {
        wire_bytes = rocketnet_client_protocol_seal_payload_frame( &w->client[endpoint->client_index].protocol,
                                                                   context->sequence, context->packet_id,
                                                                   context->frame, payload_bytes,
                                                                   w->now + w->schedule.clock_offset[endpoint->client_index + 1] );
    }

    if ( wire_bytes > 0 )
    {
        endpoint->frame_bytes[endpoint->pending_frame] = wire_bytes;
        endpoint->outgoing_count++;
        rocketnet_dst_check_issued( &w->check, endpoint->is_server ? 0 : endpoint->client_index + 1, context->packet_id );
    }

    endpoint->pending_frame = -1;

    return wire_bytes;
}

static inline void rocketnet_dst_endpoint_abort_packet( void * transport, struct rocketnet_packet_context_t * context )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;

    // a frame aborted because the send budget could not pay the reliable
    // packet the plan reported due (budget_aborts, reported); probe_paced
    // takes it out of a held sender's frames, since it is not the probe's
    if ( endpoint->pending_frame >= 0 )
    {
        const int receiver = endpoint->frame_receiver[endpoint->pending_frame];
        const int owner = endpoint->is_server ? receiver - 1 : endpoint->client_index;
        if ( owner >= 0 && owner < w->num_clients && w->client[owner].message )
        {
            struct rn_message_object_t * object = endpoint->is_server ? &w->message_server->object : &w->client[owner].message->object;
            const struct rn_message_connection_t * connection =
                rn_message_object_connection( object, endpoint->is_server ? context->client_index : 0 );
            if ( connection && connection->budget.reliable_waiting )
            {
                w->budget_aborts++;
                const int dark = rocketnet_dst_outage_frame_client( w, endpoint, context->client_index, receiver );
                const int side = endpoint->is_server ? 1 : 0;
                if ( dark >= 0 && w->schedule.frames[dark][side] > 0 )
                {
                    w->schedule.frames[dark][side]--;
                }
            }
        }
    }
    endpoint->pending_frame = -1;
}

static inline void rocketnet_dst_endpoint_send_packets( void * transport )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    const struct rocketnet_address_t * from = endpoint->is_server ? &w->server_address
                                                                  : &w->client[endpoint->client_index].address;

    for ( int i = 0; i < endpoint->outgoing_count; i++ )
    {
        const int bytes = endpoint->frame_bytes[i];
        const int receiver = endpoint->frame_receiver[i];
        const struct rocketnet_address_t * to;
        if ( endpoint->is_server )
        {
            const int client_index = receiver - 1;
            if ( client_index < 0 || client_index >= w->num_clients )
            {
                continue;
            }
            to = &w->client[client_index].address;
        }
        else
        {
            to = &w->server_address;
        }
        rocketnet_dst_world_trace_send( w, endpoint->is_server ? 0 : endpoint->client_index + 1, receiver,
                                        endpoint->frame[i] + ROCKETNET_FRAME_WIRE_OFFSET, bytes );
        rocketnet_dst_wire_send( w, from, to, endpoint->frame[i] + ROCKETNET_FRAME_WIRE_OFFSET, bytes );
        if ( !endpoint->is_server && endpoint->client_index == w->schedule.endpoint &&
             w->schedule.kind == ROCKETNET_DST_SCHEDULE_BURST && w->step == w->schedule.start_step )
        {
            w->schedule.burst_sent++;
        }
    }

    endpoint->outgoing_count = 0;
}

static inline uint64_t rocketnet_dst_endpoint_connection( void * transport, int client_index )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;

    // as the real links answer: the server's the protocol's id of the
    // connection in the slot, new for every connection the slot holds (P9a);
    // the client's 1, the id of its one connection, while it is CONNECTED

    if ( endpoint->is_server )
    {
        return rocketnet_server_protocol_connection_id( &w->server, client_index );
    }

    return w->client[endpoint->client_index].protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ? 1 : 0;
}

// publishes a payload's ack only once the message layer has taken it, as
// the real transports do under rocketnet_*_transport_defer_payload_acks

static inline void rocketnet_dst_endpoint_accept_payload( void * transport, int k )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( k < 0 || k >= endpoint->payload_count )
    {
        return;
    }

    if ( endpoint->is_server )
    {
        rocketnet_server_protocol_accept_payload( &w->server, endpoint->payload_client_index[k],
                                                  endpoint->payload_connection_id[k], endpoint->payload_sequence[k] );
    }
    else
    {
        rocketnet_client_protocol_accept_payload( &w->client[endpoint->client_index].protocol,
                                                  endpoint->payload_sequence[k] );
    }
}

static inline uint64_t rocketnet_dst_endpoint_payload_connection( void * transport, int k )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    return k >= 0 && k < endpoint->payload_count ? endpoint->payload_connection_id[k] : 0;
}

static inline uint64_t rocketnet_dst_endpoint_acked_connection( void * transport, int k )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( !endpoint->is_server )
    {
        return 1;
    }

    return k >= 0 && k < w->server.acks->num_acked ? w->server.acked_connection_ids[k] : 0;
}

static inline int rocketnet_dst_endpoint_max_payload_bytes( void * transport )
{
    (void) transport;
    return ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
}

static inline int rocketnet_dst_endpoint_send_capacity_available( void * transport )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    return ROCKETNET_DST_MAX_PACKETS - endpoint->outgoing_count;
}

static inline int rocketnet_dst_endpoint_send_connected_count( void * transport )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    return rocketnet_client_slots_num_connected( endpoint->world->server.client_slots );
}

static inline int rocketnet_dst_endpoint_end_reason( void * transport, int client_index )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    return rocketnet_server_protocol_end_reason( &endpoint->world->server, client_index );
}

static inline int rocketnet_dst_endpoint_state( void * transport )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( endpoint->is_server )
    {
        return 1;
    }

    return w->client[endpoint->client_index].protocol.state;
}

static inline void rocketnet_dst_endpoint_disconnect( void * transport, int client_index )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    uint8_t packets[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
    int packet_count = 0;
    int packet_bytes = 0;

    // the server's client_index is a slot, not a DST client: the burst goes
    // to the client holding the slot, looked up before the disconnect
    // releases it, as the payload path resolves its receiver by address

    const int target = endpoint->is_server ? rocketnet_dst_world_client_for_slot( w, client_index ) : -1;

    // the message layer's kick records its reason before it calls here
    const int reason = endpoint->is_server
                           ? rocketnet_message_server_disconnect_reason( w->message_server, client_index )
                           : rocketnet_message_client_disconnect_reason( w->client[endpoint->client_index].message );
    if ( reason == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT )
    {
        w->reliable_timeouts[endpoint->is_server ? 0 : 1]++;
    }

    // no_unscheduled_end: the message layer ends a connection only for a
    // reliable message past an opted-in timeout. Any other kick (a full send
    // side, a wire violation, memory exhausted, a message not received) is a
    // session end no script explains. The explained one names the session it
    // ends: the slot's for the server, the client's current one otherwise.
    w->message_kicks++;
    const int kicked = endpoint->is_server ? target : endpoint->client_index;
    const uint64_t kicked_generation = endpoint->is_server
                                           ? rocketnet_client_slots_client_id( w->server.client_slots, client_index ) >> 8
                                           : ( kicked >= 0 ? w->client[kicked].generation : 0 );
    if ( reason == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT && w->reliable_timeout_run )
    {
        if ( kicked >= 0 && kicked < w->num_clients && kicked_generation == w->client[kicked].generation )
        {
            w->client[kicked].explained_generation = kicked_generation;
        }
    }
    else
    {
        char what[160];
        snprintf( what, sizeof( what ), "%s kicked client=%d generation=%llu reason=%d",
                  endpoint->is_server ? "server" : "client", kicked, (unsigned long long) kicked_generation, reason );
        rocketnet_dst_check_unscheduled_kick( &w->check, what );
    }

    if ( endpoint->is_server )
    {
        rocketnet_server_protocol_disconnect_client( &w->server, client_index, packets, &packet_count, &packet_bytes );
    }
    else
    {
        rocketnet_client_protocol_disconnect( &w->client[endpoint->client_index].protocol, packets, &packet_count, &packet_bytes );
    }
    if ( endpoint->is_server && ( target < 0 || target >= w->num_clients ) )
    {
        return;
    }

    const struct rocketnet_address_t * from = endpoint->is_server ? &w->server_address
                                                                  : &w->client[endpoint->client_index].address;
    const struct rocketnet_address_t * to = endpoint->is_server ? &w->client[target].address : &w->server_address;
    const int receiver = endpoint->is_server ? target + 1 : 0;

    for ( int i = 0; i < packet_count; i++ )
    {
        const uint8_t * packet = packets + (size_t) i * (size_t) ROCKETNET_SEND_BUDGET;
        rocketnet_dst_world_trace_send( w, endpoint->is_server ? 0 : endpoint->client_index + 1, receiver, packet, packet_bytes );
        rocketnet_dst_wire_send( w, from, to, packet, packet_bytes );
    }
}

// the transport's reported round trip, read as the real links read it
// (rn_message_server_link, rn_message_client_link)
static inline double rocketnet_dst_endpoint_rtt( void * transport, int client_index )
{
    rocketnet_dst_endpoint_t * endpoint = (rocketnet_dst_endpoint_t *) transport;
    rocketnet_dst_world_t * w = endpoint->world;
    if ( endpoint->is_server )
    {
        return rocketnet_server_protocol_client_rtt( &w->server, client_index );
    }
    return rocketnet_client_protocol_rtt( &w->client[endpoint->client_index].protocol );
}

// The link the real ones set (rn_message_server_link, rn_message_client_link),
// hook for hook, apart from what the harness owns itself: the transport's
// lifetime (stop, stopped, destroy), the client's end reason (the clients
// share one counter block, which the real link reads it from), and the
// client's clock origin (the harness connects the client protocol on the
// shared clock, so its update runs on that clock too).

static inline void rocketnet_dst_endpoint_link( struct rn_message_link_t * link, rocketnet_dst_endpoint_t * endpoint )
{
    memset( link, 0, sizeof( *link ) );
    link->transport = endpoint;
    link->update = rocketnet_dst_endpoint_update;
    link->payloads = rocketnet_dst_endpoint_payloads;
    link->accept_payload = rocketnet_dst_endpoint_accept_payload;
    link->acked = rocketnet_dst_endpoint_acked;
    link->payload_connection = rocketnet_dst_endpoint_payload_connection;
    link->acked_connection = rocketnet_dst_endpoint_acked_connection;
    link->max_payload_bytes = rocketnet_dst_endpoint_max_payload_bytes;
    link->start_packet = rocketnet_dst_endpoint_start_packet;
    link->finish_packet = rocketnet_dst_endpoint_finish_packet;
    link->abort_packet = rocketnet_dst_endpoint_abort_packet;
    link->send_packets = rocketnet_dst_endpoint_send_packets;
    if ( endpoint->is_server )
    {
        link->send_capacity_available = rocketnet_dst_endpoint_send_capacity_available;
        link->send_connected_count = rocketnet_dst_endpoint_send_connected_count;
        link->end_reason = rocketnet_dst_endpoint_end_reason;
    }
    link->connection = rocketnet_dst_endpoint_connection;
    link->disconnect = rocketnet_dst_endpoint_disconnect;
    link->state = rocketnet_dst_endpoint_state;
    link->rtt = rocketnet_dst_endpoint_rtt;
    link->clock_from_first_update = 0;
}

static inline int rocketnet_dst_payload_build( uint8_t * buffer, int size, uint32_t sequence, int direction,
                                               int type, int channel, uint32_t generation, int incarnation,
                                               uint32_t epoch )
{
    if ( size < 14 )
    {
        return 0;
    }
    buffer[0] = (uint8_t) ( sequence );
    buffer[1] = (uint8_t) ( sequence >> 8 );
    buffer[2] = (uint8_t) ( sequence >> 16 );
    buffer[3] = (uint8_t) ( sequence >> 24 );
    buffer[4] = (uint8_t) ( 0xA0 | ( direction << 3 ) | ( type & 7 ) );
    buffer[5] = (uint8_t) channel;
    buffer[6] = (uint8_t) ( generation );
    buffer[7] = (uint8_t) ( generation >> 8 );
    buffer[8] = (uint8_t) ( generation >> 16 );
    buffer[9] = (uint8_t) incarnation;
    buffer[10] = (uint8_t) ( epoch );
    buffer[11] = (uint8_t) ( epoch >> 8 );
    buffer[12] = (uint8_t) ( epoch >> 16 );
    buffer[13] = (uint8_t) ( epoch >> 24 );
    for ( int i = 14; i < size; i++ )
    {
        buffer[i] = (uint8_t) ( ( (uint32_t) i + sequence ) % 256 );
    }

    return 1;
}

static inline int rocketnet_dst_payload_verify( const uint8_t * buffer, int bytes, uint32_t * sequence,
                                                int * type, int * channel, uint32_t * generation )
{
    if ( bytes < 14 )
    {
        return 0;
    }
    if ( ( buffer[4] & 0xF0 ) != 0xA0 )
    {
        return 0;
    }
    *sequence = (uint32_t) buffer[0] | ( (uint32_t) buffer[1] << 8 ) |
                ( (uint32_t) buffer[2] << 16 ) | ( (uint32_t) buffer[3] << 24 );
    *type = buffer[4] & 7;
    *channel = buffer[5];
    *generation = (uint32_t) buffer[6] | ( (uint32_t) buffer[7] << 8 ) | ( (uint32_t) buffer[8] << 16 );
    if ( *channel >= ROCKETNET_DST_NUM_CHANNELS )
    {
        return 0;
    }

    for ( int i = 14; i < bytes; i++ )
    {
        if ( buffer[i] != (uint8_t) ( ( (uint32_t) i + *sequence ) % 256 ) )
        {
            return 0;
        }
    }

    return 1;
}

static inline int rocketnet_dst_draw_size( struct rocketnet_dst_substream_t * traffic, int type )
{
    const double band = rocketnet_dst_substream_real( traffic );
    int lo, hi;

    if ( band < 0.70 )
    {
        lo = 14;
        hi = 256;
    }
    else if ( band < 0.95 )
    {
        lo = 257;
        hi = 4000;
    }
    else
    {
        lo = 4001;
        hi = ROCKETNET_DST_CHECK_MAX_MESSAGE_BYTES;
    }

    if ( type == 0 || type == 1 || type == 5 )
    {
        if ( hi > 1400 )
        {
            hi = 1400;
        }
        if ( lo > hi )
        {
            lo = hi;
        }
    }

    return (int) rocketnet_dst_substream_uniform( traffic, (uint64_t) lo, (uint64_t) hi );
}

static inline int rocketnet_dst_channel_type( rocketnet_dst_client_t * client, int channel, int direction )
{
    if ( channel < 8 )
    {
        return channel;
    }

    const unsigned pick = client->mixed[direction]++ % 3;
    return pick == 0 ? 3 : ( pick == 1 ? 4 : 1 );
}

static inline uint32_t rocketnet_dst_next_sequence( rocketnet_dst_client_t * client, int type, int direction )
{
    return client->sequence[type & 7][direction]++;
}

// Adds one counter block into another, value by value.

static inline void rocketnet_dst_counters_add( struct rocketnet_counters_t * into, const struct rocketnet_counters_t * from )
{
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        into->values[i] += from->values[i];
    }
}

// A new session gets a fresh message object, as section 3's restart does:
// nothing of the old session survives into the new one.

static inline int rocketnet_dst_client_message_build( rocketnet_dst_world_t * w, int index )
{
    rocketnet_dst_client_t * client = &w->client[index];
    if ( client->message )
    {
        rocketnet_dst_counters_add( &w->message_counters[1], &client->message->object.counters );
        rocketnet_message_client_destroy( client->message );
    }
    client->endpoint.pending_frame = -1;
    client->endpoint.outgoing_count = 0;
    client->endpoint.next_packet_id = 0;
    rocketnet_dst_check_endpoint_reset( &w->check, index + 1 );

    struct rn_message_link_t client_link;
    rocketnet_dst_endpoint_link( &client_link, &client->endpoint );
    client->message = (struct rocketnet_message_client_t *) rn_message_object_create(
        NULL, &client_link, 0, 1, &w->message_config, NULL, 0,
        ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, w->now );
    return client->message != NULL;
}

static inline void rocketnet_dst_connect_client( rocketnet_dst_world_t * w, int index )
{
    rocketnet_dst_client_t * client = &w->client[index];
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    memset( &private_fields, 0, sizeof( private_fields ) );

    // a new session follows an end a script explains, or none before it
    rocketnet_dst_check_session_end( &w->check, index, client->generation, client->explained_generation,
                                     "client reconnects", 0 );

    client->generation++;
    client->connect_step = w->step;
    rocketnet_dst_check_generation( &w->check, index, client->generation );

    // every session, the first included, gets a fresh message object at
    // connect, as the product creates its message client over a transport
    // that is already connecting: the pre-connect updates of the one made at
    // create see the transport DISCONNECTED, which ends and frees its one
    // connection for good, so every payload of a first session would be
    // dropped as a stale connection entry and acked unread

    if ( !rocketnet_dst_client_message_build( w, index ) )
    {
        rocketnet_dst_fail( w, "message_object", "the restarted message object was refused" );
        return;
    }

    const uint64_t client_id = client->generation * 256 + (uint64_t) index;
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = w->unix_ts;
    signed_fields.expire_timestamp = w->unix_ts + 300;
    signed_fields.timeout_seconds = ROCKETNET_DST_TOKEN_TIMEOUT_SECONDS;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address_bytes[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address_bytes, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );
    private_fields.client_id = client_id;
    randombytes_buf( private_fields.client_to_server_key, 32 );
    randombytes_buf( private_fields.server_to_client_key, 32 );

    if ( !rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token ) )
    {
        rocketnet_dst_fail( w, "connect_token", "the token was refused" );
        return;
    }

    rocketnet_client_protocol_connect( &client->protocol, token, w->now + w->schedule.clock_offset[index + 1], w->unix_ts );

    for ( int t = 0; t < ROCKETNET_DST_MAX_STREAMS; t++ )
    {
        client->sequence[t][0] = 0;
        client->sequence[t][1] = 0;
    }

    client->epoch[0] = 0;
    client->epoch[1] = 0;
    client->mixed[0] = 0;
    client->mixed[1] = 0;
}

static inline void rocketnet_dst_send_one( rocketnet_dst_world_t * w, int index, int direction, int slot )
{
    rocketnet_dst_client_t * client = &w->client[index];
    struct rocketnet_dst_substream_t * traffic = direction == 0 ? &client->traffic : &w->traffic_server;
    if ( rocketnet_dst_substream_real( traffic ) >= 0.5 )
    {
        return;
    }

    const int channel = (int) rocketnet_dst_substream_uniform( traffic, 0, ROCKETNET_DST_NUM_CHANNELS - 1 );
    const int type = rocketnet_dst_channel_type( client, channel, direction );
    int size = rocketnet_dst_draw_size( traffic, type );
    int session = 16 + index;

    // outage: a dark client's messages fit one packet all run, and in the
    // window only reliable ones are offered, past the outstanding cap
    const int dark_open = rocketnet_dst_outage_dark( w, index ) && rocketnet_dst_outage_open( w );
    if ( rocketnet_dst_outage_dark( w, index ) && size > ROCKETNET_DST_OUTAGE_MAX_BYTES )
    {
        size = ROCKETNET_DST_OUTAGE_MAX_BYTES;
    }
    if ( dark_open && ( type == 0 || type == 1 || type == 5 ) )
    {
        return;
    }

    if ( direction == 0 )
    {
        session = rocketnet_dst_world_slot_for_client( w, index );
        if ( session < 0 )
        {
            return;
        }
    }

    // a server send belongs to the session its slot holds, which is the
    // client's current one or one the client already ended. A send on an
    // ended session is owed to nobody (CHANNELS.md: reliability does not
    // persist across a disconnect), so it is stamped with that session's
    // generation, kept out of the client's books, and its receipt is judged
    // against that session: delivered into the client's new session it fails
    // stale_session_payload.

    const uint32_t generation = direction == 0 ? (uint32_t) client->generation
                                               : (uint32_t) ( rocketnet_client_slots_client_id( w->server.client_slots, slot ) >> 8 );
    const int current = generation == (uint32_t) client->generation;
    const uint64_t identity = direction == 0 ? ( (uint64_t) generation * 256 + (uint64_t) index ) : (uint64_t) generation;
    const int reliable = ( type == 2 || type == 3 || type == 4 || type == 6 || type == 7 );
    if ( reliable && !dark_open &&
         rocketnet_dst_check_outstanding( &w->check, session, identity ) > ROCKETNET_DST_CHECK_OUTSTANDING_CAP )
    {
        return;
    }

    uint32_t epoch = 0;
    if ( channel == 8 && type == 3 )
    {
        epoch = client->epoch[direction]++;
    }

    const uint32_t sequence = rocketnet_dst_next_sequence( client, type, direction );
    uint8_t payload[16384];
    if ( size > (int) sizeof( payload ) )
    {
        return;
    }
    if ( !rocketnet_dst_payload_build( payload, size, sequence, direction, type, channel,
                                       generation, w->server_incarnation, epoch ) )
    {
        return;
    }

    uint32_t sent = 0;
    if ( direction == 0 )
    {
        sent = rocketnet_message_client_send( client->message, channel, type, payload, size );
    }
    else
    {
        sent = rocketnet_message_server_send( w->message_server, slot, channel, type, payload, size );
    }

    // send_floor counts the reliable sends the library has no reason to
    // refuse: every one but a dark session's in its outage window
    w->offered[direction]++;
    w->offered_reliable[direction] += reliable && !dark_open ? 1 : 0;
    w->accepted[direction] += sent ? 1 : 0;
    w->accepted_reliable[direction] += sent && reliable && !dark_open ? 1 : 0;
    w->refused[direction] += sent ? 0 : 1;

    if ( sent )
    {
        if ( current )
        {
            rocketnet_dst_check_sent( &w->check, w->now, session, channel, type, sequence, generation,
                                      identity, epoch, size );
        }
        rocketnet_dst_check_receipt_sent( &w->check, direction == 0 ? 16 + index : slot, identity, sent, session,
                                          channel, type, sequence );
        rocketnet_dst_check_receipt_due_from( &w->check, direction == 0 ? 16 + index : slot, identity, sent, type, w->now );
    }
    else
    {
        // a refused send is not sent: it leaves no gap in the stream
        if ( client->sequence[type & 7][direction] > 0 )
        {
            client->sequence[type & 7][direction]--;
        }
        if ( channel == 8 && type == 3 && client->epoch[direction] > 0 )
        {
            client->epoch[direction]--;
        }
    }
}

// burst: the drawn client's live server slot, or -1.

static inline int rocketnet_dst_burst_slot( rocketnet_dst_world_t * w, int index )
{
    return rocketnet_dst_world_slot_for_client( w, index );
}

// burst: one endpoint queues its whole burst in the step the window opens.
// Every message is unreliable and just under one packet, so it takes one
// packet and 300 messages fill the update's 256 packets, the rest spilling
// into the next step. Draws come from the schedule's own substream; the
// caller uncaps both directions first so the send budget cannot thin the
// burst.

static inline void rocketnet_dst_send_burst( rocketnet_dst_world_t * w, int index, int direction, int slot,
                                             int messages )
{
    rocketnet_dst_client_t * client = &w->client[index];
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    if ( slot < 0 )
    {
        return;
    }

    const int channel = 0;
    const int type = 0;                                     // ROCKETNET_RELIABILITY_UNRELIABLE
    const int session = direction == 0 ? slot : 16 + index;

    for ( int n = 0; n < messages; n++ )
    {
        const int size = (int) rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_BURST_MIN_BYTES,
                                                                ROCKETNET_DST_BURST_MAX_BYTES );
        const uint32_t sequence = rocketnet_dst_next_sequence( client, type, direction );
        const uint32_t generation = (uint32_t) client->generation;
        uint8_t payload[2048];
        if ( size > (int) sizeof( payload ) ||
             !rocketnet_dst_payload_build( payload, size, sequence, direction, type, channel, generation,
                                           w->server_incarnation, 0 ) )
        {
            return;
        }

        const uint32_t sent = direction == 0
                                  ? rocketnet_message_client_send( client->message, channel, type, payload, size )
                                  : rocketnet_message_server_send( w->message_server, slot, channel, type, payload, size );
        w->offered[direction]++;
        w->accepted[direction] += sent ? 1 : 0;
        w->refused[direction] += sent ? 0 : 1;
        if ( sent )
        {
            const uint64_t identity = direction == 0
                                          ? ( (uint64_t) generation * 256 + (uint64_t) index )
                                          : (uint64_t) generation;
            rocketnet_dst_check_sent( &w->check, w->now, session, channel, type, sequence, generation,
                                      identity, 0, size );
        }
        else if ( client->sequence[type & 7][direction] > 0 )
        {
            client->sequence[type & 7][direction]--;
        }
    }
}

static inline void rocketnet_dst_check_deliveries( rocketnet_dst_world_t * w, int session,
                                                   uint32_t expected_generation, int expected_incarnation,
                                                   uint64_t identity, const uint8_t * data, int bytes )
{
    uint32_t sequence;
    uint32_t generation;
    uint32_t epoch = 0;
    int incarnation = 0;
    int type;
    int channel;
    if ( !rocketnet_dst_payload_verify( data, bytes, &sequence, &type, &channel, &generation ) )
    {
        char detail[256];
        int mismatch = -1;

        if ( bytes >= 14 )
        {
            const uint32_t seq = (uint32_t) data[0] | ( (uint32_t) data[1] << 8 ) |
                                 ( (uint32_t) data[2] << 16 ) | ( (uint32_t) data[3] << 24 );

            for ( int i = 14; i < bytes; i++ )
            {
                if ( data[i] != (uint8_t) ( ( (uint32_t) i + seq ) % 256 ) )
                {
                    mismatch = i;
                    break;
                }
            }
        }

        snprintf( detail, sizeof( detail ), "session=%d bytes=%d b4=%02x b5=%02x at=%d",
                  session, bytes, bytes > 4 ? data[4] : 0, bytes > 5 ? data[5] : 0, mismatch );
        rocketnet_dst_fail( w, "payload_pattern", detail );
        return;
    }

    incarnation = data[9];
    epoch = (uint32_t) data[10] | ( (uint32_t) data[11] << 8 ) | ( (uint32_t) data[12] << 16 ) |
            ( (uint32_t) data[13] << 24 );
    rocketnet_dst_trace_u64( &w->trace, w->step );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) session );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) channel );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) type );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) sequence );
    rocketnet_dst_trace_u64( &w->trace, (uint64_t) bytes );

    if ( w->witness == ROCKETNET_DST_WITNESS_ORDERED_SWAP && type == 3 && w->witness_state < 2 )
    {
        struct rocketnet_dst_held_delivery_t * held = &w->witness_held;
        if ( w->witness_state == 0 )
        {
            held->session = session;
            held->channel = channel;
            held->type = type;
            held->sequence = sequence;
            held->generation = generation;
            held->incarnation = incarnation;
            held->identity = identity;
            held->epoch = epoch;
            held->bytes = bytes;
            held->expected_generation = expected_generation;
            held->expected_incarnation = expected_incarnation;
            w->witness_state = 1;
            return;
        }
        if ( held->session == session && held->channel == channel )
        {
            rocketnet_dst_check_delivery( &w->check, w->now, session, channel, type, sequence, generation,
                                          incarnation, identity, epoch, bytes, expected_generation,
                                          expected_incarnation );
            rocketnet_dst_check_delivery( &w->check, w->now, held->session, held->channel, held->type,
                                          held->sequence, held->generation, held->incarnation, held->identity,
                                          held->epoch, held->bytes, held->expected_generation,
                                          held->expected_incarnation );
            w->witness_state = 2;
            return;
        }
    }

    rocketnet_dst_check_delivery( &w->check, w->now, session, channel, type, sequence, generation,
                                  incarnation, identity, epoch, bytes, expected_generation,
                                  expected_incarnation );
}

static inline void rocketnet_dst_world_draw( rocketnet_dst_world_t * w, uint64_t seed,
                                             const struct rocketnet_dst_options_t * options )
{
    rocketnet_dst_substream_init( &w->link, seed, "link" );
    rocketnet_dst_substream_init( &w->fault_loss, seed, "fault.loss" );
    rocketnet_dst_substream_init( &w->fault_dup, seed, "fault.dup" );
    rocketnet_dst_substream_init( &w->fault_jitter, seed, "fault.jitter" );
    rocketnet_dst_substream_init( &w->traffic_server, seed, "traffic.server" );

    struct rocketnet_dst_substream_t schedule;
    rocketnet_dst_substream_init( &schedule, seed, "schedule" );

    w->base_latency = 0.005 + rocketnet_dst_substream_real( &w->link ) * 0.145;
    if ( options->faults & ROCKETNET_DST_FAULT_JITTER )
    {
        w->base_jitter = rocketnet_dst_substream_real( &w->fault_jitter ) * ( w->base_latency * 0.5 );
    }
    else
    {
        w->base_jitter = 0.0;
    }

    if ( options->faults & ROCKETNET_DST_FAULT_LOSS )
    {
        static const int rates[] = { 0, 1, 2, 5, 10, 20 };
        static const int weights[] = { 10, 30, 30, 15, 10, 5 };
        const int roll = (int) rocketnet_dst_substream_uniform( &w->fault_loss, 0, 99 );
        int total = 0;
        int chosen = 0;

        for ( int i = 0; i < 6; i++ )
        {
            total += weights[i];
            if ( roll < total )
            {
                chosen = rates[i];
                break;
            }
        }

        w->loss_percent = (float) chosen;
    }
    else
    {
        w->loss_percent = 0.0f;
    }

    if ( options->faults & ROCKETNET_DST_FAULT_DUP )
    {
        static const int rates[] = { 0, 1, 2, 5 };
        static const int weights[] = { 40, 30, 20, 10 };
        const int roll = (int) rocketnet_dst_substream_uniform( &w->fault_dup, 0, 99 );
        int total = 0;
        int chosen = 0;

        for ( int i = 0; i < 4; i++ )
        {
            total += weights[i];
            if ( roll < total )
            {
                chosen = rates[i];
                break;
            }
        }

        w->dup_percent = (float) chosen;
    }
    else
    {
        w->dup_percent = 0.0f;
    }

    int lo = 2, hi = 8;

    if ( options->shape == ROCKETNET_DST_SHAPE_PUSH )
    {
        lo = 2;
        hi = 4;
    }

    int count = (int) rocketnet_dst_substream_uniform( &schedule, (uint64_t) lo, (uint64_t) hi );
    if ( options->clients > 0 )
    {
        count = options->clients;
    }
    w->num_clients = count;
    for ( int i = 0; i < w->num_clients; i++ )
    {
        char name[32];
        snprintf( name, sizeof( name ), "schedule.c%d", i );
        rocketnet_dst_substream_init( &w->client[i].schedule, seed, name );
        snprintf( name, sizeof( name ), "traffic.c%d", i );
        rocketnet_dst_substream_init( &w->client[i].traffic, seed, name );
        w->client[i].next_connect_step = (int) rocketnet_dst_substream_uniform( &w->client[i].schedule, 0, 99 );

        const int runs = rocketnet_dst_substream_real( &w->client[i].schedule ) < 0.2 ? 1 : 0;
        w->client[i].churn_step = runs ? (int) rocketnet_dst_substream_uniform( &w->client[i].schedule, 0,
                                                                                options->fault_steps > 1 ? options->fault_steps - 1 : 0 )
                                       : -1;
        w->client[i].generation = 0;
        w->client[i].pause_until = -1;
        w->client[i].last_state = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
        w->client[i].end_state = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
        w->client[i].last_server_slot = -1;
    }
}

// A step drawn inside the fault window, at least a tenth of the way in so the
// clients have connected, and leaving room for a window of the given length.

static inline uint64_t rocketnet_dst_schedule_draw_step( struct rocketnet_dst_schedule_t * s, uint64_t fault_steps,
                                                         uint64_t length )
{
    const uint64_t lo = fault_steps / 10;
    const uint64_t hi = fault_steps > length + lo ? fault_steps - length : lo;
    return rocketnet_dst_substream_uniform( &s->draws, lo, hi > lo ? hi : lo );
}

// Every draw a schedule makes comes from "schedule.<name>"; a run with no
// schedule makes none.

static inline int rocketnet_dst_schedule_draw( rocketnet_dst_world_t * w, uint64_t seed,
                                               const struct rocketnet_dst_options_t * options )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    s->kind = options->schedule;
    s->witness_client = -1;
    if ( s->kind <= ROCKETNET_DST_SCHEDULE_NONE || s->kind >= ROCKETNET_DST_NUM_SCHEDULES )
    {
        s->kind = ROCKETNET_DST_SCHEDULE_NONE;
        return 1;
    }

    char name[64];
    snprintf( name, sizeof( name ), "schedule.%s", rocketnet_dst_schedule_name( s->kind ) );
    rocketnet_dst_substream_init( &s->draws, seed, name );

    const uint64_t fault_steps = options->fault_steps;

    switch ( s->kind )
    {
        case ROCKETNET_DST_SCHEDULE_CLOCK_BACK:
            s->endpoint = (int) rocketnet_dst_substream_uniform( &s->draws, 0, (uint64_t) w->num_clients );
            s->start_step = rocketnet_dst_schedule_draw_step( s, fault_steps, fault_steps / 4 );
            s->amount = rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_CLOCK_BACK_MIN_MS,
                                                         ROCKETNET_DST_CLOCK_BACK_MAX_MS );
            break;
        case ROCKETNET_DST_SCHEDULE_SEQ_GAP:
            s->side = (int) rocketnet_dst_substream_uniform( &s->draws, 0, 1 );
            s->endpoint = (int) rocketnet_dst_substream_uniform( &s->draws, 0, (uint64_t) ( w->num_clients - 1 ) );
            s->start_step = rocketnet_dst_schedule_draw_step( s, fault_steps, fault_steps / 4 );
            s->amount = rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_SEQ_GAP_MIN, ROCKETNET_DST_SEQ_GAP_MAX );
            s->pending = 1;
            break;
        case ROCKETNET_DST_SCHEDULE_BURST:
        {
            s->endpoint = (int) rocketnet_dst_substream_uniform( &s->draws, 0, (uint64_t) ( w->num_clients - 1 ) );
            const uint64_t length = rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_BURST_MIN_STEPS,
                                                                     ROCKETNET_DST_BURST_MAX_STEPS );
            s->start_step = rocketnet_dst_schedule_draw_step( s, fault_steps, length );
            s->end_step = s->start_step + length;
            s->pending = 1;
            s->held = (uint8_t( * )[ROCKETNET_RECEIVE_CEILING])
                malloc( sizeof( uint8_t[ROCKETNET_RECEIVE_CEILING] ) * ROCKETNET_DST_BURST_CAPACITY );
            if ( !s->held )
            {
                return 0;
            }
            w->check.allocation_balance++;
            break;
        }
        case ROCKETNET_DST_SCHEDULE_DUP_STORM:
        {
            const uint64_t length = rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_DUP_STORM_MIN_STEPS,
                                                                     ROCKETNET_DST_DUP_STORM_MAX_STEPS );
            s->start_step = rocketnet_dst_schedule_draw_step( s, fault_steps, length );
            s->end_step = s->start_step + length;
            break;
        }
        case ROCKETNET_DST_SCHEDULE_OUTAGE:
        case ROCKETNET_DST_SCHEDULE_OUTAGE_PAST:
        {
            // a dark subset that leaves at least one client bright (every
            // run has two clients or more), the direction (outage-past is
            // dark both ways, so both ends time out), the start, and T - 1 s
            // or T + 1 s
            // (a run of one client, --clients 1, has no bright one: nothing goes dark)
            const uint64_t all = ( 1ULL << w->num_clients ) - 1;
            s->dark_mask = w->num_clients < 2 ? 0 : (uint32_t) rocketnet_dst_substream_uniform( &s->draws, 1, all - 1 );
            if ( options->outage_control )
            {
                s->dark_mask = 0;   // the control run: the same draws, no client dark
            }
            s->direction = s->kind == ROCKETNET_DST_SCHEDULE_OUTAGE
                               ? (int) rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_OUTAGE_C2S, ROCKETNET_DST_OUTAGE_BOTH )
                               : ROCKETNET_DST_OUTAGE_BOTH;
            const uint64_t length = s->kind == ROCKETNET_DST_SCHEDULE_OUTAGE ? ROCKETNET_DST_OUTAGE_STEPS
                                                                             : ROCKETNET_DST_OUTAGE_PAST_STEPS;
            s->start_step = rocketnet_dst_schedule_draw_step( s, fault_steps, length );
            s->end_step = s->start_step + length;
            break;
        }
        case ROCKETNET_DST_SCHEDULE_SEND_FAIL:
        {
            s->side = (int) rocketnet_dst_substream_uniform( &s->draws, 0, 1 );
            const uint64_t length = rocketnet_dst_substream_uniform( &s->draws, ROCKETNET_DST_SEND_FAIL_MIN_STEPS,
                                                                     ROCKETNET_DST_SEND_FAIL_MAX_STEPS );
            s->start_step = rocketnet_dst_schedule_draw_step( s, fault_steps, length );
            s->end_step = s->start_step + length;
            break;
        }
        default:
            // seq-wrap-31 and seq-wrap-32 draw nothing: the process's session
            // sequence start (ROCKETNET_DEV_SESSION_SEQUENCE_START) is the fault
            break;
    }

    return 1;
}

static inline void rocketnet_dst_outage_open_window( rocketnet_dst_world_t * w );

// The schedule's per-step faults, applied before the endpoints update.

static inline void rocketnet_dst_schedule_step( rocketnet_dst_world_t * w )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    if ( s->kind == ROCKETNET_DST_SCHEDULE_CLOCK_BACK && w->step == s->start_step )
    {
        s->clock_offset[s->endpoint] -= (double) s->amount * 0.001;
        s->fired++;
        rocketnet_dst_schedule_trace( w, (uint64_t) s->endpoint, s->amount, 0 );
    }

    if ( rocketnet_dst_outage_run( w ) && w->step == s->start_step )
    {
        rocketnet_dst_outage_open_window( w );
    }

    if ( s->kind == ROCKETNET_DST_SCHEDULE_BURST && s->pending && w->step >= s->start_step )
    {
        // the window opens at the first step the drawn client holds a session
        // on both ends; the drawn length is shortened to end inside the window
        rocketnet_dst_client_t * client = &w->client[s->endpoint];
        const int slot = rocketnet_dst_burst_slot( w, s->endpoint );
        if ( client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && slot >= 0 &&
             w->step < w->fault_steps )
        {
            uint64_t length = s->end_step > s->start_step ? s->end_step - s->start_step : ROCKETNET_DST_BURST_MIN_STEPS;
            const uint64_t remaining = w->fault_steps - w->step;
            if ( length > remaining )
            {
                length = remaining;
            }

            // end before a drawn churn replaces the session; a churn this
            // step leaves the window unopened until the client is back

            if ( client->churn_step >= 0 && (uint64_t) client->churn_step < w->step + length )
            {
                length = (uint64_t) client->churn_step > w->step ? (uint64_t) client->churn_step - w->step : 0;
            }
            if ( length == 0 )
            {
                return;
            }
            s->start_step = w->step;
            s->end_step = w->step + length;
            s->burst_slot = slot;
            s->pending = 0;
            s->fired++;

            // the burst's two directions have no rate: this one update stages
            // the whole drawn batch and the held window releases it whole

            (void) rocketnet_message_client_set_send_kbps( client->message, 0 );
            (void) rocketnet_message_server_set_send_kbps( w->message_server, slot, 0 );
            rocketnet_dst_schedule_trace( w, (uint64_t) s->endpoint, length, 0 );
        }
    }

    if ( s->kind == ROCKETNET_DST_SCHEDULE_BURST && !s->pending && w->step == s->start_step + 1 )
    {
        // the one burst update was rate-free; the rest of the run is not
        const int slot = s->burst_slot;
        (void) rocketnet_message_client_set_send_kbps( w->client[s->endpoint].message,
                                                       ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
        (void) rocketnet_message_server_set_send_kbps( w->message_server, slot, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
    }

    if ( s->kind == ROCKETNET_DST_SCHEDULE_SEQ_GAP && s->pending && w->step >= s->start_step &&
         w->step < w->fault_steps )
    {
        // fires at the first step the drawn client holds a session on both ends
        rocketnet_dst_client_t * client = &w->client[s->endpoint];
        if ( client->protocol.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            return;
        }

        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            if ( rocketnet_dst_world_client_for_slot( w, slot ) != s->endpoint ||
                 rocketnet_client_slots_client_id( w->server.client_slots, slot ) != client->generation * 256 + (uint64_t) s->endpoint )
            {
                continue;
            }
            if ( s->side == 0 )
            {
                client->protocol.send_sequence_session += s->amount;
            }
            else
            {
                const int mapping = rocketnet_client_slots_mapping_index( w->server.client_slots, slot );
                struct rocketnet_encryption_mapping_t * entry = rocketnet_encryption_map_entry( w->server.encryption_map, mapping );
                if ( !entry )
                {
                    return;
                }
                entry->send_sequence_session += s->amount;
            }
            s->pending = 0;
            s->fired++;
            rocketnet_dst_schedule_trace( w, (uint64_t) s->side, (uint64_t) s->endpoint, s->amount );
            return;
        }
    }
}

// What the schedule observes after the step: a receiver's highest opened
// sequence rising past the replay history in one step (seq-gap), and a
// session's send sequence crossing 2^31 or 2^32 (seq-wrap).

static inline void rocketnet_dst_schedule_observe( rocketnet_dst_world_t * w, int session, uint64_t identity,
                                                   uint64_t highest_opened )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    if ( session < 0 || session >= ROCKETNET_DST_CHECK_MAX_SESSIONS )
    {
        return;
    }

    if ( s->replay_identity[session] == identity && s->replay_last[session] > 0 &&
         highest_opened > s->replay_last[session] )
    {
        const uint64_t jump = highest_opened - s->replay_last[session];
        if ( jump > s->max_sequence_jump )
        {
            s->max_sequence_jump = jump;
        }
    }

    s->replay_identity[session] = identity;
    s->replay_last[session] = highest_opened;
}

static inline void rocketnet_dst_schedule_observe_wrap( rocketnet_dst_world_t * w, int index )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    rocketnet_dst_client_t * client = &w->client[index];
    const uint64_t boundary = s->kind == ROCKETNET_DST_SCHEDULE_SEQ_WRAP_31   ? ( 1ULL << 31 )
                              : s->kind == ROCKETNET_DST_SCHEDULE_SEQ_WRAP_32 ? ( 1ULL << 32 )
                                                                              : 0;
    if ( boundary == 0 || client->wrap_generation == client->generation ||
         client->protocol.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ||
         client->protocol.send_sequence_session <= boundary )
    {
        return;
    }
    client->wrap_generation = client->generation;
    s->fired++;
    rocketnet_dst_schedule_trace( w, (uint64_t) index, client->generation, client->protocol.send_sequence_session );
}

// The skew check's bound for endpoint e: the clock-back endpoint's is the
// schedule's largest step back (plus 1 ms of rounding), every other's 0.45 s.

static inline double rocketnet_dst_skew_bound( const rocketnet_dst_world_t * w, int e )
{
    if ( w->schedule.kind == ROCKETNET_DST_SCHEDULE_CLOCK_BACK && e == w->schedule.endpoint )
    {
        return (double) ( ROCKETNET_DST_CLOCK_BACK_MAX_MS + 1 ) * 0.001;
    }
    return ROCKETNET_DST_SKEW_BOUND_SECONDS;
}

// outage: the window opens. Each dark client that holds a session on both
// ends now is the session the schedule judges: outage requires it to
// survive, outage-past to end by the timeout on both ends (which explains
// its end). The OUTAGE_BRIGHT witness takes the first bright client that has
// a delivery gap measured before the window, so bright_unaffected judges it.

static inline void rocketnet_dst_outage_open_window( rocketnet_dst_world_t * w )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    for ( int i = 0; i < w->num_clients; i++ )
    {
        rocketnet_dst_client_t * client = &w->client[i];
        const int slot = rocketnet_dst_world_slot_for_client( w, i );
        if ( rocketnet_dst_outage_dark( w, i ) && client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             slot >= 0 )
        {
            s->dark_generation[i] = client->generation;
            s->dark_slot[i] = slot;
            s->dark_client_live[i] = 1;
            s->dark_server_live[i] = 1;
            s->dark_client_heard[i] = client->protocol.last_receive_time;
            const int mapping = rocketnet_client_slots_mapping_index( w->server.client_slots, slot );
            const struct rocketnet_encryption_mapping_t * entry = rocketnet_encryption_map_entry( w->server.encryption_map, mapping );
            s->dark_server_heard[i] = entry ? entry->control->last_receive_time : w->now;
            s->resend_delay_low[i][0] = 1.0e9;
            s->resend_delay_low[i][1] = 1.0e9;
            s->remote_delay_low[i][0] = 1.0e9;
            s->remote_delay_low[i][1] = 1.0e9;
            if ( s->kind == ROCKETNET_DST_SCHEDULE_OUTAGE_PAST )
            {
                client->explained_generation = client->generation;
            }
        }
        if ( w->witness == ROCKETNET_DST_WITNESS_OUTAGE_BRIGHT && s->witness_client < 0 && !rocketnet_dst_outage_dark( w, i ) &&
             ( s->gap_before[i][0] > 0 || s->gap_before[i][1] > 0 ) )
        {
            s->witness_client = i;
        }
    }
    rocketnet_dst_schedule_trace( w, s->dark_mask, (uint64_t) s->direction, s->end_step - s->start_step );
}

// outage: one delivery to client i's session `generation` ([0] at the
// server, [1] at the client): the gap since the session's last one, before
// the window or inside it (bright_unaffected).

static inline void rocketnet_dst_outage_delivery( rocketnet_dst_world_t * w, int index, int direction, uint64_t generation )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    if ( !rocketnet_dst_outage_run( w ) || index < 0 || index >= w->num_clients )
    {
        return;
    }

    if ( s->delivery_generation[index][direction] == generation && generation != 0 )
    {
        const uint64_t gap = w->step - s->delivery_step[index][direction];
        uint64_t * longest = w->step < s->start_step ? &s->gap_before[index][direction]
                                                     : ( w->step < s->end_step ? &s->gap_inside[index][direction] : NULL );
        if ( longest && gap > *longest )
        {
            *longest = gap;
        }
    }
    s->delivery_step[index][direction] = w->step;
    s->delivery_generation[index][direction] = generation;
}

// outage-past: a judged dark session ended on one end (server_side 1: its
// slot). It ended by the silence timeout, and no earlier than T after that
// end last heard the other.

static inline void rocketnet_dst_outage_end( rocketnet_dst_world_t * w, int index, int server_side, int reason )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    int * live = server_side ? &s->dark_server_live[index] : &s->dark_client_live[index];
    if ( !*live )
    {
        return;
    }
    *live = 0;
    if ( s->kind != ROCKETNET_DST_SCHEDULE_OUTAGE_PAST )
    {
        return;     // outage: the end itself is no_unscheduled_end's
    }

    s->outage_ends++;
    const double heard = server_side ? s->dark_server_heard[index] : s->dark_client_heard[index];
    const int timeout_reason = server_side ? ROCKETNET_DISCONNECT_REASON_TIMED_OUT
                                           : ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_TIMED_OUT;
    rocketnet_dst_check_outage_end( &w->check, index, server_side, reason, timeout_reason, w->now - heard,
                                    (double) ROCKETNET_DST_TOKEN_TIMEOUT_SECONDS );
}

// outage: after each step. Records each judged session's last receive on
// both ends while it holds; judges probe_paced over each 100 steps in which
// a sender was held; at the
// window's last step judges bright_unaffected and (outage-past) that both
// ends of every judged session have ended.

static inline void rocketnet_dst_outage_observe( rocketnet_dst_world_t * w )
{
    struct rocketnet_dst_schedule_t * s = &w->schedule;
    if ( !rocketnet_dst_outage_run( w ) || w->step < s->start_step )
    {
        return;
    }

    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( s->dark_generation[i] == 0 )
        {
            continue;
        }
        const uint64_t id = s->dark_generation[i] * 256 + (uint64_t) i;
        const int slot = s->dark_slot[i];
        const int slot_holds = rocketnet_client_slots_connected( w->server.client_slots, slot ) &&
                               rocketnet_client_slots_client_id( w->server.client_slots, slot ) == id;
        if ( s->dark_client_live[i] )
        {
            s->dark_client_heard[i] = w->client[i].protocol.last_receive_time;
        }
        if ( s->dark_server_live[i] && slot_holds )
        {
            const int mapping = rocketnet_client_slots_mapping_index( w->server.client_slots, slot );
            const struct rocketnet_encryption_mapping_t * entry = rocketnet_encryption_map_entry( w->server.encryption_map, mapping );
            if ( entry )
            {
                s->dark_server_heard[i] = entry->control->last_receive_time;
            }
        }
        if ( !s->dark_server_live[i] && rocketnet_client_slots_connected( w->server.client_slots, slot ) &&
             rocketnet_client_slots_client_id( w->server.client_slots, slot ) != id )
        {
            s->dark_slot_reused[i] = 1;
        }

        if ( w->step >= s->end_step )
        {
            continue;
        }

        // probe_paced: a sender held at the end of this step (no ack progress
        // for RN_PROBE_SECONDS, so only U may be resent) has the frames it
        // starts in the next step counted; each 100 counted steps are one
        // judged second. A held sender may still send numbers it never sent
        // (m9.3), so those first sends are counted beside the frames. In a
        // one-way outage the side the bright direction reaches also answers
        // its remote endpoint's packets: it is counted only while the remote endpoint is held too,
        // and the remote endpoint's shortest resend delay and first sends bound the
        // answers.
        uint64_t sent[2] = { 0, 0 };
        int has_sent[2] = { 0, 0 };
        int held[2] = { 0, 0 };
        double delay[2] = { 0.0, 0.0 };
        for ( int side = 0; side < 2; side++ )
        {
            const int live = side ? s->dark_server_live[i] && slot_holds : s->dark_client_live[i];
            struct rn_message_object_t * object = side ? &w->message_server->object : &w->client[i].message->object;
            const struct rn_message_connection_t * connection = live ? rn_message_object_connection( object, side ? slot : 0 ) : NULL;
            struct rocketnet_conditions_t conditions;
            memset( &conditions, 0, sizeof( conditions ) );
            if ( connection && rn_message_object_conditions( object, side ? slot : 0, &conditions ) )
            {
                sent[side] = rocketnet_dst_outage_numbers_sent( connection );
                has_sent[side] = 1;
                if ( conditions.seconds_since_progress >= RN_PROBE_SECONDS )
                {
                    held[side] = 1;
                    delay[side] = connection->ordered.ack.resend_delay;
                }
            }
        }
        for ( int side = 0; side < 2; side++ )
        {
            const int reaches = rocketnet_dst_outage_reaches( s, side );
            if ( s->probe_held[i][side] )
            {
                for ( int from = 0; from < 2; from++ )
                {
                    if ( ( from == side || reaches ) && has_sent[from] && sent[from] > s->numbers_sent[i][from] )
                    {
                        s->first_sends[i][side] += sent[from] - s->numbers_sent[i][from];
                    }
                }
                s->held_steps[i][side]++;
                if ( s->held_steps[i][side] == ROCKETNET_DST_OUTAGE_SECOND_STEPS )
                {
                    s->seconds_judged++;
                    if ( s->frames[i][side] > s->frames_max )
                    {
                        s->frames_max = s->frames[i][side];
                    }
                    rocketnet_dst_check_probe_paced( &w->check, i, side, s->frames[i][side], s->resend_delay_low[i][side],
                                                     reaches ? s->remote_delay_low[i][side] : 0.0, s->first_sends[i][side] );
                    s->held_steps[i][side] = 0;
                    s->frames[i][side] = 0;
                    s->first_sends[i][side] = 0;
                    s->resend_delay_low[i][side] = 1.0e9;
                    s->remote_delay_low[i][side] = 1.0e9;
                }
            }
        }
        for ( int side = 0; side < 2; side++ )
        {
            const int reaches = rocketnet_dst_outage_reaches( s, side );
            if ( has_sent[side] )
            {
                s->numbers_sent[i][side] = sent[side];
            }
            s->probe_held[i][side] = held[side] && ( !reaches || held[1 - side] );
            if ( s->probe_held[i][side] )
            {
                if ( delay[side] < s->resend_delay_low[i][side] )
                {
                    s->resend_delay_low[i][side] = delay[side];
                }
                if ( reaches && delay[1 - side] < s->remote_delay_low[i][side] )
                {
                    s->remote_delay_low[i][side] = delay[1 - side];
                }
            }
        }
    }

    if ( w->step + 1 != s->end_step )
    {
        return;
    }

    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( rocketnet_dst_outage_dark( w, i ) )
        {
            if ( s->kind == ROCKETNET_DST_SCHEDULE_OUTAGE_PAST && ( s->dark_client_live[i] || s->dark_server_live[i] ) )
            {
                char what[160];
                snprintf( what, sizeof( what ), "client=%d generation=%llu client_live=%d server_live=%d past the timeout",
                          i, (unsigned long long) s->dark_generation[i], s->dark_client_live[i], s->dark_server_live[i] );
                rocketnet_dst_check_fail( &w->check, "outage_end", what );
            }
            continue;
        }
        for ( int direction = 0; direction < 2; direction++ )
        {
            // the gap still open at the window's end counts as inside it
            const uint64_t open = s->end_step - s->delivery_step[i][direction];
            if ( s->delivery_generation[i][direction] == w->client[i].generation && open > s->gap_inside[i][direction] )
            {
                s->gap_inside[i][direction] = open;
            }
            if ( w->bright_control )
            {
                rocketnet_dst_check_bright_unaffected( &w->check, i, direction, w->bright_control[i * 2 + direction],
                                                       s->gap_inside[i][direction], ROCKETNET_DST_OUTAGE_RESEND_MAX_MS / 10 );
            }
        }
    }
}

// no_unscheduled_end, the server's side: a slot that stops holding a session
// while its client still holds that same session CONNECTED ended a session
// the client did not end. Only a script explains that. A slot whose client
// already left the session (a churn, its own end) is judged on the client's
// side. Called each step before rocketnet_dst_check_slot records the slot.

static inline void rocketnet_dst_world_slot_end( rocketnet_dst_world_t * w, int slot, int connected, uint64_t client_id )
{
    if ( !w->check.slot_connected[slot] || ( connected && client_id == w->check.slot_client_id[slot] ) )
    {
        return;
    }

    const uint64_t ended = w->check.slot_client_id[slot];
    const int index = (int) ( ended & 0xFF );
    const uint64_t generation = ended >> 8;
    if ( index < w->num_clients && rocketnet_dst_outage_run( w ) && w->schedule.dark_generation[index] == generation &&
         generation != 0 && w->schedule.dark_slot[index] == slot )
    {
        rocketnet_dst_outage_end( w, index, 1, w->server_end_reasons[slot] );
    }
    if ( index >= w->num_clients || generation != w->client[index].generation ||
         w->client[index].protocol.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        return;
    }

    char what[64];
    snprintf( what, sizeof( what ), "server ended slot=%d reason=%d", slot, w->server_end_reasons[slot] );
    rocketnet_dst_check_session_end( &w->check, index, generation, w->client[index].explained_generation, what, 1 );
}

// no_unscheduled_end, the client's side: each session end (CONNECTED left,
// or a connect that ends in an error state) is one a script explains.

static inline void rocketnet_dst_world_client_end( rocketnet_dst_world_t * w, int index )
{
    rocketnet_dst_client_t * client = &w->client[index];
    const int state = client->protocol.state;
    const int left = client->end_state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                     state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
    const int failed = state < 0 && client->end_state >= 0 && !left;
    client->end_state = state;
    if ( !left && !failed )
    {
        return;
    }

    w->session_ends++;
    if ( left && rocketnet_dst_outage_run( w ) && w->schedule.dark_generation[index] == client->generation )
    {
        rocketnet_dst_outage_end( w, index, 0, state );
    }

    // a dark client's connect that the window overlapped lost its handshake
    // packets to the outage: the schedule explains its failure
    if ( failed && rocketnet_dst_outage_dark( w, index ) && client->connect_step < w->schedule.end_step &&
         w->step >= w->schedule.start_step )
    {
        client->explained_generation = client->generation;
    }
    char what[64];
    snprintf( what, sizeof( what ), "client %s state=%d", left ? "left CONNECTED" : "connect failed", state );
    rocketnet_dst_check_session_end( &w->check, index, client->generation, client->explained_generation, what, 1 );
}

static inline int rocketnet_dst_world_create( rocketnet_dst_world_t * w, uint64_t seed,
                                              const struct rocketnet_dst_options_t * options )
{
    memset( w, 0, sizeof( *w ) );
    w->seed = seed;
    w->faults = options->faults;
    w->witness = options->witness;
    w->outage_control = options->outage_control;
    w->fault_steps = options->fault_steps;
    w->now = 1000.0;
    w->unix_ts = 1700000000ULL;
    w->server_incarnation = 1;
    rocketnet_dst_trace_init( &w->trace );
    rocketnet_dst_world_draw( w, seed, options );

    struct rocketnet_dst_substream_t wire;
    rocketnet_dst_substream_init( &wire, seed, "wire" );

    const uint64_t simulator_seed = rocketnet_dst_substream_draw( &wire );
    w->sim = rocketnet_network_simulator_create( NULL, 4096, simulator_seed );
    if ( !w->sim )
    {
        return 0;
    }
    rocketnet_network_simulator_config( w->sim, w->base_latency, w->base_jitter, w->loss_percent, w->dup_percent );

    if ( !rocketnet_address_parse( &w->server_address, "203.0.113.10:40000" ) )
    {
        return 0;
    }

    memset( &w->server_counters, 0, sizeof( w->server_counters ) );
    memset( &w->client_counters, 0, sizeof( w->client_counters ) );
    rocketnet_dst_check_init( &w->check, seed, 0 );
    if ( !rocketnet_dst_schedule_draw( w, seed, options ) )
    {
        return 0;
    }
    w->server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    w->server.send_rate = 10.0;
    memcpy( w->server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( w->server.backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( w->server.server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( w->server.server_secret_key, rocketnet_test_server_secret_key, 32 );
    randombytes_buf( w->server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    w->server.cookie_lifetime_seconds = 3;
    w->server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    w->server.cookie_rotation_seconds = 60;
    w->server.cookie_key_rotation_time = w->now + 60.0;
    w->server.handshake_deadline_seconds = 5.0;
    w->server.advertised_ipv4 = w->server_address;
    w->server.counters = &w->server_counters;
    w->server.cookie_reply_sources = w->server_cookie_reply_sources;
    w->server.cookie_reply_source_words = 64;
    w->server.encryption_map = rocketnet_encryption_map_create( NULL, 16 );
    w->server.used_token_history = rocketnet_used_token_history_create( NULL, 256 );
    w->server.client_slots = rocketnet_client_slots_create( NULL, ROCKETNET_DST_MAX_CLIENTS );
    w->server.acks = rocketnet_server_acks_create( NULL, ROCKETNET_DST_MAX_CLIENTS );
    w->server.connection_ids = w->server_connection_ids;
    w->server.acked_connection_ids = w->server_acked_connection_ids;
    w->server.end_reasons = w->server_end_reasons;
    w->server.receive_allowances = w->server_receive_allowances;
    w->server.receive_bytes_per_second = rocketnet_server_receive_allowance_rate( ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );     // on, at the client default: the DST world exercises the allowance
    w->server.defer_payload_acks = 1;
    if ( !w->server.encryption_map || !w->server.used_token_history || !w->server.client_slots ||
         !w->server.acks )
    {
        return 0;
    }
    rocketnet_server_protocol_update( &w->server, w->now );

    w->server_endpoint.world = w;
    w->server_endpoint.is_server = 1;
    w->server_endpoint.pending_frame = -1;
    w->server_endpoint.payload = (uint8_t( * )[ROCKETNET_RECEIVE_PAYLOAD_BYTES])
        malloc( sizeof( uint8_t[ROCKETNET_RECEIVE_PAYLOAD_BYTES] ) * ROCKETNET_DST_MAX_PAYLOADS );
    if ( !w->server_endpoint.payload )
    {
        return 0;
    }
    w->check.allocation_balance++;

    rocketnet_message_default_config( &w->message_config );
    w->message_config.num_channels = ROCKETNET_DST_NUM_CHANNELS;
    if ( rocketnet_dst_outage_run( w ) )
    {
        // the outage schedules' small send side, so a dark session's
        // offers reach the library's no-room refusal (see ROCKETNET_DST_OUTAGE_*)
        w->message_config.send_message_buffer_bytes = ROCKETNET_DST_OUTAGE_SEND_BYTES;
        w->message_config.send_queue_messages = ROCKETNET_DST_OUTAGE_SEND_MESSAGES;
        w->message_config.reliable_messages_in_flight = ROCKETNET_DST_OUTAGE_WINDOW;
        w->message_config.max_message_bytes = ROCKETNET_DST_OUTAGE_MAX_MESSAGE;
        w->message_config.message_resend_delay_max_ms = ROCKETNET_DST_OUTAGE_RESEND_MAX_MS;
        if ( w->message_config.message_resend_delay_initial_ms > ROCKETNET_DST_OUTAGE_RESEND_MAX_MS )
        {
            w->message_config.message_resend_delay_initial_ms = ROCKETNET_DST_OUTAGE_RESEND_MAX_MS;
        }
        // a numbering window of 64 holds fewer numbers than the backlog a
        // send can queue behind, so it adds its round trips to the slack
        w->check.in_flight_steps = ROCKETNET_DST_OUTAGE_IN_FLIGHT_STEPS;
    }
    if ( options->reliable_timeout_ms > 0 )
    {
        w->reliable_timeout_run = 1;
        // the config check keeps four initial resend delays inside the timeout
        w->message_config.reliable_message_timeout_ms = options->reliable_timeout_ms;
        if ( w->message_config.message_resend_delay_initial_ms > options->reliable_timeout_ms / 4 )
        {
            w->message_config.message_resend_delay_initial_ms = options->reliable_timeout_ms / 4;
        }
    }

    struct rn_message_link_t server_link;
    rocketnet_dst_endpoint_link( &server_link, &w->server_endpoint );
    w->message_server = (struct rocketnet_message_server_t *) rn_message_object_create(
        NULL, &server_link, 1, ROCKETNET_DST_MAX_CLIENTS, &w->message_config, NULL, 0,
        ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, w->now );
    if ( !w->message_server )
    {
        return 0;
    }

    for ( int i = 0; i < w->num_clients; i++ )
    {
        rocketnet_dst_client_t * client = &w->client[i];
        char address_string[64];
        snprintf( address_string, sizeof( address_string ), "198.51.100.%d:56789", 10 + i );
        if ( !rocketnet_address_parse( &client->address, address_string ) )
        {
            return 0;
        }
        rocketnet_client_protocol_init( &client->protocol, &w->client_counters );
        client->protocol.defer_payload_acks = 1;
        client->endpoint.world = w;
        client->endpoint.client_index = i;
        client->endpoint.pending_frame = -1;
        client->endpoint.payload = (uint8_t( * )[ROCKETNET_RECEIVE_PAYLOAD_BYTES])
            malloc( sizeof( uint8_t[ROCKETNET_RECEIVE_PAYLOAD_BYTES] ) * ROCKETNET_DST_MAX_PAYLOADS );
        if ( !client->endpoint.payload )
        {
            return 0;
        }
        w->check.allocation_balance++;

        struct rn_message_link_t client_link;
        rocketnet_dst_endpoint_link( &client_link, &client->endpoint );
        client->message = (struct rocketnet_message_client_t *) rn_message_object_create(
            NULL, &client_link, 0, 1, &w->message_config, NULL, 0,
            ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, w->now );
        if ( !client->message )
        {
            return 0;
        }
    }

    return 1;
}

static inline int rocketnet_dst_world_destroy( rocketnet_dst_world_t * w )
{
    if ( w->message_server )
    {
        rocketnet_message_server_destroy( w->message_server );
    }

    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( w->client[i].message )
        {
            rocketnet_message_client_destroy( w->client[i].message );
        }
    }

    if ( w->server.encryption_map )
    {
        rocketnet_encryption_map_destroy( w->server.encryption_map );
    }
    if ( w->server.used_token_history )
    {
        rocketnet_used_token_history_destroy( w->server.used_token_history );
    }
    if ( w->server.client_slots )
    {
        rocketnet_client_slots_destroy( w->server.client_slots );
    }
    if ( w->server.acks )
    {
        rocketnet_server_acks_destroy( w->server.acks );
    }
    if ( w->sim )
    {
        rocketnet_network_simulator_destroy( w->sim );
    }

    if ( w->schedule.held )
    {
        free( w->schedule.held );
        if ( w->check.allocation_balance > 0 )
        {
            w->check.allocation_balance--;
        }
    }

    if ( w->server_endpoint.payload )
    {
        free( w->server_endpoint.payload );
        if ( w->check.allocation_balance > 0 )
        {
            w->check.allocation_balance--;
        }
    }

    for ( int i = 0; i < w->num_clients; i++ )
    {
        if ( w->client[i].endpoint.payload )
        {
            free( w->client[i].endpoint.payload );
            if ( w->check.allocation_balance > 0 )
            {
                w->check.allocation_balance--;
            }
        }
    }

    const int leaked = w->check.allocation_balance != 0;
    memset( w, 0, sizeof( *w ) );

    return leaked;
}

static inline int rocketnet_dst_run_world_seed( uint64_t seed, const struct rocketnet_dst_options_t * options,
                                                struct rocketnet_dst_result_t * result, const uint64_t * bright_control )
{
    memset( result, 0, sizeof( *result ) );
    result->seed = seed;

    // Refuse before allocating: past the bound the (int) k narrowings would wrap.
    if ( !rocketnet_dst_steps_valid( options->steps ) || !rocketnet_dst_steps_valid( options->fault_steps ) )
    {
        return 0;
    }

    rocketnet_dst_world_t * w = (rocketnet_dst_world_t *) calloc( 1, sizeof( rocketnet_dst_world_t ) );
    if ( !w )
    {
        return 0;
    }
    if ( !rocketnet_dst_world_create( w, seed, options ) )
    {
        rocketnet_dst_world_destroy( w );
        free( w );
        return 0;
    }
    w->bright_control = bright_control;

    for ( uint64_t k = 0; k < options->steps; k++ )
    {
        w->step = k;
        w->check.step = k;
        w->now = 1000.0 + (double) k * 0.01;
        w->unix_ts = 1700000000ULL + (uint64_t) ( w->now - 1000.0 );

        if ( options->steps > options->fault_steps && k + 1 == options->fault_steps )
        {
            rocketnet_dst_check_settle( &w->check );
        }
        if ( !w->quiet_applied && options->fault_steps > 0 && k >= options->fault_steps )
        {
            rocketnet_network_simulator_config( w->sim, w->base_latency, 0.0, 0.0f, 0.0f );
            w->quiet_applied = 1;
        }

        if ( options->force_fail_step >= 0 && (int) k == options->force_fail_step )
        {
            rocketnet_dst_fail( w, "forced", "the test forced this check" );
        }

        rocketnet_dst_schedule_step( w );

        for ( int i = 0; i < w->num_clients; i++ )
        {
            rocketnet_dst_client_t * client = &w->client[i];
            if ( client->protocol.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED &&
                 (int) k >= client->next_connect_step &&
                 ( client->pause_until < 0 || (int) k >= client->pause_until ) )
            {
                rocketnet_dst_connect_client( w, i );
                client->pause_until = -1;
            }
            if ( client->churn_step >= 0 && (int) k == client->churn_step &&
                 client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
                int burst_count = 0;
                int burst_bytes = 0;
                client->explained_generation = client->generation;     // the drawn churn ends this session
                if ( rocketnet_dst_outage_run( w ) && w->schedule.dark_generation[i] == client->generation )
                {
                    w->schedule.dark_client_live[i] = 0;                // a churn, not the timeout, ends the client side
                }
                rocketnet_client_protocol_disconnect( &client->protocol, burst, &burst_count, &burst_bytes );
                for ( int b = 0; b < burst_count; b++ )
                {
                    uint8_t * packet = burst + (size_t) b * (size_t) ROCKETNET_SEND_BUDGET;
                    rocketnet_dst_world_trace_send( w, i + 1, 0, packet, burst_bytes );
                    rocketnet_dst_wire_send( w, &client->address, &w->server_address, packet, burst_bytes );
                }
                client->pause_until = (int) k + 50;
                client->next_connect_step = (int) k + 50;
                client->churn_step = -1;
            }
            rocketnet_message_client_update( client->message, w->now );
        }

        for ( int i = 0; i < w->num_clients; i++ )
        {
            rocketnet_dst_client_t * client = &w->client[i];
            if ( client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
                 ( client->pause_until < 0 || (int) k >= client->pause_until ) &&
                 !rocketnet_dst_burst_holding( w, i ) )
            {
                const int offers = rocketnet_dst_outage_dark( w, i ) && rocketnet_dst_outage_open( w ) ? ROCKETNET_DST_OUTAGE_OFFERS : 1;
                for ( int n = 0; n < offers; n++ )
                {
                    rocketnet_dst_send_one( w, i, 0, -1 );
                }
            }
            if ( w->schedule.kind == ROCKETNET_DST_SCHEDULE_BURST && w->step == w->schedule.start_step &&
                 i == w->schedule.endpoint )
            {
                rocketnet_dst_send_burst( w, i, 0, w->schedule.burst_slot, ROCKETNET_DST_BURST_MESSAGES );
            }
            rocketnet_message_client_send_packets( client->message );
        }

        rocketnet_message_server_update( w->message_server, w->now );

        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            const int index = rocketnet_dst_world_client_for_slot( w, slot );
            if ( index >= 0 && index < w->num_clients )
            {
                if ( !rocketnet_dst_burst_holding( w, index ) )
                {
                    const int offers = rocketnet_dst_outage_dark( w, index ) && rocketnet_dst_outage_open( w ) ? ROCKETNET_DST_OUTAGE_OFFERS : 1;
                    for ( int n = 0; n < offers; n++ )
                    {
                        rocketnet_dst_send_one( w, index, 1, slot );
                    }
                }
                if ( w->schedule.kind == ROCKETNET_DST_SCHEDULE_BURST && w->step == w->schedule.start_step &&
                     index == w->schedule.endpoint )
                {
                    rocketnet_dst_send_burst( w, index, 1, slot, ROCKETNET_DST_BURST_MESSAGES );
                }
            }
        }
        rocketnet_message_server_send_packets( w->message_server );

        {
            // the server's one receive queue across every slot: each message
            // names its sender's slot
            const uint8_t * data;
            int bytes;
            int slot = 0;
            while ( rocketnet_message_server_receive( w->message_server, &slot, NULL, &data, &bytes ) )
            {
                uint64_t client_id = rocketnet_client_slots_connected( w->server.client_slots, slot )
                                         ? rocketnet_client_slots_client_id( w->server.client_slots, slot )
                                         : w->check.slot_client_id[slot];
                uint32_t expected_generation = (uint32_t) ( client_id >> 8 );
                rocketnet_dst_check_deliveries( w, slot, expected_generation, w->server_incarnation,
                                                client_id, data, bytes );
                rocketnet_dst_outage_delivery( w, (int) ( client_id & 0xFF ), 0, client_id >> 8 );
                rocketnet_message_server_release( w->message_server, data );
            }
        }

        for ( int i = 0; i < w->num_clients; i++ )
        {
            rocketnet_dst_client_t * client = &w->client[i];
            {
                const uint8_t * data;
                int bytes;
                const uint32_t expected_generation = (uint32_t) client->generation;
                while ( rocketnet_message_client_receive( client->message, NULL, &data, &bytes ) )
                {
                    rocketnet_dst_check_deliveries( w, 16 + i, expected_generation, w->server_incarnation,
                                                    client->generation, data, bytes );
                    rocketnet_dst_outage_delivery( w, i, 1, client->generation );
                    rocketnet_message_client_release( client->message, data );
                }
            }
            if ( client->protocol.state != client->last_state )
            {
                rocketnet_dst_trace_u64( &w->trace, w->step );
                rocketnet_dst_trace_u64( &w->trace, (uint64_t) ( i + 1 ) );
                rocketnet_dst_trace_u64( &w->trace, (uint64_t) client->protocol.state );
                client->last_state = client->protocol.state;
            }
        }

        {
            // each receipt this step's updates surfaced, after both ends'
            // deliveries; a list longer than the harness reads fails, so no
            // receipt goes unjudged
            struct rocketnet_message_receipt_t receipts[ROCKETNET_DST_RECEIPTS];
            for ( int i = 0; i < w->num_clients; i++ )
            {
                const uint64_t identity = (uint64_t) (uint32_t) w->client[i].generation * 256 + (uint64_t) i;
                const int count = rocketnet_message_client_receipts( w->client[i].message, receipts, ROCKETNET_DST_RECEIPTS );
                if ( count > ROCKETNET_DST_RECEIPTS )
                {
                    rocketnet_dst_fail( w, "receipt_list", "a client's receipt list is longer than the harness reads" );
                }
                for ( int r = 0; r < count && r < ROCKETNET_DST_RECEIPTS; r++ )
                {
                    rocketnet_dst_check_receipt_one_outcome( &w->check, 16 + i, identity, receipts[r].serial, receipts[r].status );
                }
            }
            for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
            {
                const int index = rocketnet_dst_world_client_for_slot( w, slot );
                if ( index < 0 || index >= w->num_clients )
                {
                    continue;
                }
                // the session the slot holds, as its sends were recorded
                const uint64_t identity = (uint32_t) ( rocketnet_client_slots_client_id( w->server.client_slots, slot ) >> 8 );
                const int count = rocketnet_message_server_receipts( w->message_server, slot, receipts, ROCKETNET_DST_RECEIPTS );
                if ( count > ROCKETNET_DST_RECEIPTS )
                {
                    rocketnet_dst_fail( w, "receipt_list", "a server slot's receipt list is longer than the harness reads" );
                }
                for ( int r = 0; r < count && r < ROCKETNET_DST_RECEIPTS; r++ )
                {
                    rocketnet_dst_check_receipt_one_outcome( &w->check, slot, identity, receipts[r].serial, receipts[r].status );
                }
            }

            // receipt_outcome_due: each live session's type 5 sends have
            // their outcomes by the receipt deadline (plus the slack the
            // check names)
            const double receipt_deadline = (double) w->message_config.message_resend_delay_max_ms / 1000.0 + ROCKETNET_DST_RECEIPT_DUE_SLACK;
            for ( int i = 0; i < w->num_clients; i++ )
            {
                const int live = w->client[i].message && w->client[i].protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
                const uint64_t identity = (uint64_t) (uint32_t) w->client[i].generation * 256 + (uint64_t) i;
                const uint32_t serial_now = w->client[i].message ? w->client[i].message->object.connections[0].serial : 0;
                rocketnet_dst_check_receipt_outcome_due( &w->check, 16 + i, identity, live, serial_now, w->now, receipt_deadline );
            }
            for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS && slot < w->message_server->object.num_connections; slot++ )
            {
                const int live = rocketnet_message_server_client_connected( w->message_server, slot );
                const uint64_t identity = (uint32_t) ( rocketnet_client_slots_client_id( w->server.client_slots, slot ) >> 8 );
                rocketnet_dst_check_receipt_outcome_due( &w->check, slot, identity, live, w->message_server->object.connections[slot].serial,
                                                         w->now, receipt_deadline );
            }
        }

        w->check.step = k;
        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            const int connected = rocketnet_client_slots_connected( w->server.client_slots, slot );
            const uint64_t client_id = connected ? rocketnet_client_slots_client_id( w->server.client_slots, slot ) : 0;
            rocketnet_dst_world_slot_end( w, slot, connected, client_id );
            rocketnet_dst_check_slot( &w->check, slot, connected, client_id, w->server_incarnation );
            if ( connected )
            {
                const int mapping = rocketnet_client_slots_mapping_index( w->server.client_slots, slot );
                struct rocketnet_encryption_mapping_t * entry =
                    rocketnet_encryption_map_entry( w->server.encryption_map, mapping );
                if ( entry )
                {
                    rocketnet_dst_check_replay( &w->check, slot, (uint64_t) entry->client_id,
                                                entry->replay_session.highest_opened );
                    rocketnet_dst_schedule_observe( w, slot, (uint64_t) entry->client_id,
                                                    entry->replay_session.highest_opened );
                }
            }
        }
        for ( int i = 0; i < w->num_clients && i < ROCKETNET_DST_MAX_CLIENTS; i++ )   // the array bound, which num_clients never passes
        {
            rocketnet_dst_client_t * client = &w->client[i];
            const int terminal_explained = (int) k < (int) w->fault_steps + 1000;
            rocketnet_dst_check_client_state( &w->check, i, client->protocol.state, terminal_explained );
            rocketnet_dst_world_client_end( w, i );
            if ( client->protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                rocketnet_dst_check_replay( &w->check, 16 + i, client->generation,
                                            client->protocol.replay_session.highest_opened );
                rocketnet_dst_schedule_observe( w, 16 + i, client->generation,
                                                client->protocol.replay_session.highest_opened );
            }
            rocketnet_dst_schedule_observe_wrap( w, i );
            rocketnet_dst_check_crashed_silent( &w->check, i + 1, 1, 0 );
            rocketnet_dst_check_clock( &w->check, i + 1, w->now + w->schedule.clock_offset[i + 1], w->now,
                                       rocketnet_dst_skew_bound( w, i + 1 ) );
        }
        rocketnet_dst_check_clock( &w->check, 0, w->now + w->schedule.clock_offset[0], w->now,
                                   rocketnet_dst_skew_bound( w, 0 ) );
        rocketnet_dst_outage_observe( w );

        if ( w->check.failed && !w->failed )
        {
            w->failed = 1;
            w->fail_step = w->check.step;
            snprintf( w->fail_check, sizeof( w->fail_check ), "%s", w->check.check );
            snprintf( w->fail_detail, sizeof( w->fail_detail ), "%s", w->check.detail );
        }
        if ( w->failed )
        {
            break;
        }
    }

    {
        int live[ROCKETNET_DST_CHECK_MAX_SESSIONS];
        uint64_t live_identity[ROCKETNET_DST_CHECK_MAX_SESSIONS];
        for ( int i = 0; i < ROCKETNET_DST_CHECK_MAX_SESSIONS; i++ )
        {
            live[i] = 0;
            live_identity[i] = 0;
        }
        for ( int slot = 0; slot < ROCKETNET_DST_MAX_CLIENTS; slot++ )
        {
            live[slot] = rocketnet_client_slots_connected( w->server.client_slots, slot );
            if ( live[slot] )
            {
                live_identity[slot] = rocketnet_client_slots_client_id( w->server.client_slots, slot );
            }
        }
        for ( int i = 0; i < w->num_clients; i++ )
        {
            live[16 + i] = w->client[i].protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED;
            live_identity[16 + i] = w->client[i].generation;
        }
        // the message objects' counters: the server's, and every client
        // object's (the ones rebuilt during the run were added as they went)
        rocketnet_dst_counters_add( &w->message_counters[0], &w->message_server->object.counters );
        for ( int i = 0; i < w->num_clients; i++ )
        {
            rocketnet_dst_counters_add( &w->message_counters[1], &w->client[i].message->object.counters );
        }

        if ( !w->check.failed )
        {
            w->check.step = options->steps;
            if ( options->steps > options->fault_steps )
            {
                for ( int i = 0; i < w->num_clients; i++ )
                {
                    rocketnet_dst_check_liveness_connected( &w->check, i, 1,
                                                            w->client[i].protocol.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
                }
                rocketnet_dst_check_end( &w->check, w->now, live, live_identity );
            }
            rocketnet_dst_check_conservation( &w->check, &w->server_counters, "server" );
            rocketnet_dst_check_conservation( &w->check, &w->client_counters, "clients" );
            // dup-storm is the duplicate fault at its worst: it explains
            // what the duplicate fault explains, and nothing more
            const int storm = w->schedule.kind == ROCKETNET_DST_SCHEDULE_DUP_STORM;
            const int duplicates = storm || ( options->faults & ROCKETNET_DST_FAULT_DUP ) != 0;
            rocketnet_dst_check_unexplained_refusal( &w->check, &w->server_counters, "server", duplicates, rocketnet_dst_outage_run( w ) );
            rocketnet_dst_check_unexplained_refusal( &w->check, &w->client_counters, "clients", duplicates, rocketnet_dst_outage_run( w ) );

            // the message objects' own counters: the same two laws, every
            // send the library refused seen by the harness and counted by
            // the library alike, and a floor on the reliable sends accepted
            rocketnet_dst_check_conservation( &w->check, &w->message_counters[0], "server messages" );
            rocketnet_dst_check_conservation( &w->check, &w->message_counters[1], "client messages" );
            rocketnet_dst_check_unexplained_refusal( &w->check, &w->message_counters[0], "server messages", duplicates, rocketnet_dst_outage_run( w ) );
            rocketnet_dst_check_unexplained_refusal( &w->check, &w->message_counters[1], "client messages", duplicates, rocketnet_dst_outage_run( w ) );
            rocketnet_dst_check_refusals_agree( &w->check, "server", w->refused[1], &w->message_counters[0] );
            rocketnet_dst_check_refusals_agree( &w->check, "clients", w->refused[0], &w->message_counters[1] );
            rocketnet_dst_check_send_floor( &w->check, "server", w->offered_reliable[1], w->accepted_reliable[1] );
            rocketnet_dst_check_send_floor( &w->check, "clients", w->offered_reliable[0], w->accepted_reliable[0] );

            // outage-past: each judged session's client is back in a newer
            // session and its old slot was taken again
            for ( int i = 0; i < w->num_clients && w->schedule.kind == ROCKETNET_DST_SCHEDULE_OUTAGE_PAST; i++ )
            {
                if ( w->schedule.dark_generation[i] != 0 &&
                     ( w->client[i].generation <= w->schedule.dark_generation[i] || !w->schedule.dark_slot_reused[i] ) )
                {
                    char what[160];
                    snprintf( what, sizeof( what ), "client=%d generation=%llu after %llu, slot=%d reused=%d", i,
                              (unsigned long long) w->client[i].generation, (unsigned long long) w->schedule.dark_generation[i],
                              w->schedule.dark_slot[i], w->schedule.dark_slot_reused[i] );
                    rocketnet_dst_check_fail( &w->check, "outage_reconnect", what );
                }
            }
        }
        if ( w->check.failed && !w->failed )
        {
            w->failed = 1;
            w->fail_step = w->check.step;
            snprintf( w->fail_check, sizeof( w->fail_check ), "%s", w->check.check );
            snprintf( w->fail_detail, sizeof( w->fail_detail ), "%s", w->check.detail );
        }
    }

    result->passed = !w->failed;
    result->trace_hash = w->trace.hash;
    result->schedule_fired = w->schedule.fired;
    result->clock_back_seen = w->schedule.clock_back_seen;
    result->max_sequence_jump = w->schedule.max_sequence_jump;
    result->sequence_gap = ( w->schedule.kind == ROCKETNET_DST_SCHEDULE_SEQ_GAP && w->schedule.fired ) ? w->schedule.amount : 0;
    result->burst_sent = w->schedule.burst_sent;
    result->burst_received = w->schedule.burst_received;
    // what the receivers took twice: the library's duplicate refusals, which
    // the dup fault and the dup-storm schedule both cause
    result->duplicate_payloads =
        rocketnet_counter_value( &w->server_counters, ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED ) +
        rocketnet_counter_value( &w->client_counters, ROCKETNET_COUNTER_CLIENT_PAYLOAD_ALREADY_RECEIVED );
    result->server_reliable_timeouts = w->reliable_timeouts[0];
    result->client_reliable_timeouts = w->reliable_timeouts[1];
    result->message_kicks = w->message_kicks;
    result->budget_aborts = w->budget_aborts;
    for ( int i = 0; i < w->num_clients; i++ )
    {
        result->bright_gap[i * 2] = w->schedule.gap_inside[i][0];
        result->bright_gap[i * 2 + 1] = w->schedule.gap_inside[i][1];
    }
    result->session_ends = w->session_ends;
    result->refused = w->refused[0] + w->refused[1];
    result->accepted_reliable = w->accepted_reliable[0] + w->accepted_reliable[1];
    result->frames_max = w->schedule.frames_max;
    result->seconds_judged = w->schedule.seconds_judged;
    result->outage_ends = w->schedule.outage_ends;
    result->step = w->fail_step;
    if ( w->failed )
    {
        snprintf( result->check, sizeof( result->check ), "%s", w->fail_check );
        snprintf( result->detail, sizeof( result->detail ), "%s", w->fail_detail );
    }

    if ( rocketnet_dst_world_destroy( w ) )
    {
        result->passed = 0;
        result->step = options->steps;
        snprintf( result->check, sizeof( result->check ), "leak" );
        snprintf( result->detail, sizeof( result->detail ), "harness allocations not freed" );
    }
    free( w );
    return 1;
}

// One seed. An outage run first runs its control: the same seed and draws
// with no client dark (outage_control), whose longest window gap per bright
// client and direction bright_unaffected judges the outage run against.

static inline int rocketnet_dst_run_seed_with_options( uint64_t seed, const struct rocketnet_dst_options_t * options,
                                                       struct rocketnet_dst_result_t * result )
{
    const int outage = options->schedule == ROCKETNET_DST_SCHEDULE_OUTAGE || options->schedule == ROCKETNET_DST_SCHEDULE_OUTAGE_PAST;
    if ( !outage || options->outage_control )
    {
        return rocketnet_dst_run_world_seed( seed, options, result, NULL );
    }

    struct rocketnet_dst_options_t control = *options;
    control.outage_control = 1;
    control.witness = ROCKETNET_DST_WITNESS_NONE;
    control.force_fail_step = -1;
    if ( !rocketnet_dst_run_world_seed( seed, &control, result, NULL ) )
    {
        return 0;
    }
    uint64_t gaps[ROCKETNET_DST_MAX_CLIENTS * 2];
    memcpy( gaps, result->bright_gap, sizeof( gaps ) );
    return rocketnet_dst_run_world_seed( seed, options, result, gaps );
}

static inline int rocketnet_dst_run_seed( uint64_t seed, uint64_t steps, int pinned_clients,
                                          struct rocketnet_dst_result_t * result )
{
    struct rocketnet_dst_options_t options;
    memset( &options, 0, sizeof( options ) );
    options.steps = steps;
    options.fault_steps = steps;
    options.shape = ROCKETNET_DST_SHAPE_FULL;
    options.clients = pinned_clients;
    options.faults = ROCKETNET_DST_FAULT_ALL;
    options.force_fail_step = -1;
    return rocketnet_dst_run_seed_with_options( seed, &options, result );
}

static inline void rocketnet_dst_faults_list( uint32_t faults, char * out, size_t out_bytes )
{
    if ( out_bytes == 0 )
    {
        return;
    }

    if ( faults == ROCKETNET_DST_FAULT_ALL )
    {
        snprintf( out, out_bytes, "all" );
        return;
    }

    if ( faults == 0 )
    {
        snprintf( out, out_bytes, "none" );
        return;
    }

    const struct
    {
        uint32_t bit;
        const char * name;
    } kinds[] = {
        { ROCKETNET_DST_FAULT_LOSS, "loss" },
        { ROCKETNET_DST_FAULT_DUP, "dup" },
        { ROCKETNET_DST_FAULT_JITTER, "jitter" },
    };
    size_t used = 0;
    out[0] = '\0';

    for ( size_t i = 0; i < sizeof( kinds ) / sizeof( kinds[0] ); i++ )
    {
        if ( faults & kinds[i].bit )
        {
            const int written = snprintf( out + used, out_bytes - used, "%s%s", used ? "," : "", kinds[i].name );
            if ( written < 0 || (size_t) written >= out_bytes - used )
            {
                out[out_bytes - 1] = '\0';
                return;
            }

            used += (size_t) written;
        }
    }
}

static inline int rocketnet_dst_seed_range_next( uint64_t seed, uint64_t last_seed, uint64_t * next_seed )
{
    if ( seed == last_seed )
    {
        return 0;
    }
    *next_seed = seed + 1;
    return 1;
}

// The replay line names the seed alone and every non-default flag of this run.

static inline void rocketnet_dst_replay_command( uint64_t seed, const struct rocketnet_dst_options_t * options,
                                                 char * out, size_t out_bytes )
{
    const char * shape = options->shape == ROCKETNET_DST_SHAPE_PUSH ? "push" : "full";
    char flags[256];
    char faults[128];
    int used = snprintf( flags, sizeof( flags ), " --shape %s", shape );
    if ( options->clients > 0 )
    {
        used += snprintf( flags + used, sizeof( flags ) - (size_t) used, " --clients %d", options->clients );
    }
    if ( options->steps != ( options->shape == ROCKETNET_DST_SHAPE_PUSH ? 2000ULL : 4000ULL ) )
    {
        used += snprintf( flags + used, sizeof( flags ) - (size_t) used, " --steps %llu",
                          (unsigned long long) options->fault_steps );
    }

    if ( options->faults != ROCKETNET_DST_FAULT_ALL )
    {
        rocketnet_dst_faults_list( options->faults, faults, sizeof( faults ) );
        used += snprintf( flags + used, sizeof( flags ) - (size_t) used, " --faults %s", faults );
    }

    if ( options->schedule > ROCKETNET_DST_SCHEDULE_NONE && options->schedule < ROCKETNET_DST_NUM_SCHEDULES )
    {
        used += snprintf( flags + used, sizeof( flags ) - (size_t) used, " --schedule %s",
                          rocketnet_dst_schedule_name( options->schedule ) );
    }
    snprintf( out, out_bytes, "./bin/dst --seeds %llu-%llu%s",
              (unsigned long long) seed, (unsigned long long) seed, flags );
}

static inline void rocketnet_dst_format_failure( const struct rocketnet_dst_result_t * result,
                                                 const struct rocketnet_dst_options_t * options,
                                                 char * out, size_t out_bytes )
{
    // "./bin/dst --seeds " (18) + two u64 and a dash (41) + flags[256] (255) = 314

    char replay[320];
    rocketnet_dst_replay_command( result->seed, options, replay, sizeof( replay ) );
    snprintf( out, out_bytes, "FAIL seed=%llu step=%llu check=%s what=%s\nreplay: %s",
              (unsigned long long) result->seed, (unsigned long long) result->step,
              result->check, result->detail, replay );
}

#endif // ROCKETNET_TESTS_DST_HARNESS_H
