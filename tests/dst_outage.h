/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The outage world (docs/DST.md, "Outage scenarios"): one server and one
    step-driven client on the public API over the in-memory loopback
    transport, each end's development link simulator as the wire (latency and
    loss on its outgoing packets, seeded at create), and a test clock that
    starts at 1000.0 and advances 10 ms a step. A direction is dark for an
    interval when its sender's outgoing loss is 100 for that interval. Nothing
    here reads a clock, opens a socket or starts a thread.

    Both ends update every step, read every message and receipt, and offer
    messages every tick throughout: a 1200-byte message cycling the eight
    reliability modes and a 2000-byte (two-piece) message cycling the five
    reliable modes, each mode on its own two channels, and one large
    reliable-ordered message started just before the outage. Every start asks
    can_send_message first. The checks:

    - no end of the connection on either side (or, for the complement, the
      ends the scenario expects and no other);
    - for reliable modes, can_send_message says no exactly when the start is
      refused, and a refusal is counted, never an end (m1.7);
    - every message accepted on a reliable, ordered or reliable-with-receipt
      mode is delivered exactly once, ordered modes in channel order, with
      its bytes intact; a reliable-sequenced message at most once and never
      behind a newer one, and the last one accepted on each channel
      delivered; an unreliable message at most once;
    - no message reaches the client application before it reports CONNECTED;
    - every receipt names a receipt-mode send of that side and is its only
      outcome; every reliable receipt-mode send is ACKED by the end;
    - recovery (the last message accepted before the outage ended delivered)
      within the bound USAGE.md "Surviving a network outage" states: one
      keep-alive interval, two round trips, one update, the largest resend
      delay, the backlog at the budget left over by the scenario's own
      offers, and one probe round (a resend delay and a round trip).
*/

#pragma once

#ifndef ROCKETNET_TESTS_DST_OUTAGE_H
#define ROCKETNET_TESTS_DST_OUTAGE_H

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"

#include <map>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#if ROCKETNET_DEV

#define DST_OUTAGE_C2S                     1
#define DST_OUTAGE_S2C                     2
#define DST_OUTAGE_BOTH                    3

// where the outage starts
#define DST_OUTAGE_START_CONNECTED         0    // warmup_seconds after both report the client connected
#define DST_OUTAGE_START_STATE_1           1    // the client enters REQUESTING_COOKIE
#define DST_OUTAGE_START_STATE_2           2    // the client enters SENDING_CONNECTION_REQUEST
#define DST_OUTAGE_START_STATE_3           3    // the client enters SENDING_CONNECTION_RESPONSE
#define DST_OUTAGE_START_TRUE_X1           4    // state 3 plus half a round trip plus 25 ms: the server has
                                                // accepted the response, the client does not yet know
#define DST_OUTAGE_START_SERVER_CONNECTED  5    // the server reports the client connected
#define DST_OUTAGE_START_CLIENT_CONNECTED  6    // the client reports CONNECTED

// what the scenario must show
#define DST_OUTAGE_EXPECT_SURVIVE          0    // no end; everything accepted delivered
#define DST_OUTAGE_EXPECT_TIMEOUT          1    // silence for T ends it: the deaf side by its timeout at T
#define DST_OUTAGE_EXPECT_RELIABLE_TIMEOUT 2    // opted in: RELIABLE_TIMED_OUT at the configured value
#define DST_OUTAGE_EXPECT_RESPONSE_TIMEOUT 3    // the attempt fails in state 3 with CONNECTION_RESPONSE_TIMED_OUT at T
#define DST_OUTAGE_EXPECT_CONNECT_OR_RETRY 4    // states 1-2: the attempt survives, or fails as the handshake allows (C)

#define DST_OUTAGE_DT                      0.01
#define DST_OUTAGE_KEEPALIVE_SECONDS       0.1
#define DST_OUTAGE_PROTOCOL                0x0A7A6E5EED0A7A6EULL
#define DST_OUTAGE_EPOCH                   1700000000ULL
#define DST_OUTAGE_MAGIC                   0x4F555447u
#define DST_OUTAGE_SMALL_BYTES             1200
#define DST_OUTAGE_FRAG_BYTES              2000
#define DST_OUTAGE_LARGE_CHANNEL           30
#define DST_OUTAGE_HEADER_BYTES            20

struct dst_outage_options_t
{
    int timeout_seconds;            // T: the connect token's timeout_seconds
    int token_lifetime_seconds;     // the connect clock (expire - create); 0: 300
    double outage_seconds;          // D
    int dark;                       // DST_OUTAGE_C2S, _S2C or _BOTH
    int rtt_ms;                     // the round trip the two link simulators add
    int loss_percent;               // residual loss both ways outside the dark interval
    int start;                      // DST_OUTAGE_START_*
    double warmup_seconds;          // START_CONNECTED: from both connected to the outage
    double phase_seconds;           // added to the start (a seed spreads it over a second)
    double offer_after_seconds;     // offers continue this long after the outage ends
    double tick_seconds;            // the offer interval; 0: 0.1
    int large_bytes;                // the large reliable-ordered message started just before the outage; 0: none
    int reliable_timeout_ms;        // 0: the default (off)
    int expect;                     // DST_OUTAGE_EXPECT_*
    struct rocketnet_loopback_hub_t * hub;
};

struct dst_outage_result_t
{
    int passed;
    char failure[256];

    int connected;                  // both ends reported the client connected at least once
    double outage_start;            // test time; 0 if the outage never started
    double outage_end;
    int server_ended;
    int server_reason;              // rocketnet_disconnect_reason_t
    double server_end_after;        // seconds after the outage start
    int client_ended;
    int client_state;               // the negative state it ended in
    double client_end_after;
    double client_end_time;         // test time of the client's end
    double server_connected_time;   // test time the server first held the client (0: never)
    int response_escaped;           // state 3: the first response left in the update that entered it

