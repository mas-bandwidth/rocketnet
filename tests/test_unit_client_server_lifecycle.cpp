/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public client and server lifecycle (tla/ClientSession.tla,
// tla/ServerSession.tla): a client connects, disconnects and connects again;
// a server starts, stops and starts again. Everything runs in one process
// over the in-memory loopback transport with step-driven clients: no socket,
// no thread, no sleep, and time is an argument starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#if ROCKETNET_DEV
#include "test_env_source.h"
#endif // #if ROCKETNET_DEV
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_server_protocol.h"
#include "rocketnet_internal.h"
#include "rocketnet_message_object.h"
#include "rocketnet_packet_builder.h"
#include "token_string.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define LIFECYCLE_PROTOCOL 0x7A11C1C1E5EED001ULL
#define LIFECYCLE_MAX_CLIENTS 4

#if ROCKETNET_DEV
uint64_t rocketnet_server_transport_disconnect_seals_for_test( struct rocketnet_server_transport_t * server_transport );
int rocketnet_server_transport_disconnect_burst_key_for_test( struct rocketnet_server_transport_t * server_transport,
                                                              int index, uint8_t * out, int out_bytes, int * head, int * count );

struct lifecycle_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
};

static double lifecycle_now = 1000.0;
static double lifecycle_clock( void * ) { return lifecycle_now; }

static void lifecycle_identity( lifecycle_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3C ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
}

// set nonzero by a test that queues more messages per connection than the
// defaults hold: the window, the send queue and the send buffer then fit
// that many messages, on the server and every client the world creates
static int lifecycle_queue_messages = 0;
static int lifecycle_disconnect_packets_per_update = 0;

static void lifecycle_queue_config( int * reliable_messages_in_flight, int * packets_in_flight, int * send_queue_messages, int * send_message_buffer_bytes )
{
    if ( lifecycle_queue_messages == 0 )
        return;
    *reliable_messages_in_flight = lifecycle_queue_messages;
    if ( *packets_in_flight < lifecycle_queue_messages )
        *packets_in_flight = lifecycle_queue_messages;                  // the sent map holds the window
    *send_queue_messages = lifecycle_queue_messages;
    *send_message_buffer_bytes = lifecycle_queue_messages * 256;      // a send-store chunk per message, with room
}

static void lifecycle_server_config( rocketnet_server_config_t * config, const lifecycle_identity_t * identity, int max_clients )
{
    rocketnet_server_default_config( config );
    config->protocol_id = LIFECYCLE_PROTOCOL;
    config->max_clients = max_clients;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
    if ( lifecycle_disconnect_packets_per_update > 0 )
    {
        config->disconnect_packets_per_update = lifecycle_disconnect_packets_per_update;
    }
    lifecycle_queue_config( &config->reliable_messages_in_flight, &config->packets_in_flight, &config->send_queue_messages, &config->send_message_buffer_bytes );
}

static void lifecycle_client_config( rocketnet_client_test_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = lifecycle_clock;
    lifecycle_queue_config( &config->reliable_messages_in_flight, &config->packets_in_flight, &config->send_queue_messages, &config->send_message_buffer_bytes );
}

// the token string sealed to the server's own key, naming 127.0.0.1:port

static void lifecycle_token( const rocketnet_server_t * server, uint16_t port, const lifecycle_identity_t * identity, uint64_t client_id, int32_t timeout_seconds, char * token, const uint8_t * user_data = NULL )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );

    const char * addresses[] = { address };
    check( rn_test_server_token_string( identity->backend_secret, server,
                                        LIFECYCLE_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, timeout_seconds, user_data,
                                        token ) == ROCKETNET_STATUS_OK );
}

struct lifecycle_world_t
{
    lifecycle_identity_t identity;
    rocketnet_server_t * server;
    rocketnet_client_t * clients[LIFECYCLE_MAX_CLIENTS];
    int num_clients;
    double time;
};

static void lifecycle_world_create( lifecycle_world_t * world, int max_clients, int num_clients )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    lifecycle_now = world->time;
    lifecycle_identity( &world->identity );

    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &world->identity, max_clients );
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world->server != NULL );
    check( rocketnet_server_running( world->server ) == 0 );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_running( world->server ) == 1 );
    world->num_clients = num_clients;

    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_test_config_t client_config;
        lifecycle_client_config( &client_config );
        world->clients[i] = rocketnet_client_create( NULL, &client_config, NULL );
        check( world->clients[i] != NULL );
        check( rocketnet_client_state( world->clients[i] ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    }
}

static void lifecycle_world_destroy( lifecycle_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
    {
        rn_test_client_stop_and_destroy( world->clients[i] );
    }
    rn_test_server_stop_and_destroy( world->server );
}

static void lifecycle_pump( lifecycle_world_t * world, double dt = 0.01 )
{
    lifecycle_now = world->time;

    for ( int i = 0; i < world->num_clients; i++ )
    {
        if ( !world->clients[i] )
            continue;
        rocketnet_client_update( world->clients[i], world->time );
        rocketnet_client_send_packets( world->clients[i] );
    }

    if ( rocketnet_server_running( world->server ) )
    {
        rocketnet_server_update( world->server, world->time );
        rocketnet_server_send_packets( world->server );
    }

    world->time += dt;
}

static void lifecycle_connect( lifecycle_world_t * world, int client, uint64_t client_id, const uint8_t * user_data = NULL )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world->server, rocketnet_server_port( world->server ), &world->identity, client_id, 300, token, user_data );
    check( rn_test_client_connect_after_completion( world->clients[client], token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( world->clients[client] ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( world );
    }
    check( rocketnet_client_state( world->clients[client] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int index = rocketnet_client_index( world->clients[client] );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world->server, index ); i++ )
    {
        lifecycle_pump( world );
    }
    check( rocketnet_server_client_connected( world->server, index ) != 0 );
    check( rocketnet_server_client_id( world->server, index ) == client_id );
}

static int lifecycle_key_nonzero( const uint8_t * key )
{
    int count = 0;
    for ( int i = 0; i < ROCKETNET_KEY_BYTES; i++ )
    {
        count += key[i] != 0 ? 1 : 0;
    }
    return count;
}

static void test_server_disconnect_allowance_one_spreads_one_burst()
{
    lifecycle_disconnect_packets_per_update = 1;

    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    lifecycle_disconnect_packets_per_update = 0;
    lifecycle_connect( &world, 0, 0x1000 );

    const int slot = rocketnet_client_index( world.clients[0] );
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( world.server ) );
    const uint64_t before = rocketnet_server_transport_disconnect_seals_for_test( transport );

    rocketnet_server_disconnect_client( world.server, slot );
    check( !rocketnet_server_client_connected( world.server, slot ) );
    check( rocketnet_server_transport_disconnect_seals_for_test( transport ) == before );

    uint8_t key[ROCKETNET_KEY_BYTES];
    int head = -1;
    int count = -1;
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, 0, key,
                                                                     (int) sizeof( key ), &head, &count ) == 1 );
    check( count == 1 );
    check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, head, key,
                                                                     (int) sizeof( key ), NULL, NULL ) == 1 );
    check( lifecycle_key_nonzero( key ) > 0 );

    uint64_t sealed = 0;

    for ( int update = 0; count > 0; update++ )
    {
        check( update < ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );

        const uint64_t step_before = rocketnet_server_transport_disconnect_seals_for_test( transport );
        lifecycle_pump( &world );

        const uint64_t step = rocketnet_server_transport_disconnect_seals_for_test( transport ) - step_before;
        check( step <= 1 );
        sealed += step;
        check( rocketnet_server_transport_disconnect_burst_key_for_test( transport, head, key,
                                                                         (int) sizeof( key ), NULL, &count ) == 1 );
    }

    check( sealed == ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( lifecycle_key_nonzero( key ) == 0 );
    lifecycle_world_destroy( &world );
}

REGISTER_UNIT_TEST( test_server_disconnect_allowance_one_spreads_one_burst );

static void lifecycle_client_send( rocketnet_client_t * client, uint8_t tag, uint8_t value )
{
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out != NULL );
    if ( !out )
        return;
    out[0] = tag;
    out[1] = value;
    check( rocketnet_client_finish_message( client, 2 ) != 0 );
}

static void lifecycle_server_send( rocketnet_server_t * server, uint64_t index, uint8_t tag, uint8_t value )
{
    uint8_t * out = rocketnet_server_start_message( server, index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out != NULL );
    if ( !out )
        return;
    out[0] = tag;
    out[1] = value;
    check( rocketnet_server_finish_message( server, 2 ) != 0 );
}

static void test_public_receipts_report_available_on_both_ends()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );
    lifecycle_connect( &world, 0, 42 );

    rocketnet_client_t * client = world.clients[0];
    const int index = rocketnet_client_index( client );
    uint32_t client_serials[3];
    uint32_t server_serials[3];

    for ( int i = 0; i < 3; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 1 );
        check( out != NULL );
        *out = (uint8_t) i;
        client_serials[i] = rocketnet_client_finish_message( client, 1 );
        check( client_serials[i] != 0 );
        out = rocketnet_server_start_message( world.server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 1 );
        check( out != NULL );
        *out = (uint8_t) i;
        server_serials[i] = rocketnet_server_finish_message( world.server, 1 );
        check( server_serials[i] != 0 );
    }

    bool client_checked = false;
    bool server_checked = false;
    for ( int tick = 0; tick < 400 && !( client_checked && server_checked ); tick++ )
    {
        lifecycle_pump( &world );

        // every delivery is received before the next update (standard/MESSAGES.md m1.6)

        while ( rocketnet_client_receive_messages( client ) != NULL )
        {
        }

        while ( rocketnet_server_receive_messages( world.server ) != NULL )
        {
        }

        for ( int side = 0; side < 2; side++ )
        {
            if ( side == 0 ? client_checked : server_checked )
                continue;

            const uint64_t misuse_before = rocketnet_api_misuse_count();
            const int available = side == 0 ? rocketnet_client_receipts( client, NULL, 0 )
                                            : rocketnet_server_receipts( world.server, index, NULL, 0 );
            check( rocketnet_api_misuse_count() == misuse_before );
            if ( available == 0 )
                continue;
            check( available == 3 );

            rocketnet_receipt_t receipts[3];
            memset( receipts, 0, sizeof( receipts ) );

            const int short_count = side == 0 ? rocketnet_client_receipts( client, receipts, 1 )
                                              : rocketnet_server_receipts( world.server, index, receipts, 1 );
            check( short_count == available );
            check( receipts[1].serial == 0 && receipts[2].serial == 0 );

            const int query_count = side == 0 ? rocketnet_client_receipts( client, receipts, 0 )
                                              : rocketnet_server_receipts( world.server, index, receipts, 0 );
            check( query_count == available );

            const int invalid_count = side == 0 ? rocketnet_client_receipts( client, receipts, -1 )
                                                : rocketnet_server_receipts( world.server, index, receipts, -1 );
            check( invalid_count == 0 );
            check( receipts[1].serial == 0 && receipts[2].serial == 0 );
            check( rocketnet_api_misuse_count() == misuse_before );

            const int full_count = side == 0 ? rocketnet_client_receipts( client, receipts, 3 )
                                             : rocketnet_server_receipts( world.server, index, receipts, 3 );
            check( full_count == available );

            for ( int i = 0; i < 3; i++ )
            {
                check( receipts[i].serial == ( side == 0 ? client_serials[i] : server_serials[i] ) );
                check( receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            }

            if ( side == 0 )
                client_checked = true;
            else
                server_checked = true;
        }
    }

    check( client_checked && server_checked );
    lifecycle_pump( &world );
    check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );
    check( rocketnet_server_receipts( world.server, index, NULL, 0 ) == 0 );
    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    check( rocketnet_server_receipts( world.server, index, NULL, 0 ) == 0 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_public_receipts_report_available_on_both_ends );

// Pumps until the server holds `want` messages from slot index and the client
// `want` from the server, all tagged `tag` and in order; anything with another
// tag fails the test.

static void lifecycle_exchange( lifecycle_world_t * world, int client, uint8_t tag, int want )
{
    rocketnet_client_t * c = world->clients[client];
    const int index = rocketnet_client_index( c );
    check( index >= 0 );

    for ( int i = 0; i < want; i++ )
    {
        lifecycle_client_send( c, tag, (uint8_t) i );
        lifecycle_server_send( world->server, (uint64_t) index, tag, (uint8_t) i );
    }

    int at_server = 0, at_client = 0;
    for ( int step = 0; step < 400 && ( at_server < want || at_client < want ); step++ )
    {
        lifecycle_pump( world );

        const uint8_t * data = NULL;
        int bytes = 0;
        uint64_t sender = ~0ULL;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
                sender = (uint64_t) batch->client_index[i];
                check( sender == (uint64_t) index );
                check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_server );

                at_server++;
            }
        }

        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( c ) ) != NULL )
        {
            for ( int i = 0; i < incoming->num_messages; i++ )
            {
                data = incoming->data[i];
                bytes = incoming->bytes[i];
                check( bytes == 2 && data[0] == tag && data[1] == (uint8_t) at_client );

                at_client++;
            }
        }
    }

    check( at_server == want && at_client == want );
}

static void lifecycle_disconnect( lifecycle_world_t * world, int client )
{
    const int index = rocketnet_client_index( world->clients[client] );
    rocketnet_client_disconnect( world->clients[client] );
    rn_test_client_wait_disconnected( world->clients[client] );
    check( rocketnet_client_state( world->clients[client] ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    for ( int i = 0; i < 200 && rocketnet_server_client_connected( world->server, index ); i++ )
    {
        lifecycle_pump( world );
    }
    check( rocketnet_server_client_connected( world->server, index ) == 0 );
}

static void test_client_reconnects_with_a_second_token()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_client_t * client = world.clients[0];

    lifecycle_connect( &world, 0, 0x1001 );
    lifecycle_exchange( &world, 0, 0xA1, 3 );

    const uint64_t sent_first = rn_client_stats_of( client ).packets_sent;
    const uint64_t payloads_first = rn_client_stats_of( client ).payloads_received;
    check( sent_first > 0 );
    check( payloads_first > 0 );
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_index( client ) == -1 );
    check( rocketnet_client_transport_name( client ) == NULL );

    // a second token with another client id: sequences and channels start
    // fresh, so the second exchange's values count from 0 again in order, and
    // nothing tagged by the first connection arrives. The new connection's
    // receive totals do not keep the ended connection's.

    lifecycle_connect( &world, 0, 0x2002 );

    const uint64_t sent_second = rn_client_stats_of( client ).packets_sent;
    const uint64_t payloads_second = rn_client_stats_of( client ).payloads_received;
    check( sent_second > 0 );
    check( payloads_second < payloads_first );
    lifecycle_exchange( &world, 0, 0xB2, 3 );
    check( rn_client_stats_of( client ).packets_sent > sent_second );
    check( rn_client_stats_of( client ).payloads_received > payloads_second );
    check( rn_server_stats_of( world.server ).connections_accepted == 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_reconnects_with_a_second_token );

// Disconnect clears the held connection before the old object is destroyed.
// The client can already be disconnected in that window. Session counters
// read 0 then. Not-connected drops stay.

static void test_client_stats_are_zero_once_disconnected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x3101 );
    lifecycle_exchange( &world, 0, 0xC3, 2 );

    struct rocketnet_client_stats_t before;
    rocketnet_client_stats( client, &before );
    check( before.packets_sent > 0 );
    check( before.payloads_received > 0 );

    const uint64_t drops = before.messages_dropped_not_connected;
    rocketnet_client_disconnect( client );
    for ( int step = 0; step < 32 && !rocketnet_client_disconnected( client ); step++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_disconnected( client ) );

    struct rocketnet_client_stats_t after;
    rocketnet_client_stats( client, &after );
    check( after.packets_sent == 0 );
    check( after.packets_received == 0 );
    check( after.bytes_sent == 0 );
    check( after.bytes_received == 0 );
    check( after.payloads_received == 0 );
    check( after.messages_resent == 0 );
    check( after.messages_duplicated == 0 );
    check( after.receipts_acked == 0 );
    check( after.receipts_lost == 0 );
    check( after.receipts_dropped == 0 );
    check( after.messages_dropped_over_budget == 0 );
    check( after.rtt_ms == 0.0 );
    check( after.messages_dropped_not_connected >= drops );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_stats_are_zero_once_disconnected );

// A connected step-driven client retains its sealed disconnect burst until
// the caller explicitly steps its worker. Polling and a duplicate request
// cannot retire that generation; only an explicit update completes it, and a
// reconnect waits for that completion.

static void test_client_disconnect_requires_explicit_completion_before_reuse()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    lifecycle_connect( &world, 0, 0x1011 );

    rocketnet_client_t * client = world.clients[0];
    char pending_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1012, 300, pending_token );

    rocketnet_client_disconnect( client );
    rocketnet_client_disconnect( client );
    for ( int i = 0; i < 8; ++i )
    {
        check( !rocketnet_client_disconnected( client ) );
    }
    check( rocketnet_client_connect( client, pending_token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( !rocketnet_client_disconnected( client ) );

    rocketnet_client_update( client, world.time );
    check( rocketnet_client_disconnected( client ) );
    lifecycle_connect( &world, 0, 0x1013 );
    lifecycle_exchange( &world, 0, 0xA3, 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_disconnect_requires_explicit_completion_before_reuse );

static void test_client_destroy_then_create_and_connect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    lifecycle_connect( &world, 0, 0x1101 );
    lifecycle_exchange( &world, 0, 0xC1, 2 );

    const int index = rocketnet_client_index( world.clients[0] );
    rn_test_client_stop_and_destroy( world.clients[0] );
    world.clients[0] = NULL;
    for ( int i = 0; i < 200 && rocketnet_server_client_connected( world.server, index ); i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_server_client_connected( world.server, index ) == 0 );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    check( world.clients[0] != NULL );
    lifecycle_connect( &world, 0, 0x1102 );
    lifecycle_exchange( &world, 0, 0xC2, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_destroy_then_create_and_connect );

static void test_client_connect_refused_unless_disconnected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_client_t * client = world.clients[0];
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1201, 300, token );

    // while connecting: refused, nothing changed

    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    const int connecting = rocketnet_client_state( client );
    check( connecting > ROCKETNET_CLIENT_STATE_DISCONNECTED && connecting < ROCKETNET_CLIENT_STATE_CONNECTED );

    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_client_state( client ) == connecting );

    // while connected: refused, nothing changed, the connection still carries

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    for ( int i = 0; i < 50; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    lifecycle_exchange( &world, 0, 0xD1, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_connect_refused_unless_disconnected );

static void test_server_start_refused_unless_stopped()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    const uint16_t port = rocketnet_server_port( world.server );
    check( port != 0 );

    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_SERVER_NOT_STOPPED );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_server_running( world.server ) == 1 );
    check( rocketnet_server_port( world.server ) == port );
    lifecycle_connect( &world, 0, 0x1301 );
    lifecycle_exchange( &world, 0, 0xE1, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_start_refused_unless_stopped );

