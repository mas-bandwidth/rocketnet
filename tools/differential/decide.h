/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The shared differential decision core: the rocketnet and netcode 1.02
    admission decisions for one abstract case, plus the verdict between them.
    Both tools/differential/netcode_decisions.cpp (the fixed gate) and
    tests/fuzz/rocketnet_fuzz_differential.cpp (the libFuzzer target) include this
    header so they run the same deciders on the same case model. The two
    wire formats differ, so the case is never packet bytes: it is a struct of
    the fields both admission paths read (create/expire relative to now,
    timeout, protocol id, the server address list, one flipped signature or
    MAC byte, occupied slots, and a replay from a second address).
*/

#pragma once

#ifndef ROCKETNET_TOOLS_DIFFERENTIAL_DECIDE_H
#define ROCKETNET_TOOLS_DIFFERENTIAL_DECIDE_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_ack.h"
#include "rocketnet_fragment.h"
#include "rocketnet_protocol.h"
#include "rocketnet_address.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_vectors.h"

// The netcode side lives only when the bench supplies the reference source.
// The default gate build diffs against the reference column in cases.tsv;
// the fuzz target always builds with it (fuzz-nightly.yml checks out the
// pinned tag into its own path).
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
#include "netcode.h"
#endif // #if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

// --- the case model -----------------------------------------------------

// One abstract token-admission case, expressed in the fields both admission
// paths actually read. Offsets are seconds relative to the server's "now"
// (the rocketnet side takes now as an argument; the netcode side reads its
// own wall clock, so the offsets are relative to the moment netcode reads,
// which makes a crashing input reproduce - README, "The netcode side, live").
// address_mode: 0 the advertised IPv4, 1 a wrong IPv4, 2 the IPv6 family,
// 3 the advertised IPv4 with a wrong port.
struct differential_case_t
{
    int64_t create_offset;
    int64_t expire_offset;
    int32_t timeout_seconds;
    unsigned protocol_wrong;
    unsigned address_mode;
    unsigned flip_authenticator;      // flip one signature byte (rocketnet) / MAC byte (netcode)
    unsigned occupied_slots;
    unsigned replay;                  // present the same token from a second address
};

struct decision_t
{
    int accept;
    char reason[64];
};

// --- small helpers ------------------------------------------------------

static void encode_ipv4_token_address( uint8_t out[6], int a, int b, int c, int d, int port )
{
    out[0] = (uint8_t) a;
    out[1] = (uint8_t) b;
    out[2] = (uint8_t) c;
    out[3] = (uint8_t) d;
    out[4] = (uint8_t) ( port & 0xff );
    out[5] = (uint8_t) ( ( port >> 8 ) & 0xff );
}

static void encode_ipv6_token_address( uint8_t out[18], const uint16_t groups[8], int port )
{
    for ( int i = 0; i < 8; i++ )
    {
        out[2 * i] = (uint8_t) ( groups[i] & 0xff );
        out[2 * i + 1] = (uint8_t) ( ( groups[i] >> 8 ) & 0xff );
    }

    out[16] = (uint8_t) ( port & 0xff );
    out[17] = (uint8_t) ( ( port >> 8 ) & 0xff );
}

// --- the rocketnet decision ---------------------------------------------

