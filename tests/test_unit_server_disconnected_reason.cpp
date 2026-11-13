/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED (proofs/tla/ClientSession.tla
// ServerDisconnect): a client that accepts the server's disconnect packet
// reads it from rocketnet_client_state until its next connect starts a new
// handshake. The server ends the connection three ways
// (rocketnet_server_disconnect_client, rocketnet_server_stop,
// rocketnet_server_destroy); a deliberate client disconnect reads
// DISCONNECTED and a connection timeout CONNECTION_TIMED_OUT. On the server,
// rocketnet_server_disconnect_reason reads SERVER_DISCONNECTED, TIMED_OUT or
// CLIENT_DISCONNECTED for those ends. Everything runs in one process over the
// in-memory loopback transport with step-driven clients: no socket, no
// thread, no sleep, and time is an argument starting at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "../sdk/rocketnet_message_object.h"

#include <stdio.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check
#define SERVER_DISCONNECTED_PROTOCOL 0x5345525644495301ULL

#if ROCKETNET_DEV
struct server_disconnected_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static double server_disconnected_now = 1000.0;
static double server_disconnected_clock( void * ) { return server_disconnected_now; }

static void server_disconnected_identity( server_disconnected_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x5A ^ i );
    }

    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static rocketnet_server_t * server_disconnected_server_create( server_disconnected_identity_t * identity )
{
    rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = SERVER_DISCONNECTED_PROTOCOL;
    config.max_clients = 2;
    memcpy( config.backend_public_key, identity->backend_public, sizeof( config.backend_public_key ) );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( server != NULL );

    rocketnet_server_public_key( server, identity->server_public );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    return server;
}

static rocketnet_client_t * server_disconnected_client_create()
{
    rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
    config.loopback_hub = rocketnet_test_loopback_hub();
    config.step_driven = true;
    config.time_callback = server_disconnected_clock;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, NULL );
    check( client != NULL );
    return client;
}

static void server_disconnected_token( const server_disconnected_identity_t * identity, uint16_t port, uint64_t client_id,
                                       int32_t timeout_seconds, char * token_string )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
                                                     SERVER_DISCONNECTED_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, timeout_seconds, NULL,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

// One step: the client, then the server (when there is one).
static void server_disconnected_pump( rocketnet_server_t * server, rocketnet_client_t * client, double * time )
{
    *time += 0.01;
    server_disconnected_now = *time;
    rocketnet_client_update( client, *time );
    rocketnet_client_send_packets( client );

    if ( server && rocketnet_server_running( server ) )
    {
        rocketnet_server_update( server, *time );
        rocketnet_server_send_packets( server );
    }
}

// Connects the client to the server and returns its slot. Before the
// connect, the state the previous connection ended in still reads
// (`state_before`); connect ends it and starts a new handshake, whose states
// are positive until CONNECTED.
static int server_disconnected_connect( const server_disconnected_identity_t * identity, rocketnet_server_t * server,
                                        rocketnet_client_t * client, uint64_t client_id, int32_t timeout_seconds,
                                        rocketnet_client_state_t state_before, double * time )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    server_disconnected_token( identity, rocketnet_server_port( server ), client_id, timeout_seconds, token );

    check( rocketnet_client_state( client ) == state_before );
    check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
    check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        server_disconnected_pump( server, client, time );
        check( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int index = rocketnet_client_index( client );
    check( index >= 0 );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( server, index ); i++ )
    {
        server_disconnected_pump( server, client, time );
    }

    check( rocketnet_server_client_connected( server, index ) );

    // let the server's keep-alives for the handshake reach the client

    for ( int i = 0; i < 4; i++ )
    {
        server_disconnected_pump( server, client, time );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    return index;
}