static void test_client_error_state_retries_after_completion()
{
    // denied: a server of one slot, held by client 0; client 1 is refused

    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );
    lifecycle_connect( &world, 0, 0x1401 );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1402, 300, token );

    rocketnet_client_t * late = world.clients[1];
    check( rocketnet_client_connect( late, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( late ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );

    rocketnet_message_client_t * old_message = rocketnet_client_message_client( late );
    check( old_message != NULL );
    check( rocketnet_client_connect( late, "bad" ) == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    check( !rocketnet_client_disconnected( late ) ); // malformed input did not retire it
    check( rocketnet_client_message_client( late ) == old_message );
    check( rocketnet_client_connect( late, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );

    for ( int i = 0; i < 8; ++i )
    {
        check( !rocketnet_client_disconnected( late ) ); // no hidden caller step
        check( rocketnet_client_message_client( late ) == old_message );
    }

    // Free the slot and explicitly advance the pending old worker. The retry
    // preserves its denied state and old message pointer until finalization.

    lifecycle_disconnect( &world, 0 );
    rn_test_client_wait_disconnected( late );
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    check( rocketnet_client_message_client( late ) == old_message ); // completed but not yet finalized
    lifecycle_connect( &world, 1, 0x1403 );
    lifecycle_exchange( &world, 1, 0xF1, 2 );
    lifecycle_world_destroy( &world );

    // timed out: the token names an address no server holds, so nothing
    // answers for the token's timeout

    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_token( world.server, 1, &world.identity, 0x1404, 1, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 100 && rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        lifecycle_pump( &world, 0.1 );
    }

    const int ended = rocketnet_client_state( client );
    check( ended == ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT || ended == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( client ) == ended );
    for ( int i = 0; i < 8; ++i )
    {
        check( !rocketnet_client_disconnected( client ) );
    }
    rn_test_client_wait_disconnected( client );
    check( rocketnet_client_state( client ) == ended );
    lifecycle_connect( &world, 0, 0x1405 );
    lifecycle_exchange( &world, 0, 0xF2, 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_error_state_retries_after_completion );

// A connect from a failure state, with no rocketnet_client_disconnect between,
// carries nothing of the failed attempt: the retry is a fresh handshake into
// a fresh connection whose sequences and channels count from zero, so the
// exchange after it delivers its values from 0 in order and nothing else.
// Once from CONNECTION_DENIED, once from COOKIE_REQUEST_TIMED_OUT, each in its
// own world. tla/ClientConnection.tla FreshAttemptFromFailure.

static void test_client_connect_from_failure_carries_nothing()
{
    // denied: the server's one slot is held by client 0

    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );
    lifecycle_connect( &world, 0, 0x1501 );

    rocketnet_client_t * denied = world.clients[1];
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1502, 300, token );
    check( rocketnet_client_connect( denied, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( denied ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( denied ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    check( rocketnet_client_index( denied ) == -1 );
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_state( denied ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );

    // a new token, connect straight from the denied state

    lifecycle_connect( &world, 1, 0x1503 );
    lifecycle_exchange( &world, 1, 0xC3, 3 );
    check( rn_server_stats_of( world.server ).connections_accepted == 2 );
    lifecycle_world_destroy( &world );

    // timed out: the first token names an address no server holds

    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_token( world.server, 1, &world.identity, 0x1504, 1, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 100 && rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        lifecycle_pump( &world, 0.1 );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT );
    check( rocketnet_client_index( client ) == -1 );

    // a new token for the real server, connect straight from the timed out state

    lifecycle_connect( &world, 0, 0x1505 );
    lifecycle_exchange( &world, 0, 0xC5, 3 );
    check( rn_server_stats_of( world.server ).connections_accepted == 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_connect_from_failure_carries_nothing );

// Explicit disconnect remains a distinct operation: it clears a negative
// connection-backed state, even when the client did not ask to retry.

static void test_client_disconnect_clears_denied_state()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );
    lifecycle_connect( &world, 0, 0x1411 );

    rocketnet_client_t * denied = world.clients[1];
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1412, 300, token );
    check( rocketnet_client_connect( denied, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( denied ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( denied ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    rocketnet_client_disconnect( denied );
    check( rocketnet_client_state( denied ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rn_test_client_wait_disconnected( denied );
    check( rocketnet_client_state( denied ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_disconnect_clears_denied_state );

static int lifecycle_finish_assertions = 0;

static void lifecycle_finish_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    lifecycle_finish_assertions++;
}

static uint32_t lifecycle_client_finish_without_start( rocketnet_client_t * client, int bytes_written )
{
    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    lifecycle_finish_assertions = 0;
    rocketnet_assert_function( lifecycle_finish_assert_hook );

    const uint32_t serial = rocketnet_client_finish_message( client, bytes_written );
    rocketnet_assert_function( saved_assert );
    check( lifecycle_finish_assertions == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    return serial;
}

static uint32_t lifecycle_server_finish_without_start( rocketnet_server_t * server, int bytes_written )
{
    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    lifecycle_finish_assertions = 0;
    rocketnet_assert_function( lifecycle_finish_assert_hook );

    const uint32_t serial = rocketnet_server_finish_message( server, bytes_written );
    rocketnet_assert_function( saved_assert );
    check( lifecycle_finish_assertions == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    return serial;
}

// A send on a connection that is not CONNECTED is dropped and counted, on the
// client and on the server alike (standard/MESSAGES.md m1.5;
// tla/ClientSession.tla DropSend, tla/ServerSession.tla ServerDropSend):
// start message returns NULL, finish message asserts in Debug and returns 0 in
// Release, the NOT_CONNECTED_DROPS counter moves by exactly 1 per start, the
// misuse count does not move, and nothing of it is ever sent.

static void lifecycle_check_client_send_dropped( rocketnet_client_t * client )
{
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t drops = rn_client_stats_of( client ).messages_dropped_not_connected;
    uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out == NULL );

    if ( out )
    {
        out[0] = 0xEE;
        out[1] = 0xEE;
    }

    check( lifecycle_client_finish_without_start( client, 2 ) == 0 );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == drops + 1 );
    check( rocketnet_api_misuse_count() == misuse );
}

static void lifecycle_check_server_send_dropped( rocketnet_server_t * server, int index )
{
    const uint64_t misuse = rocketnet_api_misuse_count();
    const uint64_t drops = rn_server_stats_of( server ).messages_dropped_not_connected;
    uint8_t * out = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
    check( out == NULL );

    if ( out )
    {
        out[0] = 0xEE;
        out[1] = 0xEE;
    }

    check( lifecycle_server_finish_without_start( server, 2 ) == 0 );
    check( rn_server_stats_of( server ).messages_dropped_not_connected == drops + 1 );
    check( rocketnet_api_misuse_count() == misuse );
}

// once CONNECTED and pumped further, nothing arrives at the server from the
// client on channel 0, nor at the client from the server; then the first send
// after CONNECTED is delivered

static void lifecycle_check_nothing_then_first_send( lifecycle_world_t * world, int client, uint8_t tag )
{
    rocketnet_client_t * c = world->clients[client];
    check( rocketnet_client_state( c ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    for ( int step = 0; step < 50; step++ )
    {
        lifecycle_pump( world );

        static const struct rocketnet_message_batch_t * nothing = NULL;
        check( ( ( nothing = rocketnet_server_receive_messages( world->server ) ) == NULL ) );
        check( ( ( nothing = rocketnet_client_receive_messages( c ) ) == NULL ) );
    }

    lifecycle_exchange( world, client, tag, 1 );
}

static void test_client_send_dropped_before_connect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];

    // right after create: DISCONNECTED, no connection object

    check( rn_client_stats_of( client ).messages_dropped_not_connected == 0 );
    lifecycle_check_client_send_dropped( client );
    lifecycle_check_client_send_dropped( client );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 2 );

    // an invalid token: the client is in an error state with no connection

    char bad[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    memset( bad, 'A', sizeof( bad ) - 1 );
    bad[sizeof( bad ) - 1] = '\0';
    check( rocketnet_client_connect( client, bad ) == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );
    lifecycle_check_client_send_dropped( client );
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );

    lifecycle_connect( &world, 0, 0x1501 );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 3 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA5 );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 3 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_send_dropped_before_connect );

static void test_client_send_dropped_while_connecting()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );

    rocketnet_client_t * client = world.clients[0];

    // a handshake state: the connection object exists, its transport is not
    // CONNECTED, and an unreliable send is dropped the same as a reliable one

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1502, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( client ) < ROCKETNET_CLIENT_STATE_CONNECTED );
    lifecycle_check_client_send_dropped( client );

    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 2 ) == NULL );
    check( lifecycle_client_finish_without_start( client, 2 ) == 0 );
    check( rocketnet_api_misuse_count() == misuse );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 2 );

    // every handshake state the pump passes through drops the same way

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( &world );
        if ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            lifecycle_check_client_send_dropped( client );
        }
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const uint64_t drops = rn_client_stats_of( client ).messages_dropped_not_connected;
    check( drops >= 2 );

    const int index = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world.server, index ); i++ )
    {
        lifecycle_pump( &world );
    }
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA6 );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == drops );

    // an error state with its connection object: the second client is denied
    // by the full server

    rocketnet_client_t * late = world.clients[1];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1503, 300, token );
    check( rocketnet_client_connect( late, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( late ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( late ) == ROCKETNET_CLIENT_STATE_CONNECTION_DENIED );
    lifecycle_check_client_send_dropped( late );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_send_dropped_while_connecting );

static void test_client_send_dropped_after_disconnect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1504 );
    lifecycle_exchange( &world, 0, 0xA7, 2 );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 0 );

    // the client disconnects itself: DISCONNECTED, no connection object

    lifecycle_disconnect( &world, 0 );
    lifecycle_check_client_send_dropped( client );

    // reconnect: the counter is a lifetime counter, kept across connections,
    // and nothing dropped is sent on the new one

    lifecycle_connect( &world, 0, 0x1505 );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 1 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA8 );

    // the server ends the connection: SERVER_DISCONNECTED while its object exists

    rocketnet_server_disconnect_client( world.server, rocketnet_client_index( client ) );
    for ( int i = 0; i < 200 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    lifecycle_check_client_send_dropped( client );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 2 );

    lifecycle_connect( &world, 0, 0x1506 );
    check( rn_client_stats_of( client ).messages_dropped_not_connected == 2 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xA9 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_send_dropped_after_disconnect );

static void test_server_send_dropped_to_slot_not_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 1 );

    rocketnet_server_t * server = world.server;
    lifecycle_connect( &world, 0, 0x1507 );

    const int index = rocketnet_client_index( world.clients[0] );
    const int free_slot = ( index + 1 ) % 3;
    check( rocketnet_server_client_connected( server, free_slot ) == 0 );
    check( rn_server_stats_of( server ).messages_dropped_not_connected == 0 );

    // a free slot below max_clients

    lifecycle_check_server_send_dropped( server, free_slot );

    // an index at or past max_clients stays a counted misuse, and is no drop

    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( server, 3, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( lifecycle_server_finish_without_start( server, 2 ) == 0 );
    check( rocketnet_server_start_message( server, 1000, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 2 );     // the finish with no start pending is not misuse
    check( rn_server_stats_of( server ).messages_dropped_not_connected == 1 );

    // the connected slot still sends; then its client leaves and the slot drops

    lifecycle_exchange( &world, 0, 0xB1, 1 );
    lifecycle_disconnect( &world, 0 );
    lifecycle_check_server_send_dropped( server, index );
    check( rn_server_stats_of( server ).messages_dropped_not_connected == 2 );

    // a stopped server: the same drop, the counter kept across the stop

    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );
    check( rocketnet_server_running( server ) == 0 );
    check( rn_server_stats_of( server ).messages_dropped_not_connected == 2 );
    lifecycle_check_server_send_dropped( server, 0 );
    lifecycle_check_server_send_dropped( server, 2 );
    misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( server, 3, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 ) == NULL );
    check( lifecycle_server_finish_without_start( server, 2 ) == 0 );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rn_server_stats_of( server ).messages_dropped_not_connected == 4 );

    // start again: nothing dropped is sent to the client that connects next

    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1508 );
    check( rn_server_stats_of( server ).messages_dropped_not_connected == 4 );
    lifecycle_check_nothing_then_first_send( &world, 0, 0xB2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_send_dropped_to_slot_not_connected );

// set_send_kbps follows the same rule: a connection that is not CONNECTED has
// no rate to set. The call returns INVALID_ARGUMENT, changes nothing and is
// not misuse; once CONNECTED the same call is OK.

static void test_server_set_send_kbps_is_a_no_op_unless_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_server_t * server = world.server;
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_set_send_kbps( server, 0, 2000 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( server, 1, 0 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_api_misuse_count() == misuse );

    lifecycle_connect( &world, 0, 0x1509 );

    const int index = rocketnet_client_index( world.clients[0] );
    const int other = 1 - index;
    check( rocketnet_server_set_send_kbps( server, other, 2000 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( server, index, 2000 ) == ROCKETNET_STATUS_OK );

    // the client leaves: a no-op again

    lifecycle_disconnect( &world, 0 );
    check( rocketnet_server_set_send_kbps( server, index, 3000 ) == ROCKETNET_STATUS_OK );

    // stopped: a no-op, not misuse

    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );
    check( rocketnet_server_set_send_kbps( server, index, 3000 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_api_misuse_count() == misuse );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x150A );
    check( rocketnet_server_set_send_kbps( server, rocketnet_client_index( world.clients[0] ), 3000 ) == ROCKETNET_STATUS_OK );
    lifecycle_exchange( &world, 0, 0xB3, 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_set_send_kbps_is_a_no_op_unless_connected );

static void test_client_set_send_kbps_is_a_no_op_unless_connected()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];
    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_OK );

    // every handshake state: a plain no-op

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x150B, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    int handshake_checks = 0;

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_OK );
        check( rocketnet_client_set_send_kbps( client, 0 ) == ROCKETNET_STATUS_OK );
        handshake_checks++;
        lifecycle_pump( &world );
    }

    check( handshake_checks > 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_api_misuse_count() == misuse );

    // CONNECTED: OK

    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_OK );

    const int index = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world.server, index ); i++ )
    {
        lifecycle_pump( &world );
    }
    lifecycle_exchange( &world, 0, 0xB4, 1 );

    // after disconnect: a no-op again

    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_api_misuse_count() == misuse );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_set_send_kbps_is_a_no_op_unless_connected );

static void test_server_start_stop_start_again()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 2 );
    lifecycle_connect( &world, 0, 0x1601 );
    lifecycle_connect( &world, 1, 0x1602 );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );

    const int a = rocketnet_client_index( world.clients[0] );
    const int b = rocketnet_client_index( world.clients[1] );
    check( a != b && a < 2 && b < 2 );
    check( rocketnet_server_client_id( world.server, a ) == 0x1601 );
    check( rocketnet_server_client_id( world.server, b ) == 0x1602 );

    // stop: both clients see the disconnect, both slots are free

    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    check( rocketnet_server_running( world.server ) == 0 );
    check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    check( rocketnet_server_client_connected( world.server, a ) == 0 );
    check( rocketnet_server_client_connected( world.server, b ) == 0 );
    check( rocketnet_server_port( world.server ) == 0 );
    check( rocketnet_server_transport_name( world.server ) == NULL );
    check( rocketnet_server_max_clients( world.server ) == 2 );
    rocketnet_server_stop( world.server );      // a stopped server's stop is a no-op
    rn_test_server_wait_stopped( world.server );

    for ( int i = 0; i < 100; i++ )
    {
        lifecycle_pump( &world );
        if ( rocketnet_client_state( world.clients[0] ) != ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_client_state( world.clients[1] ) != ROCKETNET_CLIENT_STATE_CONNECTED )
            break;
    }

    check( rocketnet_client_state( world.clients[0] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( world.clients[1] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );

    // a stopped server's update and send are plain no-ops, not misuse

    const uint64_t misuse = rocketnet_api_misuse_count();
    rocketnet_server_update( world.server, world.time );
    rocketnet_server_send_packets( world.server );
    check( rocketnet_api_misuse_count() == misuse );

    // start again, connect again

    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_running( world.server ) == 1 );
    rocketnet_client_disconnect( world.clients[0] );
    rn_test_client_wait_disconnected( world.clients[0] );
    rocketnet_client_disconnect( world.clients[1] );
    rn_test_client_wait_disconnected( world.clients[1] );
    lifecycle_connect( &world, 0, 0x1603 );
    lifecycle_connect( &world, 1, 0x1604 );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );
    lifecycle_exchange( &world, 0, 0x91, 2 );
    lifecycle_exchange( &world, 1, 0x92, 2 );
    check( rn_server_stats_of( world.server ).connections_accepted == 4 );
    lifecycle_world_destroy( &world );      // destroys a running server
}
REGISTER_UNIT_TEST( test_server_start_stop_start_again );

// tla/ServerSession.tla NoAdmitUnlessRunning. A stopped server admits no
// client. rocketnet_server_update returns while !server->running
// (sdk/rocketnet_api.cpp:1797), and the transport reads only while RUNNING
// (sdk/rocketnet_server_transport.cpp:2388).

