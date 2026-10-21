/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Cross-implementation differential: the token/server admission DECISION.

    For each case in cases.tsv this harness drives rocketnet's connect-token
    admission path in process (the token open and the server's connection
    request ladder, with time passed in as an argument) and compares the
    accept/refuse decision against the netcode 1.02 reference decision. The
    two libraries' wire formats differ, so the bytes are never compared --
    only the decision and the refusal reason class.

    The netcode side has two modes:

      * reference -- the decision recorded in cases.tsv, taken from the
        netcode 1.02 source (mas-bandwidth/netcode at the pinned tag). This
        runs anywhere and is what the table is diffed against by default.

      * live -- when built with -DROCKETNET_DIFFERENTIAL_NETCODE_DIR, the
        same decision is computed by driving the netcode 1.02 library
        (mas-bandwidth/netcode tag v1.2.1, version info "NETCODE 1.02"):
        mint a connect token, let a client emit the connection request,
        and hand that packet to a server that does not open a socket.
        The harness reports when the live answer disagrees with the
        reference column.

    Verdicts:

      agree            both libraries reach the same decision
      rocketnet-weaker netcode refuses, rocketnet admits (a finding)
      rocketnet-stricter rocketnet refuses, netcode admits (judged against
                        REQUIREMENTS.md: no property weaker than netcode 1.02)
*/

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

// The netcode side, when the bench supplies the reference source. This header
// is only pulled in when run.sh was given a NETCODE_DIR; the default build
// diffs against the reference column in cases.tsv.
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
#include "netcode.h"
#endif

// --- the case table -----------------------------------------------------

// One row per case, in the order cases.tsv lists them. The id names the
// mutation this harness applies to the minted token before admission.
enum
{
    CASE_VALID_SIGNATURE = 0,
    CASE_EXPIRED_TOKEN,
    CASE_BAD_SIGNATURE,
    CASE_WRONG_SERVER_ADDRESS,
    CASE_REPLAYED_TOKEN,
    CASE_FULL_SERVER,
    CASE_WRONG_PROTOCOL_ID,
    CASE_TIMEOUT_OUT_OF_RANGE,
    CASE_COUNT
};

struct case_row_t
{
    const char * id;
    const char * description;
    const char * netcode_reference;      // accept, or refuse:<class>
};

static const char * case_id_name( int id )
{
    switch ( id )
    {
        case CASE_VALID_SIGNATURE:    return "valid_signature";
        case CASE_EXPIRED_TOKEN:      return "expired_token";
        case CASE_BAD_SIGNATURE:      return "bad_signature";
        case CASE_WRONG_SERVER_ADDRESS: return "wrong_server_address";
        case CASE_REPLAYED_TOKEN:     return "replayed_token";
        case CASE_FULL_SERVER:        return "full_server";
        case CASE_WRONG_PROTOCOL_ID:  return "wrong_protocol_id";
        case CASE_TIMEOUT_OUT_OF_RANGE: return "timeout_out_of_range";
        default: return "unknown";
    }
}

static struct case_row_t g_cases[CASE_COUNT];

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

// --- the rocketnet decision ---------------------------------------------

// A fresh server and a freshly minted token for the case, admitted through
// the real connection request ladder. Returns accept (1) with reason
// "accept", or refuse (0) with the reason class the ladder actually took.

struct decision_t
{
    int accept;
    char reason[64];
};

// One client's handshake against the server, packet for packet, from one
// address: request, cookie, challenge, response. The server's update runs
// each step, as the transport runs it, so the per-update handshake budget
// resets. Stops when the server holds one more connected slot than it did
// at entry, or the client gives up, or max_seconds of the passed-in clock
// elapse. Returns 1 when the server admitted this client.

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
                                                          payload, sizeof(payload), &payload_bytes );
        }
        if ( rocketnet_client_slots_num_connected( server->client_slots ) > connected_before )
            return 1;
    }
    return 0;
}