// Interleaved destinations own distinct open frames, including when stop
// interrupts one pending reservation after both have committed records.
static void test_unreliable_packing_interleaved_clients_and_stop()
{
    rocketnet_quiet( 1 );

    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    double time = 1000.0;
    server_disconnected_now = time;

    rocketnet_server_t * server = server_disconnected_server_create( &identity );
    rocketnet_client_t * clients[2] = { server_disconnected_client_create(), server_disconnected_client_create() };
    int slots[2];
    for ( int c = 0; c < 2; ++c )
    {
        slots[c] = server_disconnected_connect( &identity, server, clients[c], 0x6A00 + (uint64_t) c, 300,
                                                ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );
    }
    for ( int i = 0; i < 4; ++i )
    {
        for ( int c = 0; c < 2; ++c )
        {
            server_disconnected_pump( server, clients[c], &time );
        }
    }
    rn_message_object_t * object = &rocketnet_server_message_server( server )->object;
    for ( int i = 0; i < 32; ++i )
    {
        for ( int c = 0; c < 2; ++c )
        {
            uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slots[c], 0, ROCKETNET_RELIABILITY_UNRELIABLE, 2 );
            check( p );

            p[0] = (uint8_t) c;
            p[1] = (uint8_t) i;

            check( rocketnet_server_finish_message( server, 2 ) );
        }
    }

    const auto & a = object->connections[slots[0]].unreliable_build;
    const auto & b = object->connections[slots[1]].unreliable_build;
    check( a.writer.records == 32 && b.writer.records == 32 );
    check( a.context.frame && b.context.frame && a.context.frame != b.context.frame );

    rocketnet_server_send_packets( server );

    uint32_t seen[2] = {};
    for ( int i = 0; i < 10; ++i )
    {
        for ( int c = 0; c < 2; ++c )
        {
            server_disconnected_pump( server, clients[c], &time );

            const rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_client_receive_messages( clients[c] ) ) != NULL )
            {
                for ( int j = 0; j < batch->num_messages; ++j )
                {
                    check( batch->bytes[j] == 2 && batch->data[j][0] == c && batch->data[j][1] < 32 );

                    const uint32_t bit = 1u << batch->data[j][1];
                    check( !( seen[c] & bit ) );

                    seen[c] |= bit;
                }
            }
        }
    }

    check( seen[0] == UINT32_MAX && seen[1] == UINT32_MAX );

    for ( int c = 0; c < 2; ++c )
    {
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slots[c], 0, ROCKETNET_RELIABILITY_UNRELIABLE, 2 );
        check( p );

        memset( p, 0, 2 );

        check( rocketnet_server_finish_message( server, 2 ) );
    }

    check( rocketnet_server_start_message( server, (uint64_t) slots[0], 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8 ) );

    rocketnet_server_stop( server );

    check( rn_message_open_count( object ) == 0 && object->connections[slots[0]].open.in_place == 0 );

    for ( int c = 0; c < 2; ++c )
    {
        check( object->connections[slots[c]].unreliable_build.writer.payload == NULL );
    }

    for ( int c = 0; c < 2; ++c )
    {
        rn_test_client_stop_and_destroy( clients[c] );
    }

    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_unreliable_packing_interleaved_clients_and_stop );

enum server_disconnected_how_t
{
    SERVER_DISCONNECTED_BY_DISCONNECT_CLIENT,
    SERVER_DISCONNECTED_BY_STOP,
    SERVER_DISCONNECTED_BY_STOP_THEN_DESTROY
};

// DONE-WHEN (3): for each way the server ends the connection, one client
// update after it the client state reads SERVER_DISCONNECTED; the state is
// kept through further updates until the next connect starts a handshake.
static void test_server_disconnect_sets_server_disconnected_reason()
{
    rocketnet_quiet( 1 );

    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    const server_disconnected_how_t hows[] = { SERVER_DISCONNECTED_BY_DISCONNECT_CLIENT, SERVER_DISCONNECTED_BY_STOP, SERVER_DISCONNECTED_BY_STOP_THEN_DESTROY };

    for ( int h = 0; h < 3; h++ )
    {
        const server_disconnected_how_t how = hows[h];
        double time = 1000.0;
        server_disconnected_now = time;

        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create();
        const int index = server_disconnected_connect( &identity, server, client, 0x5D00 + (uint64_t) h, 300,
                                                       ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );

        if ( how == SERVER_DISCONNECTED_BY_DISCONNECT_CLIENT )
        {
            rocketnet_server_disconnect_client( server, index );
            rocketnet_server_send_packets( server );
        }
        else if ( how == SERVER_DISCONNECTED_BY_STOP )
        {
            rocketnet_server_stop( server );
            rn_test_server_wait_stopped( server );
        }
        else
        {
            rn_test_server_stop_and_destroy( server );
            server = NULL;
        }

        // within one client update

        rocketnet_client_update( client, time );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

        // kept through further updates

        for ( int i = 0; i < 10; i++ )
        {
            server_disconnected_pump( server, client, &time );
        }

        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

        // kept until the next connect, which ends it and starts a handshake

        if ( how == SERVER_DISCONNECTED_BY_STOP )
        {
            check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
        }
        else if ( how == SERVER_DISCONNECTED_BY_STOP_THEN_DESTROY )
        {
            server = server_disconnected_server_create( &identity );
        }
        server_disconnected_connect( &identity, server, client, 0x5D10 + (uint64_t) h, 300,
                                     ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED, &time );

        rn_test_client_stop_and_destroy( client );
        rn_test_server_stop_and_destroy( server );
    }
}