    uint64_t accepted[2];           // [0] client to server, [1] server to client
    uint64_t must_arrive[2];
    uint64_t delivered[2];
    uint64_t undelivered[2];        // must-arrive accepted and not delivered at the end
    uint64_t duplicates;
    uint64_t out_of_order;
    uint64_t corrupt;
    uint64_t early;                 // messages the client application saw before CONNECTED
    uint64_t refused[2];
    uint64_t refused_counted[2];    // send_refused from the conditions getters at the end
    uint64_t can_send_mismatch;
    double first_refusal_after;     // seconds after the outage start; -1: none
    uint64_t receipts_bad;
    uint64_t receipts_unresolved;
    double recovery[2];             // seconds after the outage end; -1: nothing was owed
    double recovery_bound[2];
    double backlog_bytes[2];      // must-arrive bytes accepted and not delivered when the outage ended
    uint64_t steps;
};

struct dst_outage_message_t
{
    double sent;
    double delivered;               // -1: not yet
    uint32_t bytes;
    uint8_t dir;
    uint8_t mode;
    uint8_t channel;
    uint8_t outcome;                // receipt: 0 none, 1 ACKED, 2 LOSS
};

struct dst_outage_world_t
{
    const dst_outage_options_t * options;
    dst_outage_result_t * result;
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    double now;
    std::vector<dst_outage_message_t> messages;
    uint32_t next_sequence[2][32];
    uint32_t last_sequence[2][32];          // the highest sequence delivered on each ordered or sequenced channel
    uint32_t last_accepted[2][32];          // the message id last accepted on each reliable-sequenced channel, + 1
    std::map<uint32_t, uint32_t> serials[2];
    uint32_t offers[2];
};

static double dst_outage_clock_now = 1000.0;
static double dst_outage_clock( void * )
{
    return dst_outage_clock_now;
}

static inline void dst_outage_fail( dst_outage_world_t * w, const char * what )
{
    if ( w->result->failure[0] == 0 )
    {
        snprintf( w->result->failure, sizeof( w->result->failure ), "t=%.2f %s", w->now - 1000.0, what );
    }
}

static inline int dst_outage_mode_must_arrive( int mode )
{
    return mode == 2 || mode == 3 || mode == 6 || mode == 7;
}

static inline int dst_outage_mode_ordered( int mode )
{
    return mode == 3 || mode == 7;
}

static inline int dst_outage_mode_reliable( int mode )
{
    return mode == 2 || mode == 3 || mode == 4 || mode == 6 || mode == 7;
}

static inline int dst_outage_mode_receipt( int mode )
{
    return mode >= 5;
}

static inline uint8_t dst_outage_fill_byte( uint32_t id, uint32_t i )
{
    return (uint8_t) ( ( id * 31u + i * 7u ) & 0xFFu );
}

static inline void dst_outage_put32( uint8_t * p, uint32_t v )
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) ( v >> 8 );
    p[2] = (uint8_t) ( v >> 16 );
    p[3] = (uint8_t) ( v >> 24 );
}