static void test_server_stopped_admits_no_client()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1610, 1, token );

    const uint64_t accepted = rn_server_stats_of( world.server ).connections_accepted;
    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    check( rocketnet_server_running( world.server ) == 0 );
    check( rocketnet_client_connect( world.clients[0], token ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 100 && rocketnet_client_state( world.clients[0] ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        check( rocketnet_server_num_connected_clients( world.server ) == 0 );
        lifecycle_pump( &world, 0.1 );
    }

    check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    check( rn_server_stats_of( world.server ).connections_accepted == accepted );
    check( rocketnet_client_state( world.clients[0] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_stopped_admits_no_client );

static void test_server_stop_disconnects_every_client()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 3 );
    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_connect( &world, i, 0x1700 + (uint64_t) i );
    }
    check( rocketnet_server_num_connected_clients( world.server ) == 3 );
    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    check( rocketnet_server_num_connected_clients( world.server ) == 0 );

    // the redundant disconnect packets reach every client before any timeout

    for ( int step = 0; step < 20; step++ )
    {
        lifecycle_pump( &world );
    }
    for ( int i = 0; i < 3; i++ )
    {
        check( rocketnet_client_state( world.clients[i] ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );
    }
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_stop_disconnects_every_client );

static void test_server_disconnect_all_clients()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 3 );
    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_connect( &world, i, 0x1800 + (uint64_t) i );
    }
    rocketnet_server_disconnect_all_clients( world.server );
    check( rocketnet_server_running( world.server ) == 1 );
    check( rocketnet_server_num_connected_clients( world.server ) == 0 );
    for ( int step = 0; step < 20; step++ )
    {
        lifecycle_pump( &world );
    }
    for ( int i = 0; i < 3; i++ )
    {
        check( rocketnet_client_state( world.clients[i] ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    }

    // still running: the clients come back

    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_client_disconnect( world.clients[i] );
        rn_test_client_wait_disconnected( world.clients[i] );
        lifecycle_connect( &world, i, 0x1810 + (uint64_t) i );
    }

    check( rocketnet_server_num_connected_clients( world.server ) == 3 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_disconnect_all_clients );

static void test_connection_reset_client_reconnect_delivers()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1901 );
    lifecycle_exchange( &world, 0, 0x51, 2 );

    // mid-transfer: queue messages both ways, let one update run, then end it

    const int index = rocketnet_client_index( client );
    check( index >= 0 );

    for ( int i = 0; i < 8; i++ )
    {
        lifecycle_client_send( client, 0x5F, (uint8_t) i );
        lifecycle_server_send( world.server, (uint64_t) index, 0x5F, (uint8_t) i );
    }

    lifecycle_pump( &world );

    uint64_t sender = 0;
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( world.server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            sender = (uint64_t) batch->client_index[i];
            check( sender == (uint64_t) index );
        }
    }

    lifecycle_disconnect( &world, 0 );

    // the new connection's first messages deliver, and nothing of the old

    lifecycle_connect( &world, 0, 0x1902 );
    lifecycle_exchange( &world, 0, 0x52, 4 );

    for ( int step = 0; step < 50; step++ )
    {
        lifecycle_pump( &world );

        static const struct rocketnet_message_batch_t * nothing = NULL;
        check( ( ( nothing = rocketnet_server_receive_messages( world.server ) ) == NULL ) );
        check( ( ( nothing = rocketnet_client_receive_messages( client ) ) == NULL ) );
    }

    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_connection_reset_client_reconnect_delivers );

// ROCKETNET_FLOW_ID_BIT: a destination id with the bit set is refused and
// counted by each destination-taking function, and client indexes
// 0 .. max_clients - 1 round-trip through every index-taking function as
// uint64_t.
static int lifecycle_flow_logs = 0;
static void lifecycle_log_callback( void *, enum rocketnet_log_level_t, const char * message, size_t )
{
    if ( strstr( message, "FLOW_ID_RESERVED" ) )
    {
        lifecycle_flow_logs++;
    }
}

static void test_server_destination_flow_id_refused()
{
    check( ROCKETNET_FLOW_ID_BIT == ( 1ULL << 63 ) );

    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );
    lifecycle_connect( &world, 0, 0x1A01 );

    const int flow_index = rocketnet_client_index( world.clients[0] );
    check( flow_index >= 0 );

    const uint64_t flow = (uint64_t) ROCKETNET_FLOW_ID_BIT | (uint64_t) flow_index;

    // the flow refusal names FLOW_ID_RESERVED; an index merely out of range
    // (no flow bit) is refused without it

    rocketnet_quiet( 0 );
    rocketnet_log_function( lifecycle_log_callback, NULL );
    lifecycle_flow_logs = 0;

    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_start_message( world.server, flow, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 && lifecycle_flow_logs == 1 );

    // finish and abort act on the pending start: the refused one left none,
    // so finish asserts in Debug and returns 0 in Release, and abort is a
    // no-op, both uncounted and with no flow log

    misuse = rocketnet_api_misuse_count();
    check( lifecycle_server_finish_without_start( world.server, 4 ) == 0 );
    rocketnet_server_abort_message( world.server );
    check( rocketnet_api_misuse_count() == misuse && lifecycle_flow_logs == 1 );

    // an index merely out of range is counted misuse without the flow log

    check( rocketnet_server_start_message( world.server, 5, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 && lifecycle_flow_logs == 1 );
    rocketnet_log_function( NULL, NULL );
    rocketnet_quiet( 1 );
    check( strcmp( rocketnet_status_name( ROCKETNET_STATUS_FLOW_ID_RESERVED ), "FLOW_ID_RESERVED" ) == 0 );

    // the slot's own index still works

    lifecycle_exchange( &world, 0, 0x61, 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_destination_flow_id_refused );

// The client index is an int on every per-client call; only
// rocketnet_server_start_message takes a uint64_t destination id, which is
// the client index today.

static void test_server_client_index_round_trips_as_int()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 3, 3 );
    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_connect( &world, i, 0x1B00 + (uint64_t) i );
    }

    for ( int i = 0; i < 3; i++ )
    {
        const int index = rocketnet_client_index( world.clients[i] );
        check( index >= 0 && index < 3 );
        check( rocketnet_server_client_connected( world.server, index ) == 1 );
        check( rocketnet_server_client_id( world.server, index ) == 0x1B00 + (uint64_t) i );
        check( rocketnet_server_disconnect_reason( world.server, index ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( rocketnet_server_set_send_kbps( world.server, index, 2000 ) == ROCKETNET_STATUS_OK );

        uint8_t * out = rocketnet_server_start_message( world.server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 2 );
        check( out != NULL );
        rocketnet_server_abort_message( world.server );

        struct rocketnet_receipt_t receipts[4];
        check( rocketnet_server_receipts( world.server, index, receipts, 4 ) >= 0 );
        (void) rn_server_client_stats_of( world.server, index ).packets_sent;
        (void) rn_server_client_stats_of( world.server, index ).rtt_ms;
        lifecycle_exchange( &world, i, (uint8_t) ( 0x70 + i ), 1 );

        // an index past max_clients, and a negative one, name no client

        check( rocketnet_server_client_connected( world.server, index + 3 ) == 0 );
        check( rocketnet_server_client_connected( world.server, -1 ) == 0 );
        check( rocketnet_server_client_id( world.server, -1 ) == 0 );
        check( rocketnet_server_client_id( world.server, 0x7fffffff ) == 0 );
    }

    rocketnet_server_disconnect_client( world.server, rocketnet_client_index( world.clients[0] ) );
    check( rocketnet_server_num_connected_clients( world.server ) == 2 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_client_index_round_trips_as_int );

// rocketnet_server_client_user_data returns the connect token's user data for
// the slot, from connect until the slot is released. Release wipes it: the
// freed slot reads zeros, and the next client in that slot reads its own
// token's bytes, never the last client's.

static void lifecycle_user_data_fill( uint8_t * user_data, uint8_t seed )
{
    for ( int i = 0; i < ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES; i++ )
        user_data[i] = (uint8_t) ( seed + i * 7 );
}

static bool lifecycle_user_data_zero( const uint8_t * user_data )
{
    for ( int i = 0; i < ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES; i++ )
        if ( user_data[i] != 0 )
            return false;
    return true;
}

static void test_server_client_user_data_from_connect_until_release()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 2 );

    uint8_t first[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    uint8_t second[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    uint8_t out[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    lifecycle_user_data_fill( first, 0x11 );
    lifecycle_user_data_fill( second, 0x5C );

    const uint64_t misuse = rocketnet_api_misuse_count();

    // before any client: the slot is free and reads zeros
    memset( out, 0xA5, sizeof( out ) );
    check( !rocketnet_server_client_user_data( world.server, 0, out ) );
    check( lifecycle_user_data_zero( out ) );

    // connected: exactly the token's bytes
    lifecycle_connect( &world, 0, 0x1C01, first );
    const int slot = rocketnet_client_index( world.clients[0] );
    check( slot == 0 );
    memset( out, 0, sizeof( out ) );
    check( rocketnet_server_client_user_data( world.server, slot, out ) );
    check( memcmp( out, first, sizeof( out ) ) == 0 );
    for ( int i = 0; i < 4; i++ )
        lifecycle_pump( &world );
    check( rocketnet_server_client_user_data( world.server, slot, out ) );
    check( memcmp( out, first, sizeof( out ) ) == 0 );
    check( rocketnet_api_misuse_count() == misuse );

    // released: zeros, false, and not misuse
    rocketnet_server_disconnect_client( world.server, slot );
    check( !rocketnet_server_client_connected( world.server, slot ) );
    memset( out, 0xA5, sizeof( out ) );
    check( !rocketnet_server_client_user_data( world.server, slot, out ) );
    check( lifecycle_user_data_zero( out ) );
    check( rocketnet_api_misuse_count() == misuse );

    // the next client in the same slot reads its own token's bytes
    lifecycle_connect( &world, 1, 0x1C02, second );
    check( rocketnet_client_index( world.clients[1] ) == slot );
    check( rocketnet_server_client_user_data( world.server, slot, out ) );
    check( memcmp( out, second, sizeof( out ) ) == 0 );
    check( rocketnet_api_misuse_count() == misuse );

    // an index outside 0 .. max_clients - 1 is misuse and reads zeros
    memset( out, 0xA5, sizeof( out ) );
    check( !rocketnet_server_client_user_data( world.server, 1, out ) );
    check( lifecycle_user_data_zero( out ) );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( !rocketnet_server_client_user_data( world.server, -1, out ) );
    check( rocketnet_api_misuse_count() == misuse + 2 );

    // stopped: every slot is released and reads zeros
    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    memset( out, 0xA5, sizeof( out ) );
    check( !rocketnet_server_client_user_data( world.server, slot, out ) );
    check( lifecycle_user_data_zero( out ) );
    check( rocketnet_api_misuse_count() == misuse + 2 );

    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_client_user_data_from_connect_until_release );

// packets_per_update takes 1 .. 1024 and defaults to 256, client and
// server, through the struct and through the environment.
#if ROCKETNET_DEV
static int lifecycle_setenv( const char * name, const char * value ) { return rocketnet_test_env_setenv( name, value, 1 ); }
#endif // #if ROCKETNET_DEV
#if ROCKETNET_DEV
static int lifecycle_unsetenv( const char * name ) { return rocketnet_test_env_unsetenv( name ); }
#endif // #if ROCKETNET_DEV

static void test_config_packets_per_update_range()
{
    rocketnet_quiet( 1 );

    lifecycle_identity_t identity;
    lifecycle_identity( &identity );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    check( client_config.packets_per_update == 256 );

    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &identity, 1 );
    check( server_config.packets_per_update == 256 );

    // the struct

    enum rocketnet_status_t create_status = ROCKETNET_TEST_STATUS_UNSET;
    client_config.packets_per_update = 1024;

    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );
    client_config.packets_per_update = 1025;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );
    client_config.packets_per_update = 0;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );

    server_config.packets_per_update = 1024;
    create_status = ROCKETNET_TEST_STATUS_UNSET;

    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rn_test_server_stop_and_destroy( server );
    server_config.packets_per_update = 1025;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );
    server_config.packets_per_update = 0;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server == NULL );
    check( create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );

#if ROCKETNET_DEV
    // the environment

    rocketnet_client_default_config( &client_config );
    lifecycle_server_config( &server_config, &identity, 1 );

    // Old public spellings are not aliases. Values outside the valid range
    // make accidental reads visible: both creates must ignore these names.

    check( lifecycle_setenv( "ROCKETNET_CLIENT_MAX_PACKETS_"
                             "PER_UPDATE",
                             "1025" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RTO_"
                             "INITIAL_MS",
                             "60001" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RTO_"
                             "MIN_MS",
                             "-1" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_RTO_"
                             "MAX_MS",
                             "60001" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_MAX_PACKETS_"
                             "PER_UPDATE",
                             "1025" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RTO_"
                             "INITIAL_MS",
                             "60001" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RTO_"
                             "MIN_MS",
                             "-1" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_RTO_"
                             "MAX_MS",
                             "60001" ) == 0 );
    client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    if ( client )
    {
        rn_test_client_stop_and_destroy( client );
    }
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    if ( server )
    {
        rn_test_server_stop_and_destroy( server );
    }
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MAX_PACKETS_"
                               "PER_UPDATE" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RTO_"
                               "INITIAL_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RTO_"
                               "MIN_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_RTO_"
                               "MAX_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MAX_PACKETS_"
                               "PER_UPDATE" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RTO_"
                               "INITIAL_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RTO_"
                               "MIN_MS" ) == 0 );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_RTO_"
                               "MAX_MS" ) == 0 );

    // the new resend-delay names are read for both endpoint configs

    check( lifecycle_setenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS", "9" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_INITIAL_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS", "2001" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MIN_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS", "9" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_MESSAGE_RESEND_DELAY_MAX_MS" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS", "9" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_INITIAL_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS", "2001" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MIN_MS" ) == 0 );
    check( lifecycle_setenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS", "9" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_MESSAGE_RESEND_DELAY_MAX_MS" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE", "1024" ) == 0 );
    client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    rn_test_client_stop_and_destroy( client );
    check( lifecycle_setenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE", "1025" ) == 0 );
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_CLIENT_PACKETS_PER_UPDATE" ) == 0 );

    check( lifecycle_setenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE", "1024" ) == 0 );
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    rn_test_server_stop_and_destroy( server );
    check( lifecycle_setenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE", "1025" ) == 0 );
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );
    check( lifecycle_unsetenv( "ROCKETNET_SERVER_PACKETS_PER_UPDATE" ) == 0 );
#endif // #if ROCKETNET_DEV
}
REGISTER_UNIT_TEST( test_config_packets_per_update_range );

// A connection the server ends leaves the client SERVER_DISCONNECTED with its
// connection object still held. Connect straight after (no disconnect call)
// ends that connection first. The new connection's packet, byte and payload
// counts do not include the ended connection. Not-connected drops stay on
// the handle.

static void test_client_reconnects_after_server_stop_without_disconnect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1C01 );
    lifecycle_exchange( &world, 0, 0x81, 3 );
    rocketnet_server_stop( world.server );
    rn_test_server_wait_stopped( world.server );
    for ( int i = 0; i < 50 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

    struct rocketnet_client_stats_t before;
    rocketnet_client_stats( client, &before );
    check( before.packets_sent > 0 );
    check( before.payloads_received > 0 );
    check( rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1C02 );      // connect, no disconnect first

    struct rocketnet_client_stats_t after;
    rocketnet_client_stats( client, &after );

    // The new handshake has its own packets. The ended session also carried
    // messages, so its receive totals stay strictly above this handshake.

    check( after.packets_sent > 0 );
    check( before.packets_received > after.packets_received );
    check( before.bytes_received > after.bytes_received );
    check( before.payloads_received > after.payloads_received );
    check( after.messages_dropped_not_connected >= before.messages_dropped_not_connected );
    lifecycle_exchange( &world, 0, 0x82, 3 );

    struct rocketnet_client_stats_t exchanged;
    rocketnet_client_stats( client, &exchanged );
    check( exchanged.packets_sent > after.packets_sent );
    check( exchanged.payloads_received > after.payloads_received );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_reconnects_after_server_stop_without_disconnect );

// A config mismatch (num_channels is a WIRE field) ends the first attempt.
// Direct retry retains its public reason while the old generation retires,
// then starts a fresh handshake without an explicit disconnect.

static void test_client_connection_ended_state_kept_until_retry()
{
    lifecycle_world_t world;
    memset( &world, 0, sizeof( world ) );
    rocketnet_quiet( 1 );
    world.time = 1000.0;
    lifecycle_now = world.time;
    lifecycle_identity( &world.identity );

    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &world.identity, 1 );     // 32 channels
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world.server && rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.num_channels = 4;
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    world.num_clients = 1;

    rocketnet_client_t * client = world.clients[0];
    check( client != NULL );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1D01, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 2000 && rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }

    const enum rocketnet_client_state_t ended = rocketnet_client_state( client );
    check( ended == ROCKETNET_CLIENT_STATE_CONFIG_MISMATCH || ended == ROCKETNET_CLIENT_STATE_RELIABLE_TIMED_OUT );

    // kept through further updates

    for ( int i = 0; i < 10; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( client ) == ended );

    // a matching server: the next connect runs a fresh handshake

    rn_test_server_stop_and_destroy( world.server );
    server_config.num_channels = 4;
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world.server && rocketnet_server_start( world.server ) == ROCKETNET_STATUS_OK );
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1D02, 300, token );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( client ) == ended );
    check( !rocketnet_client_disconnected( client ) );
    rn_test_client_wait_disconnected( client );
    check( rocketnet_client_state( client ) == ended );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_connection_ended_state_kept_until_retry );

// A token-only expiry owns no transport worker, so a fresh token can retry
// immediately. Reusing the expired token still reports its expiry.

static void test_client_connect_token_expired()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world.server ) );

    const char * addresses[] = { address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( world.identity.backend_secret, world.server,
                                        LIFECYCLE_PROTOCOL, addresses, 1, 0x1E01, 0, 1, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_disconnected( client ) );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );
    rocketnet_client_disconnect( client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    lifecycle_connect( &world, 0, 0x1E02 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_connect_token_expired );

// A correctly sized, decodable but invalid token is a terminal failure with
// no transport owner. A subsequent valid token starts immediately.

static void test_client_invalid_token_retries_without_disconnect()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 1 );

    rocketnet_client_t * client = world.clients[0];
    char invalid[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    memset( invalid, 'A', sizeof( invalid ) - 1 );
    invalid[sizeof( invalid ) - 1] = '\0';
    check( rocketnet_client_connect( client, invalid ) == ROCKETNET_STATUS_INVALID_CONNECT_TOKEN );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN );
    check( rocketnet_client_disconnected( client ) );
    lifecycle_connect( &world, 0, 0x1E03 );
    lifecycle_exchange( &world, 0, 0xE3, 1 );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_invalid_token_retries_without_disconnect );

// One update sends more packets than one builder call tracks
// (ROCKETNET_PACKET_BUILDER_MAX_PACKETS, 16): the per-update limit is the
// config's packets_per_update.

static void test_client_one_update_sends_more_than_sixteen_packets()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 1, 0 );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;   // no budget: the packet limit alone bounds the update
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    world.num_clients = 1;
    check( world.clients[0] != NULL );
    lifecycle_connect( &world, 0, 0x1F01 );

    rocketnet_client_t * client = world.clients[0];
    for ( int i = 0; i < 20; i++ )
    {
        lifecycle_pump( &world );
    }

    for ( int i = 0; i < 40; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( client, i % 4, ROCKETNET_RELIABILITY_RELIABLE, 1200 );
        check( out != NULL );

        if ( out )
        {
            memset( out, i, 1200 );
            check( rocketnet_client_finish_message( client, 1200 ) != 0 );
        }
    }

    const uint64_t before = rn_client_stats_of( client ).packets_sent;
    lifecycle_now = world.time;
    rocketnet_client_update( client, world.time );
    rocketnet_client_send_packets( client );

    const uint64_t sent = rn_client_stats_of( client ).packets_sent - before;
    check( sent > ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_one_update_sends_more_than_sixteen_packets );