// The replay case. The server binds a token to its source when a connection
// response opens, never at the request (STANDARD.md; tla/RNSecurityTables.tla,
// BoundOnlyToOpener), so "a token already admitted" means a handshake driven
// through response open: a first client holding the token connects and the
// server admits it. A second client holding the same token then connects
// from another address, and its handshake is driven through response open as
// far as the server lets it. The refusal is read from the counters that
// fired: the request ladder (already used by a different address, already
// admitted its connection) or the response (already admitted its
// connection). The second client holds the token keys, so it is at least as
// strong as an attacker replaying the captured request, which cannot open
// the challenge at all.

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
    memset( &first_counters, 0, sizeof(first_counters) );
    memset( &replay_counters, 0, sizeof(replay_counters) );
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
    if ( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] ||
         counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED] )
        strcpy( d->reason, "replay" );
    else
        strcpy( d->reason, "refused" );
}

static struct decision_t rocketnet_decide( int case_id, uint64_t now )
{
    struct decision_t d;
    memset( &d, 0, sizeof(d) );

    // a server, exactly as the unit suite's request-ladder test builds one
    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof(server) );
    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_test_backend_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );
    memcpy( server.server_secret_key, rocketnet_test_server_secret_key, ROCKETNET_KEY_BYTES );
    rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" );
    memcpy( server.cookie_key, rocketnet_test_backend_secret_key, 32 );   // any 32-byte cookie key
    server.cookie_lifetime_seconds = 3;
    server.max_token_lifetime_seconds = 600;
    server.handshake_deadline_seconds = 5.0;
    server.max_timeout_seconds = 0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    server.acks = rocketnet_server_acks_create( NULL, 4 );
    server.reassembly = rocketnet_reassembly_set_create( NULL, 4, 4, 16384, 16 );
    server.counters = &counters;
    rocketnet_reassembly_set_begin_update( server.reassembly );
    rocketnet_server_protocol_update( &server, (double) now );

    struct rocketnet_address_t client_address;
    rocketnet_address_parse( &client_address, "198.51.100.7:56789" );

    // the signed and private token fields, then the case mutation
    struct rocketnet_signed_connect_data_t sf;
    struct rocketnet_connect_token_private_t pf;
    memset( &sf, 0, sizeof(sf) );
    memset( &pf, 0, sizeof(pf) );

    sf.version = ROCKETNET_WIRE_VERSION;
    sf.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    sf.create_timestamp = now - 10;
    sf.expire_timestamp = now + 300;
    sf.timeout_seconds = 10;
    sf.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 10, 40000 );
    memcpy( sf.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    pf.client_id = ROCKETNET_TEST_CLIENT_ID;
    memcpy( pf.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( pf.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    switch ( case_id )
    {
        case CASE_EXPIRED_TOKEN:
            sf.expire_timestamp = now - 1;
            break;
        case CASE_WRONG_PROTOCOL_ID:
            sf.protocol_id = 0xdeadbeefdeadbeefULL;
            break;
        case CASE_TIMEOUT_OUT_OF_RANGE:
            sf.timeout_seconds = 0;
            break;
        case CASE_WRONG_SERVER_ADDRESS:
            encode_ipv4_token_address( sf.ipv4_address, 203, 0, 113, 11, 40000 );
            break;
        default:
            break;
    }

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_connect_token( &sf, &pf, rocketnet_test_backend_secret_key, token ) )
    {
        d.accept = 0;
        strcpy( d.reason, "mint" );
        goto out;
    }

    if ( case_id == CASE_REPLAYED_TOKEN )
    {
        rocketnet_decide_replay( &server, &counters, token, now, &d );
        goto out;
    }

    {
        struct rocketnet_connect_token_t ct;
        rocketnet_read_connect_token( &ct, token );

        if ( case_id == CASE_BAD_SIGNATURE )
            ct.signature[0] ^= 0xff;

        uint8_t request[ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES];
        rocketnet_write_connection_request_packet( request, rocketnet_test_filter_value, NULL,
                                                   rocketnet_test_salt, rocketnet_test_client_eph_public,
                                                   ct.signed_connect_data, ct.signature );

        if ( case_id == CASE_FULL_SERVER )
        {
            uint8_t dummy[ROCKETNET_USER_DATA_BYTES];
            memset( dummy, 0, sizeof(dummy) );
            for ( int i = 0; i < 4; i++ )
                rocketnet_client_slots_assign( server.client_slots, 0xD000 + (uint64_t) i, -1, dummy );
        }

        uint8_t response[256];
        int response_bytes = 0;

        int result = rocketnet_server_protocol_process_connection_request(
                        &server, &client_address, request, sizeof(request), now, (double) now,
                        response, &response_bytes );

        if ( result == ROCKETNET_SERVER_PROCESS_SEND_COOKIE )
        {
            memcpy( request + ROCKETNET_PREFILTER_BYTES,
                    response + ROCKETNET_PREFILTER_BYTES + ROCKETNET_CONNECTION_SALT_BYTES,
                    ROCKETNET_COOKIE_FIELD_BYTES );
            result = rocketnet_server_protocol_process_connection_request(
                        &server, &client_address, request, sizeof(request), now, (double) now,
                        response, &response_bytes );
        }

        if ( result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE )
        {
            d.accept = 1;
            strcpy( d.reason, "accept" );
        }
        else
        {
            d.accept = 0;
            // read the reason the ladder actually took, in the standard's order
            if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] )
                strcpy( d.reason, "expired" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID] )
                strcpy( d.reason, "timeout" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED] )
                strcpy( d.reason, "address" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS] )
                strcpy( d.reason, "address" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID] )
                strcpy( d.reason, "protocol" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] )
                strcpy( d.reason, "version" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] )
                strcpy( d.reason, "signature" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK] )
                strcpy( d.reason, "open" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH] )
                strcpy( d.reason, "mismatch" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] ||
                      counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED] )
                strcpy( d.reason, "replay" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] )
                strcpy( d.reason, "full" );
            else if ( counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] )
                strcpy( d.reason, "server-key" );
            else
                strcpy( d.reason, "refused" );
        }
    }