static inline uint32_t dst_outage_get32( const uint8_t * p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

// One offer from one side. dir 0 is the client's, 1 the server's.

static inline void dst_outage_offer( dst_outage_world_t * w, int dir, int mode, int channel, int bytes,
                                     double outage_start )
{
    const enum rocketnet_reliability_t reliability = (enum rocketnet_reliability_t) mode;
    const bool room = dir == 0 ? rocketnet_client_can_send_message( w->client, channel, reliability, bytes )
                               : rocketnet_server_can_send_message( w->server, 0, channel, reliability, bytes );
    uint8_t * out = dir == 0 ? rocketnet_client_start_message( w->client, channel, reliability, bytes )
                             : rocketnet_server_start_message( w->server, 0, channel, reliability, bytes );

    // m1.7 and the query's contract: for a reliable mode the query says no
    // exactly when the start is refused; for any mode a yes is honoured
    if ( ( room && !out ) || ( dst_outage_mode_reliable( mode ) && !room && out ) )
    {
        w->result->can_send_mismatch++;
        dst_outage_fail( w, "can_send_message disagrees with start_message" );
    }

    if ( !out )
    {
        w->result->refused[dir]++;
        if ( w->result->first_refusal_after < 0.0 && outage_start > 0.0 && w->now >= outage_start )
        {
            w->result->first_refusal_after = w->now - outage_start;
        }
        return;
    }

    const uint32_t id = (uint32_t) w->messages.size();
    const uint32_t sequence = ++w->next_sequence[dir][channel];
    dst_outage_put32( out, DST_OUTAGE_MAGIC );
    dst_outage_put32( out + 4, id );
    dst_outage_put32( out + 8, sequence );
    dst_outage_put32( out + 12, (uint32_t) dir | ( (uint32_t) mode << 8 ) | ( (uint32_t) channel << 16 ) );
    dst_outage_put32( out + 16, (uint32_t) bytes );
    for ( int i = DST_OUTAGE_HEADER_BYTES; i < bytes; i++ )
    {
        out[i] = dst_outage_fill_byte( id, (uint32_t) i );
    }

    const uint32_t serial = dir == 0 ? rocketnet_client_finish_message( w->client, bytes )
                                     : rocketnet_server_finish_message( w->server, bytes );
    if ( serial == 0 )
    {
        dst_outage_fail( w, "finish_message returned 0 after a successful start" );
        return;
    }

    dst_outage_message_t m;
    m.sent = w->now;
    m.delivered = -1.0;
    m.bytes = (uint32_t) bytes;
    m.dir = (uint8_t) dir;
    m.mode = (uint8_t) mode;
    m.channel = (uint8_t) channel;
    m.outcome = 0;
    w->messages.push_back( m );
    w->result->accepted[dir]++;
    if ( dst_outage_mode_must_arrive( mode ) )
    {
        w->result->must_arrive[dir]++;
    }
    if ( mode == 4 )
    {
        w->last_accepted[dir][channel] = id + 1;
    }
    if ( dst_outage_mode_receipt( mode ) )
    {
        if ( w->serials[dir].count( serial ) != 0 )
        {
            dst_outage_fail( w, "a receipt serial was issued twice" );
        }
        w->serials[dir][serial] = id;
    }
}

// One tick of offers from one side: the small message cycles the eight
// modes, the two-piece one the five reliable modes; mode m uses channels 2m
// and 2m + 1.

static inline void dst_outage_offer_tick( dst_outage_world_t * w, int dir, double outage_start )
{
    static const int reliable_modes[5] = { 2, 3, 4, 6, 7 };
    const uint32_t k = w->offers[dir]++;
    const int small_mode = (int) ( k % 8u );
    const int frag_mode = reliable_modes[k % 5u];
    dst_outage_offer( w, dir, small_mode, small_mode * 2, DST_OUTAGE_SMALL_BYTES, outage_start );
    dst_outage_offer( w, dir, frag_mode, frag_mode * 2 + 1, DST_OUTAGE_FRAG_BYTES, outage_start );
}

static inline void dst_outage_take( dst_outage_world_t * w, const rocketnet_message_batch_t * batch, int dir )
{
    for ( int i = 0; i < batch->num_messages; i++ )
    {
        const uint8_t * p = batch->data[i];
        const int bytes = batch->bytes[i];
        if ( bytes < DST_OUTAGE_HEADER_BYTES || dst_outage_get32( p ) != DST_OUTAGE_MAGIC )
        {
            w->result->corrupt++;
            dst_outage_fail( w, "a delivered message has no header" );
            continue;
        }
        const uint32_t id = dst_outage_get32( p + 4 );
        const uint32_t sequence = dst_outage_get32( p + 8 );
        const uint32_t meta = dst_outage_get32( p + 12 );
        if ( id >= w->messages.size() || (int) dst_outage_get32( p + 16 ) != bytes )
        {
            w->result->corrupt++;
            dst_outage_fail( w, "a delivered message names no accepted send" );
            continue;
        }
        dst_outage_message_t * m = &w->messages[id];
        if ( (int) ( meta & 0xFFu ) != m->dir || m->dir != dir || (int) ( ( meta >> 8 ) & 0xFFu ) != m->mode ||
             (int) ( meta >> 16 ) != m->channel || (uint32_t) bytes != m->bytes )
        {
            w->result->corrupt++;
            dst_outage_fail( w, "a delivered message differs from the send it names" );
            continue;
        }
        for ( int b = DST_OUTAGE_HEADER_BYTES; b < bytes; b++ )
        {
            if ( p[b] != dst_outage_fill_byte( id, (uint32_t) b ) )
            {
                w->result->corrupt++;
                dst_outage_fail( w, "a delivered message's bytes differ from what was sent" );
                break;
            }
        }
        if ( m->delivered >= 0.0 )
        {
            w->result->duplicates++;
            dst_outage_fail( w, "a message was delivered twice" );
            continue;
        }
        m->delivered = w->now;
        w->result->delivered[dir]++;
        if ( dst_outage_mode_ordered( m->mode ) )
        {
            if ( sequence != w->last_sequence[dir][m->channel] + 1 )
            {
                w->result->out_of_order++;
                dst_outage_fail( w, "an ordered channel delivered out of order" );
            }
            w->last_sequence[dir][m->channel] = sequence;
        }
        else if ( m->mode == 1 || m->mode == 4 )
        {
            if ( sequence <= w->last_sequence[dir][m->channel] )
            {
                w->result->out_of_order++;
                dst_outage_fail( w, "a sequenced channel delivered an older message after a newer one" );
            }
            w->last_sequence[dir][m->channel] = sequence;
        }
    }
}

static inline void dst_outage_receipts( dst_outage_world_t * w, int dir, const rocketnet_receipt_t * receipts, int count )
{
    for ( int r = 0; r < count; r++ )
    {
        std::map<uint32_t, uint32_t>::iterator it = w->serials[dir].find( receipts[r].serial );
        if ( it == w->serials[dir].end() )
        {
            // read again before the next update: the list is not consumed by a read
            continue;
        }
        dst_outage_message_t * m = &w->messages[it->second];
        const uint8_t outcome = receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED ? 1 : 2;
        if ( m->outcome != 0 && m->outcome != outcome )
        {
            w->result->receipts_bad++;
            dst_outage_fail( w, "a send has two receipt outcomes" );
        }
        if ( outcome == 2 && m->mode != 5 )
        {
            w->result->receipts_bad++;
            dst_outage_fail( w, "a reliable send reported LOSS" );
        }
        // ACKED names a message the receiver delivered; a type 7 message may
        // also be held behind an earlier undelivered one (docs/DST.md), and
        // the end-of-run check still requires its delivery
        if ( outcome == 1 && m->delivered < 0.0 && m->mode == 6 )
        {
            w->result->receipts_bad++;
            dst_outage_fail( w, "ACKED for a reliable message the receiver has not delivered" );
        }
        m->outcome = outcome;
    }
}

static inline void dst_outage_read_receipts( dst_outage_world_t * w, int dir )
{
    static rocketnet_receipt_t receipts[4096];
    const int count = dir == 0 ? rocketnet_client_receipts( w->client, receipts, 4096 )
                               : rocketnet_server_receipts( w->server, 0, receipts, 4096 );
    if ( count > 4096 )
    {
        dst_outage_fail( w, "a receipt list is longer than the harness reads" );
    }
    dst_outage_receipts( w, dir, receipts, count < 4096 ? count : 4096 );
}

static inline void dst_outage_set_links( dst_outage_world_t * w, int c2s_dark, int s2c_dark )
{
    const int rtt = w->options->rtt_ms;
    const int client_latency = ( rtt + 1 ) / 2;
    const int server_latency = rtt / 2;
    rocketnet_client_network_simulator( w->client, client_latency, 0, c2s_dark ? 100 : w->options->loss_percent, 0 );
    rocketnet_server_network_simulator( w->server, server_latency, 0, s2c_dark ? 100 : w->options->loss_percent, 0 );
}

static inline int dst_outage_create( dst_outage_world_t * w )
{
    const dst_outage_options_t * o = w->options;
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x0A ^ i );
    }
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    if ( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) != ROCKETNET_STATUS_OK )
    {
        return 0;
    }

    rocketnet_server_config_t server_config;
    if ( rocketnet_server_default_config( &server_config ) != ROCKETNET_STATUS_OK )
    {
        return 0;
    }
    server_config.protocol_id = DST_OUTAGE_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = o->hub;
    server_config.token_epoch_unix_seconds = DST_OUTAGE_EPOCH;
    server_config.max_timeout_seconds = o->timeout_seconds > 60 ? o->timeout_seconds : 60;
    if ( o->reliable_timeout_ms > 0 )
    {
        server_config.reliable_message_timeout_ms = o->reliable_timeout_ms;
    }

    w->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    if ( !w->server || rocketnet_server_start( w->server ) != ROCKETNET_STATUS_OK )
    {
        return 0;
    }

    rocketnet_client_config_t client_config;
    if ( rocketnet_client_default_config( &client_config ) != ROCKETNET_STATUS_OK )
    {
        return 0;
    }
    client_config.transport = "loopback";
    client_config.loopback_hub = o->hub;
    client_config.step_driven = true;
    client_config.time_callback = dst_outage_clock;
    client_config.token_epoch_unix_seconds = DST_OUTAGE_EPOCH;
    if ( o->reliable_timeout_ms > 0 )
    {
        client_config.reliable_message_timeout_ms = o->reliable_timeout_ms;
    }
    w->client = rocketnet_client_create( NULL, &client_config, NULL );
    if ( !w->client )
    {
        return 0;
    }

    dst_outage_set_links( w, 0, 0 );

    // the client is updated once before connect, so its token check reads
    // the epoch plus the test clock (rocketnet.h, token_epoch_unix_seconds)
    dst_outage_clock_now = w->now;
    rocketnet_client_update( w->client, w->now );

    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( w->server, server_public );
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( w->server ) );
    const char * addresses[] = { address };
    const uint64_t create = DST_OUTAGE_EPOCH + (uint64_t) w->now;
    const uint64_t lifetime = o->token_lifetime_seconds > 0 ? (uint64_t) o->token_lifetime_seconds : 300u;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( rocketnet_write_production_connect_token( backend_secret, server_public, DST_OUTAGE_PROTOCOL, addresses, 1, 0x0A7A6Eu, create,
                                                   create + lifetime, o->timeout_seconds, NULL, token, sizeof( token ) ) != ROCKETNET_STATUS_OK )
    {
        return 0;
    }
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_base64_encode_data( token, sizeof( token ), token_string, sizeof( token_string ) );
    return rocketnet_client_connect( w->client, token_string ) == ROCKETNET_STATUS_OK;
}