static int differential_handshake( struct rocketnet_server_protocol_t * server,
                                   struct rocketnet_client_protocol_t * client,
                                   const struct rocketnet_address_t * from,
                                   uint64_t timestamp, double start_time, double max_seconds )
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    int response_bytes = 0;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    const int connected_before = rocketnet_client_slots_num_connected( server->client_slots );

    for ( int step = 0; step * 0.05 < max_seconds; step++ )
    {
        const double time = start_time + step * 0.05;
        rocketnet_server_protocol_update( server, time );
        rocketnet_client_protocol_update( client, time, packet, &packet_bytes );
        if ( client->state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            break;

        if ( packet_bytes > 0 )
        {
            response_bytes = 0;
            rocketnet_server_protocol_process_packet( server, from, packet, packet_bytes, timestamp, time,
                                                      response, &response_bytes,
                                                      payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_client_protocol_process_packet( client, response, response_bytes, time,
                                                          payload, sizeof( payload ), &payload_bytes );
        }

        if ( rocketnet_client_slots_num_connected( server->client_slots ) > connected_before )
            return 1;
    }

    return 0;
}

static void rocketnet_decide_replay( struct rocketnet_server_protocol_t * server,
                                     struct rocketnet_counters_t * counters,
                                     const uint8_t * token, uint64_t now,
                                     struct decision_t * d )
{
    struct rocketnet_address_t first_address;
    struct rocketnet_address_t replay_address;
    rocketnet_address_parse( &first_address, "198.51.100.7:56789" );
    rocketnet_address_parse( &replay_address, "198.51.100.50:56789" );

    struct rocketnet_counters_t first_counters;
    struct rocketnet_counters_t replay_counters;
    memset( &first_counters, 0, sizeof( first_counters ) );
    memset( &replay_counters, 0, sizeof( replay_counters ) );

    struct rocketnet_client_protocol_t first;
    struct rocketnet_client_protocol_t replay;
    rocketnet_client_protocol_init( &first, &first_counters );
    rocketnet_client_protocol_init( &replay, &replay_counters );

    const double start = (double) now;

    if ( !rocketnet_client_protocol_connect( &first, token, start, now ) ||
         !differential_handshake( server, &first, &first_address, now, start, 2.0 ) )
    {
        d->accept = 0;
        strcpy( d->reason, "first-not-admitted" );
        return;
    }

    if ( !rocketnet_client_protocol_connect( &replay, token, start + 2.0, now ) )
    {
        d->accept = 0;
        strcpy( d->reason, "mint" );
        return;
    }

    if ( differential_handshake( server, &replay, &replay_address, now, start + 2.0, 2.0 ) )
    {
        d->accept = 1;
        strcpy( d->reason, "accept" );
        return;
    }

    d->accept = 0;
    if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] )
    {
        strcpy( d->reason, "replay" );
    }
    else
    {
        strcpy( d->reason, "refused" );
    }
}

static void rocketnet_server_setup( struct rocketnet_server_protocol_t * server, struct rocketnet_counters_t * counters, uint64_t now )
{
    memset( server, 0, sizeof( *server ) );
    memset( counters, 0, sizeof( *counters ) );

    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" );
    memcpy( server->cookie_key, rocketnet_test_backend_secret_key, 32 );   // any 32-byte cookie key
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->max_timeout_seconds = 0;
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, 8 );
    server->acks = rocketnet_server_acks_create( NULL, 8 );
    server->reassembly = rocketnet_reassembly_set_create( NULL, 8, 4, 16384, 16 );
    server->counters = counters;
    rocketnet_reassembly_set_begin_update( server->reassembly );
    rocketnet_server_protocol_update( server, (double) now );
}

static void rocketnet_server_teardown( struct rocketnet_server_protocol_t * server )
{
    rocketnet_encryption_map_destroy( server->encryption_map );
    rocketnet_used_token_history_destroy( server->used_token_history );
    rocketnet_client_slots_destroy( server->client_slots );
    rocketnet_server_acks_destroy( server->acks );
    rocketnet_reassembly_set_destroy( server->reassembly );
}

static void rocketnet_classify( const struct rocketnet_counters_t * counters, struct decision_t * d )
{
    if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] )
    {
        strcpy( d->reason, "expired" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_CREATED_IN_FUTURE] )
    {
        strcpy( d->reason, "future" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID] )
    {
        strcpy( d->reason, "timeout" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED] )
    {
        strcpy( d->reason, "address" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS] )
    {
        strcpy( d->reason, "address" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID] )
    {
        strcpy( d->reason, "protocol" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] )
    {
        strcpy( d->reason, "version" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] )
    {
        strcpy( d->reason, "signature" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK] )
    {
        strcpy( d->reason, "open" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH] )
    {
        strcpy( d->reason, "mismatch" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] )
    {
        strcpy( d->reason, "replay" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] )
    {
        strcpy( d->reason, "full" );
    }
    else if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] )
    {
        strcpy( d->reason, "server-key" );
    }
    else
    {
        strcpy( d->reason, "refused" );
    }
}