REGISTER_UNIT_TEST( test_server_disconnect_sets_server_disconnected_reason );

// DONE-WHEN (4): a deliberate client disconnect reads DISCONNECTED, never
// SERVER_DISCONNECTED, and a connection timeout reads CONNECTION_TIMED_OUT.
static void test_client_disconnect_and_timeout_read_none()
{
    rocketnet_quiet( 1 );

    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    // deliberate disconnect
    {
        double time = 1000.0;
        server_disconnected_now = time;

        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create();
        const int index = server_disconnected_connect( &identity, server, client, 0x5E01ULL, 300,
                                                       ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );
        rocketnet_client_disconnect( client );
        rn_test_client_wait_disconnected( client );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );

        for ( int i = 0; i < 10; i++ )
        {
            server_disconnected_pump( server, client, &time );
        }

        check( !rocketnet_server_client_connected( server, index ) );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );

        rn_test_client_stop_and_destroy( client );
        rn_test_server_stop_and_destroy( server );
    }

    // connection timeout: the server stops being updated, so it sends
    // nothing, and the client's clock runs past the token's 2 second timeout
    {
        double time = 1000.0;
        server_disconnected_now = time;

        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create();
        server_disconnected_connect( &identity, server, client, 0x5E02ULL, 2,
                                     ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );
        for ( int i = 0; i < 100 && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        {
            time += 0.1;
            server_disconnected_now = time;
            rocketnet_client_update( client, time );
            rocketnet_client_send_packets( client );
        }

        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT );

        rn_test_client_stop_and_destroy( client );
        rn_test_server_stop_and_destroy( server );
    }
}

REGISTER_UNIT_TEST( test_client_disconnect_and_timeout_read_none );

// The server's side of the same ends (proofs/tla/RNSlots.tla Disconnect, Observe;
// proofs/tla/ServerSession.tla SlotTimeout, ClientLeaves): a slot the transport
// ends reads the reason its ending path named, TIMED_OUT when no packet
// arrived within the connection timeout and CLIENT_DISCONNECTED when the
// client's disconnect packet arrived, kept across updates until the slot's
// next connection, which reads NONE.

static void server_disconnected_reason_kept( rocketnet_server_t * server, rocketnet_client_t * client, int index,
                                             enum rocketnet_disconnect_reason_t reason, double * time )
{
    check( !rocketnet_server_client_connected( server, index ) );
    check( rocketnet_server_disconnect_reason( server, index ) == reason );
    for ( int i = 0; i < 4; i++ )
    {
        server_disconnected_pump( server, client, time );
    }
    check( rocketnet_server_disconnect_reason( server, index ) == reason );
}