static inline void dst_outage_destroy( dst_outage_world_t * w )
{
    if ( w->client )
    {
        // destroy takes a disconnected client (a debug assert): disconnect,
        // then step it until the attempt or session has retired
        rocketnet_client_disconnect( w->client );
        for ( int i = 0; i < 64 && !rocketnet_client_disconnected( w->client ); i++ )
        {
            w->now += DST_OUTAGE_DT;
            rocketnet_client_update( w->client, w->now );
        }
        rocketnet_client_destroy( w->client );
        w->client = NULL;
    }
    if ( w->server )
    {
        rocketnet_server_stop( w->server );
        for ( int i = 0; i < 64 && !rocketnet_server_stopped( w->server ); i++ )
        {
            w->now += DST_OUTAGE_DT;
            rocketnet_server_update( w->server, w->now );
        }
        rocketnet_server_destroy( w->server );
        w->server = NULL;
    }
}

// The bytes per second the scenario's own offers take from one direction's
// budget: one small and one two-piece message per tick, each with its
// packet framing (an upper bound of 64 bytes a piece).

static inline double dst_outage_offered_rate( const dst_outage_options_t * o )
{
    const double tick = o->tick_seconds > 0.0 ? o->tick_seconds : 0.1;
    return ( DST_OUTAGE_SMALL_BYTES + DST_OUTAGE_FRAG_BYTES + 3.0 * 64.0 ) / tick;
}

// Runs one scenario. Returns 0 only when the world could not be built.