// ---------------------------------------------------------------------------
// The one receive queue, pumped by each receive: every reliability type sent on
// every channel by every client is received through the server's one queue,
// each naming its sender, and through each client's one queue. The send's
// channel orders ORDERED and sequences SEQUENCED messages within it and is
// ignored by the other types (sdk/rocketnet_message_server.cpp,
// rn_message_object_start, and rn_message_object_next).
// ---------------------------------------------------------------------------

#define RQ_CLIENTS 2
#define RQ_ROUNDS 3
#define RQ_TYPES 8
#define RQ_LOG ( RQ_CLIENTS * RQ_ROUNDS * RQ_TYPES * ROCKETNET_MAX_CHANNELS )

struct rq_entry_t
{
    uint64_t sender;            // the server's client index, or the receiving client
    uint8_t payload[4];         // origin, type, channel, round
};

struct rq_log_t
{
    int count;
    rq_entry_t entry[RQ_LOG];
};

struct rq_run_t
{
    rq_log_t at_server;
    rq_log_t at_client;
    int index[RQ_CLIENTS];
    int num_channels;
};

static void rq_record( rq_log_t * log, uint64_t sender, const uint8_t * data, int bytes )
{
    check( bytes == 4 );
    check( log->count < RQ_LOG );
    if ( bytes != 4 || log->count >= RQ_LOG )
        return;
    log->entry[log->count].sender = sender;
    memcpy( log->entry[log->count].payload, data, 4 );
    log->count++;
}

// one update's receive: the server's queue and each client's, a batch per call

static void rq_drain( lifecycle_world_t * world, rq_run_t * run )
{
    const struct rocketnet_message_batch_t * received = NULL;
    while ( ( received = rocketnet_server_receive_messages( world->server ) ) != NULL )
    {
        check( received->num_messages <= ROCKETNET_RECEIVE_BATCH_MESSAGES );

        for ( int i = 0; i < received->num_messages; i++ )
        {
            // the channel a message was delivered on: its ordering channel for
            // an ordered or sequenced type, 0 for the types that carry none
            const uint8_t type = received->data[i][1];
            const bool carries = type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
                                 type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
            check( received->bytes[i] == 4 && received->channel[i] == ( carries ? received->data[i][2] : 0 ) );
            check( received->client_index[i] >= 0 );
            rq_record( &run->at_server, (uint64_t) received->client_index[i], received->data[i], received->bytes[i] );
        }
    }

    for ( int k = 0; k < world->num_clients; k++ )
    {
        while ( ( received = rocketnet_client_receive_messages( world->clients[k] ) ) != NULL )
        {
            for ( int i = 0; i < received->num_messages; i++ )
            {
                check( received->client_index[i] == 0 );
                rq_record( &run->at_client, (uint64_t) k, received->data[i], received->bytes[i] );
            }
        }
    }
}

// every type on every channel, both ways: a client's payload names the client
// (origin k), the server's names 0x80 + the client it is sent to

static void rq_send_round( lifecycle_world_t * world, rq_run_t * run, int round )
{
    for ( int k = 0; k < world->num_clients; k++ )
    {
        for ( int channel = 0; channel < run->num_channels; channel++ )
        {
            for ( int type = 0; type < RQ_TYPES; type++ )
            {
                uint8_t * out = rocketnet_client_start_message( world->clients[k], channel, (enum rocketnet_reliability_t) type, 4 );
                check( out != NULL );

                if ( out )
                {
                    out[0] = (uint8_t) k;
                    out[1] = (uint8_t) type;
                    out[2] = (uint8_t) channel;
                    out[3] = (uint8_t) round;
                    check( rocketnet_client_finish_message( world->clients[k], 4 ) != 0 );
                }

                out = rocketnet_server_start_message( world->server, (uint64_t) run->index[k], channel, (enum rocketnet_reliability_t) type, 4 );
                check( out != NULL );

                if ( out )
                {
                    out[0] = (uint8_t) ( 0x80 + k );
                    out[1] = (uint8_t) type;
                    out[2] = (uint8_t) channel;
                    out[3] = (uint8_t) round;
                    check( rocketnet_server_finish_message( world->server, 4 ) != 0 );
                }
            }
        }
    }
}

static void rq_run( rq_run_t * run )
{
    memset( run, 0, sizeof( *run ) );

    lifecycle_world_t world;
    lifecycle_world_create( &world, RQ_CLIENTS, RQ_CLIENTS );

    rocketnet_server_config_t defaults;
    rocketnet_server_default_config( &defaults );
    run->num_channels = defaults.num_channels;
    check( run->num_channels >= 6 && run->num_channels <= ROCKETNET_MAX_CHANNELS );

    for ( int k = 0; k < RQ_CLIENTS; k++ )
    {
        lifecycle_connect( &world, k, 0x1A00 + (uint64_t) k );
        run->index[k] = rocketnet_client_index( world.clients[k] );
    }

    // the rounds are a few kilobytes an update each way: a budget that sends
    // them whole, so no unreliable message is dropped for budget

    for ( int k = 0; k < RQ_CLIENTS; k++ )
    {
        check( rocketnet_client_set_send_kbps( world.clients[k], 100000 ) == ROCKETNET_STATUS_OK );
        check( rocketnet_server_set_send_kbps( world.server, run->index[k], 100000 ) == ROCKETNET_STATUS_OK );
    }

    for ( int round = 0; round < RQ_ROUNDS; round++ )
    {
        rq_send_round( &world, run, round );
        lifecycle_pump( &world );
        rq_drain( &world, run );
    }

    for ( int step = 0; step < 100; step++ )
    {
        lifecycle_pump( &world );
        rq_drain( &world, run );
    }

    lifecycle_world_destroy( &world );
}

// The delivery guarantees over one log: nothing twice; every reliable type
// (RELIABLE, RELIABLE_ORDERED and the two reliable receipt types) exactly
// once whatever channel it was sent on; ordered messages in send order within
// their sender and channel; sequenced messages never older than one seen
// before on their sender and channel. from_client names the direction.

static void rq_check_log( const rq_run_t * run, const rq_log_t * log, bool at_server )
{
    static uint8_t seen[RQ_CLIENTS][RQ_TYPES][ROCKETNET_MAX_CHANNELS][RQ_ROUNDS];
    int ordered_last[RQ_CLIENTS][ROCKETNET_MAX_CHANNELS];
    int sequenced_last[RQ_CLIENTS][ROCKETNET_MAX_CHANNELS];
    memset( seen, 0, sizeof( seen ) );
    for ( int k = 0; k < RQ_CLIENTS; k++ )
        for ( int c = 0; c < ROCKETNET_MAX_CHANNELS; c++ )
            ordered_last[k][c] = sequenced_last[k][c] = -1;

    for ( int i = 0; i < log->count; i++ )
    {
        const rq_entry_t * e = &log->entry[i];
        const int origin = at_server ? e->payload[0] : e->payload[0] - 0x80;
        const int type = e->payload[1], channel = e->payload[2], round = e->payload[3];
        check( origin >= 0 && origin < RQ_CLIENTS && type < RQ_TYPES && channel < run->num_channels && round < RQ_ROUNDS );
        if ( !( origin >= 0 && origin < RQ_CLIENTS && type < RQ_TYPES && channel < run->num_channels && round < RQ_ROUNDS ) )
            continue;

        // the message names its sender: the server's queue the client index,
        // a client's queue is its own

        check( e->sender == ( at_server ? (uint64_t) run->index[origin] : (uint64_t) origin ) );
        check( seen[origin][type][channel][round] == 0 );
        seen[origin][type][channel][round]++;

        if ( type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED || type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT )
        {
            const int order = round * 2 + ( type == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT );
            check( order == ordered_last[origin][channel] + 1 );                         // in send order, none skipped
            ordered_last[origin][channel] = order;
        }

        if ( type == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED || type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED )
        {
            const int order = round * 2 + ( type == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED );
            check( order > sequenced_last[origin][channel] );                            // never an older state
            sequenced_last[origin][channel] = order;
        }
    }

    for ( int k = 0; k < RQ_CLIENTS; k++ )
        for ( int c = 0; c < run->num_channels; c++ )
            for ( int r = 0; r < RQ_ROUNDS; r++ )
            {
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE][c][r] == 1 );
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE_ORDERED][c][r] == 1 );
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT][c][r] == 1 );
                check( seen[k][ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT][c][r] == 1 );
            }
}

static void test_message_receive_one_queue_every_type_every_channel()
{
    static rq_run_t run;
    rq_run( &run );
    rq_check_log( &run, &run.at_server, true );
    rq_check_log( &run, &run.at_client, false );
}
REGISTER_UNIT_TEST( test_message_receive_one_queue_every_type_every_channel );

// The receive is pumped: each call fills the caller's batch up to
// ROCKETNET_RECEIVE_BATCH_MESSAGES and returns true, and what one call does
// not return waits for the next call of the same update, in order, with no
// per-update cap. More messages than one batch holds arrive in one update;
// pumped to empty they are each seen once, in send order, and the call that
// finds nothing pending returns false with num_messages 0.

static void test_message_receive_pumped_in_order()
{
    // each client sends more than one batch; together the clients' deliveries
    // in one update are more than one batch of the server's one queue, so
    // the window, the send queue and the send buffer hold every message

    enum
    {
        NUM_SENDERS = 3
    };

    const int per_client = ROCKETNET_RECEIVE_BATCH_MESSAGES + 17;
    lifecycle_queue_messages = 1;
    while ( lifecycle_queue_messages < per_client )
    {
        lifecycle_queue_messages *= 2;
    }

    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1B01 );

    const int index = (int) rocketnet_client_index( client );
    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );

    const int total = 900;
    for ( int i = 0; i < total; i++ )
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        check( p != NULL );
        if ( !p )
            break;
        p[0] = (uint8_t) i;
        p[1] = (uint8_t) ( i >> 8 );
        p[2] = 0;
        p[3] = 0;
        check( rocketnet_client_finish_message( client, 4 ) != 0 );
    }

    const struct rocketnet_message_batch_t * batch = NULL;
    int received = 0;
    for ( int step = 0; step < 200 && received < total; step++ )
    {
        lifecycle_pump( &world );

        while ( ( batch = rocketnet_server_receive_messages( world.server ) ) != NULL )
        {
            check( batch->num_messages > 0 && batch->num_messages <= ROCKETNET_RECEIVE_BATCH_MESSAGES );

            for ( int i = 0; i < batch->num_messages; i++ )
            {
                check( batch->bytes[i] == 4 && batch->client_index[i] == index && batch->channel[i] == 0 );

                const int value = batch->data[i][0] | ( batch->data[i][1] << 8 );
                check( value == received );                // once each, in send order
                received++;
            }
        }
    }

    check( received == total );

    // an update with nothing delivered: the first receive returns NULL

    lifecycle_pump( &world );
    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( rocketnet_client_receive_messages( client ) == NULL );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_message_receive_pumped_in_order );

static int lifecycle_receive_null_assertions = 0;

static void lifecycle_receive_null_assert( const char * condition, const char * function, const char * file, int line )
{
    check( strcmp( condition, "p" ) == 0 );
    check( strcmp( function, "rocketnet_api_null_guard" ) == 0 );
    (void) file;
    (void) line;
    lifecycle_receive_null_assertions++;
}

// The library owns one receive batch per object: each receive refills and
// returns it, a receive that fills nothing returns NULL, the second of two
// receives in a row wipes the first, and a NULL object is one counted misuse
// that returns NULL.

static void test_receive_returns_the_objects_one_batch()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_client_t * client = world.clients[0];
    lifecycle_connect( &world, 0, 0x1B02 );

    // nothing pending: NULL, never an empty batch

    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( rocketnet_client_receive_messages( client ) == NULL );

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
        check( p != NULL );

        if ( p )
        {
            p[0] = (uint8_t) ( 0x40 + i );
            check( rocketnet_client_finish_message( client, 1 ) != 0 );
        }
    }

    const struct rocketnet_message_batch_t * first = NULL;
    for ( int step = 0; step < 50 && first == NULL; step++ )
    {
        lifecycle_pump( &world );
        first = rocketnet_server_receive_messages( world.server );
    }

    check( first != NULL && first->num_messages == 2 && first->data[0][0] == 0x40 && first->data[1][0] == 0x41 );

    // the next receive refills the same batch: nothing left, so NULL, and the
    // batch's count is wiped

    check( rocketnet_server_receive_messages( world.server ) == NULL );
    check( first->num_messages == 0 );

    // NULL is an expected misuse: Debug reports it through the assert hook

    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    lifecycle_receive_null_assertions = 0;
    rocketnet_assert_function( lifecycle_receive_null_assert );

    const uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_receive_messages( NULL ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( rocketnet_client_receive_messages( NULL ) == NULL );
    check( rocketnet_api_misuse_count() == misuse + 2 );
    rocketnet_assert_function( saved_assert );
    check( lifecycle_receive_null_assertions == ( ROCKETNET_ASSERTS ? 2 : 0 ) );
    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_receive_returns_the_objects_one_batch );

static int lifecycle_send_kbps_assertions = 0;

static void lifecycle_send_kbps_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    lifecycle_send_kbps_assertions++;
}

static rn_message_connection_t * lifecycle_client_connection( rocketnet_client_t * client )
{
    rocketnet_message_client_t * mc = rocketnet_client_message_client( client );
    check( mc != NULL );
    return mc ? &mc->object.connections[0] : NULL;
}

static rn_message_connection_t * lifecycle_server_connection( rocketnet_server_t * server, int slot )
{
    rocketnet_message_server_t * ms = rocketnet_server_message_server( server );
    check( ms != NULL && slot >= 0 && slot < ms->object.num_connections );
    return ( ms && slot >= 0 && slot < ms->object.num_connections ) ? &ms->object.connections[slot] : NULL;
}

static void lifecycle_queue_client_message( rocketnet_client_t * client, enum rocketnet_reliability_t reliability, int bytes, uint8_t tag, uint8_t seq )
{
    uint8_t * out = rocketnet_client_start_message( client, 0, reliability, bytes );
    check( out != NULL );
    if ( !out )
        return;
    memset( out, (uint8_t) ( tag ^ seq ), (size_t) bytes );
    out[0] = tag;
    if ( bytes > 1 )
    {
        out[1] = seq;
    }
    check( rocketnet_client_finish_message( client, bytes ) != 0 );
}

static void lifecycle_queue_server_message( rocketnet_server_t * server, int slot, enum rocketnet_reliability_t reliability, int bytes, uint8_t tag, uint8_t seq )
{
    uint8_t * out = rocketnet_server_start_message( server, (uint64_t) slot, 0, reliability, bytes );
    check( out != NULL );
    if ( !out )
        return;
    memset( out, (uint8_t) ( tag ^ seq ), (size_t) bytes );
    out[0] = tag;
    if ( bytes > 1 )
    {
        out[1] = seq;
    }
    check( rocketnet_server_finish_message( server, bytes ) != 0 );
}

static void lifecycle_drain_server_counts( rocketnet_server_t * server, int slot_a, uint8_t tag_a, int * rel_a, int * unrel_a,
                                           int slot_b, uint8_t tag_b, int * rel_b, int * unrel_b )
{
    if ( rel_a )
    {
        *rel_a = 0;
    }
    if ( unrel_a )
    {
        *unrel_a = 0;
    }
    if ( rel_b )
    {
        *rel_b = 0;
    }
    if ( unrel_b )
    {
        *unrel_b = 0;
    }

    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            const int sender = batch->client_index[i];
            const int bytes = batch->bytes[i];
            const uint8_t tag = batch->data[i][0];
            if ( sender == slot_a && tag == tag_a )
            {
                if ( bytes == 1000 && rel_a )
                    ( *rel_a )++;
                else if ( bytes == 900 && unrel_a )
                    ( *unrel_a )++;
                else
                    check( false );
            }
            else if ( sender == slot_b && tag == tag_b )
            {
                if ( bytes == 1000 && rel_b )
                    ( *rel_b )++;
                else if ( bytes == 900 && unrel_b )
                    ( *unrel_b )++;
                else
                    check( false );
            }
            else
            {
                check( false );
            }
        }
    }
}

static void lifecycle_server_pump( lifecycle_world_t * world, double dt = 0.01 )
{
    lifecycle_now = world->time;

    if ( rocketnet_server_running( world->server ) )
    {
        rocketnet_server_update( world->server, world->time );
        rocketnet_server_send_packets( world->server );
    }

    for ( int i = 0; i < world->num_clients; i++ )
    {
        if ( !world->clients[i] )
            continue;
        rocketnet_client_update( world->clients[i], world->time );
        rocketnet_client_send_packets( world->clients[i] );
    }

    world->time += dt;
}

static void lifecycle_drain_client_counts( rocketnet_client_t * client, uint8_t expected_tag, int * rel_1000, int * unrel_900 )
{
    if ( rel_1000 )
    {
        *rel_1000 = 0;
    }
    if ( unrel_900 )
    {
        *unrel_900 = 0;
    }

    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            check( batch->client_index[i] == 0 );
            check( batch->data[i][0] == expected_tag );
            if ( batch->bytes[i] == 1000 && rel_1000 )
                ( *rel_1000 )++;
            else if ( batch->bytes[i] == 900 && unrel_900 )
                ( *unrel_900 )++;
            else
                check( false );
        }
    }
}

struct lifecycle_expected_budget_t
{
    uint32_t kbps;
    double bytes_per_second;
    double depth;
    double tokens;
    double last_time;
    int reliable_waiting;
    uint64_t bytes_spent;
    uint64_t unreliable_dropped;
    uint64_t unreliable_dropped_bytes;
};

static lifecycle_expected_budget_t lifecycle_expected_budget_capture( const rn_message_connection_t * conn )
{
    lifecycle_expected_budget_t exp;
    memset( &exp, 0, sizeof( exp ) );
    check( conn != NULL );
    if ( !conn )
        return exp;
    exp.kbps = conn->budget.kbps;
    exp.bytes_per_second = conn->budget.bytes_per_second;
    exp.depth = conn->budget.depth;
    exp.tokens = conn->budget.tokens;
    exp.last_time = conn->budget.last_time;
    exp.reliable_waiting = conn->budget.reliable_waiting;
    exp.bytes_spent = conn->budget.bytes_spent;
    exp.unreliable_dropped = conn->budget.unreliable_dropped;
    exp.unreliable_dropped_bytes = conn->budget.unreliable_dropped_bytes;
    return exp;
}

static void lifecycle_expected_budget_apply_rate( lifecycle_expected_budget_t * exp, uint32_t kbps )
{
    exp->kbps = kbps;
    exp->bytes_per_second = (double) kbps * ( 1000.0 / 8.0 );

    const double window_bytes = exp->bytes_per_second * 0.1;
    exp->depth = window_bytes < (double) ROCKETNET_SEND_BUDGET ? (double) ROCKETNET_SEND_BUDGET : window_bytes;
}