out:
    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_reassembly_set_destroy( server.reassembly );
    return d;
}

// --- the netcode decision ------------------------------------------------

// The netcode 1.02 reference decision for a case is what the pinned netcode
// source reaches for the equivalent token. In reference mode this is read
// straight from cases.tsv. In live mode the netcode library is asked and the
// answer compared against the reference column; a mismatch is reported but
// does not change the verdict against rocketnet.

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )

// Live mode drives mas-bandwidth/netcode tag v1.2.1 (version info
// "NETCODE 1.02"). The library mints the token, a client writes the
// connection request, and a server with send/receive overrides decides.
// Overrides keep the run off the network: no socket is opened. Expiry inside
// the library uses the wall clock (the public generator and the packet
// reader both call time), so the expired case passes expire_seconds of zero,
// which that generator treats as already expired. The double clocks passed
// to create and update stay arguments and are not a sleep.
//
// netcode 1.02 authenticates the sealed private token with an AEAD MAC, not
// an Ed25519 signature. The bad-signature case flips one byte of that MAC;
// the reason class is still "signature", the authenticity check.

static const char * NC_SERVER_ADDRESS = "203.0.113.10:40000";
static const char * NC_CLIENT_ADDRESS = "198.51.100.7:40000";
static const char * NC_REPLAY_ADDRESS = "198.51.100.50:40000";
static const char * NC_OTHER_ADDRESS = "203.0.113.11:40000";
static const char * NC_OCCUPANT_ADDRESS = "198.51.100.20:41000";
static const uint64_t NC_PROTOCOL_ID = 0x1122334455667788ULL;

enum { NC_PIPE_SLOTS = 8, NC_PIPE_BYTES = 2048 };

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
        memcpy( g_netcode_log + used, line, add + 1 );
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
        key[i] = (uint8_t) ( 0x40 + i );
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
        decision = "accept";
    else if ( strstr( g_netcode_log, "connect token expired" ) )
        decision = "refuse:expired";
    else if ( strstr( g_netcode_log, "wrong protocol id" ) )
        decision = "refuse:protocol";
    else if ( strstr( g_netcode_log, "failed to decrypt" ) )
        decision = "refuse:signature";
    else if ( strstr( g_netcode_log, "whitelist" ) )
        decision = "refuse:address";
    else if ( strstr( g_netcode_log, "already been used" ) )
        decision = "refuse:replay";
    else if ( strstr( g_netcode_log, "server is full" ) )
        decision = "refuse:full";
    snprintf( reason, reason_size, "%s", decision );
    if ( strcmp( decision, "refuse:unclassified" ) == 0 )
        fprintf( stderr, "netcode live unclassified log:\n%s\n", g_netcode_log );
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