static inline int dst_outage_run( const dst_outage_options_t * o, dst_outage_result_t * result )
{
    memset( result, 0, sizeof( *result ) );
    result->first_refusal_after = -1.0;
    result->recovery[0] = result->recovery[1] = -1.0;

    dst_outage_world_t * w = new dst_outage_world_t();
    w->options = o;
    w->result = result;
    w->now = 1000.0;
    memset( w->next_sequence, 0, sizeof( w->next_sequence ) );
    memset( w->last_sequence, 0, sizeof( w->last_sequence ) );
    memset( w->last_accepted, 0, sizeof( w->last_accepted ) );
    w->offers[0] = w->offers[1] = 0;
    w->messages.reserve( 16384 );

    if ( !dst_outage_create( w ) )
    {
        dst_outage_destroy( w );
        delete w;
        return 0;
    }

    const double tick = o->tick_seconds > 0.0 ? o->tick_seconds : 0.1;
    const double rtt = (double) o->rtt_ms * 0.001;
    double out_start = 0.0;
    double out_end = 0.0;
    double offer_stop = 1e30;
    double run_end = 1000.0 + 120.0;    // the handshake (and an establishment outage) must settle by then
    double client_next = 0.0;
    double server_next = 0.0;
    double state3_at = 0.0;
    int dark_on = 0;
    int large_sent = 0;
    int server_connected_seen = 0;
    int client_connected_seen = 0;
    double backlog_bytes[2] = { 0.0, 0.0 };

    for ( uint64_t step = 0;; step++ )
    {
        w->now = 1000.0 + (double) step * DST_OUTAGE_DT;
        dst_outage_clock_now = w->now;
        result->steps = step;
        if ( w->now >= run_end || result->failure[0] != 0 )
        {
            break;
        }

        // the outage: armed by a start condition, dark from out_start to out_end
        if ( out_start > 0.0 && !dark_on && w->now >= out_start && w->now < out_end )
        {
            dst_outage_set_links( w, o->dark & DST_OUTAGE_C2S, o->dark & DST_OUTAGE_S2C );
            dark_on = 1;
        }
        if ( dark_on && w->now >= out_end )
        {
            dst_outage_set_links( w, 0, 0 );
            dark_on = 0;
            // the backlog the recovery bound drains: what each sender may
            // have to send again, every reliable message it accepted from
            // its last acknowledged round trip before the outage to its end
            // (delivered or not: a dark direction's acknowledgements are
            // lost with it), and anything older still owed
            const double unacked_from = out_start - rtt - tick - DST_OUTAGE_DT;
            for ( size_t i = 0; i < w->messages.size(); i++ )
            {
                const dst_outage_message_t * m = &w->messages[i];
                if ( ( dst_outage_mode_must_arrive( m->mode ) && m->delivered < 0.0 ) ||
                     ( dst_outage_mode_reliable( m->mode ) && m->sent >= unacked_from ) )
                {
                    backlog_bytes[m->dir] += (double) m->bytes;
                    result->backlog_bytes[m->dir] += (double) m->bytes;
                }
            }
        }

        // the client
        if ( !result->client_ended )
        {
            rocketnet_client_update( w->client, w->now );
            const int state = (int) rocketnet_client_state( w->client );
            const rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_client_receive_messages( w->client ) ) != NULL )
            {
                if ( state != ROCKETNET_CLIENT_STATE_CONNECTED )
                {
                    result->early += (uint64_t) batch->num_messages;
                    dst_outage_fail( w, "a message reached the client before it reported CONNECTED" );
                }
                dst_outage_take( w, batch, 1 );
            }
            dst_outage_read_receipts( w, 0 );

            if ( state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE && state3_at == 0.0 )
            {
                state3_at = w->now;
            }
            if ( out_start == 0.0 )
            {
                double arm = 0.0;
                if ( ( o->start == DST_OUTAGE_START_STATE_1 && state == ROCKETNET_CLIENT_STATE_REQUESTING_COOKIE ) ||
                     ( o->start == DST_OUTAGE_START_STATE_2 && state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_REQUEST ) ||
                     ( o->start == DST_OUTAGE_START_STATE_3 && state == ROCKETNET_CLIENT_STATE_SENDING_CONNECTION_RESPONSE ) ||
                     ( o->start == DST_OUTAGE_START_CLIENT_CONNECTED && state == ROCKETNET_CLIENT_STATE_CONNECTED ) )
                {
                    arm = w->now + o->phase_seconds;
                }
                if ( o->start == DST_OUTAGE_START_TRUE_X1 && state3_at > 0.0 )
                {
                    arm = state3_at + rtt * 0.5 + 0.025 + o->phase_seconds;
                }
                if ( arm > 0.0 )
                {
                    out_start = arm;
                    out_end = arm + o->outage_seconds;
                }
                // an outage that starts now is dark before this update's
                // sends: the packet the new state sends is the first lost
                if ( out_start > 0.0 && w->now >= out_start )
                {
                    dst_outage_set_links( w, o->dark & DST_OUTAGE_C2S, o->dark & DST_OUTAGE_S2C );
                    dark_on = 1;
                }
            }

            if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                client_connected_seen = 1;
                if ( w->now < offer_stop && w->now >= client_next )
                {
                    while ( client_next <= w->now )
                    {
                        client_next = ( client_next == 0.0 ? w->now : client_next ) + tick;
                    }
                    dst_outage_offer_tick( w, 0, out_start );
                }
            }
            else if ( state < 0 )
            {
                result->client_ended = 1;
                result->client_state = state;
                result->client_end_after = out_start > 0.0 ? w->now - out_start : -1.0;
                result->client_end_time = w->now;
            }
            if ( !result->client_ended )
            {
                rocketnet_client_send_packets( w->client );
            }
        }

        // the server
        if ( !result->server_ended )
        {
            rocketnet_server_update( w->server, w->now );
            const rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_server_receive_messages( w->server ) ) != NULL )
            {
                dst_outage_take( w, batch, 0 );
            }
            dst_outage_read_receipts( w, 1 );

            const bool connected = rocketnet_server_client_connected( w->server, 0 ) != 0;
            if ( connected )
            {
                if ( !server_connected_seen && out_start == 0.0 && o->start == DST_OUTAGE_START_SERVER_CONNECTED )
                {
                    out_start = w->now + o->phase_seconds;
                    out_end = out_start + o->outage_seconds;
                    if ( w->now >= out_start )
                    {
                        dst_outage_set_links( w, o->dark & DST_OUTAGE_C2S, o->dark & DST_OUTAGE_S2C );
                        dark_on = 1;
                    }
                }
                if ( !server_connected_seen )
                {
                    result->server_connected_time = w->now;
                }
                server_connected_seen = 1;
                if ( w->now < offer_stop && w->now >= server_next )
                {
                    while ( server_next <= w->now )
                    {
                        server_next = ( server_next == 0.0 ? w->now : server_next ) + tick;
                    }
                    dst_outage_offer_tick( w, 1, out_start );
                }
            }
            else if ( server_connected_seen )
            {
                result->server_ended = 1;
                result->server_reason = (int) rocketnet_server_disconnect_reason( w->server, 0 );
                result->server_end_after = out_start > 0.0 ? w->now - out_start : -1.0;
            }
            if ( !result->server_ended )
            {
                rocketnet_server_send_packets( w->server );
            }
        }

        if ( server_connected_seen && client_connected_seen && !result->connected )
        {
            result->connected = 1;
            if ( out_start == 0.0 && o->start == DST_OUTAGE_START_CONNECTED )
            {
                out_start = w->now + o->warmup_seconds + o->phase_seconds;
                out_end = out_start + o->outage_seconds;
            }
        }

        // the large message: started just before the outage, both ways, so
        // its pieces are on the wire when the direction goes dark
        if ( !large_sent && o->large_bytes > 0 && out_start > 0.0 && w->now >= out_start - 0.05 &&
             client_connected_seen && server_connected_seen && !result->client_ended && !result->server_ended )
        {
            dst_outage_offer( w, 0, 3, DST_OUTAGE_LARGE_CHANNEL, o->large_bytes, out_start );
            dst_outage_offer( w, 1, 3, DST_OUTAGE_LARGE_CHANNEL, o->large_bytes, out_start );
            large_sent = 1;
        }

        if ( out_start > 0.0 && offer_stop > 1e29 )
        {
            offer_stop = out_end + o->offer_after_seconds;
            if ( o->expect == DST_OUTAGE_EXPECT_SURVIVE || o->expect == DST_OUTAGE_EXPECT_CONNECT_OR_RETRY )
            {
                run_end = offer_stop + 120.0;
            }
            else
            {
                // a one-way outage ends the other side a timeout after the
                // deaf side ends and goes quiet
                run_end = out_start + 2.0 * (double) o->timeout_seconds + 30.0;
            }
        }

        // a surviving run ends once offers have stopped and nothing is owed
        if ( ( o->expect == DST_OUTAGE_EXPECT_SURVIVE || o->expect == DST_OUTAGE_EXPECT_CONNECT_OR_RETRY ) && w->now >= offer_stop && ( step % 10 ) == 0 )
        {
            int owed = 0;
            for ( size_t i = 0; i < w->messages.size() && !owed; i++ )
            {
                const dst_outage_message_t * m = &w->messages[i];
                owed = dst_outage_mode_must_arrive( m->mode ) && ( m->delivered < 0.0 || ( m->mode >= 6 && m->outcome == 0 ) );
            }
            // the last reliable-sequenced message on each channel is owed
            // too: a lost one waits a resend delay and a one-way trip
            for ( int dir = 0; dir < 2 && !owed; dir++ )
            {
                for ( int channel = 0; channel < 32 && !owed; channel++ )
                {
                    const uint32_t id1 = w->last_accepted[dir][channel];
                    owed = id1 > 0 && w->messages[id1 - 1].delivered < 0.0;
                }
            }
            if ( !owed )
            {
                // one more second for the last receipts and the sequenced tails
                if ( run_end > w->now + 1.0 )
                {
                    run_end = w->now + 1.0;
                }
            }
        }
        if ( o->expect != DST_OUTAGE_EXPECT_SURVIVE && result->client_ended && ( result->server_ended || !server_connected_seen ) )
        {
            break;
        }
    }

    result->outage_start = out_start;
    result->outage_end = out_end;

    // the refusals each side's getter counted, read while still connected
    if ( !result->client_ended )
    {
        rocketnet_conditions_t conditions;
        memset( &conditions, 0, sizeof( conditions ) );
        if ( rocketnet_client_conditions( w->client, &conditions, (int) sizeof( conditions ) ) )
        {
            result->refused_counted[0] = conditions.send_refused;
        }
    }
    if ( !result->server_ended )
    {
        rocketnet_conditions_t conditions;
        memset( &conditions, 0, sizeof( conditions ) );
        if ( rocketnet_server_client_conditions( w->server, 0, &conditions, (int) sizeof( conditions ) ) )
        {
            result->refused_counted[1] = conditions.send_refused;
        }
    }

    // delivery, receipts and recovery
    uint32_t last_sequenced[2][32];
    memset( last_sequenced, 0, sizeof( last_sequenced ) );
    for ( size_t i = 0; i < w->messages.size(); i++ )
    {
        const dst_outage_message_t * m = &w->messages[i];
        if ( dst_outage_mode_must_arrive( m->mode ) )
        {
            if ( m->delivered < 0.0 )
            {
                result->undelivered[m->dir]++;
            }
            else if ( out_end > 0.0 && m->sent < out_end && m->delivered >= out_end )
            {
                const double after = m->delivered - out_end;
                if ( after > result->recovery[m->dir] )
                {
                    result->recovery[m->dir] = after;
                }
            }
            if ( m->mode >= 6 && m->outcome != 1 )
            {
                result->receipts_unresolved++;
            }
        }
    }
    for ( int dir = 0; dir < 2; dir++ )
    {
        for ( int channel = 0; channel < 32; channel++ )
        {
            const uint32_t id1 = w->last_accepted[dir][channel];
            if ( id1 > 0 && w->messages[id1 - 1].delivered < 0.0 && o->expect == DST_OUTAGE_EXPECT_SURVIVE )
            {
                dst_outage_fail( w, "the last reliable-sequenced message on a channel was never delivered" );
            }
        }
        const double budget = ( dir == 0 ? ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS : ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS ) * 125.0;
        const double usable = budget - dst_outage_offered_rate( o );
        // (R): kappa + 2 r + u + rd + backlog / rate + one probe round (rd +
        // r), with rd at its largest (message_resend_delay_max_ms, 2 s).
        // The design states it for a clean path after the outage. With
        // residual loss each lost last copy costs one more resend delay and
        // round trip; the harness allows two (its own allowance, not the
        // standard's: a 2% path rarely loses one piece three times).
        const double resend = 2.0;
        const double losses = o->loss_percent > 0 ? 2.0 : 0.0;
        result->recovery_bound[dir] = DST_OUTAGE_KEEPALIVE_SECONDS + 2.0 * rtt + DST_OUTAGE_DT + resend + backlog_bytes[dir] / usable +
                                      ( resend + rtt ) + losses * ( resend + rtt );
    }

    // the scenario's verdict
    const double T = (double) o->timeout_seconds;
    if ( result->failure[0] == 0 )
    {
        // state 3: the client seals its response when the challenge
        // arrives and may send it in that same update, before the harness
        // can see the new state and go dark. That one response is already
        // on the wire; the server holds the client one one-way delay after
        // the outage starts, and the connection must then survive. Any
        // later server connect while the client's direction is dark is a
        // packet through a dark link, and fails below.
        if ( o->expect == DST_OUTAGE_EXPECT_RESPONSE_TIMEOUT && result->server_connected_time > 0.0 && out_start > 0.0 &&
             result->server_connected_time <= out_start + ( (double) o->rtt_ms + 1.0 ) * 0.0005 + 2.0 * DST_OUTAGE_DT )
        {
            result->response_escaped = 1;
        }
        const int survive = o->expect == DST_OUTAGE_EXPECT_SURVIVE || ( o->expect == DST_OUTAGE_EXPECT_CONNECT_OR_RETRY && result->connected ) ||
                            result->response_escaped;
        if ( survive )
        {
            if ( !result->connected )
            {
                dst_outage_fail( w, "the client never connected" );
            }
            else if ( result->client_ended || result->server_ended )
            {
                char what[160];
                snprintf( what, sizeof( what ), "an end in a survivable outage: client %s at %.2f, server %s at %.2f",
                          rocketnet_client_state_name( (enum rocketnet_client_state_t) result->client_state ), result->client_end_after,
                          rocketnet_disconnect_reason_name( (enum rocketnet_disconnect_reason_t) result->server_reason ), result->server_end_after );
                dst_outage_fail( w, what );
            }
            else if ( result->undelivered[0] + result->undelivered[1] > 0 )
            {
                dst_outage_fail( w, "an accepted reliable message was never delivered" );
            }
            else if ( result->receipts_unresolved > 0 )
            {
                dst_outage_fail( w, "a reliable receipt-mode send never resolved" );
            }
            else if ( result->refused_counted[0] != result->refused[0] || result->refused_counted[1] != result->refused[1] )
            {
                dst_outage_fail( w, "send_refused differs from the starts refused" );
            }
            else if ( result->recovery[0] > result->recovery_bound[0] || result->recovery[1] > result->recovery_bound[1] )
            {
                char what[160];
                snprintf( what, sizeof( what ), "recovery past the bound: c2s %.2f (bound %.2f), s2c %.2f (bound %.2f)", result->recovery[0],
                          result->recovery_bound[0], result->recovery[1], result->recovery_bound[1] );
                dst_outage_fail( w, what );
            }
        }
        else if ( o->expect == DST_OUTAGE_EXPECT_TIMEOUT )
        {
            // (K): the deaf side (a dark direction's receiver) ends by its
            // timeout T after the last packet it accepted. That packet left
            // its sender at most one keep-alive interval and an update before
            // the outage, and was still on the wire for up to the one-way
            // delay after it. A timeout ends silently, so the other side then
            // hears nothing and ends by its own timeout T after the deaf
            // side's last packet. Both ends are the timeout, and nothing else.
            // The upper edge carries three steps: the outage arms at the first
            // step at or after its start, a packet is taken at the first update
            // at or after it lands, and the step clock adds 0.01 each step, so a
            // difference of exactly T can round short and end one step later
            // (seed 11 measured T + one-way + 2.5 steps).
            const double one_way = rtt * 0.5;
            const double lo = T - DST_OUTAGE_KEEPALIVE_SECONDS - 2.0 * DST_OUTAGE_DT;
            const double hi = T + one_way + 3.0 * DST_OUTAGE_DT;
            const int server_deaf = ( o->dark & DST_OUTAGE_C2S ) != 0;
            const int client_deaf = ( o->dark & DST_OUTAGE_S2C ) != 0;
            const double deaf_end = server_deaf ? result->server_end_after : result->client_end_after;
            if ( !result->client_ended || !result->server_ended )
            {
                dst_outage_fail( w, "an outage longer than T did not end both sides" );
            }
            else if ( result->server_reason != ROCKETNET_DISCONNECT_REASON_TIMED_OUT )
            {
                dst_outage_fail( w, "the server ended for a reason other than its timeout" );
            }
            else if ( result->client_state != ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT )
            {
                dst_outage_fail( w, "the client ended for a reason other than its timeout" );
            }
            else if ( server_deaf && ( result->server_end_after < lo || result->server_end_after > hi ) )
            {
                dst_outage_fail( w, "the server did not end at T" );
            }
            else if ( client_deaf && ( result->client_end_after < lo || result->client_end_after > hi ) )
            {
                dst_outage_fail( w, "the client did not end at T" );
            }
            else if ( !server_deaf && ( result->server_end_after < deaf_end + lo || result->server_end_after > deaf_end + hi ) )
            {
                dst_outage_fail( w, "the server did not end T after the client went quiet" );
            }
            else if ( !client_deaf && ( result->client_end_after < deaf_end + lo || result->client_end_after > deaf_end + hi ) )
            {
                dst_outage_fail( w, "the client did not end T after the server went quiet" );
            }
        }
        else if ( o->expect == DST_OUTAGE_EXPECT_RELIABLE_TIMEOUT )
        {
            // opted in, as v1.0.0: the oldest reliable number unacknowledged
            // for the configured time since its first send ends the
            // connection, outage or not. The oldest unacknowledged at the
            // outage start was first sent at most one round trip and one
            // tick before it.
            const double limit = (double) o->reliable_timeout_ms * 0.001;
            const double lo = limit - rtt - tick - 2.0 * DST_OUTAGE_DT;
            const double hi = limit + 2.0 * DST_OUTAGE_DT;
            const int server_reliable = result->server_ended && result->server_reason == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT &&
                                        result->server_end_after >= lo && result->server_end_after <= hi;
            const int client_reliable = result->client_ended && result->client_state == ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT &&
                                        result->client_end_after >= lo && result->client_end_after <= hi;
            if ( !server_reliable && !client_reliable )
            {
                dst_outage_fail( w, "opted in, neither side ended with RELIABLE_TIMED_OUT at the configured time" );
            }
        }
        else if ( o->expect == DST_OUTAGE_EXPECT_RESPONSE_TIMEOUT )
        {
            // the client in state 3 is on per-state silence T alone: it last
            // heard the challenge just before it entered state 3, where the
            // outage starts (USAGE.md, the handshake; design (C))
            if ( !result->client_ended || result->client_state != ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT || result->connected )
            {
                dst_outage_fail( w, "the attempt did not fail in state 3 with CONNECTION_RESPONSE_TIMED_OUT" );
            }
            else if ( result->client_end_after < T - DST_OUTAGE_KEEPALIVE_SECONDS - 2.0 * DST_OUTAGE_DT ||
                      result->client_end_after > T + 2.0 * DST_OUTAGE_DT )
            {
                dst_outage_fail( w, "the attempt in state 3 did not fail at T" );
            }
        }
        else if ( o->expect == DST_OUTAGE_EXPECT_CONNECT_OR_RETRY )
        {
            // never connected: the attempt failed as states 1 and 2 allow,
            // by per-state silence for T (-2, -3) or by the connect clock,
            // the token's lifetime from connect (-7); the server never held
            // the client, and the application saw nothing
            const double lifetime = (double) ( o->token_lifetime_seconds > 0 ? o->token_lifetime_seconds : 300 );
            const int state = result->client_state;
            if ( !result->client_ended || result->server_ended || result->early > 0 )
            {
                dst_outage_fail( w, "the attempt neither connected nor failed cleanly" );
            }
            else if ( state != ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT && state != ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT &&
                      state != ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED )
            {
                dst_outage_fail( w, "the attempt failed in states 1-2 with a reason they do not allow" );
            }
            else if ( state == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED && result->client_end_time - 1000.0 > lifetime + 2.0 * DST_OUTAGE_DT )
            {
                dst_outage_fail( w, "the connect clock ended the attempt after the token's lifetime" );
            }
            else if ( state != ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED && result->client_end_after < T - DST_OUTAGE_KEEPALIVE_SECONDS - rtt - 2.0 * DST_OUTAGE_DT )
            {
                dst_outage_fail( w, "per-state silence ended the attempt before T" );
            }
        }
    }

    result->passed = result->failure[0] == 0;
    dst_outage_destroy( w );
    delete w;
    return 1;
}