static void lifecycle_expected_budget_init( lifecycle_expected_budget_t * exp, uint32_t kbps, double now )
{
    memset( exp, 0, sizeof( *exp ) );
    lifecycle_expected_budget_apply_rate( exp, kbps );
    exp->tokens = exp->depth;
    exp->last_time = now;
}

static void lifecycle_expected_budget_refill( lifecycle_expected_budget_t * exp, double now )
{
    if ( now > exp->last_time )
    {
        exp->tokens += exp->bytes_per_second * ( now - exp->last_time );
        if ( exp->tokens > exp->depth )
        {
            exp->tokens = exp->depth;
        }
        exp->last_time = now;
    }
}

static void lifecycle_expected_budget_set_kbps( lifecycle_expected_budget_t * exp, uint32_t kbps, double now )
{
    lifecycle_expected_budget_refill( exp, now );
    lifecycle_expected_budget_apply_rate( exp, kbps );
    if ( exp->tokens > exp->depth )
    {
        exp->tokens = exp->depth;
    }
}

static void lifecycle_expected_budget_begin_update( lifecycle_expected_budget_t * exp, double now )
{
    lifecycle_expected_budget_refill( exp, now );
    exp->reliable_waiting = 0;
}

static int lifecycle_expected_budget_max_wire_bytes( int payload_bytes )
{
    const int head_and_payload = ROCKETNET_PACKET_OVERHEAD_BYTES + payload_bytes;
    const int room = ROCKETNET_SEND_BUDGET - head_and_payload;
    const int ack_bytes = room < ROCKETNET_ACK_BLOCK_MAX_BYTES ? ( room > 0 ? room : 0 ) : ROCKETNET_ACK_BLOCK_MAX_BYTES;
    return head_and_payload + ack_bytes;
}

static void lifecycle_expected_budget_reliable_send( lifecycle_expected_budget_t * exp, int admitted_wire_bytes, int actual_wire_bytes )
{
    check( exp->reliable_waiting == 0 );
    check( (double) admitted_wire_bytes <= exp->tokens );
    exp->tokens -= (double) admitted_wire_bytes;
    exp->bytes_spent += (uint64_t) admitted_wire_bytes;

    const int refund = admitted_wire_bytes - actual_wire_bytes;
    check( refund >= 0 );
    exp->tokens += (double) refund;
    if ( exp->tokens > exp->depth )
    {
        exp->tokens = exp->depth;
    }
    exp->bytes_spent -= (uint64_t) refund;
}

static void lifecycle_expected_budget_reliable_wait( lifecycle_expected_budget_t * exp, int admitted_wire_bytes )
{
    check( exp->reliable_waiting != 0 || (double) admitted_wire_bytes > exp->tokens );
    exp->reliable_waiting = 1;
}

static void lifecycle_expected_budget_unreliable_drop( lifecycle_expected_budget_t * exp, int admitted_wire_bytes )
{
    check( exp->reliable_waiting != 0 || (double) admitted_wire_bytes > exp->tokens );
    exp->unreliable_dropped += 1;
    exp->unreliable_dropped_bytes += (uint64_t) admitted_wire_bytes;
}

static void lifecycle_check_budget_matches_oracle( const rn_message_connection_t * conn, const lifecycle_expected_budget_t * oracle )
{
    check( conn != NULL && oracle != NULL );
    if ( !conn || !oracle )
        return;
    check( conn->send_budget_enabled == 1 );
    check( conn->ordered.budget == &conn->budget );
    check( conn->ordered.builder.budget == &conn->budget );
    check( conn->unreliable_builder.budget == &conn->budget );
    check( conn->budget.kbps == oracle->kbps );
    check( conn->budget.bytes_per_second == oracle->bytes_per_second );
    check( conn->budget.depth == oracle->depth );
    check( conn->budget.tokens == oracle->tokens );
    check( conn->budget.last_time == oracle->last_time );
    check( conn->budget.reliable_waiting == oracle->reliable_waiting );
    check( conn->budget.bytes_spent == oracle->bytes_spent );
    check( conn->budget.unreliable_dropped == oracle->unreliable_dropped );
    check( conn->budget.unreliable_dropped_bytes == oracle->unreliable_dropped_bytes );
}

static void lifecycle_check_budget_unlimited( const rn_message_connection_t * conn )
{
    check( conn != NULL );
    if ( !conn )
        return;
    check( conn->send_budget_enabled == 0 );
    check( conn->ordered.budget == NULL );
    check( conn->ordered.builder.budget == NULL );
    check( conn->unreliable_builder.budget == NULL );
    check( conn->budget.kbps == 0 );
    check( conn->budget.tokens == 0.0 );
    check( conn->budget.reliable_waiting == 0 );
}

static void test_client_set_send_kbps_limit_budget_and_reconnect_rules()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 1 );

    rocketnet_client_t * client = world.clients[0];

    // NULL client and negative kbps validation before connect

    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    lifecycle_send_kbps_assertions = 0;
    rocketnet_assert_function( lifecycle_send_kbps_assert );

    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_set_send_kbps( NULL, 1000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( lifecycle_send_kbps_assertions == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    rocketnet_assert_function( saved_assert );

    misuse = rocketnet_api_misuse_count();
    check( rocketnet_client_set_send_kbps( client, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_client_set_send_kbps( client, INT_MIN ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );

    // disconnected and handshake calls with UNLIMITED, finite, and 0 are no-ops
    // that do not alter the configured starting rate (default 1000 kbps)

    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_set_send_kbps( client, 1 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_set_send_kbps( client, 0 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( world.server, rocketnet_server_port( world.server ), &world.identity, 0x1F10, 300, token );
    check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        check( rocketnet_client_set_send_kbps( client, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_client_set_send_kbps( client, INT_MIN ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
        check( rocketnet_client_set_send_kbps( client, 1 ) == ROCKETNET_STATUS_OK );
        check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
        lifecycle_pump( &world );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int slot = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world.server, slot ); i++ )
    {
        lifecycle_pump( &world );
    }
    check( rocketnet_server_client_connected( world.server, slot ) != 0 );
    check( rocketnet_api_misuse_count() == misuse );

    rn_message_connection_t * conn = lifecycle_client_connection( client );
    check( conn != NULL );
    check( conn->default_kbps == ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( conn->send_budget_enabled == 1 );
    check( conn->budget.kbps == (uint32_t) ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( conn->budget.depth == 12500.0 );

    // settle initial CONFIG exchange so no control packet is pending

    for ( int i = 0; i < 10; i++ )
    {
        lifecycle_pump( &world );
    }

    // negative values while connected return INVALID_ARGUMENT without changing budget state

    const lifecycle_expected_budget_t saved_budget = lifecycle_expected_budget_capture( conn );
    check( rocketnet_client_set_send_kbps( client, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_client_set_send_kbps( client, INT_MIN ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );
    lifecycle_check_budget_matches_oracle( conn, &saved_budget );

    // connected -> ROCKETNET_SEND_KBPS_UNLIMITED detaches the budget and admits a multi-packet burst in one update

    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    lifecycle_check_budget_unlimited( conn );
    for ( int i = 0; i < 4; i++ )
    {
        lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x30, (uint8_t) i );
    }
    for ( int i = 0; i < 2; i++ )
    {
        lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_UNRELIABLE, 900, 0x30, (uint8_t) i );
    }

    struct rocketnet_client_stats_t stats_before = rn_client_stats_of( client );
    lifecycle_pump( &world, 0.01 );

    struct rocketnet_client_stats_t stats_after = rn_client_stats_of( client );
    int rel_got = 0, unrel_got = 0;
    lifecycle_drain_server_counts( world.server, slot, 0x30, &rel_got, &unrel_got, -1, 0, NULL, NULL );
    check( stats_after.packets_sent - stats_before.packets_sent == 6 );
    check( stats_after.messages_dropped_over_budget == stats_before.messages_dropped_over_budget );
    check( rel_got == 4 && unrel_got == 2 );

    // connected UNLIMITED -> finite 1 kbps (minimum finite limit) and 90 kbps (11250 B/s, depth ROCKETNET_SEND_BUDGET):
    // matches independent arithmetic oracle and admits 1 reliable packet per update while deferring remaining reliable work

    rocketnet_message_client_t * mc = rocketnet_client_message_client( client );
    check( rocketnet_client_set_send_kbps( client, 1 ) == ROCKETNET_STATUS_OK );

    lifecycle_expected_budget_t oracle;
    lifecycle_expected_budget_init( &oracle, 1, mc->object.time );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    check( rocketnet_client_set_send_kbps( client, 90 ) == ROCKETNET_STATUS_OK );
    lifecycle_expected_budget_set_kbps( &oracle, 90, mc->object.time );
    lifecycle_check_budget_matches_oracle( conn, &oracle );

    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x31, (uint8_t) i );
    }
    for ( int i = 0; i < 2; i++ )
    {
        lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_UNRELIABLE, 900, 0x31, (uint8_t) i );
    }

    stats_before = rn_client_stats_of( client );

    const double update_1_time = world.time;
    lifecycle_pump( &world, 0.01 ); // runs update at update_1_time, advances world.time by 0.01s (+112.5 B at 90 kbps)
    stats_after = rn_client_stats_of( client );
    lifecycle_drain_server_counts( world.server, slot, 0x31, &rel_got, &unrel_got, -1, 0, NULL, NULL );

    const int rel_admitted = lifecycle_expected_budget_max_wire_bytes( (int) conn->ordered.builder.packet[0].payload_bytes );
    const int wire_1 = (int) ( stats_after.bytes_sent - stats_before.bytes_sent ) - 28;
    check( wire_1 > 1000 && wire_1 <= rel_admitted );
    lifecycle_expected_budget_begin_update( &oracle, update_1_time );
    lifecycle_expected_budget_reliable_send( &oracle, rel_admitted, wire_1 );
    lifecycle_expected_budget_begin_update( &oracle, update_1_time );
    lifecycle_expected_budget_reliable_wait( &oracle, rel_admitted );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    check( conn->written == 1 );
    check( stats_after.packets_sent - stats_before.packets_sent == 1 );
    check( stats_after.messages_dropped_over_budget - stats_before.messages_dropped_over_budget == 2 );
    check( rel_got == 1 && unrel_got == 0 );

    // after 0.01s at 90 kbps (+112.5 B), tokens < rel_admitted so the 2 remaining reliable messages stay deferred

    stats_before = rn_client_stats_of( client );

    const double update_2_time = world.time;
    lifecycle_pump( &world, 0.085 ); // runs update at update_2_time (t + 0.01s), advances world.time by 0.085s (+956.25 B at 90 kbps)
    stats_after = rn_client_stats_of( client );
    lifecycle_drain_server_counts( world.server, slot, 0x31, &rel_got, &unrel_got, -1, 0, NULL, NULL );
    lifecycle_expected_budget_begin_update( &oracle, update_2_time );
    lifecycle_expected_budget_reliable_wait( &oracle, rel_admitted );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    check( conn->written == 0 );
    check( rel_got == 0 && unrel_got == 0 );

    // after 0.085s at 90 kbps (+956.25 B, clamped to depth), the 2nd reliable message is admitted while the 3rd stays deferred

    stats_before = rn_client_stats_of( client );

    const double update_3_time = world.time;
    lifecycle_pump( &world, 0.02 ); // runs update at update_3_time, advances world.time by 0.02s
    stats_after = rn_client_stats_of( client );
    lifecycle_drain_server_counts( world.server, slot, 0x31, &rel_got, &unrel_got, -1, 0, NULL, NULL );

    const int wire_2 = (int) ( stats_after.bytes_sent - stats_before.bytes_sent ) - 28;
    check( wire_2 > 1000 && wire_2 <= rel_admitted );
    lifecycle_expected_budget_begin_update( &oracle, update_3_time );
    lifecycle_expected_budget_reliable_send( &oracle, rel_admitted, wire_2 );
    lifecycle_expected_budget_begin_update( &oracle, update_3_time );
    lifecycle_expected_budget_reliable_wait( &oracle, rel_admitted );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    check( conn->written == 1 );
    check( stats_after.packets_sent - stats_before.packets_sent == 1 );
    check( rel_got == 1 && unrel_got == 0 );

    // setting kbps to 0 while connected restores the directional default (1000 kbps = 125000 B/s, depth 12500 B)

    check( rocketnet_client_set_send_kbps( client, 0 ) == ROCKETNET_STATUS_OK );
    lifecycle_expected_budget_set_kbps( &oracle, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, mc->object.time );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    check( conn->budget.kbps == (uint32_t) ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( conn->budget.depth == 12500.0 );

    // at 1000 kbps, 0.02s refills +2500 B, so the 3rd deferred reliable message is admitted on the next update

    stats_before = rn_client_stats_of( client );

    const double update_4_time = world.time;
    lifecycle_pump( &world, 0.01 );
    stats_after = rn_client_stats_of( client );
    lifecycle_drain_server_counts( world.server, slot, 0x31, &rel_got, &unrel_got, -1, 0, NULL, NULL );

    const int wire_3 = (int) ( stats_after.bytes_sent - stats_before.bytes_sent ) - 28;
    lifecycle_expected_budget_begin_update( &oracle, update_4_time );
    lifecycle_expected_budget_reliable_send( &oracle, rel_admitted, wire_3 );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    check( conn->budget.reliable_waiting == 0 );
    check( rel_got == 1 && unrel_got == 0 );

    // also test when reliable work finishes without deferral, an over-budget unreliable packet is dropped by the builder

    check( rocketnet_client_set_send_kbps( client, 1 ) == ROCKETNET_STATUS_OK );
    lifecycle_expected_budget_set_kbps( &oracle, 1, mc->object.time );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x32, 0 );
    lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_UNRELIABLE, 900, 0x32, 0 );
    stats_before = rn_client_stats_of( client );

    const double update_5_time = world.time;
    lifecycle_pump( &world, 0.01 );
    stats_after = rn_client_stats_of( client );
    lifecycle_drain_server_counts( world.server, slot, 0x32, &rel_got, &unrel_got, -1, 0, NULL, NULL );

    const int unrel_admitted = lifecycle_expected_budget_max_wire_bytes( 900 + ROCKETNET_MESSAGE_RECORD_FIXED_BYTES + 1 );
    lifecycle_expected_budget_begin_update( &oracle, update_5_time );
    lifecycle_expected_budget_reliable_send( &oracle, rel_admitted, wire_1 );
    lifecycle_expected_budget_unreliable_drop( &oracle, unrel_admitted );
    lifecycle_check_budget_matches_oracle( conn, &oracle );
    check( conn->written == 1 );
    check( conn->budget.reliable_waiting == 0 );
    check( conn->budget.unreliable_dropped == 1 );
    check( stats_after.messages_dropped_over_budget - stats_before.messages_dropped_over_budget == 1 );
    check( rel_got == 1 && unrel_got == 0 );

    // finite maximum ROCKETNET_SEND_KBPS_UNLIMITED - 1 is accepted as a finite rate

    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED - 1 ) == ROCKETNET_STATUS_OK );
    check( conn->send_budget_enabled == 1 );
    check( conn->budget.kbps == (uint32_t) ( ROCKETNET_SEND_KBPS_UNLIMITED - 1 ) );

    // reconnect rule (1): a client configured with finite send_kbps = 1 that overrides to UNLIMITED at runtime
    // starts its next connection back at the configured 1 kbps

    rn_test_client_stop_and_destroy( world.clients[0] );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.send_kbps = 1;
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    client = world.clients[0];
    check( client != NULL );
    lifecycle_connect( &world, 0, 0x1F11 );
    for ( int i = 0; i < 10; i++ )
    {
        lifecycle_pump( &world );
    }
    conn = lifecycle_client_connection( client );
    check( conn->send_budget_enabled == 1 && conn->budget.kbps == 1 );
    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    lifecycle_check_budget_unlimited( conn );
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1F12 );
    for ( int i = 0; i < 12; i++ )
        lifecycle_pump( &world, 1.0 ); // refill 1 kbps bucket after CONFIG exchange
    conn = lifecycle_client_connection( client );
    check( conn->send_budget_enabled == 1 && conn->budget.kbps == 1 && conn->budget.tokens == (double) ROCKETNET_SEND_BUDGET );
    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x33, (uint8_t) i );
    }
    lifecycle_pump( &world, 0.01 );
    lifecycle_drain_server_counts( world.server, rocketnet_client_index( client ), 0x33, &rel_got, &unrel_got, -1, 0, NULL, NULL );
    check( rel_got == 1 && conn->budget.reliable_waiting == 1 );

    // reconnect rule (2): a client configured with ROCKETNET_SEND_KBPS_UNLIMITED that overrides to 1 kbps at runtime
    // starts its next connection back at UNLIMITED

    rn_test_client_stop_and_destroy( world.clients[0] );
    lifecycle_client_config( &client_config );
    client_config.send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    client = world.clients[0];
    check( client != NULL );
    lifecycle_connect( &world, 0, 0x1F13 );
    for ( int i = 0; i < 10; i++ )
    {
        lifecycle_pump( &world );
    }
    conn = lifecycle_client_connection( client );
    lifecycle_check_budget_unlimited( conn );
    check( rocketnet_client_set_send_kbps( client, 1 ) == ROCKETNET_STATUS_OK );
    check( conn->send_budget_enabled == 1 && conn->budget.kbps == 1 );
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_client_set_send_kbps( client, 1 ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1F14 );
    for ( int i = 0; i < 10; i++ )
    {
        lifecycle_pump( &world );
    }
    conn = lifecycle_client_connection( client );
    lifecycle_check_budget_unlimited( conn );
    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_queue_client_message( client, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x34, (uint8_t) i );
    }
    lifecycle_pump( &world, 0.01 );
    lifecycle_drain_server_counts( world.server, rocketnet_client_index( client ), 0x34, &rel_got, &unrel_got, -1, 0, NULL, NULL );
    check( rel_got == 3 && conn->budget.reliable_waiting == 0 );

    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_client_set_send_kbps_limit_budget_and_reconnect_rules );