static void test_server_reason_names_timeout_and_client_disconnect()
{
    rocketnet_quiet( 1 );

    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    double time = 1000.0;
    server_disconnected_now = time;

    rocketnet_server_t * server = server_disconnected_server_create( &identity );
    rocketnet_client_t * silent = server_disconnected_client_create();
    const int index = server_disconnected_connect( &identity, server, silent, 0x5E10ULL, 2,
                                                   ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );
    check( rocketnet_server_disconnect_reason( server, index ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // the client goes silent: only the server runs, past the token's 2 second timeout

    const uint64_t * counters = rocketnet_server_transport_counters( rocketnet_message_server_transport( rocketnet_server_message_server( server ) ) )->values;
    const uint64_t timed_out_before = counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT];
    for ( int i = 0; i < 100 && rocketnet_server_client_connected( server, index ); i++ )
    {
        time += 0.1;
        server_disconnected_now = time;
        rocketnet_server_update( server, time );
        rocketnet_server_send_packets( server );
    }

    check( counters[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] == timed_out_before + 1 );
    rn_test_client_stop_and_destroy( silent );

    rocketnet_client_t * client = server_disconnected_client_create();
    server_disconnected_reason_kept( server, client, index, ROCKETNET_DISCONNECT_REASON_TIMED_OUT, &time );

    // the slot's next connection reads NONE

    const int reused = server_disconnected_connect( &identity, server, client, 0x5E11ULL, 300,
                                                    ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );
    check( reused == index );
    check( rocketnet_server_disconnect_reason( server, reused ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // the client says goodbye: its disconnect packet ends the slot

    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );
    for ( int i = 0; i < 10 && rocketnet_server_client_connected( server, reused ); i++ )
    {
        server_disconnected_pump( server, client, &time );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    server_disconnected_reason_kept( server, client, reused, ROCKETNET_DISCONNECT_REASON_CLIENT_DISCONNECTED, &time );

    // and its next connection reads NONE again

    const int again = server_disconnected_connect( &identity, server, client, 0x5E12ULL, 300,
                                                   ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );
    check( again == index );
    check( rocketnet_server_disconnect_reason( server, again ) == ROCKETNET_DISCONNECT_REASON_NONE );

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_server_reason_names_timeout_and_client_disconnect );

// A slot's next connection inherits nothing from the connection that timed
// out in it (proofs/tla/RNSlots.tla Connect: the new connection has not
// ended; standard/MESSAGES.md m1.4 rule 5: it takes the next id from the
// server-wide counter). The transport's own end reason reads NONE while the
// slot's connection is up (rocketnet_server_transport_client_end_reason in
// sdk/rocketnet_layers.h), as the public reason does; the client id and user
// data are the new token's; the per-connection stats, conditions, receipts
// and send budget read what a first connection reads at the same point of its
// handshake. Both connections are made the same way, so each field of the
// reused slot is compared with the same field of the first connection.

struct server_reused_slot_view_t
{
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    double rtt_ms;
    double jitter_ms;
    double packet_loss_percent;
    uint64_t reliable_in_flight;
    uint64_t send_refused;
    double seconds_since_progress;
    int receipts;
    uint32_t budget_kbps;
    int budget_enabled;
    uint64_t budget_spent;
};

static void server_reused_slot_view( rocketnet_server_t * server, int index, server_reused_slot_view_t * view )
{
    rocketnet_server_client_stats_t stats;
    rocketnet_server_client_stats( server, index, &stats );
    rocketnet_conditions_t conditions;
    check( rocketnet_server_client_conditions( server, index, &conditions, (int) sizeof( conditions ) ) );
    const rn_message_connection_t * connection = &rocketnet_server_message_server( server )->object.connections[index];

    memset( view, 0, sizeof( *view ) );
    view->packets_sent = stats.packets_sent;
    view->packets_received = stats.packets_received;
    view->bytes_sent = stats.bytes_sent;
    view->bytes_received = stats.bytes_received;
    view->rtt_ms = stats.rtt_ms;
    view->jitter_ms = stats.jitter_ms;
    view->packet_loss_percent = stats.packet_loss_percent;
    view->reliable_in_flight = conditions.reliable_in_flight;
    view->send_refused = conditions.send_refused;
    view->seconds_since_progress = conditions.seconds_since_progress;
    view->receipts = rocketnet_server_receipts( server, index, NULL, 0 );
    view->budget_kbps = connection->budget.kbps;
    view->budget_enabled = connection->send_budget_enabled;
    view->budget_spent = connection->budget.bytes_spent;
}

static void server_reused_token( const server_disconnected_identity_t * identity, uint16_t port, uint64_t client_id,
                                 int32_t timeout_seconds, uint8_t user_byte, char * token_string )
{
    uint8_t user_data[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    memset( user_data, user_byte, sizeof( user_data ) );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
                                                     SERVER_DISCONNECTED_PROTOCOL, addresses, 1, client_id, 1000, 1ULL << 40, timeout_seconds, user_data,
                                                     token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

// Connects until the server's update has taken the connection response (the
// slot reads connected), with no further pump: the view taken then is the
// connection's first.
static int server_reused_connect( const server_disconnected_identity_t * identity, rocketnet_server_t * server,
                                  rocketnet_client_t * client, uint64_t client_id, uint8_t user_byte, int32_t timeout_seconds,
                                  double * time )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    server_reused_token( identity, rocketnet_server_port( server ), client_id, timeout_seconds, user_byte, token );
    check( rn_test_client_connect_after_completion( client, token ) == ROCKETNET_STATUS_OK );

    const int connected_before = rocketnet_server_num_connected_clients( server );
    for ( int i = 0; i < 400 && rocketnet_server_num_connected_clients( server ) == connected_before; i++ )
    {
        server_disconnected_pump( server, client, time );
    }
    check( rocketnet_server_num_connected_clients( server ) == connected_before + 1 );

    int index = -1;
    for ( int i = 0; i < rocketnet_server_max_clients( server ); i++ )
    {
        if ( rocketnet_server_client_connected( server, i ) && rocketnet_server_client_id( server, i ) == client_id )
        {
            index = i;
        }
    }
    check( index >= 0 );
    return index;
}

static void server_reused_check_user_data( rocketnet_server_t * server, int index, uint8_t user_byte )
{
    uint8_t user_data[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );
    check( rocketnet_server_client_user_data( server, index, user_data ) );
    for ( int i = 0; i < ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES; i++ )
    {
        check( user_data[i] == user_byte );
    }
}

static void test_server_reused_slot_inherits_nothing_from_a_timeout()
{
    rocketnet_quiet( 1 );

    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    double time = 1000.0;
    server_disconnected_now = time;

    rocketnet_server_t * server = server_disconnected_server_create( &identity );
    rocketnet_server_transport_t * transport = rocketnet_message_server_transport( rocketnet_server_message_server( server ) );

    // the first connection, and the view a connection has at its start

    rocketnet_client_t * first = server_disconnected_client_create();
    const int index = server_reused_connect( &identity, server, first, 0x5E20ULL, 0xA1, 2, &time );
    const uint64_t first_id = rocketnet_server_client_connection_id( server, index );
    check( first_id != 0 );
    check( rocketnet_server_disconnect_reason( server, index ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_server_transport_client_end_reason( transport, index ) == ROCKETNET_DISCONNECT_REASON_NONE );
    server_reused_check_user_data( server, index, 0xA1 );

    server_reused_slot_view_t fresh;
    server_reused_slot_view( server, index, &fresh );

    // it lives: messages both ways, receipts, a send rate of its own

    for ( int i = 0; i < 20 && rocketnet_client_state( first ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        server_disconnected_pump( server, first, &time );
    }
    check( rocketnet_client_state( first ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int kbps = (int) fresh.budget_kbps == 1234 ? 1235 : 1234;
    check( rocketnet_server_set_send_kbps( server, index, kbps ) == ROCKETNET_STATUS_OK );
    int receipts = 0;
    for ( int i = 0; i < 40; i++ )
    {
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 8 );
        check( p != NULL );
        memset( p, i, 8 );
        check( rocketnet_server_finish_message( server, 8 ) != 0 );

        p = rocketnet_client_start_message( first, 0, ROCKETNET_RELIABILITY_RELIABLE, 8 );
        check( p != NULL );
        memset( p, i, 8 );
        check( rocketnet_client_finish_message( first, 8 ) != 0 );

        server_disconnected_pump( server, first, &time );
        while ( rocketnet_server_receive_messages( server ) != NULL )
        {
        }
        while ( rocketnet_client_receive_messages( first ) != NULL )
        {
        }
        receipts += rocketnet_server_receipts( server, index, NULL, 0 );
    }
    check( receipts > 0 );

    // then goes silent with reliable messages unacknowledged: only the
    // server runs, past the token's 2 second timeout

    for ( int i = 0; i < 4; i++ )
    {
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 8 );
        check( p != NULL );
        memset( p, 0xEE, 8 );
        check( rocketnet_server_finish_message( server, 8 ) != 0 );
    }

    time += 0.01;
    server_disconnected_now = time;
    rocketnet_server_update( server, time );
    rocketnet_server_send_packets( server );

    server_reused_slot_view_t lived;
    server_reused_slot_view( server, index, &lived );
    check( lived.packets_sent > fresh.packets_sent + 40 );
    check( lived.packets_received > fresh.packets_received + 40 );
    check( lived.bytes_sent > fresh.bytes_sent );
    check( lived.bytes_received > fresh.bytes_received );
    check( lived.rtt_ms > 0.0 );
    check( lived.reliable_in_flight >= 4 );
    check( lived.seconds_since_progress >= 0.0 );
    check( lived.budget_kbps == (uint32_t) kbps );
    check( lived.budget_spent > fresh.budget_spent );

    for ( int i = 0; i < 100 && rocketnet_server_client_connected( server, index ); i++ )
    {
        time += 0.1;
        server_disconnected_now = time;
        rocketnet_server_update( server, time );
        rocketnet_server_send_packets( server );
    }

    check( !rocketnet_server_client_connected( server, index ) );
    check( rocketnet_server_disconnect_reason( server, index ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );
    check( rocketnet_server_transport_client_end_reason( transport, index ) == ROCKETNET_DISCONNECT_REASON_TIMED_OUT );
    check( rocketnet_server_client_connection_id( server, index ) == 0 );
    rn_test_client_stop_and_destroy( first );

    // the next connection takes the same slot, made the same way

    rocketnet_client_t * next = server_disconnected_client_create();
    const int reused = server_reused_connect( &identity, server, next, 0x5E21ULL, 0xB2, 300, &time );
    check( reused == index );

    check( rocketnet_server_transport_client_end_reason( transport, reused ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_server_disconnect_reason( server, reused ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_server_client_connection_id( server, reused ) > first_id );
    check( rocketnet_server_client_id( server, reused ) == 0x5E21ULL );
    server_reused_check_user_data( server, reused, 0xB2 );

    server_reused_slot_view_t again;
    server_reused_slot_view( server, reused, &again );
    check( again.packets_sent == fresh.packets_sent );
    check( again.packets_received == fresh.packets_received );
    check( again.bytes_sent == fresh.bytes_sent );
    check( again.bytes_received == fresh.bytes_received );
    check( again.rtt_ms == fresh.rtt_ms );
    check( again.jitter_ms == fresh.jitter_ms );
    check( again.packet_loss_percent == fresh.packet_loss_percent );
    check( again.reliable_in_flight == fresh.reliable_in_flight );
    check( again.send_refused == fresh.send_refused );
    check( again.seconds_since_progress == fresh.seconds_since_progress );
    check( again.budget_kbps == fresh.budget_kbps );
    check( again.budget_enabled == fresh.budget_enabled );
    check( again.budget_spent == fresh.budget_spent );

    // the first connection's receipts were readable until the slot's next
    // update; none of them is the new connection's

    check( again.receipts == 0 );

    // and the new connection works: one message each way

    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) reused, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 );
    check( p != NULL );
    memset( p, 0x42, 4 );
    check( rocketnet_server_finish_message( server, 4 ) != 0 );
    int delivered = 0;
    for ( int i = 0; i < 20 && delivered == 0; i++ )
    {
        server_disconnected_pump( server, next, &time );
        const rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( next ) ) != NULL )
        {
            for ( int j = 0; j < batch->num_messages; j++ )
            {
                check( batch->bytes[j] == 4 && batch->data[j][0] == 0x42 );
                delivered++;
            }
        }
    }
    check( delivered == 1 );
    check( rocketnet_server_transport_client_end_reason( transport, reused ) == ROCKETNET_DISCONNECT_REASON_NONE );

    rn_test_client_stop_and_destroy( next );
    rn_test_server_stop_and_destroy( server );
}

REGISTER_UNIT_TEST( test_server_reused_slot_inherits_nothing_from_a_timeout );
#endif // #if ROCKETNET_DEV

// A client message object over a link that is not a client transport (the
// unit suites' in-memory links, the functional channel harness): the link
// leaves end_reason NULL (sdk/rocketnet_message_object.h), and the
// terminal check in rn_message_object_update
// (sdk/rocketnet_message_server.cpp) calls end_reason only when it is
// set, so a link whose transport ends reads NONE and nothing reads the
// transport as a client transport. A link that names an end reason has it
// recorded, and the reset keeps it.

struct server_disconnected_fake_link_t
{
    int connected;
    int end_reason;
};

static void server_disconnected_fake_update( void *, double ) {}
static int server_disconnected_fake_payloads( void *, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    *client_index = NULL;
    *payload_data = NULL;
    *payload_bytes = NULL;
    return 0;
}

static int server_disconnected_fake_acked( void *, const int ** client_index, const uint64_t ** packet_ids )
{
    *client_index = NULL;
    *packet_ids = NULL;
    return 0;
}

static uint8_t * server_disconnected_fake_start_packet( void *, int, int, struct rocketnet_packet_context_t * ) { return NULL; }
static int server_disconnected_fake_finish_packet( void *, struct rocketnet_packet_context_t *, int ) { return 0; }
static void server_disconnected_fake_abort_packet( void *, struct rocketnet_packet_context_t * ) {}
static void server_disconnected_fake_send_packets( void * ) {}
static uint64_t server_disconnected_fake_connection( void * transport, int )
{
    return ( (server_disconnected_fake_link_t *) transport )->connected ? 1 : 0;
}

static void server_disconnected_fake_disconnect( void * transport, int )
{
    ( (server_disconnected_fake_link_t *) transport )->connected = 0;
}

static int server_disconnected_fake_state( void * transport )
{
    return ( (server_disconnected_fake_link_t *) transport )->connected ? ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED
                                                                        : ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
}

static int server_disconnected_fake_end_reason( void * transport, int client_index )
{
    (void) client_index;
    return ( (server_disconnected_fake_link_t *) transport )->end_reason;
}

static void server_disconnected_fake_case( int names_end_reason, int expected_reason )
{
    server_disconnected_fake_link_t fake;
    fake.connected = 1;
    fake.end_reason = ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED;

    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = &fake;
    link.update = server_disconnected_fake_update;
    link.payloads = server_disconnected_fake_payloads;
    link.acked = server_disconnected_fake_acked;
    link.start_packet = server_disconnected_fake_start_packet;
    link.finish_packet = server_disconnected_fake_finish_packet;
    link.abort_packet = server_disconnected_fake_abort_packet;
    link.send_packets = server_disconnected_fake_send_packets;
    link.connection = server_disconnected_fake_connection;
    link.disconnect = server_disconnected_fake_disconnect;
    link.state = server_disconnected_fake_state;
    link.end_reason = names_end_reason ? server_disconnected_fake_end_reason : NULL;
    link.destroy = NULL;                                        // the case owns the fake

    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1, NULL, NULL, 0,
                                                             ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 1000.0 );
    check( object );

    rocketnet_message_client_t * client = (rocketnet_message_client_t *) object;
    double time = 1000.0;
    check( rocketnet_message_client_update( client, time ) == ROCKETNET_UPDATE_OK );
    check( object->hot_connections[0].state == RN_MESSAGE_CONNECTION_CONNECTED );
    check( object->connections[0].disconnect_reason == ROCKETNET_DISCONNECT_REASON_NONE );

    // the link's transport ends: the terminal check resets the connection

    fake.connected = 0;
    time += 0.01;
    check( rocketnet_message_client_update( client, time ) == ROCKETNET_UPDATE_OK );
    check( object->hot_connections[0].state != RN_MESSAGE_CONNECTION_CONNECTED );
    check( object->connections[0].disconnect_reason == expected_reason );

    // further updates keep it

    time += 0.01;
    check( rocketnet_message_client_update( client, time ) == ROCKETNET_UPDATE_OK );
    check( object->connections[0].disconnect_reason == expected_reason );

    rn_message_object_destroy( object );
}

static void test_client_link_without_end_reason_reads_none()
{
    server_disconnected_fake_case( 0, ROCKETNET_DISCONNECT_REASON_NONE );
    server_disconnected_fake_case( 1, ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
}

REGISTER_UNIT_TEST( test_client_link_without_end_reason_reads_none );

#if ROCKETNET_DEV
// A server disconnect ends the slot in every query and send at once, before
// the next update: rocketnet_server_disconnect_client and
// rocketnet_server_disconnect_all_clients go through the message layer's kick
// (proofs/tla/RNSlots.tla PublicDisconnect), as the client's disconnect does. The
// slot reads not connected, a start on it is dropped and counted in
// NOT_CONNECTED_DROPS, and an in-place start opened on it before the
// disconnect is cancelled, so its finish returns 0. Each failing line is
// printed before the case fails, so a run shows every one.

static int server_disconnect_now_failures = 0;

#define server_disconnect_now_check( condition )                                                         \
do                                                                                                       \
{                                                                                                        \
    if ( !(condition) )                                                                                  \
    {                                                                                                    \
        printf( "check failed: ( %s ), file %s, line %d\n", #condition, __FILE__, __LINE__ );          \
        server_disconnect_now_failures++;                                                                \
    }                                                                                                    \
} while ( 0 )

static void server_disconnect_now_case( bool all )
{
    rocketnet_quiet( 1 );

    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    double time = 1000.0;
    server_disconnected_now = time;

    rocketnet_server_t * server = server_disconnected_server_create( &identity );
    rocketnet_client_t * client = server_disconnected_client_create();
    rocketnet_client_t * other = all ? server_disconnected_client_create() : NULL;
    const int index = server_disconnected_connect( &identity, server, client, 0x5F00ULL + ( all ? 1 : 0 ), 300,
                                                   ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );
    const int other_index = all ? server_disconnected_connect( &identity, server, other, 0x5F10ULL, 300,
                                                               ROCKETNET_CLIENT_STATE_DISCONNECTED, &time )
                                : index;
    check( rocketnet_server_num_connected_clients( server ) == ( all ? 2 : 1 ) );
    check( index >= 0 && other_index >= 0 );

    // an in-place start, open when the disconnect comes

    rn_message_object_t * object = &rocketnet_server_message_server( server )->object;
    check( rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 ) != NULL );
    check( object->connections[index].open.in_place != 0 );
    check( object->connections[index].open.pending );

    const uint64_t drops = rn_server_stats_of( server ).messages_dropped_not_connected;

    if ( all )
    {
        rocketnet_server_disconnect_all_clients( server );
    }
    else
    {
        rocketnet_server_disconnect_client( server, index );
    }

    // before the next update

    server_disconnect_now_failures = 0;

    server_disconnect_now_check( rocketnet_server_num_connected_clients( server ) == 0 );
    server_disconnect_now_check( rocketnet_server_client_id( server, index ) == 0 );
    server_disconnect_now_check( !rocketnet_server_client_connected( server, index ) );
    server_disconnect_now_check( !rocketnet_server_client_connected( server, other_index ) );
    server_disconnect_now_check( rocketnet_server_disconnect_reason( server, index ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
    server_disconnect_now_check( rocketnet_server_disconnect_reason( server, other_index ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
    server_disconnect_now_check( object->connections[index].open.in_place == 0 );
    server_disconnect_now_check( rn_message_open_count( object ) == 0 );

    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    static int disconnect_now_finish_asserts = 0;
    disconnect_now_finish_asserts = 0;
    rocketnet_assert_function( []( const char *, const char *, const char *, int )
                               { disconnect_now_finish_asserts++; } );

    server_disconnect_now_check( rocketnet_server_finish_message( server, 16 ) == 0 );

    rocketnet_assert_function( saved_assert );

    server_disconnect_now_check( disconnect_now_finish_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );
    server_disconnect_now_check( rocketnet_server_start_message( server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 ) == NULL );
    server_disconnect_now_check( rn_server_stats_of( server ).messages_dropped_not_connected == drops + 1 );
    server_disconnect_now_check( rocketnet_server_start_message( server, (uint64_t) other_index, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 ) == NULL );
    server_disconnect_now_check( rn_server_stats_of( server ).messages_dropped_not_connected == drops + 2 );
    check( server_disconnect_now_failures == 0 );

    // the disconnect reaches the client as SERVER_DISCONNECTED, and the
    // server's reason for the slot reads SERVER_DISCONNECTED across the next
    // updates

    rocketnet_server_send_packets( server );
    rocketnet_client_update( client, time );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

    if ( all )
    {
        rocketnet_client_update( other, time );
        check( rocketnet_client_state( other ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    }

    for ( int i = 0; i < 4; i++ )
    {
        server_disconnected_pump( server, client, &time );
    }
    check( !rocketnet_server_client_connected( server, index ) );
    check( !rocketnet_server_client_connected( server, other_index ) );
    check( rocketnet_server_num_connected_clients( server ) == 0 );
    check( rocketnet_server_disconnect_reason( server, index ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
    check( rocketnet_server_disconnect_reason( server, other_index ) == ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

    if ( other )
    {
        rn_test_client_stop_and_destroy( other );
    }
    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
}

static void test_server_disconnect_client_ends_slot_at_once()
{
    server_disconnect_now_case( false );
}

REGISTER_UNIT_TEST( test_server_disconnect_client_ends_slot_at_once );

static void test_server_disconnect_all_clients_ends_slots_at_once()
{
    server_disconnect_now_case( true );
}

REGISTER_UNIT_TEST( test_server_disconnect_all_clients_ends_slots_at_once );
#endif // #if ROCKETNET_DEV