static inline void dst_outage_print( const char * label, const dst_outage_options_t * o, const dst_outage_result_t * r )
{
    printf( "%s %s T=%d D=%.1f dark=%s rtt=%d loss=%d start=%d: c2s %llu/%llu s2c %llu/%llu refused %llu/%llu first_refusal %.2f "
            "backlog %.0f/%.0f recovery %.2f/%.2f (bound %.2f/%.2f) ends client %d at %.2f server %d at %.2f steps %llu%s%s%s\n",
            r->passed ? "PASS" : "FAIL", label, o->timeout_seconds, o->outage_seconds,
            o->dark == DST_OUTAGE_BOTH ? "both" : o->dark == DST_OUTAGE_C2S ? "c2s" : "s2c", o->rtt_ms, o->loss_percent, o->start,
            (unsigned long long) r->delivered[0], (unsigned long long) r->accepted[0], (unsigned long long) r->delivered[1],
            (unsigned long long) r->accepted[1], (unsigned long long) r->refused[0], (unsigned long long) r->refused[1],
            r->first_refusal_after, r->backlog_bytes[0], r->backlog_bytes[1], r->recovery[0], r->recovery[1], r->recovery_bound[0], r->recovery_bound[1], r->client_state,
            r->client_end_after, r->server_reason, r->server_end_after, (unsigned long long) r->steps, r->response_escaped ? " response-escaped" : "", r->passed ? "" : ": ", r->failure );
}

static inline void dst_outage_default_options( dst_outage_options_t * o, struct rocketnet_loopback_hub_t * hub )
{
    memset( o, 0, sizeof( *o ) );
    o->timeout_seconds = 10;
    o->outage_seconds = 9.0;
    o->dark = DST_OUTAGE_BOTH;
    o->rtt_ms = 150;
    o->start = DST_OUTAGE_START_CONNECTED;
    o->warmup_seconds = 3.0;
    o->offer_after_seconds = 5.0;
    o->large_bytes = 65536;
    o->expect = DST_OUTAGE_EXPECT_SURVIVE;
    o->hub = hub;
}

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_TESTS_DST_OUTAGE_H
