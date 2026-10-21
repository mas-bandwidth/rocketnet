/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED (tla/ClientSession.tla
// ServerDisconnect): a client that accepts the server's disconnect packet
// reads it from rocketnet_client_state until its next connect starts a new
// handshake. The server ends the connection three ways
// (rocketnet_server_disconnect_client, rocketnet_server_stop,
// rocketnet_server_destroy); a deliberate client disconnect reads
// DISCONNECTED and a connection timeout CONNECTION_TIMED_OUT. Everything runs in one process over the
// in-memory loopback transport with step-driven clients: no socket, no
// thread, no sleep, and time is an argument starting at 1000.0.

#include "test_unit_registry.h"
#include "sdk/rocketnet_connect_token.h"
#include "sdk/rocketnet_base64.h"
#include "sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "sdk/rocketnet_message_object.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check
#define SERVER_DISCONNECTED_PROTOCOL 0x5345525644495301ULL

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
        seed[i] = (uint8_t) ( 0x5A ^ i );
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
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( server != NULL );
    rocketnet_server_public_key( server, identity->server_public );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
    return server;
}

static rocketnet_client_t * server_disconnected_client_create()
{
    rocketnet_client_test_config_t config;
    rocketnet_client_default_config( &config );
    config.transport = "loopback";
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
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
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
        server_disconnected_pump( server, client, time );
    check( rocketnet_server_client_connected( server, index ) );
    // let the server's keep-alives for the handshake reach the client
    for ( int i = 0; i < 4; i++ )
        server_disconnected_pump( server, client, time );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    return index;
}

enum server_disconnected_how_t
{
    SERVER_DISCONNECTED_BY_DISCONNECT_CLIENT,
    SERVER_DISCONNECTED_BY_STOP,
    SERVER_DISCONNECTED_BY_DESTROY
};

// DONE-WHEN (3): for each way the server ends the connection, one client
// update after it the client state reads SERVER_DISCONNECTED; the state is
// kept through further updates until the next connect starts a handshake.
static void test_server_disconnect_sets_server_disconnected_reason()
{
    rocketnet_quiet( 1 );
    server_disconnected_identity_t identity;
    server_disconnected_identity( &identity );

    const server_disconnected_how_t hows[] = { SERVER_DISCONNECTED_BY_DISCONNECT_CLIENT, SERVER_DISCONNECTED_BY_STOP, SERVER_DISCONNECTED_BY_DESTROY };
    for ( int h = 0; h < 3; h++ )
    {
        const server_disconnected_how_t how = hows[h];
        double time = 1000.0;
        server_disconnected_now = time;
        rocketnet_server_t * server = server_disconnected_server_create( &identity );
        rocketnet_client_t * client = server_disconnected_client_create();
        const int index = server_disconnected_connect( &identity, server, client, 0x5D00ULL + (uint64_t) h, 300,
                                                       ROCKETNET_CLIENT_STATE_DISCONNECTED, &time );

        if ( how == SERVER_DISCONNECTED_BY_DISCONNECT_CLIENT )
        {
            rocketnet_server_disconnect_client( server, index );
            rocketnet_server_send_packets( server );
        }
        else if ( how == SERVER_DISCONNECTED_BY_STOP )
        {
            rocketnet_server_stop( server );
        }
        else
        {
            rocketnet_server_destroy( server );
            server = NULL;
        }

        // within one client update
        rocketnet_client_update( client, time );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

        // kept through further updates
        for ( int i = 0; i < 10; i++ )
            server_disconnected_pump( server, client, &time );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );

        // kept until the next connect, which ends it and starts a handshake
        if ( how == SERVER_DISCONNECTED_BY_STOP )
            check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );
        else if ( how == SERVER_DISCONNECTED_BY_DESTROY )
            server = server_disconnected_server_create( &identity );
        server_disconnected_connect( &identity, server, client, 0x5D10ULL + (uint64_t) h, 300,
                                     ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED, &time );

        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
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
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        for ( int i = 0; i < 10; i++ )
            server_disconnected_pump( server, client, &time );
        check( !rocketnet_server_client_connected( server, index ) );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
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
        rocketnet_client_destroy( client );
        rocketnet_server_destroy( server );
    }
}