static void rocketnet_admit_request( struct rocketnet_server_protocol_t * server, const struct rocketnet_counters_t * counters,
                                     const uint8_t * signed_connect_data, const uint8_t * signature, uint64_t now,
                                     struct decision_t * d )
{
    struct rocketnet_address_t client_address;
    rocketnet_address_parse( &client_address, "198.51.100.7:56789" );

    uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
    rocketnet_write_connection_request_packet( request, rocketnet_test_filter_value, NULL,
                                               rocketnet_test_salt, rocketnet_test_client_eph_public,
                                               signed_connect_data, signature );

    uint8_t response[256];
    int response_bytes = 0;

    int result = rocketnet_server_protocol_process_connection_request(
        server, &client_address, request, sizeof( request ), now, (double) now,
        response, &response_bytes );

    if ( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
    {
        memcpy( request + ROCKETNET_PREFILTER_BYTES,
                response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
                ROCKETNET_COOKIE_FIELD_BYTES );
        result = rocketnet_server_protocol_process_connection_request(
            server, &client_address, request, sizeof( request ), now, (double) now,
            response, &response_bytes );
    }

    if ( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
    {
        d->accept = 1;
        strcpy( d->reason, "accept" );
    }
    else
    {
        d->accept = 0;
        rocketnet_classify( counters, d );
    }
}

// The rocketnet decision for one case: a fresh server, a token minted with
// the case's mutation, admitted through the real connection request ladder
// (or the replay handshakes). time is an argument.
static struct decision_t rocketnet_decide_case( const struct differential_case_t * c, uint64_t now )
{
    struct decision_t d;
    memset( &d, 0, sizeof( d ) );

    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    rocketnet_server_setup( &server, &counters, now );

    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    memset( &sf, 0, sizeof( sf ) );
    memset( &pf, 0, sizeof( pf ) );

    sf.version = ROCKETNET_WIRE_VERSION;
    sf.protocol_id = c->protocol_wrong ? 0xdeadbeefdeadbeefULL : ROCKETNET_TEST_PROTOCOL_ID;
    sf.create_timestamp = (uint64_t) ( (int64_t) now + c->create_offset );
    sf.expire_timestamp = (uint64_t) ( (int64_t) now + c->expire_offset );
    sf.timeout_seconds = c->timeout_seconds;
    sf.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    switch ( c->address_mode )
    {
        case 1:
            encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 11, 40000 );
            break;
        case 2:
        {
            static const uint16_t v6[8] = { 0x2001, 0x0db8, 0, 0, 0, 0, 0, 0x0010 };

            sf.address_flags = ROCKETNET_ADDRESS_FLAG_IPV6;
            encode_ipv6_token_address( sf.ipv6_address, v6, 40000 );
            break;
        }
        case 3:
            encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 10, 40123 );
            break;
        default:
            encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 10, 40000 );
            break;
    }

    memcpy( sf.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    pf.client_id = ROCKETNET_TEST_CLIENT_ID;
    memcpy( pf.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( pf.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_connect_token( &sf, &pf, rocketnet_test_backend_secret_key, token ) )
    {
        d.accept = 0;
        strcpy( d.reason, "mint" );
        goto out;
    }

    if ( c->replay )
    {
        rocketnet_decide_replay( &server, &counters, token, now, &d );
        goto out;
    }

    {
        struct rocketnet_connect_token_t ct;
        rocketnet_read_connect_token( &ct, token );

        if ( c->flip_authenticator )
        {
            ct.signature[0] ^= 0xff;
        }

        uint32_t slots = c->occupied_slots;
        const int max_slots = rocketnet_client_slots_max_clients( server.client_slots );
        if ( slots > (uint32_t) max_slots )
        {
            slots = (uint32_t) max_slots;
        }

        for ( uint32_t i = 0; i < slots; i++ )
        {
            uint8_t dummy[ROCKETNET_USER_DATA_BYTES];
            memset( dummy, 0, sizeof( dummy ) );
            rocketnet_client_slots_assign( server.client_slots, 0xD000 + (uint64_t) i, -1,
                                           0xD000 + (uint64_t) i, dummy );
        }

        rocketnet_admit_request( &server, &counters, ct.signed_connect_data, ct.signature, now, &d );
    }

out:
    rocketnet_server_teardown( &server );

    return d;
}

// --- the netcode live decision ------------------------------------------

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

// Live mode drives mas-bandwidth/netcode tag v1.2.1 (version info
// "NETCODE 1.02"). The library mints the token, a client writes the
// connection request, and a server with send/receive overrides decides.
// Overrides keep the run off the network: no socket is opened. Expiry inside
// the library uses the wall clock (the public generator and the packet
// reader both call time), so every case's times are expressed relative to
// the moment netcode reads the clock: a negative expire offset maps to
// expire_seconds 0 (already expired), and any other offset maps to the same
// count of seconds, so a crash input reproduces. The in-process clocks passed
// to create and update stay arguments and are not a sleep.
//
// netcode 1.02 authenticates the sealed private token with an AEAD MAC, not
// an Ed25519 signature. The bad-signature case flips one byte of that MAC;
// the reason class is still "signature", the authenticity check.

static const char * NC_SERVER_ADDRESS = "203.0.113.10:40000";
static const char * NC_CLIENT_ADDRESS = "198.51.100.7:40000";
static const char * NC_REPLAY_ADDRESS = "198.51.100.50:40000";
static const char * NC_OTHER_ADDRESS = "203.0.113.11:40000";
static const uint64_t NC_PROTOCOL_ID = 0x1122334455667788ULL;

enum
{
    NC_PIPE_SLOTS = 8,
    NC_PIPE_BYTES = 2048
};

struct nc_pipe_packet_t
{
    uint8_t data[NC_PIPE_BYTES];
    int bytes;
};

struct nc_pipe_t
{
    struct nc_pipe_packet_t packets[NC_PIPE_SLOTS];
    int count;
};

static struct nc_pipe_t g_from_client;
static struct nc_pipe_t g_from_server;
static char g_netcode_log[16384];

extern "C" int netcode_log_sink( const char * fmt, ... )
{
    char line[1024];
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( line, sizeof( line ), fmt, ap );
    va_end( ap );

    size_t used = strlen( g_netcode_log );
    size_t add = strlen( line );
    if ( used + add + 1 < sizeof( g_netcode_log ) )
    {
        memcpy( g_netcode_log + used, line, add + 1 );
    }

    return 0;
}

static void nc_pipe_push( struct nc_pipe_t * pipe, const uint8_t * data, int bytes )
{
    if ( pipe->count >= NC_PIPE_SLOTS || bytes <= 0 || bytes > NC_PIPE_BYTES )
        return;
    memcpy( pipe->packets[pipe->count].data, data, (size_t) bytes );
    pipe->packets[pipe->count].bytes = bytes;
    pipe->count++;
}

extern "C" void netcode_send_from_client( void * context, struct netcode_address_t * to,
                                          const uint8_t * packet_data, int packet_bytes )
{
    (void) context;
    (void) to;
    nc_pipe_push( &g_from_client, packet_data, packet_bytes );
}

extern "C" void netcode_send_from_server( void * context, struct netcode_address_t * to,
                                          const uint8_t * packet_data, int packet_bytes )
{
    (void) context;
    (void) to;
    nc_pipe_push( &g_from_server, packet_data, packet_bytes );
}

extern "C" int netcode_receive_nothing( void * context, struct netcode_address_t * from,
                                        uint8_t * packet_data, int max_packet_bytes )
{
    (void) context;
    (void) from;
    (void) packet_data;
    (void) max_packet_bytes;

    return 0;
}

static int nc_parse( struct netcode_address_t * out, const char * text )
{
    return netcode_parse_address( text, out ) == NETCODE_OK;
}

static void nc_private_key( uint8_t key[NETCODE_KEY_BYTES] )
{
    int i;
    for ( i = 0; i < NETCODE_KEY_BYTES; i++ )
    {
        key[i] = (uint8_t) ( 0x40 + i );
    }
}

static int nc_mint( uint8_t token[NETCODE_CONNECT_TOKEN_BYTES], const char * server_address,
                    int expire_seconds, int timeout_seconds, uint64_t client_id,
                    uint64_t protocol_id, const uint8_t * private_key )
{
    const char * public_addresses[1];
    const char * internal_addresses[1];
    uint8_t user_data[NETCODE_USER_DATA_BYTES];

    public_addresses[0] = server_address;
    internal_addresses[0] = server_address;
    memset( user_data, 0, sizeof( user_data ) );
    return netcode_generate_connect_token( 1, public_addresses, internal_addresses,
                                           expire_seconds, timeout_seconds, client_id,
                                           protocol_id, private_key, user_data, token ) == NETCODE_OK;
}

static struct netcode_server_t * nc_open_server( const char * address, int max_clients,
                                                 uint64_t protocol_id, const uint8_t * private_key )
{
    struct netcode_server_config_t config;
    struct netcode_server_t * server;

    netcode_default_server_config( &config );
    config.protocol_id = protocol_id;
    memcpy( config.private_key, private_key, NETCODE_KEY_BYTES );
    config.override_send_and_receive = 1;
    config.send_packet_override = netcode_send_from_server;
    config.receive_packet_override = netcode_receive_nothing;

    server = netcode_server_create( address, &config, 0.0 );
    if ( !server )
        return NULL;
    netcode_server_start( server, max_clients );
    if ( !netcode_server_running( server ) )
    {
        netcode_server_destroy( server );
        return NULL;
    }

    return server;
}

static struct netcode_client_t * nc_open_client( const char * address )
{
    struct netcode_client_config_t config;
    netcode_default_client_config( &config );
    config.override_send_and_receive = 1;
    config.send_packet_override = netcode_send_from_client;
    config.receive_packet_override = netcode_receive_nothing;
    return netcode_client_create( address, &config, 0.0 );
}

static int nc_capture_request( struct netcode_client_t * client, uint8_t * out, int * out_bytes )
{
    g_from_client.count = 0;
    netcode_client_update( client, 0.0 );
    if ( g_from_client.count < 1 )
        return 0;
    if ( g_from_client.packets[0].bytes <= 0 || g_from_client.packets[0].bytes > NC_PIPE_BYTES )
        return 0;
    memcpy( out, g_from_client.packets[0].data, (size_t) g_from_client.packets[0].bytes );
    *out_bytes = g_from_client.packets[0].bytes;

    return 1;
}

static void nc_classify( char * reason, size_t reason_size )
{
    const char * decision = "refuse:unclassified";

    if ( strstr( g_netcode_log, "server sent connection challenge packet" ) )
    {
        decision = "accept";
    }
    else if ( strstr( g_netcode_log, "connect token expired" ) )
    {
        decision = "refuse:expired";
    }
    else if ( strstr( g_netcode_log, "bad version info" ) )
    {
        decision = "refuse:version";
    }
    else if ( strstr( g_netcode_log, "wrong protocol id" ) )
    {
        decision = "refuse:protocol";
    }
    else if ( strstr( g_netcode_log, "failed to decrypt" ) )
    {
        decision = "refuse:signature";
    }
    else if ( strstr( g_netcode_log, "whitelist" ) )
    {
        decision = "refuse:address";
    }
    else if ( strstr( g_netcode_log, "already been used" ) )
    {
        decision = "refuse:replay";
    }
    else if ( strstr( g_netcode_log, "server is full" ) )
    {
        decision = "refuse:full";
    }

    snprintf( reason, reason_size, "%s", decision );
    if ( strcmp( decision, "refuse:unclassified" ) == 0 )
    {
        fprintf( stderr, "netcode live unclassified log:\n%s\n", g_netcode_log );
    }
}

static void nc_feed( struct netcode_server_t * server, struct netcode_address_t * from,
                     const uint8_t * packet, int bytes )
{
    uint8_t copy[NC_PIPE_BYTES];

    if ( bytes <= 0 || bytes > NC_PIPE_BYTES )
        return;
    memcpy( copy, packet, (size_t) bytes );
    g_netcode_log[0] = 0;
    g_from_server.count = 0;
    netcode_server_process_packet( server, from, copy, bytes );
}

// One client completes the handshake so a later request sees an occupied
// slot. idx makes the occupant's source address and client id distinct, so
// several occupants fill distinct slots. Returns 1 when the server counts
// that client as connected.
static int nc_occupy_one_slot( struct netcode_server_t * server, const uint8_t * private_key, int idx )
{
    struct netcode_client_t * client;
    struct netcode_address_t from;
    struct netcode_address_t server_address;
    uint8_t token[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t request[NC_PIPE_BYTES];
    int request_bytes = 0;
    char occupant_address[64];
    int i;
    int connected;

    const int connected_before = netcode_server_num_connected_clients( server );

    snprintf( occupant_address, sizeof( occupant_address ), "198.51.100.%d:41000", 20 + ( idx % 180 ) );
    client = nc_open_client( occupant_address );
    if ( !client )
        return 0;

    if ( !nc_mint( token, NC_SERVER_ADDRESS, 60, 15, 0x0ccULL + (uint64_t) idx,
                   NC_PROTOCOL_ID, private_key ) )
    {
        netcode_client_destroy( client );
        return 0;
    }

    netcode_client_connect( client, token );
    if ( !nc_capture_request( client, request, &request_bytes ) )
    {
        netcode_client_destroy( client );
        return 0;
    }

    if ( !nc_parse( &from, occupant_address ) || !nc_parse( &server_address, NC_SERVER_ADDRESS ) )
    {
        netcode_client_destroy( client );
        return 0;
    }

    nc_feed( server, &from, request, request_bytes );

    for ( i = 0; i < g_from_server.count; i++ )
    {
        netcode_client_process_packet( client, &server_address,
                                       g_from_server.packets[i].data,
                                       g_from_server.packets[i].bytes );
    }

    g_from_client.count = 0;
    netcode_client_update( client, 0.2 );

    for ( i = 0; i < g_from_client.count; i++ )
    {
        nc_feed( server, &from, g_from_client.packets[i].data, g_from_client.packets[i].bytes );
    }

    connected = netcode_server_num_connected_clients( server ) > connected_before;
    netcode_client_destroy( client );

    return connected;
}

// The netcode 1.02 live decision for one case. The netcode side reads its
// own wall clock for expiry; the case's offsets are relative to the moment
// netcode reads it (README, "The netcode side, live"). Returns accept
// (1) with the reason string, or refuse (0).
static int netcode_decide_case_live( const struct differential_case_t * c, uint64_t now,
                                     char * reason, size_t reason_size )
{
    (void) now;

    int expire_seconds = 60;

    {
        int64_t e = c->expire_offset;
        if ( e < 0 )
        {
            e = 0;
        }

        if ( e > 0x7fffffffLL )
        {
            e = 0x7fffffffLL;
        }

        expire_seconds = (int) e;
    }

    const int timeout_seconds = (int) c->timeout_seconds;
    const int max_clients = 8;
    uint64_t protocol_id = NC_PROTOCOL_ID;

    if ( c->protocol_wrong )
    {
        protocol_id = NC_PROTOCOL_ID ^ 0x1ULL;
    }

    const char * token_address = NC_SERVER_ADDRESS;

    if ( c->address_mode )
    {
        token_address = NC_OTHER_ADDRESS;
    }

    uint8_t private_key[NETCODE_KEY_BYTES];
    uint8_t token[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t request[NC_PIPE_BYTES];
    int request_bytes = 0;
    struct netcode_server_t * server;
    struct netcode_client_t * client;
    struct netcode_address_t from;

    nc_private_key( private_key );

    unsigned occupy = c->occupied_slots;
    if ( occupy > (unsigned) max_clients )
    {
        occupy = (unsigned) max_clients;
    }

    server = nc_open_server( NC_SERVER_ADDRESS, max_clients, NC_PROTOCOL_ID, private_key );
    if ( !server )
    {
        snprintf( reason, reason_size, "refuse:mint" );
        return 0;
    }

    for ( unsigned i = 0; i < occupy; i++ )
    {
        if ( !nc_occupy_one_slot( server, private_key, (int) i ) )
        {
            fprintf( stderr, "netcode live: full-server occupant %u did not connect\n%s\n", i, g_netcode_log );
            netcode_server_destroy( server );
            snprintf( reason, reason_size, "refuse:unclassified" );
            return 0;
        }
    }

    client = nc_open_client( NC_CLIENT_ADDRESS );
    if ( !client || !nc_mint( token, token_address, expire_seconds, timeout_seconds,
                              0x1000ULL + (uint64_t) ( now & 0xff ), protocol_id, private_key ) )
    {
        if ( client )
        {
            netcode_client_destroy( client );
        }

        netcode_server_destroy( server );
        snprintf( reason, reason_size, "refuse:mint" );
        return 0;
    }

    netcode_client_connect( client, token );
    if ( netcode_client_state( client ) != NETCODE_CLIENT_STATE_SENDING_CONNECTION_REQUEST ||
         !nc_capture_request( client, request, &request_bytes ) ||
         !nc_parse( &from, NC_CLIENT_ADDRESS ) )
    {
        netcode_client_destroy( client );
        netcode_server_destroy( server );
        snprintf( reason, reason_size, "refuse:mint" );
        return 0;
    }

    netcode_client_destroy( client );

    if ( c->flip_authenticator )
    {
        request[request_bytes - 1] ^= 0xff;
    }

    nc_feed( server, &from, request, request_bytes );

    if ( c->replay )
    {
        struct netcode_address_t replay_from;
        if ( !nc_parse( &replay_from, NC_REPLAY_ADDRESS ) )
        {
            netcode_server_destroy( server );
            snprintf( reason, reason_size, "refuse:mint" );
            return 0;
        }

        nc_feed( server, &replay_from, request, request_bytes );
    }

    nc_classify( reason, reason_size );
    netcode_server_destroy( server );

    return strncmp( reason, "accept", 6 ) == 0;
}

#endif // defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

// --- the verdict --------------------------------------------------------

static const char * verdict( const struct decision_t * rocketnet, const char * netcode )
{
    int netcode_accept = strncmp( netcode, "accept", 6 ) == 0;
    if ( rocketnet->accept == netcode_accept )
        return "agree";
    if ( !netcode_accept && rocketnet->accept )
        return "rocketnet-weaker";
    return "rocketnet-stricter";
}

// A rocketnet-stricter reason is permitted only when it is one of the classes
// tools/differential/README.md documents as permitted-stricter: timeout (the
// table's timeout_out_of_range row) and future (the corpus's
// structured_before_create row). A stricter class not on the list is a
// finding and aborts the fuzz target.
static inline int stricter_class_permitted( const char * reason )
{
    if ( strcmp( reason, "timeout" ) == 0 )
        return 1;
    if ( strcmp( reason, "future" ) == 0 )
        return 1;

    return 0;
}

#endif // ROCKETNET_TOOLS_DIFFERENTIAL_DECIDE_H