// One client completes the handshake so a later request sees a full server.
// Returns 1 when the server counts that client as connected.
static int nc_occupy_one_slot( struct netcode_server_t * server, const uint8_t * private_key )
{
    struct netcode_client_t * client;
    struct netcode_address_t from;
    struct netcode_address_t server_address;
    uint8_t token[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t request[NC_PIPE_BYTES];
    int request_bytes = 0;
    int i;
    int connected;

    client = nc_open_client( NC_OCCUPANT_ADDRESS );
    if ( !client )
        return 0;
    if ( !nc_mint( token, NC_SERVER_ADDRESS, 60, 15, 0x0ccULL, NC_PROTOCOL_ID, private_key ) )
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
    if ( !nc_parse( &from, NC_OCCUPANT_ADDRESS ) || !nc_parse( &server_address, NC_SERVER_ADDRESS ) )
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
        nc_feed( server, &from, g_from_client.packets[i].data, g_from_client.packets[i].bytes );

    connected = netcode_server_num_connected_clients( server ) == 1;
    netcode_client_destroy( client );
    return connected;
}

static int netcode_decide_live( int case_id, uint64_t now, char * reason, size_t reason_size )
{
    const char * token_address = NC_SERVER_ADDRESS;
    uint64_t protocol_id = NC_PROTOCOL_ID;
    int expire_seconds = 60;
    int timeout_seconds = 15;
    int max_clients = 8;
    uint8_t private_key[NETCODE_KEY_BYTES];
    uint8_t token[NETCODE_CONNECT_TOKEN_BYTES];
    uint8_t request[NC_PIPE_BYTES];
    int request_bytes = 0;
    struct netcode_server_t * server;
    struct netcode_client_t * client;
    struct netcode_address_t from;

    (void) now;
    nc_private_key( private_key );

    if ( case_id == CASE_EXPIRED_TOKEN )
        expire_seconds = 0;
    if ( case_id == CASE_WRONG_PROTOCOL_ID )
        protocol_id = NC_PROTOCOL_ID ^ 0x1ULL;
    if ( case_id == CASE_TIMEOUT_OUT_OF_RANGE )
        timeout_seconds = 0;
    if ( case_id == CASE_WRONG_SERVER_ADDRESS )
        token_address = NC_OTHER_ADDRESS;
    if ( case_id == CASE_FULL_SERVER )
        max_clients = 1;

    server = nc_open_server( NC_SERVER_ADDRESS, max_clients, NC_PROTOCOL_ID, private_key );
    if ( !server )
    {
        snprintf( reason, reason_size, "refuse:mint" );
        return 0;
    }

    if ( case_id == CASE_FULL_SERVER && !nc_occupy_one_slot( server, private_key ) )
    {
        fprintf( stderr, "netcode live: full-server occupant did not connect\n%s\n", g_netcode_log );
        netcode_server_destroy( server );
        snprintf( reason, reason_size, "refuse:unclassified" );
        return 0;
    }

    client = nc_open_client( NC_CLIENT_ADDRESS );
    if ( !client || !nc_mint( token, token_address, expire_seconds, timeout_seconds,
                              0x1000ULL + (uint64_t) case_id, protocol_id, private_key ) )
    {
        if ( client )
            netcode_client_destroy( client );
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

    if ( case_id == CASE_BAD_SIGNATURE )
        request[request_bytes - 1] ^= 0xff;

    nc_feed( server, &from, request, request_bytes );

    if ( case_id == CASE_REPLAYED_TOKEN )
    {
        struct netcode_address_t replay_from;
        if ( !nc_parse( &replay_from, NC_REPLAY_ADDRESS ) )
        {
            netcode_server_destroy( server );
            snprintf( reason, reason_size, "refuse:mint" );
            return 0;
        }
        // the first presentation records the token MAC. the second, from
        // another address, is the decision under test.
        nc_feed( server, &replay_from, request, request_bytes );
    }

    nc_classify( reason, reason_size );
    netcode_server_destroy( server );
    return strncmp( reason, "accept", 6 ) == 0;
}
#endif

// --- transport --------------------------------------------------------------

static int parse_case_table( const char * path )
{
    FILE * f = fopen( path, "rb" );
    if ( !f )
        return -1;

    char line[512];
    int n = 0;
    while ( fgets( line, sizeof(line), f ) )
    {
        if ( line[0] == '#' || line[0] == '\n' || line[0] == '\r' )
            continue;
        char * nl = strchr( line, '\n' );
        if ( nl ) *nl = 0;
        nl = strchr( line, '\r' );
        if ( nl ) *nl = 0;

        char * id = line;
        char * t1 = strchr( id, '\t' );
        if ( !t1 ) continue;
        *t1++ = 0;
        char * description = t1;
        char * t2 = strchr( description, '\t' );
        if ( !t2 ) continue;
        *t2++ = 0;
        char * reference = t2;

        if ( n < CASE_COUNT )
        {
            g_cases[n].id = strdup( id );
            g_cases[n].description = strdup( description );
            g_cases[n].netcode_reference = strdup( reference );
            n++;
        }
    }
    fclose( f );
    return n;
}

static void free_case_table()
{
    for ( int i = 0; i < CASE_COUNT; i++ )
    {
        free( (void*) g_cases[i].id );
        free( (void*) g_cases[i].description );
        free( (void*) g_cases[i].netcode_reference );
        g_cases[i].id = NULL;
        g_cases[i].description = NULL;
        g_cases[i].netcode_reference = NULL;
    }
}

static const char * verdict( const struct decision_t * rocketnet, const char * netcode )
{
    int netcode_accept = strncmp( netcode, "accept", 6 ) == 0;
    if ( rocketnet->accept == netcode_accept )
        return "agree";
    if ( !netcode_accept && rocketnet->accept )
        return "rocketnet-weaker";
    return "rocketnet-stricter";
}

int main( int argc, const char ** argv )
{
    const char * cases_path = "tools/differential/cases.tsv";
    if ( argc > 1 )
        cases_path = argv[1];

    rocketnet_quiet( true );

#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    netcode_log_level( NETCODE_LOG_LEVEL_DEBUG );
    netcode_set_printf_function( netcode_log_sink );
    if ( netcode_init() != NETCODE_OK )
    {
        fprintf( stderr, "netcode_init failed\n" );
        return 1;
    }
#endif

    if ( parse_case_table( cases_path ) != CASE_COUNT )
    {
        fprintf( stderr, "could not read %d cases from %s\n", CASE_COUNT, cases_path );
        return 1;
    }

    const uint64_t now = ROCKETNET_TEST_CREATE_TS;

    printf( "# case\tnetcode\trocketnet\tverdict\n" );
    for ( int i = 0; i < CASE_COUNT; i++ )
    {
        struct decision_t rn = rocketnet_decide( i, now );

        const char * netcode = g_cases[i].netcode_reference;
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
        char live_reason[64];
        if ( netcode_decide_live( i, now, live_reason, sizeof(live_reason) ) >= 0 )
        {
            if ( strcmp( live_reason, g_cases[i].netcode_reference ) != 0 )
                fprintf( stderr, "live netcode disagrees with the reference column on %s\n", g_cases[i].id );
            netcode = live_reason;
        }
#endif

        char rocketnet[72];
        if ( rn.accept )
            strcpy( rocketnet, "accept" );
        else
            snprintf( rocketnet, sizeof(rocketnet), "refuse:%s", rn.reason );

        printf( "%s\t%s\t%s\t%s\n", g_cases[i].id, netcode, rocketnet,
                verdict( &rn, netcode ) );
    }

    free_case_table();
#if defined( ROCKETNET_DIFFERENTIAL_NETCODE_DIR )
    netcode_term();
#endif
    return 0;
}