static void test_server_set_send_kbps_limit_budget_slot_isolation_and_reset_rules()
{
    lifecycle_world_t world;
    lifecycle_world_create( &world, 2, 2 );

    rocketnet_server_t * server = world.server;

    // NULL server, negative kbps, and out-of-range client_index before any client connects

    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    lifecycle_send_kbps_assertions = 0;
    rocketnet_assert_function( lifecycle_send_kbps_assert );

    uint64_t misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_set_send_kbps( NULL, 0, 1000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse + 1 );
    check( lifecycle_send_kbps_assertions == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    rocketnet_assert_function( saved_assert );

    misuse = rocketnet_api_misuse_count();
    check( rocketnet_server_set_send_kbps( server, 0, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, 0, INT_MIN ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, -1, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, 2, INT_MIN ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );

    check( rocketnet_server_set_send_kbps( server, -1, 1000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, 2, 1000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, INT_MIN, 0 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, INT_MAX, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse + 4 );
    misuse = rocketnet_api_misuse_count();

    // disconnected valid slots accept 0, finite, and UNLIMITED as no-ops without altering config defaults

    check( rocketnet_server_set_send_kbps( server, 0, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( server, 0, 1 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( server, 1, 1 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( server, 1, 0 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_api_misuse_count() == misuse );

    lifecycle_connect( &world, 0, 0x1F20 );
    lifecycle_connect( &world, 1, 0x1F21 );

    const int slot_a = rocketnet_client_index( world.clients[0] );
    const int slot_b = rocketnet_client_index( world.clients[1] );
    check( slot_a >= 0 && slot_a < 2 && slot_b >= 0 && slot_b < 2 && slot_a != slot_b );
    for ( int i = 0; i < 10; i++ )
    {
        lifecycle_pump( &world );
    }

    rn_message_connection_t * conn_a = lifecycle_server_connection( server, slot_a );
    rn_message_connection_t * conn_b = lifecycle_server_connection( server, slot_b );
    check( conn_a != NULL && conn_b != NULL );
    check( conn_a->default_kbps == ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
    check( conn_b->default_kbps == ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
    check( conn_a->send_budget_enabled == 1 && conn_a->budget.kbps == (uint32_t) ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS && conn_a->budget.depth == 125000.0 );
    check( conn_b->send_budget_enabled == 1 && conn_b->budget.kbps == (uint32_t) ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS && conn_b->budget.depth == 125000.0 );

    // invalid calls while both slots are connected do not alter either slot

    const lifecycle_expected_budget_t saved_a = lifecycle_expected_budget_capture( conn_a );
    const lifecycle_expected_budget_t saved_b = lifecycle_expected_budget_capture( conn_b );
    check( rocketnet_server_set_send_kbps( server, slot_a, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, slot_b, INT_MIN ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse );
    check( rocketnet_server_set_send_kbps( server, -1, 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, 2, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_api_misuse_count() == misuse + 2 );
    misuse = rocketnet_api_misuse_count();
    lifecycle_check_budget_matches_oracle( conn_a, &saved_a );
    lifecycle_check_budget_matches_oracle( conn_b, &saved_b );

    // cap slot_a at 1 kbps while slot_b stays at 10000 kbps: only slot_a defers/drops in the next update

    rocketnet_message_server_t * ms = rocketnet_server_message_server( server );
    check( rocketnet_server_set_send_kbps( server, slot_a, 1 ) == ROCKETNET_STATUS_OK );

    lifecycle_expected_budget_t oracle_a = saved_a;
    lifecycle_expected_budget_t oracle_b = saved_b;
    lifecycle_expected_budget_set_kbps( &oracle_a, 1, ms->object.time );
    lifecycle_check_budget_matches_oracle( conn_a, &oracle_a );
    lifecycle_check_budget_matches_oracle( conn_b, &oracle_b );

    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_queue_server_message( server, slot_a, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x41, (uint8_t) i );
        lifecycle_queue_server_message( server, slot_b, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x42, (uint8_t) i );
    }

    for ( int i = 0; i < 2; i++ )
    {
        lifecycle_queue_server_message( server, slot_a, ROCKETNET_RELIABILITY_UNRELIABLE, 900, 0x41, (uint8_t) i );
        lifecycle_queue_server_message( server, slot_b, ROCKETNET_RELIABILITY_UNRELIABLE, 900, 0x42, (uint8_t) i );
    }

    struct rocketnet_server_client_stats_t stats_a_before = rn_server_client_stats_of( server, slot_a );
    struct rocketnet_server_client_stats_t stats_b_before = rn_server_client_stats_of( server, slot_b );
    struct rocketnet_server_stats_t srv_before = rn_server_stats_of( server );
    const double update_1_time = world.time;
    lifecycle_server_pump( &world, 0.01 );

    struct rocketnet_server_client_stats_t stats_a_after = rn_server_client_stats_of( server, slot_a );
    struct rocketnet_server_client_stats_t stats_b_after = rn_server_client_stats_of( server, slot_b );
    struct rocketnet_server_stats_t srv_after = rn_server_stats_of( server );

    int rel_a = 0, unrel_a = 0, rel_b = 0, unrel_b = 0;
    lifecycle_drain_client_counts( world.clients[0], 0x41, &rel_a, &unrel_a );
    lifecycle_drain_client_counts( world.clients[1], 0x42, &rel_b, &unrel_b );

    const int rel_admitted_a = lifecycle_expected_budget_max_wire_bytes( (int) conn_a->ordered.builder.packet[0].payload_bytes );
    const int wire_a_1 = (int) ( stats_a_after.bytes_sent - stats_a_before.bytes_sent ) - 28;
    lifecycle_expected_budget_begin_update( &oracle_a, update_1_time );
    lifecycle_expected_budget_reliable_send( &oracle_a, rel_admitted_a, wire_a_1 );
    lifecycle_expected_budget_begin_update( &oracle_a, update_1_time );
    lifecycle_expected_budget_reliable_wait( &oracle_a, rel_admitted_a );
    lifecycle_check_budget_matches_oracle( conn_a, &oracle_a );

    check( conn_a->written == 1 );
    check( stats_a_after.packets_sent - stats_a_before.packets_sent == 1 );
    check( srv_after.messages_dropped_over_budget - srv_before.messages_dropped_over_budget == 2 );
    check( rel_a == 1 && unrel_a == 0 );

    check( conn_b->written == 3 );
    check( stats_b_after.packets_sent - stats_b_before.packets_sent == 5 );
    check( conn_b->budget.reliable_waiting == 0 );
    check( conn_b->budget.unreliable_dropped == 0 );
    check( rel_b == 3 && unrel_b == 2 );

    // switch slot_a to ROCKETNET_SEND_KBPS_UNLIMITED and slot_b to 1 kbps: roles reverse immediately on the next update

    check( rocketnet_server_set_send_kbps( server, slot_a, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( server, slot_b, 1 ) == ROCKETNET_STATUS_OK );
    lifecycle_check_budget_unlimited( conn_a );
    check( conn_b->send_budget_enabled == 1 && conn_b->budget.kbps == 1 &&
           conn_b->budget.depth == (double) ROCKETNET_SEND_BUDGET &&
           conn_b->budget.tokens == (double) ROCKETNET_SEND_BUDGET );
    oracle_b = lifecycle_expected_budget_capture( conn_b );

    for ( int i = 0; i < 2; i++ )
    {
        lifecycle_queue_server_message( server, slot_a, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x41, (uint8_t) ( 3 + i ) );
        lifecycle_queue_server_message( server, slot_a, ROCKETNET_RELIABILITY_UNRELIABLE, 900, 0x41, (uint8_t) ( 2 + i ) );
    }

    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_queue_server_message( server, slot_b, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x43, (uint8_t) i );
    }
    for ( int i = 0; i < 2; i++ )
    {
        lifecycle_queue_server_message( server, slot_b, ROCKETNET_RELIABILITY_UNRELIABLE, 900, 0x43, (uint8_t) i );
    }

    stats_a_before = rn_server_client_stats_of( server, slot_a );
    stats_b_before = rn_server_client_stats_of( server, slot_b );
    srv_before = rn_server_stats_of( server );

    const double update_2_time = world.time;
    lifecycle_server_pump( &world, 0.02 );
    stats_a_after = rn_server_client_stats_of( server, slot_a );
    stats_b_after = rn_server_client_stats_of( server, slot_b );
    srv_after = rn_server_stats_of( server );

    lifecycle_drain_client_counts( world.clients[0], 0x41, &rel_a, &unrel_a );
    lifecycle_drain_client_counts( world.clients[1], 0x43, &rel_b, &unrel_b );

    // slot_a (UNLIMITED) flushed its 2 deferred + 2 new reliable messages and 2 new unreliable messages in 1 update

    check( rel_a == 4 && unrel_a == 2 );

    // slot_b (1 kbps) admitted 1 reliable packet, deferred 2, and dropped 2 unreliable messages

    const int rel_admitted_b = lifecycle_expected_budget_max_wire_bytes( (int) conn_b->ordered.builder.packet[0].payload_bytes );
    const int wire_b_1 = wire_a_1;
    lifecycle_expected_budget_begin_update( &oracle_b, update_2_time );
    lifecycle_expected_budget_reliable_send( &oracle_b, rel_admitted_b, wire_b_1 );
    lifecycle_expected_budget_begin_update( &oracle_b, update_2_time );
    lifecycle_expected_budget_reliable_wait( &oracle_b, rel_admitted_b );
    lifecycle_check_budget_matches_oracle( conn_b, &oracle_b );
    check( conn_b->written == 1 );
    check( srv_after.messages_dropped_over_budget - srv_before.messages_dropped_over_budget == 2 );
    check( rel_b == 1 && unrel_b == 0 );

    // setting slot_b to 0 restores the server directional default (10000 kbps = 1250000 B/s, depth 125000 B) without changing slot_a

    check( rocketnet_server_set_send_kbps( server, slot_b, 0 ) == ROCKETNET_STATUS_OK );
    lifecycle_expected_budget_set_kbps( &oracle_b, ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS, update_2_time );
    lifecycle_check_budget_matches_oracle( conn_b, &oracle_b );
    lifecycle_check_budget_unlimited( conn_a );

    // advance 0.02s (+25000 B at 10000 kbps): slot_b's 2 deferred reliable messages flush on the next update

    const double update_3_time = world.time;
    stats_b_before = rn_server_client_stats_of( server, slot_b );
    lifecycle_server_pump( &world, 0.01 );
    stats_b_after = rn_server_client_stats_of( server, slot_b );
    lifecycle_drain_client_counts( world.clients[1], 0x43, &rel_b, &unrel_b );
    check( stats_b_after.packets_sent - stats_b_before.packets_sent == 2 );
    lifecycle_expected_budget_begin_update( &oracle_b, update_3_time );
    lifecycle_expected_budget_reliable_send( &oracle_b, rel_admitted_b, wire_b_1 );
    lifecycle_expected_budget_begin_update( &oracle_b, update_3_time );
    lifecycle_expected_budget_reliable_send( &oracle_b, rel_admitted_b, wire_b_1 );
    lifecycle_check_budget_matches_oracle( conn_b, &oracle_b );
    check( conn_b->budget.reliable_waiting == 0 );
    check( rel_b == 2 && unrel_b == 0 );

    // finite maximum ROCKETNET_SEND_KBPS_UNLIMITED - 1 on slot_a is accepted as a finite rate without changing slot_b

    const lifecycle_expected_budget_t saved_b_after = lifecycle_expected_budget_capture( conn_b );
    check( rocketnet_server_set_send_kbps( server, slot_a, ROCKETNET_SEND_KBPS_UNLIMITED - 1 ) == ROCKETNET_STATUS_OK );
    check( conn_a->send_budget_enabled == 1 );
    check( conn_a->budget.kbps == (uint32_t) ( ROCKETNET_SEND_KBPS_UNLIMITED - 1 ) );
    lifecycle_check_budget_matches_oracle( conn_b, &saved_b_after );

    // slot reset rule (1): server configured with finite per_client_send_kbps = 1 resets a reconnected slot back to 1 kbps after a runtime UNLIMITED override

    lifecycle_world_destroy( &world );
    memset( &world, 0, sizeof( world ) );
    rocketnet_quiet( 1 );
    world.time = 2000.0;
    lifecycle_now = world.time;
    lifecycle_identity( &world.identity );

    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &world.identity, 2 );
    server_config.per_client_send_kbps = 1;
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    server = world.server;
    check( server != NULL && rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    world.num_clients = 1;
    check( world.clients[0] != NULL );

    lifecycle_connect( &world, 0, 0x1F22 );

    int slot = rocketnet_client_index( world.clients[0] );
    conn_a = lifecycle_server_connection( server, slot );
    check( conn_a->send_budget_enabled == 1 && conn_a->budget.kbps == 1 );
    check( rocketnet_server_set_send_kbps( server, slot, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    lifecycle_check_budget_unlimited( conn_a );
    lifecycle_disconnect( &world, 0 );
    check( rocketnet_server_set_send_kbps( server, slot, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1F23 );
    slot = rocketnet_client_index( world.clients[0] );
    for ( int i = 0; i < 12; i++ )
    {
        lifecycle_pump( &world, 1.0 );
    }
    conn_a = lifecycle_server_connection( server, slot );
    check( conn_a->send_budget_enabled == 1 && conn_a->budget.kbps == 1 && conn_a->budget.tokens == (double) ROCKETNET_SEND_BUDGET );
    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_queue_server_message( server, slot, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x44, (uint8_t) i );
    }
    lifecycle_server_pump( &world, 0.01 );
    lifecycle_drain_client_counts( world.clients[0], 0x44, &rel_a, &unrel_a );
    check( rel_a == 1 && conn_a->budget.reliable_waiting == 1 );

    // slot reset rule (2): server configured with ROCKETNET_SEND_KBPS_UNLIMITED resets slots back to UNLIMITED across stop/start and reconnect after a runtime 1 kbps override

    lifecycle_world_destroy( &world );
    memset( &world, 0, sizeof( world ) );
    rocketnet_quiet( 1 );
    world.time = 3000.0;
    lifecycle_now = world.time;
    lifecycle_identity( &world.identity );
    lifecycle_server_config( &server_config, &world.identity, 2 );
    server_config.per_client_send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    world.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    server = world.server;
    check( server != NULL && rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    world.clients[0] = rocketnet_client_create( NULL, &client_config, NULL );
    world.num_clients = 1;
    check( world.clients[0] != NULL );

    lifecycle_connect( &world, 0, 0x1F24 );
    slot = rocketnet_client_index( world.clients[0] );
    conn_a = lifecycle_server_connection( server, slot );
    lifecycle_check_budget_unlimited( conn_a );
    check( rocketnet_server_set_send_kbps( server, slot, 1 ) == ROCKETNET_STATUS_OK );
    check( conn_a->send_budget_enabled == 1 && conn_a->budget.kbps == 1 );

    rocketnet_server_stop( server );
    rn_test_server_wait_stopped( server );
    check( rocketnet_server_set_send_kbps( server, slot, 1 ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_set_send_kbps( server, -1, 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, 2, 1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( rocketnet_server_set_send_kbps( server, slot, -1 ) == ROCKETNET_STATUS_INVALID_ARGUMENT );
    rocketnet_client_disconnect( world.clients[0] );
    rn_test_client_wait_disconnected( world.clients[0] );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    lifecycle_connect( &world, 0, 0x1F25 );
    slot = rocketnet_client_index( world.clients[0] );
    for ( int i = 0; i < 10; i++ )
    {
        lifecycle_pump( &world );
    }
    conn_a = lifecycle_server_connection( server, slot );
    lifecycle_check_budget_unlimited( conn_a );
    for ( int i = 0; i < 3; i++ )
    {
        lifecycle_queue_server_message( server, slot, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0x45, (uint8_t) i );
    }
    lifecycle_server_pump( &world, 0.01 );
    lifecycle_drain_client_counts( world.clients[0], 0x45, &rel_a, &unrel_a );
    check( rel_a == 3 && conn_a->budget.reliable_waiting == 0 );

    lifecycle_world_destroy( &world );
}
REGISTER_UNIT_TEST( test_server_set_send_kbps_limit_budget_slot_isolation_and_reset_rules );

static uint32_t lifecycle_send_tagged_message( rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, uint8_t tag, uint8_t index_byte, int bytes )
{
    uint8_t * out = rocketnet_client_start_message( client, channel, reliability, bytes );
    check( out != NULL );
    if ( !out )
        return 0;
    memset( out, (int) tag, (size_t) bytes );
    out[0] = tag;
    if ( bytes > 1 )
    {
        out[1] = index_byte;
    }

    const uint32_t serial = rocketnet_client_finish_message( client, bytes );
    check( serial != 0 );

    return serial;
}

static uint32_t lifecycle_server_send_tagged_message( rocketnet_server_t * server, uint64_t dest, int channel, enum rocketnet_reliability_t reliability, uint8_t tag, uint8_t index_byte, int bytes )
{
    uint8_t * out = rocketnet_server_start_message( server, dest, channel, reliability, bytes );
    check( out != NULL );
    if ( !out )
        return 0;
    memset( out, (int) tag, (size_t) bytes );
    out[0] = tag;
    if ( bytes > 1 )
    {
        out[1] = index_byte;
    }

    const uint32_t serial = rocketnet_server_finish_message( server, bytes );
    check( serial != 0 );

    return serial;
}

static void lifecycle_assert_session_metrics_reset( const rocketnet_client_stats_t * stats )
{
    check( stats->rtt_ms == 0.0 );
    check( stats->jitter_ms == 0.0 );
    check( stats->packet_loss_percent == 0.0 );
    check( stats->packets_sent_per_second == 0.0 );
    check( stats->packets_received_per_second == 0.0 );
    check( stats->bandwidth_sent_kbps == 0.0 );
    check( stats->bandwidth_received_kbps == 0.0 );
}

// Reconnects one rocketnet_client_t across three connection generations:
//   1. server_a (slot 0, tag 0xA1, queued unreliable + reliable ordered with
//      ACK receipt + multi-fragment reliable ordered traffic at disconnect)
//   2. server_a again on slot 0 (tag 0xB1)
//   3. a second server_b on slot 0 (tag 0xC1)
// Proves that session-scoped client metrics and per-slot server stats reset at
// each boundary, while zero stale messages or ACK receipts from an earlier
// generation ever surface in a later generation.

static void test_client_reconnect_same_and_other_server_isolates_generations()
{
    rocketnet_quiet( 1 );

    double now = 2000.0;
    lifecycle_now = now;

    lifecycle_identity_t identity_a;
    lifecycle_identity_t identity_b;
    lifecycle_identity( &identity_a );

    uint8_t seed_b[32];
    for ( int i = 0; i < 32; ++i )
    {
        seed_b[i] = (uint8_t) ( 0x7A ^ i );
    }
    check( rocketnet_generate_backend_keypair_from_seed( seed_b, identity_b.backend_public, identity_b.backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config_a;
    rocketnet_server_config_t server_config_b;
    lifecycle_server_config( &server_config_a, &identity_a, 1 );
    lifecycle_server_config( &server_config_b, &identity_b, 1 );

    rocketnet_server_t * server_a = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config_a, NULL );
    rocketnet_server_t * server_b = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config_b, NULL );
    check( server_a != NULL && server_b != NULL );
    check( rocketnet_server_start( server_a ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server_b ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );

    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );

    // --- Generation 1 on server_a (tag 0xA1) ---

    char token_a1[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( server_a, rocketnet_server_port( server_a ), &identity_a, 0x3101, 300, token_a1 );
    check( rn_test_client_connect_after_completion( client, token_a1 ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 400 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( server_a, 0 ) ); ++i )
    {
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
        now += 0.01;
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == 0 );
    check( rocketnet_server_client_connected( server_a, 0 ) == 1 );

    // exchange initial Gen-1 traffic so RTT and counters warm up

    (void) lifecycle_send_tagged_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 0, 16 );
    (void) lifecycle_server_send_tagged_message( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 0, 16 );

    for ( int i = 0; i < 60; ++i )
    {
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );

        while ( rocketnet_client_receive_messages( client ) != NULL )
        {
        }

        while ( rocketnet_server_receive_messages( server_a ) != NULL )
        {
        }

        struct rocketnet_receipt_t drain_rcpt[8];
        (void) rocketnet_client_receipts( client, drain_rcpt, 8 );
        (void) rocketnet_server_receipts( server_a, 0, drain_rcpt, 8 );
        now += 0.02;
    }

    // queue fresh Gen-1 traffic in both directions (unreliable, reliable ordered
    // with ACK receipt, and multi-fragment reliable ordered) immediately before
    // disconnecting, plus send one step so packets are in-flight on loopback

    const uint32_t stale_c_serial_a1 = lifecycle_send_tagged_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 1, 32 );
    (void) lifecycle_send_tagged_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 0xA1, 2, 24 );
    (void) lifecycle_send_tagged_message( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xA1, 3, 2600 );

    const uint32_t stale_s_serial_a1 = lifecycle_server_send_tagged_message( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 4, 32 );
    (void) lifecycle_server_send_tagged_message( server_a, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 0xA1, 5, 24 );
    (void) lifecycle_server_send_tagged_message( server_a, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xA1, 6, 2600 );
    check( stale_c_serial_a1 != 0 && stale_s_serial_a1 != 0 );

    lifecycle_now = now;
    rocketnet_client_send_packets( client );
    rocketnet_server_send_packets( server_a );

    // queue another wave that stays in the send queues at disconnect time

    (void) lifecycle_send_tagged_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 7, 32 );
    (void) lifecycle_server_send_tagged_message( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xA1, 8, 32 );

    char token_a2[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( server_a, rocketnet_server_port( server_a ), &identity_a, 0x3102, 300, token_a2 );

    rocketnet_client_disconnect( client );
    check( !rocketnet_client_disconnected( client ) );
    check( rocketnet_client_connect( client, token_a2 ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    rocketnet_client_update( client, now );
    check( rocketnet_client_disconnected( client ) );

    const rocketnet_client_stats_t after_disconnect_1 = rn_client_stats_of( client );
    lifecycle_assert_session_metrics_reset( &after_disconnect_1 );
    check( rocketnet_client_receive_messages( client ) == NULL );
    check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );

    // pump server_a so it processes the disconnect and clears slot 0

    for ( int i = 0; i < 40 && rocketnet_server_client_connected( server_a, 0 ); ++i )
    {
        now += 0.01;
        lifecycle_now = now;
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
    }

    check( rocketnet_server_client_connected( server_a, 0 ) == 0 );

    while ( rocketnet_server_receive_messages( server_a ) != NULL )
    {
    }

    check( rocketnet_server_receipts( server_a, 0, NULL, 0 ) == 0 );

    const rocketnet_server_client_stats_t empty_slot_a = rn_server_client_stats_of( server_a, 0 );
    check( empty_slot_a.packets_sent == 0 && empty_slot_a.packets_received == 0 );
    check( empty_slot_a.bytes_sent == 0 && empty_slot_a.bytes_received == 0 );
    check( empty_slot_a.rtt_ms == 0.0 && empty_slot_a.jitter_ms == 0.0 && empty_slot_a.packet_loss_percent == 0.0 );

    // --- Generation 2 on the same server_a, reusing slot 0 (tag 0xB1) ---

    check( rocketnet_client_connect( client, token_a2 ) == ROCKETNET_STATUS_OK );

    const rocketnet_client_stats_t after_connect_2 = rn_client_stats_of( client );
    lifecycle_assert_session_metrics_reset( &after_connect_2 );

    const uint64_t cumulative_client_payloads_recv_base_2 = after_connect_2.payloads_received;
    const uint64_t cumulative_client_receipts_acked_base_2 = after_connect_2.receipts_acked;

    for ( int i = 0; i < 400 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( server_a, 0 ) ); ++i )
    {
        now += 0.01;
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == 0 );
    check( rocketnet_server_client_connected( server_a, 0 ) == 1 );
    check( rocketnet_server_client_id( server_a, 0 ) == 0x3102 );

    // before sending any Gen-2 messages, no Gen-1 messages or receipts leak on either side

    const rocketnet_server_client_stats_t slot_a_fresh = rn_server_client_stats_of( server_a, 0 );
    check( slot_a_fresh.packets_sent > 0 && slot_a_fresh.packets_received > 0 );
    check( rocketnet_client_receive_messages( client ) == NULL );
    check( rocketnet_server_receive_messages( server_a ) == NULL );
    check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );
    check( rocketnet_server_receipts( server_a, 0, NULL, 0 ) == 0 );

    const rocketnet_server_stats_t server_a_base_2 = rn_server_stats_of( server_a );

    const uint32_t gen2_c_serial = lifecycle_send_tagged_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 1, 24 );
    (void) lifecycle_send_tagged_message( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xB1, 2, 2600 );

    const uint32_t gen2_s_serial = lifecycle_server_send_tagged_message( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 1, 24 );
    (void) lifecycle_server_send_tagged_message( server_a, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xB1, 2, 2600 );

    int gen2_client_msgs = 0;
    int gen2_server_msgs = 0;
    int gen2_client_rcpts = 0;
    int gen2_server_rcpts = 0;
    for ( int step = 0; step < 200 && ( gen2_client_msgs < 2 || gen2_server_msgs < 2 || gen2_client_rcpts < 1 || gen2_server_rcpts < 1 ); ++step )
    {
        now += 0.01;
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                check( batch->bytes[i] >= 2 );
                check( batch->data[i][0] == 0xB1 );
                gen2_client_msgs++;
            }
        }

        while ( ( batch = rocketnet_server_receive_messages( server_a ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                check( batch->client_index[i] == 0 );
                check( batch->bytes[i] >= 2 );
                check( batch->data[i][0] == 0xB1 );
                gen2_server_msgs++;
            }
        }

        struct rocketnet_receipt_t rcpts[8];
        const int cr = rocketnet_client_receipts( client, rcpts, 8 );
        for ( int i = 0; i < cr; ++i )
        {
            check( rcpts[i].serial == gen2_c_serial );
            check( rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen2_client_rcpts++;
        }

        const int sr = rocketnet_server_receipts( server_a, 0, rcpts, 8 );
        for ( int i = 0; i < sr; ++i )
        {
            check( rcpts[i].serial == gen2_s_serial );
            check( rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen2_server_rcpts++;
        }
    }

    check( gen2_client_msgs == 2 && gen2_server_msgs == 2 );
    check( gen2_client_rcpts == 1 && gen2_server_rcpts == 1 );

    const rocketnet_server_client_stats_t slot_a_gen2 = rn_server_client_stats_of( server_a, 0 );
    check( slot_a_gen2.packets_sent > slot_a_fresh.packets_sent );
    check( slot_a_gen2.packets_received > slot_a_fresh.packets_received );

    const rocketnet_server_stats_t server_a_gen2 = rn_server_stats_of( server_a );
    check( server_a_gen2.receipts_acked - server_a_base_2.receipts_acked == 1 );

    const rocketnet_client_stats_t client_gen2 = rn_client_stats_of( client );
    check( client_gen2.payloads_received > cumulative_client_payloads_recv_base_2 );
    check( client_gen2.receipts_acked - cumulative_client_receipts_acked_base_2 == 1 );

    // queue late Gen-2 traffic on both ends before disconnecting and moving to server_b

    (void) lifecycle_send_tagged_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 9, 32 );
    (void) lifecycle_server_send_tagged_message( server_a, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xB1, 10, 32 );
    lifecycle_now = now;
    rocketnet_client_send_packets( client );
    rocketnet_server_send_packets( server_a );

    rocketnet_client_disconnect( client );
    rocketnet_client_update( client, now );
    check( rocketnet_client_disconnected( client ) );

    // --- Generation 3 on a different server_b (tag 0xC1) ---

    char token_b1[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( server_b, rocketnet_server_port( server_b ), &identity_b, 0x4101, 300, token_b1 );
    check( rocketnet_client_connect( client, token_b1 ) == ROCKETNET_STATUS_OK );

    const rocketnet_client_stats_t after_connect_3 = rn_client_stats_of( client );
    lifecycle_assert_session_metrics_reset( &after_connect_3 );

    const uint64_t cumulative_client_payloads_recv_base_3 = after_connect_3.payloads_received;
    const uint64_t cumulative_client_receipts_acked_base_3 = after_connect_3.receipts_acked;

    // step both server_a (draining its old teardown) and server_b + client

    for ( int i = 0; i < 400 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( server_b, 0 ) ); ++i )
    {
        now += 0.01;
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
        rocketnet_server_update( server_b, now );
        rocketnet_server_send_packets( server_b );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server_b, 0 ) == 1 );
    check( rocketnet_server_client_id( server_b, 0 ) == 0x4101 );
    check( rocketnet_server_client_connected( server_a, 0 ) == 0 );

    const rocketnet_server_client_stats_t slot_b_fresh = rn_server_client_stats_of( server_b, 0 );
    const rocketnet_server_stats_t server_b_base_3 = rn_server_stats_of( server_b );

    const uint32_t gen3_c_serial = lifecycle_send_tagged_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xC1, 1, 24 );
    const uint32_t gen3_s_serial = lifecycle_server_send_tagged_message( server_b, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xC1, 1, 24 );

    int gen3_client_msgs = 0;
    int gen3_server_msgs = 0;
    int gen3_client_rcpts = 0;
    int gen3_server_rcpts = 0;
    for ( int step = 0; step < 200 && ( gen3_client_msgs < 1 || gen3_server_msgs < 1 || gen3_client_rcpts < 1 || gen3_server_rcpts < 1 ); ++step )
    {
        now += 0.01;
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server_a, now );
        rocketnet_server_send_packets( server_a );
        rocketnet_server_update( server_b, now );
        rocketnet_server_send_packets( server_b );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                check( batch->bytes[i] >= 2 );
                check( batch->data[i][0] == 0xC1 );
                gen3_client_msgs++;
            }
        }

        while ( ( batch = rocketnet_server_receive_messages( server_b ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; ++i )
            {
                check( batch->client_index[i] == 0 );
                check( batch->bytes[i] >= 2 );
                check( batch->data[i][0] == 0xC1 );
                gen3_server_msgs++;
            }
        }

        struct rocketnet_receipt_t rcpts[8];
        const int cr = rocketnet_client_receipts( client, rcpts, 8 );
        for ( int i = 0; i < cr; ++i )
        {
            check( rcpts[i].serial == gen3_c_serial );
            check( rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen3_client_rcpts++;
        }

        const int sr = rocketnet_server_receipts( server_b, 0, rcpts, 8 );
        for ( int i = 0; i < sr; ++i )
        {
            check( rcpts[i].serial == gen3_s_serial );
            check( rcpts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            gen3_server_rcpts++;
        }
    }

    check( gen3_client_msgs == 1 && gen3_server_msgs == 1 );
    check( gen3_client_rcpts == 1 && gen3_server_rcpts == 1 );

    const rocketnet_server_client_stats_t slot_b_gen3 = rn_server_client_stats_of( server_b, 0 );
    check( slot_b_gen3.packets_sent > slot_b_fresh.packets_sent );
    check( slot_b_gen3.packets_received > slot_b_fresh.packets_received );

    const rocketnet_server_stats_t server_b_gen3 = rn_server_stats_of( server_b );
    check( server_b_gen3.receipts_acked - server_b_base_3.receipts_acked == 1 );

    const rocketnet_client_stats_t client_gen3 = rn_client_stats_of( client );
    check( client_gen3.payloads_received > cumulative_client_payloads_recv_base_3 );
    check( client_gen3.receipts_acked - cumulative_client_receipts_acked_base_3 == 1 );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server_a );
    rn_test_server_stop_and_destroy( server_b );
}
REGISTER_UNIT_TEST( test_client_reconnect_same_and_other_server_isolates_generations );

struct lifecycle_pressure_allocator_t
{
    int fail_countdown;
    int refused_count;
    int active_allocations;
};

static void * lifecycle_pressure_malloc( void * context, size_t size )
{
    lifecycle_pressure_allocator_t * state = (lifecycle_pressure_allocator_t *) context;
    if ( state->fail_countdown > 0 )
    {
        state->fail_countdown--;

        if ( state->fail_countdown == 0 )
        {
            state->refused_count++;
            return NULL;
        }
    }

    void * ptr = malloc( size > 0 ? size : 1 );
    if ( ptr )
    {
        state->active_allocations++;
    }

    return ptr;
}

static void lifecycle_pressure_free( void * context, void * ptr )
{
    if ( !ptr )
        return;

    lifecycle_pressure_allocator_t * state = (lifecycle_pressure_allocator_t *) context;
    state->active_allocations--;
    free( ptr );
}

struct lifecycle_delayed_ack_drop_link_t
{
    rn_message_link_t base;
    bool hold_acks;
    int held_count;
    int held_clients[8];
    uint64_t held_packets[8];
    uint64_t held_connections[8];
    bool release_held_acks;
    bool drop_next_packet;
    int dropped_packets;
};

static lifecycle_delayed_ack_drop_link_t * g_lifecycle_delayed_link = NULL;

static int lifecycle_delayed_link_acked( void * transport, const int ** client_index, const uint64_t ** packet_ids )
{
    lifecycle_delayed_ack_drop_link_t * ctx = g_lifecycle_delayed_link;
    const int * base_clients = NULL;
    const uint64_t * base_packets = NULL;
    const int n = ctx->base.acked( transport, &base_clients, &base_packets );

    if ( ctx->hold_acks )
    {
        for ( int k = 0; k < n && ctx->held_count < 8; ++k )
        {
            ctx->held_clients[ctx->held_count] = base_clients[k];
            ctx->held_packets[ctx->held_count] = base_packets[k];
            ctx->held_connections[ctx->held_count] = ctx->base.acked_connection
                                                         ? ctx->base.acked_connection( transport, k )
                                                         : 0;
            ctx->held_count++;
        }

        *client_index = NULL;
        *packet_ids = NULL;
        return 0;
    }

    if ( ctx->release_held_acks && ctx->held_count > 0 )
    {
        *client_index = ctx->held_clients;
        *packet_ids = ctx->held_packets;
        return ctx->held_count;
    }

    *client_index = base_clients;
    *packet_ids = base_packets;

    return n;
}

static uint64_t lifecycle_delayed_link_acked_connection( void * transport, int index )
{
    lifecycle_delayed_ack_drop_link_t * ctx = g_lifecycle_delayed_link;
    if ( ctx->release_held_acks && ctx->held_count > 0 )
    {
        const uint64_t conn = ( index >= 0 && index < ctx->held_count ) ? ctx->held_connections[index] : 0;

        if ( index + 1 >= ctx->held_count )
        {
            ctx->release_held_acks = false;
            ctx->held_count = 0;
        }

        return conn;
    }

    return ctx->base.acked_connection ? ctx->base.acked_connection( transport, index ) : 0;
}

static int lifecycle_delayed_link_finish_packet( void * transport, struct rocketnet_packet_context_t * context, int payload_bytes )
{
    lifecycle_delayed_ack_drop_link_t * ctx = g_lifecycle_delayed_link;
    if ( ctx->drop_next_packet )
    {
        ctx->drop_next_packet = false;
        ctx->dropped_packets++;
        ctx->base.abort_packet( transport, context );
        return 1;
    }

    return ctx->base.finish_packet( transport, context, payload_bytes );
}

// Single bounded lifecycle story integrating:
//   1. Slot 0 reuse across generations while a Generation-1 reliable ACK is
//      deliberately delayed on the server link and delivered after Generation 2
//      occupies slot 0, proving the late ACK increments
//      ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY and cannot alter
//      Generation 2's window, counters, receipts, or delivery.
//   2. Allocator-backed memory pressure during transition (custom allocator
//      refusal returning ROCKETNET_STATUS_OUT_OF_MEMORY on reconnect and
//      whole-message send-region refusal returning NULL / false via
//      rocketnet_client_can_send_message and rocketnet_server_can_send_message
//      with zero partial bytes or stale receipts, plus bounded recovery after
//      ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED).
//   3. Disconnect notification draining across bounded fake-clock updates with
//      disconnect_packets_per_update = 2, proving each update seals at most 2
//      packets while the reused session connects and remains usable.
//   4. One fragmented reliable payload (2600 bytes, 3 fragments, ACK receipt)
//      and a follow-up ordered message in the new generation with one fragment
//      frame deliberately dropped; after bounded fake-clock retransmission and
//      reassembly, asserts byte-exact payload, message order, ACK receipt
//      identity, and ownership by the new session generation.
//
// Oracle sensitivity verified against single-concern production mutations across:
//   - Generation-isolation guard (rocketnet_server_connection_id_assigned in
//     sdk/rocketnet_server_protocol.cpp and rn_endpoint_reset in sdk/rocketnet_endpoint.cpp),
//   - Delayed-ACK generation tagging and lookup (server->acked_connection_ids in
//     sdk/rocketnet_server_protocol.cpp and rocketnet_server_transport_acked_connection_ids
//     in sdk/rocketnet_server_transport.cpp),
//   - Allocator/send-region refusal and exhaustion transitions (rn_region_send_refuse
//     in sdk/rocketnet_allocator.cpp and rn_endpoint_send_room in sdk/rocketnet_endpoint.cpp),
//   - Per-update disconnect allowance (disconnect_packets_per_update drain cap in
//     rocketnet_server_transport_update in sdk/rocketnet_server_transport.cpp),
//   - Fragmented chunk retransmission, ACK receipt emission, and payload byte/order
//     integrity (rn_endpoint_ordered_due, rn_endpoint_on_acked, and rn_endpoint_finish
//     in sdk/rocketnet_endpoint.cpp).

static void test_integrated_slot_reuse_memory_pressure_disconnect_budget_and_delayed_ack()
{
    rocketnet_quiet( 1 );

    double now = 3000.0;
    lifecycle_now = now;

    lifecycle_pressure_allocator_t alloc_state;
    memset( &alloc_state, 0, sizeof( alloc_state ) );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( rocketnet_init( &alloc_state, lifecycle_pressure_malloc, lifecycle_pressure_free ) == ROCKETNET_STATUS_OK );

    lifecycle_identity_t identity;
    lifecycle_identity( &identity );

    rocketnet_server_config_t server_config;
    lifecycle_server_config( &server_config, &identity, 1 );
    server_config.disconnect_packets_per_update = 2;
    server_config.message_resend_delay_initial_ms = 20;
    server_config.message_resend_delay_max_ms = 20;
    server_config.per_client_send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    server_config.send_queue_messages = 16;

    enum rocketnet_status_t create_status = ROCKETNET_TEST_STATUS_UNSET;
    rocketnet_server_t * server = rocketnet_server_create( &alloc_state, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL && create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    lifecycle_client_config( &client_config );
    client_config.message_resend_delay_initial_ms = 20;
    client_config.message_resend_delay_max_ms = 20;
    client_config.send_kbps = 1;
    client_config.send_queue_messages = 16;
    create_status = ROCKETNET_TEST_STATUS_UNSET;

    rocketnet_client_t * client = rocketnet_client_create( &alloc_state, &client_config, &create_status );
    check( client != NULL && create_status == ROCKETNET_STATUS_OK );

    rn_message_object_t * server_msg_obj = &rocketnet_server_message_server( server )->object;
    struct rocketnet_server_transport_t * server_transport = rocketnet_message_server_transport( rocketnet_server_message_server( server ) );
    check( server_msg_obj != NULL && server_transport != NULL );

    lifecycle_delayed_ack_drop_link_t delayed_link;
    memset( &delayed_link, 0, sizeof( delayed_link ) );
    delayed_link.base = server_msg_obj->link;
    g_lifecycle_delayed_link = &delayed_link;
    server_msg_obj->link.acked = lifecycle_delayed_link_acked;
    server_msg_obj->link.acked_connection = lifecycle_delayed_link_acked_connection;
    server_msg_obj->link.finish_packet = lifecycle_delayed_link_finish_packet;

    // --- Generation 1 on slot 0 (client_id 0x5101, tag 0xD1) ---

    char token_gen1[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( server, rocketnet_server_port( server ), &identity, 0x5101, 300, token_gen1 );
    check( rn_test_client_connect_after_completion( client, token_gen1 ) == ROCKETNET_STATUS_OK );

    for ( int i = 0; i < 400 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( server, 0 ) ); ++i )
    {
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        now += 0.01;
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == 0 );
    check( rocketnet_server_client_connected( server, 0 ) == 1 );

    const uint64_t gen1_connection_id = server_msg_obj->hot_connections[0].connection_id;
    check( gen1_connection_id != 0 );

    // Hold transport ACKs on the server message link while Generation 1 sends a
    // reliable message from server -> client and client ACKs it on the wire.
    // Send two dummy unreceipted messages first so gen1_server_serial (3) is
    // distinct from Generation 2's fresh serials (1 and 2).

    (void) lifecycle_server_send_tagged_message(
        server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xD0, 1, 16 );
    (void) lifecycle_server_send_tagged_message(
        server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0xD0, 2, 16 );
    delayed_link.hold_acks = true;

    const uint32_t gen1_server_serial = lifecycle_server_send_tagged_message(
        server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xD1, 1, 64 );
    check( gen1_server_serial == 3 );

    int gen1_client_received = 0;
    for ( int i = 0; i < 40 && ( gen1_client_received < 3 || delayed_link.held_count < 1 ); ++i )
    {
        now += 0.005;
        lifecycle_now = now;
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; ++m )
            {
                if ( gen1_client_received == 2 )
                {
                    check( batch->bytes[m] == 64 && batch->data[m][0] == 0xD1 && batch->data[m][1] == 1 );
                }
                gen1_client_received++;
            }
        }
    }

    check( gen1_client_received == 3 );
    check( delayed_link.held_count >= 1 );
    check( delayed_link.held_connections[0] == gen1_connection_id );
    check( rocketnet_server_receipts( server, 0, NULL, 0 ) == 0 );
    delayed_link.hold_acks = false;

    // exercise whole-message send-region pressure on Generation 1 before disconnect:
    // filling all 17 send descriptors (send_queue_messages=16 + 1 reserve) causes
    // rocketnet_client_can_send_message to report false before start_message, and
    // start_message under pressure refuses the whole message (NULL) without partial
    // bytes or stale receipts while transitioning the client to MESSAGE_MEMORY_EXHAUSTED

    int pre_exhaust_queued = 0;

    while ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 900 ) )
    {
        const uint32_t s = lifecycle_send_tagged_message(
            client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xD2, (uint8_t) pre_exhaust_queued, 900 );
        check( s != 0 );
        pre_exhaust_queued++;
        check( pre_exhaust_queued <= 17 );
    }

    check( pre_exhaust_queued == 17 );
    check( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 900 ) == 0 );
    check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 900 ) == NULL );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED );
    check( rocketnet_client_receive_messages( client ) == NULL );
    check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );

    // advance the client one tick so its region resets while retaining the
    // MESSAGE_MEMORY_EXHAUSTED terminal state, and kick slot 0 on the server so
    // the server queues its 10-packet disconnect burst regulated by
    // disconnect_packets_per_update = 2

    now += 0.01;
    lifecycle_now = now;
    rocketnet_client_update( client, now );
    rocketnet_client_send_packets( client );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED );

    const uint64_t seals_before_disconnect = rocketnet_server_transport_disconnect_seals_for_test( server_transport );
    rocketnet_server_disconnect_client( server, 0 );

    // first server update after disconnect_client drains at most disconnect_packets_per_update (2)
    // disconnect packets while freeing slot 0 for reuse

    now += 0.005;
    lifecycle_now = now;
    rocketnet_server_update( server, now );
    rocketnet_server_send_packets( server );

    uint64_t prev_seals = rocketnet_server_transport_disconnect_seals_for_test( server_transport );
    check( prev_seals - seals_before_disconnect == 2 );
    check( rocketnet_server_client_connected( server, 0 ) == 0 );
    check( rocketnet_server_receive_messages( server ) == NULL );
    check( rocketnet_server_receipts( server, 0, NULL, 0 ) == 0 );

    // --- Allocator-backed memory pressure and terminal-state recovery during transition ---
    // From the terminal MESSAGE_MEMORY_EXHAUSTED state, the first connect requests
    // transport retirement (returning ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED while
    // retaining MESSAGE_MEMORY_EXHAUSTED); after bounded update completion,
    // rocketnet_client_disconnected( client ) becomes true. Then force the custom
    // allocator to refuse the reconnect allocation in rocketnet_client_connect,
    // proving it returns ROCKETNET_STATUS_OUT_OF_MEMORY, leaves the client cleanly
    // disconnected with zero partial state, and recovers when allocator pressure lifts.

    char token_gen2[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    lifecycle_token( server, rocketnet_server_port( server ), &identity, 0x5102, 300, token_gen2 );

    check( rocketnet_client_connect( client, token_gen2 ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED );
    now += 0.005;
    lifecycle_now = now;
    rocketnet_client_update( client, now );
    check( rocketnet_client_disconnected( client ) );

    alloc_state.fail_countdown = 1;

    const int refused_before = alloc_state.refused_count;
    check( rocketnet_client_connect( client, token_gen2 ) == ROCKETNET_STATUS_OUT_OF_MEMORY );
    check( alloc_state.refused_count == refused_before + 1 );
    check( rocketnet_client_disconnected( client ) );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_receive_messages( client ) == NULL );
    check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );

    // lift allocator pressure and reconnect into slot 0 while the server's
    // disconnect notification burst from Generation 1 is still draining across
    // bounded fake-clock updates (at most 2 disconnect packets per update)

    check( rocketnet_client_connect( client, token_gen2 ) == ROCKETNET_STATUS_OK );

    int burst_active_updates_during_gen2 = 0;

    for ( int i = 0; i < 400 && ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED || !rocketnet_server_client_connected( server, 0 ) || prev_seals - seals_before_disconnect < (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS ); ++i )
    {
        now += 0.01;
        lifecycle_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );

        const uint64_t cur_seals = rocketnet_server_transport_disconnect_seals_for_test( server_transport );
        const uint64_t step_seals = cur_seals - prev_seals;
        check( step_seals <= (uint64_t) server_config.disconnect_packets_per_update );
        if ( step_seals > 0 )
        {
            burst_active_updates_during_gen2++;
        }
        prev_seals = cur_seals;
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_index( client ) == 0 );
    check( rocketnet_server_client_connected( server, 0 ) == 1 );
    check( rocketnet_server_client_id( server, 0 ) == 0x5102 );
    check( prev_seals - seals_before_disconnect == (uint64_t) ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS );
    check( burst_active_updates_during_gen2 >= 4 );

    uint8_t wiped_burst_key[ROCKETNET_KEY_BYTES];
    uint8_t zero_burst_key[ROCKETNET_KEY_BYTES];
    int burst_head = -1;
    int burst_count = -1;
    memset( wiped_burst_key, 0xFF, sizeof( wiped_burst_key ) );
    memset( zero_burst_key, 0, sizeof( zero_burst_key ) );
    check( rocketnet_server_transport_disconnect_burst_key_for_test(
               server_transport, 0, wiped_burst_key, (int) sizeof( wiped_burst_key ), &burst_head, &burst_count ) != 0 );
    check( burst_count == 0 );
    check( memcmp( wiped_burst_key, zero_burst_key, sizeof( zero_burst_key ) ) == 0 );

    const uint64_t gen2_connection_id = server_msg_obj->hot_connections[0].connection_id;
    check( gen2_connection_id != 0 && gen2_connection_id != gen1_connection_id );
    check( rocketnet_client_receive_messages( client ) == NULL );
    check( rocketnet_server_receive_messages( server ) == NULL );
    check( rocketnet_client_receipts( client, NULL, 0 ) == 0 );
    check( rocketnet_server_receipts( server, 0, NULL, 0 ) == 0 );

    // --- Deliver the delayed Generation-1 ACK after slot 0 reuse ---
    // Release the held Generation-1 ACK while Generation 2 occupies slot 0 and
    // prove it increments ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY without
    // touching Generation 2's window, receipts, counters, or delivery

    const uint64_t stale_conn_before = server_msg_obj->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY];
    const uint64_t acked_receipts_before = server_msg_obj->counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED];
    const rocketnet_server_stats_t server_stats_before_late_ack = rn_server_stats_of( server );
    struct rocketnet_delivery_capture_t server_capture_before;
    memset( &server_capture_before, 0, sizeof( server_capture_before ) );
    rocketnet_message_server_delivery_capture( rocketnet_server_message_server( server ), 0, &server_capture_before );

    delayed_link.release_held_acks = true;
    now += 0.005;
    lifecycle_now = now;
    rocketnet_server_update( server, now );
    check( !delayed_link.release_held_acks && delayed_link.held_count == 0 );
    check( server_msg_obj->counters.values[ROCKETNET_COUNTER_MESSAGE_STALE_CONNECTION_ENTRY] == stale_conn_before + 1 );
    check( server_msg_obj->counters.values[ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED] == acked_receipts_before );

    const rocketnet_server_stats_t server_stats_after_late_ack = rn_server_stats_of( server );
    check( server_stats_after_late_ack.receipts_acked == server_stats_before_late_ack.receipts_acked );
    check( server_stats_after_late_ack.payloads_received == server_stats_before_late_ack.payloads_received );
    check( rocketnet_server_receipts( server, 0, NULL, 0 ) == 0 );
    check( rocketnet_server_receive_messages( server ) == NULL );

    struct rocketnet_delivery_capture_t server_capture_after;
    memset( &server_capture_after, 0, sizeof( server_capture_after ) );
    rocketnet_message_server_delivery_capture( rocketnet_server_message_server( server ), 0, &server_capture_after );
    check( memcmp( &server_capture_before, &server_capture_after, sizeof( server_capture_before ) ) == 0 );

    // --- Fragmented reliable payload + 1 deliberately dropped frame in Generation 2 ---
    // Send a 2600-byte reliable ordered message (3 fragments: 1152 + 1152 + 296 bytes)
    // with ACK receipt on channel 1, followed by a 48-byte ordered message on channel 1,
    // and drop the first outbound packet frame on the server link so fragment 0 must
    // be retransmitted after message_resend_delay_initial_ms

    const int frag_bytes = 2600;
    const int tail_bytes = 48;
    check( rocketnet_server_can_send_message( server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, frag_bytes ) == 1 );

    uint8_t * frag_out = rocketnet_server_start_message( server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, frag_bytes );
    check( frag_out != NULL );
    for ( int i = 0; i < frag_bytes; ++i )
    {
        frag_out[i] = (uint8_t) ( 0xE1u ^ ( ( (unsigned int) i * 31u ) & 0xFFu ) );
    }
    frag_out[0] = 0xE1;

    const uint32_t gen2_frag_serial = rocketnet_server_finish_message( server, frag_bytes );
    check( gen2_frag_serial != 0 );

    check( rocketnet_server_can_send_message( server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, tail_bytes ) == 1 );

    uint8_t * tail_out = rocketnet_server_start_message( server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, tail_bytes );
    check( tail_out != NULL );
    for ( int i = 0; i < tail_bytes; ++i )
    {
        tail_out[i] = (uint8_t) ( 0xE2u ^ (uint8_t) i );
    }
    tail_out[0] = 0xE2;

    const uint32_t gen2_tail_serial = rocketnet_server_finish_message( server, tail_bytes );
    check( gen2_tail_serial != 0 && gen2_tail_serial != gen2_frag_serial );

    // also send a Generation-2 client->server reliable message with ACK receipt to confirm
    // bidirectional ownership in the new generation

    check( rocketnet_client_set_send_kbps( client, ROCKETNET_SEND_KBPS_UNLIMITED ) == ROCKETNET_STATUS_OK );

    const uint32_t gen2_client_serial = lifecycle_send_tagged_message(
        client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0xE3, 7, 40 );
    check( gen2_client_serial != 0 );

    delayed_link.drop_next_packet = true;

    const uint64_t resends_before = rn_server_stats_of( server ).messages_resent;

    int client_rx_order = 0;
    int server_rx_count = 0;
    bool got_frag_receipt = false;
    bool got_tail_receipt = false;
    bool got_client_receipt = false;
    for ( int step = 0; step < 200 && ( client_rx_order < 2 || server_rx_count < 1 || !got_frag_receipt || !got_tail_receipt || !got_client_receipt ); ++step )
    {
        now += 0.01;
        lifecycle_now = now;
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; ++m )
            {
                check( batch->channel[m] == 1 );

                if ( client_rx_order == 0 )
                {
                    check( batch->bytes[m] == frag_bytes );
                    check( batch->data[m][0] == 0xE1 );
                    for ( int b = 1; b < frag_bytes; ++b )
                    {
                        check( batch->data[m][b] == (uint8_t) ( 0xE1u ^ ( ( (unsigned int) b * 31u ) & 0xFFu ) ) );
                    }
                }
                else if ( client_rx_order == 1 )
                {
                    check( batch->bytes[m] == tail_bytes );
                    check( batch->data[m][0] == 0xE2 );
                    for ( int b = 1; b < tail_bytes; ++b )
                    {
                        check( batch->data[m][b] == (uint8_t) ( 0xE2u ^ (uint8_t) b ) );
                    }
                }

                client_rx_order++;
            }
        }

        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; ++m )
            {
                check( batch->client_index[m] == 0 );
                check( batch->channel[m] == 1 );
                check( batch->bytes[m] == 40 );
                check( batch->data[m][0] == 0xE3 && batch->data[m][1] == 7 );
                server_rx_count++;
            }
        }

        struct rocketnet_receipt_t rcpts[8];
        const int sr = rocketnet_server_receipts( server, 0, rcpts, 8 );
        for ( int r = 0; r < sr; ++r )
        {
            check( rcpts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            check( rcpts[r].serial != gen1_server_serial );
            if ( rcpts[r].serial == gen2_frag_serial )
                got_frag_receipt = true;
            else if ( rcpts[r].serial == gen2_tail_serial )
                got_tail_receipt = true;
            else
                check( false );
        }

        const int cr = rocketnet_client_receipts( client, rcpts, 8 );
        for ( int r = 0; r < cr; ++r )
        {
            check( rcpts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            check( rcpts[r].serial == gen2_client_serial );
            got_client_receipt = true;
        }
    }

    check( delayed_link.dropped_packets == 1 );
    check( client_rx_order == 2 );
    check( server_rx_count == 1 );
    check( got_frag_receipt && got_tail_receipt && got_client_receipt );
    check( rn_server_stats_of( server ).messages_resent > resends_before );
    check( server_msg_obj->hot_connections[0].connection_id == gen2_connection_id );

    g_lifecycle_delayed_link = NULL;
    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( alloc_state.active_allocations == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}
REGISTER_UNIT_TEST( test_integrated_slot_reuse_memory_pressure_disconnect_budget_and_delayed_ack );
#endif // #if ROCKETNET_DEV

#if !ROCKETNET_DEV
// Production keeps the public default and range oracle without compiling the
// caller-stepped lifecycle fixture or its environment hooks.

static void test_config_packets_per_update_range()
{
    static const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x31, 0x43, 0x59, 0x71 };
    rocketnet_quiet( 1 );

    rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    memcpy( client_config.packet_filter, packet_filter, sizeof( client_config.packet_filter ) );
    check( client_config.packets_per_update == 256 );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = LIFECYCLE_PROTOCOL;
    server_config.max_clients = 1;
    memset( server_config.backend_public_key, 0x52, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.packet_filter, packet_filter, sizeof( server_config.packet_filter ) );
    check( server_config.packets_per_update == 256 );

    enum rocketnet_status_t create_status = ROCKETNET_TEST_STATUS_UNSET;
    client_config.packets_per_update = 1024;

    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL && create_status == ROCKETNET_STATUS_OK );
    rn_test_client_stop_and_destroy( client );
    client_config.packets_per_update = 1025;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client == NULL && create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );
    client_config.packets_per_update = 0;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client == NULL && create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );

    server_config.packets_per_update = 1024;
    create_status = ROCKETNET_TEST_STATUS_UNSET;

    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL && create_status == ROCKETNET_STATUS_OK );
    rn_test_server_stop_and_destroy( server );
    server_config.packets_per_update = 1025;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server == NULL && create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );
    server_config.packets_per_update = 0;
    create_status = ROCKETNET_TEST_STATUS_UNSET;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server == NULL && create_status != ROCKETNET_STATUS_OK && create_status != ROCKETNET_TEST_STATUS_UNSET );
}
REGISTER_UNIT_TEST( test_config_packets_per_update_range );
#endif // #if !ROCKETNET_DEV