REGISTER_UNIT_TEST( test_client_disconnect_and_timeout_read_none );

// A client message object over a link that is not a client transport (the
// unit suites' in-memory links, the functional channel harness): the link
// leaves end_reason NULL (sdk/rocketnet_message_object.h:80), and the
// terminal check in rn_message_object_update
// (sdk/rocketnet_message_server.cpp:471) calls end_reason only when it is
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
static int server_disconnected_fake_end_reason( void * transport )
{
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
    check( object->connections[0].state == RN_MESSAGE_CONNECTION_CONNECTED );
    check( object->connections[0].disconnect_reason == ROCKETNET_DISCONNECT_REASON_NONE );

    // the link's transport ends: the terminal check resets the connection
    fake.connected = 0;
    time += 0.01;
    check( rocketnet_message_client_update( client, time ) == ROCKETNET_UPDATE_OK );
    check( object->connections[0].state != RN_MESSAGE_CONNECTION_CONNECTED );
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

// A server disconnect ends the slot in every query and send at once, before
// the next update: rocketnet_server_disconnect_client and
// rocketnet_server_disconnect_all_clients go through the message layer's kick
// (tla/RNSlots.tla PublicDisconnect), as the client's disconnect does. The
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
                                                               ROCKETNET_CLIENT_STATE_DISCONNECTED, &time ) : index;
    check( rocketnet_server_num_connected_clients( server ) == ( all ? 2 : 1 ) );

    // an in-place start, open when the disconnect comes
    rn_message_object_t * object = &rocketnet_server_message_server( server )->object;
    check( rocketnet_server_start_message( server, index, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 ) != NULL );
    check( object->in_place == 1 );
    check( object->pending_connection == (int) index );
    const uint64_t drops = rn_server_stats_of( server ).messages_dropped_not_connected;

    if ( all )
        rocketnet_server_disconnect_all_clients( server );
    else
        rocketnet_server_disconnect_client( server, index );

    // before the next update
    server_disconnect_now_failures = 0;
    server_disconnect_now_check( rocketnet_server_num_connected_clients( server ) == 0 );
    server_disconnect_now_check( rocketnet_server_client_id( server, index ) == 0 );
    server_disconnect_now_check( !rocketnet_server_client_connected( server, index ) );
    server_disconnect_now_check( !rocketnet_server_client_connected( server, other_index ) );
    server_disconnect_now_check( object->in_place == 0 );
    server_disconnect_now_check( object->pending_connection == -1 );
    server_disconnect_now_check( rocketnet_server_finish_message( server, 16 ) == 0 );
    server_disconnect_now_check( rocketnet_server_start_message( server, index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 16 ) == NULL );
    server_disconnect_now_check( rn_server_stats_of( server ).messages_dropped_not_connected == drops + 1 );
    server_disconnect_now_check( rocketnet_server_start_message( server, other_index, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 16 ) == NULL );
    server_disconnect_now_check( rn_server_stats_of( server ).messages_dropped_not_connected == drops + 2 );
    check( server_disconnect_now_failures == 0 );

    // the disconnect reaches the client as SERVER_DISCONNECTED, and the
    // server's reason for the slot reads NONE across the next updates
    rocketnet_server_send_packets( server );
    rocketnet_client_update( client, time );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    if ( all )
    {
        rocketnet_client_update( other, time );
        check( rocketnet_client_state( other ) == ROCKETNET_CLIENT_STATE_SERVER_DISCONNECTED );
    }
    for ( int i = 0; i < 4; i++ )
        server_disconnected_pump( server, client, &time );
    check( !rocketnet_server_client_connected( server, index ) );
    check( !rocketnet_server_client_connected( server, other_index ) );
    check( rocketnet_server_num_connected_clients( server ) == 0 );
    check( rocketnet_server_disconnect_reason( server, index ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_server_disconnect_reason( server, other_index ) == ROCKETNET_DISCONNECT_REASON_NONE );

    if ( other )
        rocketnet_client_destroy( other );
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
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
