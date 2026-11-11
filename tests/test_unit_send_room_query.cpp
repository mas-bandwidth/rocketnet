/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public room query: rocketnet_client_can_send_message and
// rocketnet_server_can_send_message. A sender asks whether a message would
// fit; a start that finds no room is refused and counted, and the connection
// stays up (m1.7). The query says no exactly when the start would be refused,
// room returns as the receiver acknowledges, and asking changes nothing.
// In memory over the loopback transport, step driven, no clock, sockets or
// network; time starts at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "token_string.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include "loopback_hub.h"

#define check rocketnet_unit_check

#define SEND_ROOM_PROTOCOL 0x5E2D500D0E5A11CEULL

static double g_send_room_now = 1000.0;
static double send_room_clock( void * ) { return g_send_room_now; }

static int g_send_room_asserts = 0;
static void send_room_count_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    g_send_room_asserts++;
}

static void send_room_frame( rocketnet_server_t * server, rocketnet_client_t ** clients, int num_clients )
{
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_update( clients[i], g_send_room_now );
        while ( rocketnet_client_receive_messages( clients[i] ) != NULL )
        {
        }

        rocketnet_client_send_packets( clients[i] );
    }

    rocketnet_server_update( server, g_send_room_now );
    while ( rocketnet_server_receive_messages( server ) != NULL )
    {
    }

    rocketnet_server_send_packets( server );
    g_send_room_now += 0.01;
}

static void send_room_stats_unchanged_by_asking_client( rocketnet_client_t * client, int bytes )
{
    struct rocketnet_client_stats_t before;
    struct rocketnet_client_stats_t after;
    memset( &before, 0, sizeof( before ) );
    memset( &after, 0, sizeof( after ) );
    rocketnet_client_stats( client, &before );
    const bool answer = rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    for ( int i = 0; i < 64; i++ )
    {
        check( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == answer );

        (void) rocketnet_client_can_send_message( client, i % 4, ( enum rocketnet_reliability_t )( i % 8 ), i * 37 );
    }

    rocketnet_client_stats( client, &after );
    check( memcmp( &before, &after, sizeof( before ) ) == 0 );
}

static void send_room_stats_unchanged_by_asking_server( rocketnet_server_t * server, int slot, int bytes )
{
    struct rocketnet_server_client_stats_t before;
    struct rocketnet_server_client_stats_t after;
    memset( &before, 0, sizeof( before ) );
    memset( &after, 0, sizeof( after ) );
    rocketnet_server_client_stats( server, slot, &before );
    const bool answer = rocketnet_server_can_send_message( server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    for ( int i = 0; i < 64; i++ )
    {
        check( rocketnet_server_can_send_message( server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == answer );

        (void) rocketnet_server_can_send_message( server, slot, i % 4, ( enum rocketnet_reliability_t )( i % 8 ), i * 37 );
    }

    rocketnet_server_client_stats( server, slot, &after );
    check( memcmp( &before, &after, sizeof( before ) ) == 0 );
}

static void test_send_room_query_public_api_client_and_server()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( send_room_count_assert );
    g_send_room_asserts = 0;
    g_send_room_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x3A ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );

    server_config.protocol_id = SEND_ROOM_PROTOCOL;
    server_config.max_clients = 3;
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    // stopped: no room, and not misuse

    uint64_t misuses = rocketnet_api_misuse_count();
    check( !rocketnet_server_can_send_message( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 ) );
    check( rocketnet_api_misuse_count() == misuses );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    // client 0 fills its own send memory, client 1's slot is where the server fills its

    rocketnet_client_t * clients[2];
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = 1;
        client_config.time_callback = send_room_clock;
        create_status = ROCKETNET_STATUS_OK;
        clients[i] = rocketnet_client_create( NULL, &client_config, &create_status );
        check( clients[i] != NULL );
        check( create_status == ROCKETNET_STATUS_OK );

        // before connect: nothing to send on, so no room, and not misuse

        misuses = rocketnet_api_misuse_count();

        check( !rocketnet_client_can_send_message( clients[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 ) );
        check( rocketnet_api_misuse_count() == misuses );

        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        check( rn_test_server_token_string( backend_secret, server, SEND_ROOM_PROTOCOL, addresses, 1,
                                            (uint64_t) ( 0x51 + i ), 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
        check( rocketnet_client_connect( clients[i], token ) == ROCKETNET_STATUS_OK );

        // connecting: dropped like the start, so no room

        check( !rocketnet_client_can_send_message( clients[i], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 100 ) );
    }

    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_num_connected_clients( server ) == 2 )
        {
            break;
        }
        send_room_frame( server, clients, 2 );
    }

    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int server_slot = rocketnet_client_index( clients[1] );
    const int other_slot = rocketnet_client_index( clients[0] );
    check( server_slot >= 0 && server_slot < 3 );

    int empty_slot = 0;
    while ( empty_slot == server_slot || empty_slot == other_slot )
    {
        empty_slot++;
    }

    const int bytes = 1000;
    check( rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( rocketnet_server_can_send_message( server, server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );

    // a slot with no connected client: no room, not misuse; an index outside
    // 0 .. max_clients - 1: misuse, no room

    misuses = rocketnet_api_misuse_count();

    check( !rocketnet_server_can_send_message( server, empty_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( rocketnet_api_misuse_count() == misuses );
    check( !rocketnet_server_can_send_message( server, 3, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( !rocketnet_server_can_send_message( server, -1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( rocketnet_api_misuse_count() == misuses + 2 );

    // a refused argument: no room, as the start would refuse it

    check( !rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0 ) );
    check( !rocketnet_client_can_send_message( clients[0], ROCKETNET_MAX_CHANNELS, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    const int invalid_reliabilities[] = { INT_MIN, -1, 8, 99, INT_MAX };
    for ( size_t i = 0; i < sizeof( invalid_reliabilities ) / sizeof( invalid_reliabilities[0] ); i++ )
    {
        const enum rocketnet_reliability_t invalid_rel = (enum rocketnet_reliability_t) invalid_reliabilities[i];
        check( !rocketnet_client_can_send_message( clients[0], 0, invalid_rel, bytes ) );
        check( !rocketnet_server_can_send_message( server, server_slot, 0, invalid_rel, bytes ) );
    }
    check( rocketnet_api_misuse_count() == misuses + 2 );

    // an open start: the next start would be refused, so no room until finish

    uint8_t * open = rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    check( open != NULL );
    check( !rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );

    rocketnet_client_abort_message( clients[0] );
    check( rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );

    // the client queues without updating, asking first: every yes is a start
    // that is given room, until the first no

    int client_sent = 0;
    for ( ;; )
    {
        send_room_stats_unchanged_by_asking_client( clients[0], bytes );
        if ( !rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
        {
            break;
        }
        uint8_t * p = rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );

        memset( p, (int) ( client_sent & 0xFF ), (size_t) bytes );
        check( rocketnet_client_finish_message( clients[0], bytes ) != 0 );

        client_sent++;
        check( client_sent < 1000000 );
    }

    check( client_sent > 0 );

    // asking again and again ends nothing: still CONNECTED, still no room

    send_room_stats_unchanged_by_asking_client( clients[0], bytes );
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    // the server does the same to client 1's slot

    int server_sent = 0;
    for ( ;; )
    {
        send_room_stats_unchanged_by_asking_server( server, server_slot, bytes );
        if ( !rocketnet_server_can_send_message( server, server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
        {
            break;
        }
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );

        memset( p, (int) ( server_sent & 0xFF ), (size_t) bytes );
        check( rocketnet_server_finish_message( server, bytes ) != 0 );

        server_sent++;
        check( server_sent < 1000000 );
    }

    check( server_sent > 0 );

    send_room_stats_unchanged_by_asking_server( server, server_slot, bytes );
    check( rocketnet_server_client_connected( server, server_slot ) );

    // the other slot is its own memory: the server can still send to it

    check( rocketnet_server_can_send_message( server, other_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );

    // a send after the no is refused (m1.7): start returns NULL, the refusal
    // is counted on that connection only, its stats are as they were, and the
    // update ends nothing

    struct rocketnet_client_stats_t client_stats_before;
    struct rocketnet_client_stats_t client_stats_after;
    memset( &client_stats_before, 0, sizeof( client_stats_before ) );
    memset( &client_stats_after, 0, sizeof( client_stats_after ) );
    rocketnet_client_stats( clients[0], &client_stats_before );
    struct rocketnet_server_client_stats_t slot_stats_before;
    struct rocketnet_server_client_stats_t slot_stats_after;
    memset( &slot_stats_before, 0, sizeof( slot_stats_before ) );
    memset( &slot_stats_after, 0, sizeof( slot_stats_after ) );
    rocketnet_server_client_stats( server, server_slot, &slot_stats_before );

    check( rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == NULL );
    check( rocketnet_server_start_message( server, (uint64_t) server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == NULL );

    rocketnet_client_stats( clients[0], &client_stats_after );
    check( memcmp( &client_stats_before, &client_stats_after, sizeof( client_stats_before ) ) == 0 );
    rocketnet_server_client_stats( server, server_slot, &slot_stats_after );
    check( memcmp( &slot_stats_before, &slot_stats_after, sizeof( slot_stats_before ) ) == 0 );

    struct rocketnet_conditions_t conditions;
    check( rocketnet_client_conditions( clients[0], &conditions, (int) sizeof( conditions ) ) );
    check( conditions.send_refused == 1 );
    check( rocketnet_client_conditions( clients[1], &conditions, (int) sizeof( conditions ) ) );
    check( conditions.send_refused == 0 );
    check( rocketnet_server_client_conditions( server, server_slot, &conditions, (int) sizeof( conditions ) ) );
    check( conditions.send_refused == 1 );
    check( rocketnet_server_client_conditions( server, other_slot, &conditions, (int) sizeof( conditions ) ) );
    check( conditions.send_refused == 0 );

    rocketnet_client_update( clients[0], g_send_room_now );
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    rocketnet_server_update( server, g_send_room_now );
    check( rocketnet_server_disconnect_reason( server, server_slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rocketnet_server_client_connected( server, server_slot ) );

    while ( rocketnet_server_receive_messages( server ) != NULL )
    {
    }
    rocketnet_server_send_packets( server );
    rocketnet_client_send_packets( clients[0] );
    g_send_room_now += 0.01;

    // room returns as the receivers acknowledge: the query's yes is again a start's pointer

    for ( int i = 0; i < 2000 && ( !rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) ||
                                   !rocketnet_server_can_send_message( server, server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
          i++ )
    {
        send_room_frame( server, clients, 2 );
        check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( server, server_slot ) );
    }

    uint8_t * again = rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    check( again != NULL );
    memset( again, 0x5A, (size_t) bytes );
    check( rocketnet_client_finish_message( clients[0], bytes ) != 0 );
    again = rocketnet_server_start_message( server, (uint64_t) server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    check( again != NULL );
    memset( again, 0x5A, (size_t) bytes );
    check( rocketnet_server_finish_message( server, bytes ) != 0 );
    check( rocketnet_client_conditions( clients[0], &conditions, (int) sizeof( conditions ) ) );
    check( conditions.send_refused == 1 );
    check( rocketnet_server_client_conditions( server, server_slot, &conditions, (int) sizeof( conditions ) ) );
    check( conditions.send_refused == 1 );

    // a gone connection has no room

    rocketnet_client_disconnect( clients[0] );
    check( !rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    rocketnet_server_disconnect_client( server, server_slot );
    rocketnet_server_update( server, g_send_room_now );
    check( !rocketnet_server_client_connected( server, server_slot ) );
    check( !rocketnet_server_can_send_message( server, server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( g_send_room_asserts == 0 );

    for ( int i = 0; i < 2; i++ )
    {
        rn_test_client_stop_and_destroy( clients[i] );
    }

    rn_test_server_stop_and_destroy( server );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_send_room_query_public_api_client_and_server );

// rocketnet_client_conditions and rocketnet_server_client_conditions: false and
// zeroed until connected (the client in states 1-3 included), true once
// connected; send_refused counts each refused start on its own connection
// only; a short conditions_bytes writes only the fields that fit. Loopback,
// step driven, time starts at 1000.0.
static void test_conditions_public_api_client_and_server()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( send_room_count_assert );
    g_send_room_asserts = 0;
    g_send_room_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x4B ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = SEND_ROOM_PROTOCOL;
    server_config.max_clients = 3;
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    struct rocketnet_conditions_t c;
    rocketnet_client_t * clients[2];
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = 1;
        client_config.time_callback = send_room_clock;
        clients[i] = rocketnet_client_create( NULL, &client_config, &create_status );
        check( clients[i] != NULL );

        // disconnected: false and zeroed

        memset( &c, 0xA5, sizeof( c ) );
        check( !rocketnet_client_conditions( clients[i], &c, (int) sizeof( c ) ) );
        check( c.seconds_since_heard == 0.0 && c.reliable_in_flight == 0 && c.send_refused == 0 );

        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        check( rn_test_server_token_string( backend_secret, server, SEND_ROOM_PROTOCOL, addresses, 1,
                                            (uint64_t) ( 0x61 + i ), 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
        check( rocketnet_client_connect( clients[i], token ) == ROCKETNET_STATUS_OK );
    }

    // connecting (states 1-3): false on every frame until CONNECTED

    int connecting_frames = 0;
    for ( int i = 0; i < 400; i++ )
    {
        for ( int k = 0; k < 2; k++ )
        {
            const int state = rocketnet_client_state( clients[k] );
            if ( state > 0 && state < ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                memset( &c, 0xA5, sizeof( c ) );
                check( !rocketnet_client_conditions( clients[k], &c, (int) sizeof( c ) ) );
                check( c.send_refused == 0 );
                connecting_frames++;
            }
        }
        if ( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_num_connected_clients( server ) == 2 )
        {
            break;
        }
        send_room_frame( server, clients, 2 );
    }
    check( connecting_frames > 0 );
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    for ( int i = 0; i < 20; i++ )
    {
        send_room_frame( server, clients, 2 );
    }

    const int slot0 = rocketnet_client_index( clients[0] );
    const int slot1 = rocketnet_client_index( clients[1] );
    int empty_slot = 0;
    while ( empty_slot == slot0 || empty_slot == slot1 )
    {
        empty_slot++;
    }

    // connected: true on both sides, per client; heard within the last frames

    for ( int k = 0; k < 2; k++ )
    {
        check( rocketnet_client_conditions( clients[k], &c, (int) sizeof( c ) ) );
        check( c.seconds_since_heard >= 0.0 && c.seconds_since_heard < 1.0 );
        check( c.send_refused == 0 );
        check( c.reliable_in_flight == 0 );
        check( c.seconds_since_progress == -1.0 );
    }
    const int slots[2] = { slot0, slot1 };
    for ( int k = 0; k < 2; k++ )
    {
        check( rocketnet_server_client_conditions( server, slots[k], &c, (int) sizeof( c ) ) );
        check( c.seconds_since_heard >= 0.0 && c.seconds_since_heard < 1.0 );
        check( c.send_refused == 0 );
    }

    // an empty slot and an index out of range: false and zeroed

    memset( &c, 0xA5, sizeof( c ) );
    check( !rocketnet_server_client_conditions( server, empty_slot, &c, (int) sizeof( c ) ) );
    check( c.send_refused == 0 );
    check( !rocketnet_server_client_conditions( server, 3, &c, (int) sizeof( c ) ) );
    check( !rocketnet_server_client_conditions( server, -1, &c, (int) sizeof( c ) ) );

    // the client fills its send side without updating: the first refused start
    // counts once on that client and nowhere else, and the connection stays

    const int bytes = 1000;
    int sent = 0;
    while ( rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
    {
        uint8_t * p = rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );
        memset( p, 0x11, (size_t) bytes );
        check( rocketnet_client_finish_message( clients[0], bytes ) != 0 );
        sent++;
        check( sent < 1000000 );
    }
    check( rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == NULL );
    check( rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == NULL );
    check( rocketnet_client_conditions( clients[0], &c, (int) sizeof( c ) ) );
    check( c.send_refused == 2 );
    check( rocketnet_client_conditions( clients[1], &c, (int) sizeof( c ) ) );
    check( c.send_refused == 0 );

    // the server fills slot1 only: refusals count on that client only

    sent = 0;
    while ( rocketnet_server_can_send_message( server, slot1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
    {
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slot1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );
        memset( p, 0x22, (size_t) bytes );
        check( rocketnet_server_finish_message( server, bytes ) != 0 );
        sent++;
        check( sent < 1000000 );
    }
    check( rocketnet_server_start_message( server, (uint64_t) slot1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == NULL );
    check( rocketnet_server_client_conditions( server, slot1, &c, (int) sizeof( c ) ) );
    check( c.send_refused == 1 );
    check( rocketnet_server_client_conditions( server, slot0, &c, (int) sizeof( c ) ) );
    check( c.send_refused == 0 );

    // after a frame the queued messages are numbered and sent: in flight, at most the window

    send_room_frame( server, clients, 2 );
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( server, slot1 ) );
    check( rocketnet_client_conditions( clients[0], &c, (int) sizeof( c ) ) );
    check( c.reliable_in_flight > 0 );
    check( c.reliable_in_flight <= 1024 );
    check( rocketnet_server_client_conditions( server, slot1, &c, (int) sizeof( c ) ) );
    check( c.reliable_in_flight > 0 );

    // a short conditions_bytes: only the first field is written

    memset( &c, 0xA5, sizeof( c ) );
    check( rocketnet_client_conditions( clients[1], &c, (int) sizeof( double ) ) );
    check( c.seconds_since_heard >= 0.0 );
    uint8_t untouched[sizeof( c ) - sizeof( double )];
    memset( untouched, 0xA5, sizeof( untouched ) );
    check( memcmp( (const uint8_t *) &c + sizeof( double ), untouched, sizeof( untouched ) ) == 0 );
    check( !rocketnet_client_conditions( clients[1], &c, 0 ) );
    check( g_send_room_asserts == 0 );

    // a NULL struct is API misuse: one assert in an asserts build
    check( !rocketnet_client_conditions( clients[1], NULL, (int) sizeof( c ) ) );
    check( g_send_room_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    for ( int i = 0; i < 2; i++ )
    {
        rn_test_client_stop_and_destroy( clients[i] );
    }
    rn_test_server_stop_and_destroy( server );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_conditions_public_api_client_and_server );

// ---------------------------------------------------------------------------
// m1.7 through the public API: a start that finds no room is refused at each
// send path, nothing accepted is lost across a refusal, and no amount or
// order of send-side calls ends a connection. Two clients on a three-slot
// server over loopback, step driven, time starts at 1000.0.
// ---------------------------------------------------------------------------

struct refuse_world_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * clients[2];
    int slots[2];
    int empty_slot;
    int max_message_bytes;
    int num_channels;
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    char address[64];
};

static void refuse_world_create( refuse_world_t * w, uint8_t seed_xor, int packets_per_update )
{
    memset( w, 0, sizeof( *w ) );
    g_send_room_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( seed_xor ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, w->backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = SEND_ROOM_PROTOCOL;
    server_config.max_clients = 3;
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    if ( packets_per_update > 0 )
    {
        server_config.packets_per_update = packets_per_update;
    }
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    w->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( w->server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( w->server ) == ROCKETNET_STATUS_OK );
    snprintf( w->address, sizeof( w->address ), "127.0.0.1:%d", (int) rocketnet_server_port( w->server ) );

    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
        client_config.transport = "loopback";
        client_config.loopback_hub = rocketnet_test_loopback_hub();
        client_config.step_driven = 1;
        client_config.time_callback = send_room_clock;
        if ( packets_per_update > 0 )
        {
            client_config.packets_per_update = packets_per_update;
        }
        w->max_message_bytes = client_config.max_message_bytes;
        w->num_channels = client_config.num_channels;
        create_status = ROCKETNET_STATUS_OK;
        w->clients[i] = rocketnet_client_create( NULL, &client_config, &create_status );
        check( w->clients[i] != NULL );
        check( create_status == ROCKETNET_STATUS_OK );
    }
}

static void refuse_world_connect( refuse_world_t * w, int i )
{
    const char * addresses[] = { w->address };
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( w->backend_secret, w->server, SEND_ROOM_PROTOCOL, addresses, 1,
                                        (uint64_t) ( 0x91 + i ), 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( w->clients[i], token ) == ROCKETNET_STATUS_OK );
}

static void refuse_world_connected( refuse_world_t * w )
{
    for ( int i = 0; i < 400; i++ )
    {
        if ( rocketnet_client_state( w->clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_client_state( w->clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED &&
             rocketnet_server_num_connected_clients( w->server ) == 2 )
        {
            break;
        }
        send_room_frame( w->server, w->clients, 2 );
    }

    check( rocketnet_client_state( w->clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_state( w->clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    w->slots[0] = rocketnet_client_index( w->clients[0] );
    w->slots[1] = rocketnet_client_index( w->clients[1] );
    check( w->slots[0] >= 0 && w->slots[0] < 3 && w->slots[1] >= 0 && w->slots[1] < 3 && w->slots[0] != w->slots[1] );
    w->empty_slot = 0;
    while ( w->empty_slot == w->slots[0] || w->empty_slot == w->slots[1] )
    {
        w->empty_slot++;
    }
}

static void refuse_world_destroy( refuse_world_t * w )
{
    for ( int i = 0; i < 2; i++ )
    {
        rn_test_client_stop_and_destroy( w->clients[i] );
    }
    rn_test_server_stop_and_destroy( w->server );
}

// send_refused of a connection; 0 when it is not connected (the getter zeroes)

static uint64_t refuse_client_refused( const rocketnet_client_t * client )
{
    struct rocketnet_conditions_t conditions;
    memset( &conditions, 0, sizeof( conditions ) );
    (void) rocketnet_client_conditions( client, &conditions, (int) sizeof( conditions ) );
    return conditions.send_refused;
}

static uint64_t refuse_slot_refused( const rocketnet_server_t * server, int slot )
{
    struct rocketnet_conditions_t conditions;
    memset( &conditions, 0, sizeof( conditions ) );
    (void) rocketnet_server_client_conditions( server, slot, &conditions, (int) sizeof( conditions ) );
    return conditions.send_refused;
}

// one send path: client 0 fills its send side, then the server fills client
// 1's slot; each start past the first no is refused, counted once on that
// connection only, and ends nothing; frames let the receivers acknowledge (or
// write the queued unreliable messages) and a start succeeds again

static void refuse_one_path( enum rocketnet_reliability_t reliability, int bytes, int packets_per_update )
{
    refuse_world_t w;
    refuse_world_create( &w, (uint8_t) ( 0x20 + (int) reliability ), packets_per_update );
    refuse_world_connect( &w, 0 );
    refuse_world_connect( &w, 1 );
    refuse_world_connected( &w );

    rocketnet_client_t * client = w.clients[0];
    const int slot = w.slots[1];
    const int other_slot = w.slots[0];

    int sent = 0;
    while ( rocketnet_client_can_send_message( client, 0, reliability, bytes ) )
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, reliability, bytes );
        check( p != NULL );
        memset( p, 0x31, (size_t) bytes );
        check( rocketnet_client_finish_message( client, bytes ) != 0 );
        sent++;
        check( sent < 100000 );
    }
    check( sent > 0 );

    for ( uint64_t k = 1; k <= 3; k++ )
    {
        check( !rocketnet_client_can_send_message( client, 0, reliability, bytes ) );
        check( rocketnet_client_start_message( client, 0, reliability, bytes ) == NULL );
        check( refuse_client_refused( client ) == k );
        check( refuse_client_refused( w.clients[1] ) == 0 );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( !rocketnet_client_can_send_message( client, 0, reliability, bytes ) );
    }
    check( refuse_slot_refused( w.server, slot ) == 0 );
    check( refuse_slot_refused( w.server, other_slot ) == 0 );

    sent = 0;
    while ( rocketnet_server_can_send_message( w.server, slot, 0, reliability, bytes ) )
    {
        uint8_t * p = rocketnet_server_start_message( w.server, (uint64_t) slot, 0, reliability, bytes );
        check( p != NULL );
        memset( p, 0x32, (size_t) bytes );
        check( rocketnet_server_finish_message( w.server, bytes ) != 0 );
        sent++;
        check( sent < 100000 );
    }
    check( sent > 0 );

    for ( uint64_t k = 1; k <= 3; k++ )
    {
        check( !rocketnet_server_can_send_message( w.server, slot, 0, reliability, bytes ) );
        check( rocketnet_server_start_message( w.server, (uint64_t) slot, 0, reliability, bytes ) == NULL );
        check( refuse_slot_refused( w.server, slot ) == k );
        check( refuse_slot_refused( w.server, other_slot ) == 0 );
        check( rocketnet_server_client_connected( w.server, slot ) );
        check( rocketnet_server_disconnect_reason( w.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
        check( !rocketnet_server_can_send_message( w.server, slot, 0, reliability, bytes ) );
        check( rocketnet_server_client_connected( w.server, other_slot ) );
        check( rocketnet_server_can_send_message( w.server, other_slot, 0, reliability, bytes ) );
    }
    check( refuse_client_refused( client ) == 3 );
    check( rocketnet_server_num_connected_clients( w.server ) == 2 );

    // room returns: the query's yes is a start's pointer again, on both sides

    for ( int i = 0; i < 4000 && ( !rocketnet_client_can_send_message( client, 0, reliability, bytes ) ||
                                   !rocketnet_server_can_send_message( w.server, slot, 0, reliability, bytes ) );
          i++ )
    {
        send_room_frame( w.server, w.clients, 2 );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( w.server, slot ) );
        check( rocketnet_server_disconnect_reason( w.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    uint8_t * p = rocketnet_client_start_message( client, 0, reliability, bytes );
    check( p != NULL );
    memset( p, 0x33, (size_t) bytes );
    check( rocketnet_client_finish_message( client, bytes ) != 0 );
    p = rocketnet_server_start_message( w.server, (uint64_t) slot, 0, reliability, bytes );
    check( p != NULL );
    memset( p, 0x34, (size_t) bytes );
    check( rocketnet_server_finish_message( w.server, bytes ) != 0 );

    check( refuse_client_refused( client ) == 3 );
    check( refuse_client_refused( w.clients[1] ) == 0 );
    check( refuse_slot_refused( w.server, slot ) == 3 );
    check( refuse_slot_refused( w.server, other_slot ) == 0 );
    check( rocketnet_client_state( w.clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_num_connected_clients( w.server ) == 2 );

    refuse_world_destroy( &w );
}

static void test_send_refused_at_each_send_path_connection_stays_up()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( send_room_count_assert );
    g_send_room_asserts = 0;

    // the reliable modes, queued in the region
    refuse_one_path( ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1000, 0 );
    refuse_one_path( ROCKETNET_RELIABILITY_RELIABLE, 1000, 0 );
    refuse_one_path( ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED, 1000, 0 );
    refuse_one_path( ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, 1000, 0 );
    refuse_one_path( ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, 3000, 0 );

    // the unreliable path with no room in the send region: one packet an
    // update, so the in-place frame path is spent and starts fall back to
    // the region; and an oversized unreliable send promoted to its reliable twin
    refuse_one_path( ROCKETNET_RELIABILITY_UNRELIABLE, 64, 1 );
    refuse_one_path( ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 64, 1 );
    refuse_one_path( ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 64, 1 );
    refuse_one_path( ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES + 1, 1 );

    check( g_send_room_asserts == 0 );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_send_refused_at_each_send_path_connection_stays_up );

// nothing accepted is lost across a refusal: client 0 and the server (to
// client 0's slot) keep starting tracked reliable ordered messages, each
// payload carrying its index, through refusals and frames; every accepted
// message arrives once, in order, intact, and nothing else arrives

static int refuse_tracked_bytes( uint32_t index )
{
    static const int sizes[] = { 100, 900, 40, 2000, 1200 };
    return sizes[index % ( sizeof( sizes ) / sizeof( sizes[0] ) )];
}

static void refuse_tracked_fill( uint8_t * p, uint32_t index, int bytes )
{
    memcpy( p, &index, sizeof( index ) );
    for ( int i = (int) sizeof( index ); i < bytes; i++ )
    {
        p[i] = (uint8_t) ( index * 29u + (uint32_t) i * 7u + 3u );
    }
}

static void refuse_tracked_verify( const uint8_t * p, int bytes, uint32_t * next )
{
    check( bytes >= (int) sizeof( uint32_t ) );
    uint32_t index = 0;
    memcpy( &index, p, sizeof( index ) );
    check( index == *next );
    check( bytes == refuse_tracked_bytes( index ) );
    for ( int i = (int) sizeof( index ); i < bytes; i++ )
    {
        check( p[i] == (uint8_t) ( index * 29u + (uint32_t) i * 7u + 3u ) );
    }
    ( *next )++;
}

// one tracked start from the client (to_server) or the server: true when accepted

static bool refuse_tracked_send( refuse_world_t * w, bool to_server, uint32_t * accepted, uint64_t * refused )
{
    const int bytes = refuse_tracked_bytes( *accepted );
    uint8_t * p = to_server ? rocketnet_client_start_message( w->clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes )
                            : rocketnet_server_start_message( w->server, (uint64_t) w->slots[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    if ( !p )
    {
        ( *refused )++;
        return false;
    }
    refuse_tracked_fill( p, *accepted, bytes );
    check( ( to_server ? rocketnet_client_finish_message( w->clients[0], bytes ) : rocketnet_server_finish_message( w->server, bytes ) ) != 0 );
    ( *accepted )++;
    return true;
}

static void test_send_refused_loses_nothing_accepted()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( send_room_count_assert );
    g_send_room_asserts = 0;

    refuse_world_t w;
    refuse_world_create( &w, 0x5C, 0 );
    refuse_world_connect( &w, 0 );
    refuse_world_connect( &w, 1 );
    refuse_world_connected( &w );

    uint32_t up_accepted = 0;
    uint32_t down_accepted = 0;
    uint64_t up_refused = 0;
    uint64_t down_refused = 0;
    uint32_t up_got = 0;
    uint32_t down_got = 0;

    // both directions fill without a frame until each has been refused

    while ( refuse_tracked_send( &w, true, &up_accepted, &up_refused ) )
    {
        check( up_accepted < 100000 );
    }
    while ( refuse_tracked_send( &w, false, &down_accepted, &down_refused ) )
    {
        check( down_accepted < 100000 );
    }
    check( up_refused == 1 && down_refused == 1 );

    // frames: each one first tries eight more starts each way (refused or
    // accepted as room allows), then runs the pair, checking every delivery

    for ( int frame = 0; frame < 6000; frame++ )
    {
        if ( frame < 300 )
        {
            for ( int k = 0; k < 8; k++ )
            {
                (void) refuse_tracked_send( &w, true, &up_accepted, &up_refused );
                (void) refuse_tracked_send( &w, false, &down_accepted, &down_refused );
            }
        }
        else if ( up_got == up_accepted && down_got == down_accepted )
        {
            break;
        }

        for ( int i = 0; i < 2; i++ )
        {
            rocketnet_client_update( w.clients[i], g_send_room_now );
            const struct rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_client_receive_messages( w.clients[i] ) ) != NULL )
            {
                for ( int m = 0; m < batch->num_messages; m++ )
                {
                    check( i == 0 );                                    // nothing reaches the other client
                    check( batch->channel[m] == 0 );
                    refuse_tracked_verify( batch->data[m], batch->bytes[m], &down_got );
                }
            }
            rocketnet_client_send_packets( w.clients[i] );
        }

        rocketnet_server_update( w.server, g_send_room_now );
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( w.server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                check( batch->client_index[m] == w.slots[0] );
                check( batch->channel[m] == 0 );
                refuse_tracked_verify( batch->data[m], batch->bytes[m], &up_got );
            }
        }
        rocketnet_server_send_packets( w.server );
        g_send_room_now += 0.01;

        check( rocketnet_client_state( w.clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( w.server, w.slots[0] ) );
    }

    // every accepted message once, in order; the refusals counted exactly, on
    // the sending connection only; more were refused than the fill's one

    check( up_got == up_accepted );
    check( down_got == down_accepted );
    check( up_refused > 1 && down_refused > 1 );
    check( refuse_client_refused( w.clients[0] ) == up_refused );
    check( refuse_slot_refused( w.server, w.slots[0] ) == down_refused );
    check( refuse_client_refused( w.clients[1] ) == 0 );
    check( refuse_slot_refused( w.server, w.slots[1] ) == 0 );
    check( rocketnet_server_disconnect_reason( w.server, w.slots[0] ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( g_send_room_asserts == 0 );

    refuse_world_destroy( &w );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_send_refused_loses_nothing_accepted );

// the send hammer: every public send-side call (start, finish, abort, the room
// query, the send rate setter, receipts) with valid and invalid arguments, far
// past capacity, in every client state and on the server for a connected
// slot, an empty slot and indices outside the slots. Per call: misuse is
// counted exactly where the API defines it, the debug assert fires exactly
// where it documents one, a yes from the query is a start's pointer, and a
// strictly valid start on a connected handle with nothing open that is
// refused counts one send_refused on that connection (and nothing else
// does). The connections that were connected are still connected at the end.

static uint32_t g_hammer_rng = 0x2545F491u;

static uint32_t hammer_rand()
{
    g_hammer_rng = g_hammer_rng * 1664525u + 1013904223u;
    return g_hammer_rng >> 8;
}

#define HAMMER_CALL( misuses, asserts, call )                                                                \
    do                                                                                                       \
    {                                                                                                        \
        const uint64_t hammer_misuse_before = rocketnet_api_misuse_count();                                  \
        const int hammer_asserts_before = g_send_room_asserts;                                               \
        call;                                                                                                \
        check( rocketnet_api_misuse_count() - hammer_misuse_before == (uint64_t) ( misuses ) );              \
        check( g_send_room_asserts - hammer_asserts_before == ( ROCKETNET_ASSERTS ? ( asserts ) : 0 ) );     \
    } while ( 0 )

struct hammer_t
{
    bool open;
    int open_bytes;
    uint64_t room_refusals;
    uint64_t given;
};

struct hammer_args_t
{
    int channel;
    enum rocketnet_reliability_t reliability;
    int bytes;
    bool strict;            // every argument inside its documented range
};

static hammer_args_t hammer_args( const refuse_world_t * w )
{
    const int nc = w->num_channels;
    const int max = w->max_message_bytes;
    const int channels[] = { INT_MIN, -1, 0, 0, 1, nc - 1, nc, INT_MAX };
    const int reliabilities[] = { INT_MIN, -1, 0, 1, 2, 3, 3, 4, 5, 6, 7, 8, 99, INT_MAX };
    const int sizes[] = { INT_MIN, -1, 0, 1, 64, 100, 1000, 1000, 2000, ROCKETNET_MAX_UNRELIABLE_MESSAGE_BYTES + 1, max, max + 1, INT_MAX };

    hammer_args_t a;
    a.channel = channels[hammer_rand() % ( sizeof( channels ) / sizeof( channels[0] ) )];
    const int r = reliabilities[hammer_rand() % ( sizeof( reliabilities ) / sizeof( reliabilities[0] ) )];
    a.reliability = (enum rocketnet_reliability_t) r;
    a.bytes = sizes[hammer_rand() % ( sizeof( sizes ) / sizeof( sizes[0] ) )];
    // the ordered and sequenced modes use the channel; the others ignore it
    // (rocketnet_client_start_message)
    const bool uses_channel = r == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED || r == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
                              r == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED || r == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
    const bool channel_ok = !uses_channel || ( a.channel >= 0 && a.channel < nc );
    a.strict = channel_ok && r >= 0 && r <= 7 && a.bytes >= 1 && a.bytes <= max;
    return a;
}

static void hammer_client_step( const refuse_world_t * w, rocketnet_client_t * client, hammer_t * h )
{
    const hammer_args_t a = hammer_args( w );
    const bool connected = rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED;
    const uint32_t what = hammer_rand() % 10;

    if ( what < 5 )
    {
        bool room = false;
        HAMMER_CALL( 0, 0, room = rocketnet_client_can_send_message( client, a.channel, a.reliability, a.bytes ) );
        const uint64_t refused_before = refuse_client_refused( client );
        uint8_t * p = NULL;
        HAMMER_CALL( 0, 0, p = rocketnet_client_start_message( client, a.channel, a.reliability, a.bytes ) );
        const uint64_t refused_after = refuse_client_refused( client );
        if ( room )
        {
            check( p != NULL );
        }
        if ( !connected || h->open )
        {
            check( p == NULL );
        }
        if ( p )
        {
            check( refused_after == refused_before );
            memset( p, 0x6D, (size_t) a.bytes );
            h->open = true;
            h->open_bytes = a.bytes;
            h->given++;
        }
        else if ( a.strict && connected && !h->open )
        {
            check( refused_after == refused_before + 1 );
            h->room_refusals++;
        }
        else
        {
            check( refused_after == refused_before );
        }
    }
    else if ( what < 7 )
    {
        uint32_t serial = 0;
        if ( h->open )
        {
            const int choices[] = { h->open_bytes, h->open_bytes, 1, 0, -1, h->open_bytes + 1 };
            const int written = choices[hammer_rand() % ( sizeof( choices ) / sizeof( choices[0] ) )];
            const bool good = written >= 1 && written <= h->open_bytes;
            HAMMER_CALL( 0, 0, serial = rocketnet_client_finish_message( client, written ) );
            check( good ? serial != 0 : serial == 0 );
            h->open = false;
        }
        else
        {
            HAMMER_CALL( 0, 1, serial = rocketnet_client_finish_message( client, a.bytes ) );
            check( serial == 0 );
        }
    }
    else if ( what < 8 )
    {
        HAMMER_CALL( 0, 0, rocketnet_client_abort_message( client ) );
        h->open = false;
    }
    else if ( what < 9 )
    {
        const int rates[] = { -1, -100, INT_MIN, 0, ROCKETNET_SEND_KBPS_UNLIMITED };
        const int kbps = rates[hammer_rand() % ( sizeof( rates ) / sizeof( rates[0] ) )];
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        HAMMER_CALL( 0, 0, status = rocketnet_client_set_send_kbps( client, kbps ) );
        check( kbps < 0 ? status == ROCKETNET_STATUS_INVALID_ARGUMENT : status == ROCKETNET_STATUS_OK );
    }
    else
    {
        struct rocketnet_receipt_t receipts[4];
        const int capacities[] = { -1, 0, 4 };
        const int capacity = capacities[hammer_rand() % 3];
        struct rocketnet_receipt_t * out = ( hammer_rand() & 1 ) ? receipts : NULL;
        const int null_out = capacity > 0 && out == NULL ? 1 : 0;   // NULL with room to fill is misuse
        int count = 0;
        HAMMER_CALL( null_out, null_out, count = rocketnet_client_receipts( client, out, capacity ) );
        check( count >= 0 );
    }
}

static void hammer_server_step( const refuse_world_t * w, int connected_slot, hammer_t * h )
{
    const hammer_args_t a = hammer_args( w );
    const int indices[] = { connected_slot, connected_slot, connected_slot, w->empty_slot, -1, 3, INT_MAX, INT_MIN };
    const int index = indices[hammer_rand() % ( sizeof( indices ) / sizeof( indices[0] ) )];
    const bool in_range = index >= 0 && index < 3;
    const bool connected = in_range && rocketnet_server_client_connected( w->server, index );
    const uint32_t what = hammer_rand() % 10;

    if ( what < 5 )
    {
        bool room = false;
        HAMMER_CALL( in_range ? 0 : 1, 0, room = rocketnet_server_can_send_message( w->server, index, a.channel, a.reliability, a.bytes ) );
        const uint64_t refused_before = in_range ? refuse_slot_refused( w->server, index ) : 0;
        uint8_t * p = NULL;
        HAMMER_CALL( in_range ? 0 : 1, 0, p = rocketnet_server_start_message( w->server, (uint64_t) (int64_t) index, a.channel, a.reliability, a.bytes ) );
        const uint64_t refused_after = in_range ? refuse_slot_refused( w->server, index ) : 0;
        if ( room )
        {
            check( p != NULL );
        }
        if ( !connected || h->open )
        {
            check( p == NULL );
        }
        if ( p )
        {
            check( refused_after == refused_before );
            memset( p, 0x6E, (size_t) a.bytes );
            h->open = true;
            h->open_bytes = a.bytes;
            h->given++;
        }
        else if ( a.strict && connected && !h->open )
        {
            check( refused_after == refused_before + 1 );
            h->room_refusals++;
        }
        else
        {
            check( refused_after == refused_before );
        }
    }
    else if ( what < 7 )
    {
        uint32_t serial = 0;
        if ( h->open )
        {
            const int choices[] = { h->open_bytes, h->open_bytes, 1, 0, -1, h->open_bytes + 1 };
            const int written = choices[hammer_rand() % ( sizeof( choices ) / sizeof( choices[0] ) )];
            const bool good = written >= 1 && written <= h->open_bytes;
            HAMMER_CALL( 0, 0, serial = rocketnet_server_finish_message( w->server, written ) );
            check( good ? serial != 0 : serial == 0 );
            h->open = false;
        }
        else
        {
            HAMMER_CALL( 0, 1, serial = rocketnet_server_finish_message( w->server, a.bytes ) );
            check( serial == 0 );
        }
    }
    else if ( what < 8 )
    {
        HAMMER_CALL( 0, 0, rocketnet_server_abort_message( w->server ) );
        h->open = false;
    }
    else if ( what < 9 )
    {
        const int rates[] = { -1, -100, INT_MIN, 0, ROCKETNET_SEND_KBPS_UNLIMITED };
        const int kbps = rates[hammer_rand() % ( sizeof( rates ) / sizeof( rates[0] ) )];
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        HAMMER_CALL( in_range ? 0 : 1, 0, status = rocketnet_server_set_send_kbps( w->server, index, kbps ) );
        check( ( kbps < 0 || !in_range ) ? status == ROCKETNET_STATUS_INVALID_ARGUMENT : status == ROCKETNET_STATUS_OK );
    }
    else
    {
        struct rocketnet_receipt_t receipts[4];
        const int capacities[] = { -1, 0, 4 };
        const int capacity = capacities[hammer_rand() % 3];
        struct rocketnet_receipt_t * out = ( hammer_rand() & 1 ) ? receipts : NULL;
        const int null_out = capacity > 0 && out == NULL ? 1 : 0;   // NULL with room to fill is misuse, checked first
        int count = 0;
        HAMMER_CALL( ( null_out || !in_range ) ? 1 : 0, null_out, count = rocketnet_server_receipts( w->server, index, out, capacity ) );
        check( count >= 0 );
    }
}

static void hammer_null_handles()
{
    HAMMER_CALL( 1, 1, check( rocketnet_client_start_message( NULL, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) == NULL ) );
    HAMMER_CALL( 1, 1, check( rocketnet_client_finish_message( NULL, 4 ) == 0 ) );
    HAMMER_CALL( 1, 1, rocketnet_client_abort_message( NULL ) );
    HAMMER_CALL( 1, 1, check( !rocketnet_client_can_send_message( NULL, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) ) );
    HAMMER_CALL( 1, 1, check( rocketnet_client_set_send_kbps( NULL, 1000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT ) );
    HAMMER_CALL( 1, 1, check( rocketnet_client_receipts( NULL, NULL, 0 ) == 0 ) );
    HAMMER_CALL( 1, 1, check( rocketnet_server_start_message( NULL, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) == NULL ) );
    HAMMER_CALL( 1, 1, check( rocketnet_server_finish_message( NULL, 4 ) == 0 ) );
    HAMMER_CALL( 1, 1, rocketnet_server_abort_message( NULL ) );
    HAMMER_CALL( 1, 1, check( !rocketnet_server_can_send_message( NULL, 0, 0, ROCKETNET_RELIABILITY_RELIABLE, 4 ) ) );
    HAMMER_CALL( 1, 1, check( rocketnet_server_set_send_kbps( NULL, 0, 1000 ) == ROCKETNET_STATUS_INVALID_ARGUMENT ) );
    HAMMER_CALL( 1, 1, check( rocketnet_server_receipts( NULL, 0, NULL, 0 ) == 0 ) );
}

static void test_send_hammer_every_send_call_every_state()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( send_room_count_assert );
    g_send_room_asserts = 0;
    g_hammer_rng = 0x2545F491u;

    refuse_world_t w;
    refuse_world_create( &w, 0x7E, 0 );
    rocketnet_client_t * client = w.clients[0];
    hammer_t ch;
    hammer_t sh;
    memset( &ch, 0, sizeof( ch ) );
    memset( &sh, 0, sizeof( sh ) );

    hammer_null_handles();

    // disconnected: never connected

    for ( int i = 0; i < 500; i++ )
    {
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
        hammer_client_step( &w, client, &ch );
    }
    check( !ch.open && ch.given == 0 && ch.room_refusals == 0 );

    // connecting (states 1-3): a burst every frame until connected; the
    // server's slots are all empty or connecting meanwhile

    refuse_world_connect( &w, 0 );
    refuse_world_connect( &w, 1 );
    int connecting_steps = 0;
    for ( int frame = 0; frame < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; frame++ )
    {
        const int state = rocketnet_client_state( client );
        check( state > 0 && state < ROCKETNET_CLIENT_STATE_CONNECTED );
        for ( int i = 0; i < 20; i++ )
        {
            hammer_client_step( &w, client, &ch );
            connecting_steps++;
        }
        check( !ch.open );
        send_room_frame( w.server, w.clients, 2 );
    }
    check( connecting_steps > 0 );
    check( ch.given == 0 && ch.room_refusals == 0 );
    refuse_world_connected( &w );

    // connected: far past capacity on client 0 and on the server, with frames
    // between bursts so room returns and is spent again

    const int slot = w.slots[1];
    for ( int burst = 0; burst < 12; burst++ )
    {
        for ( int i = 0; i < 400; i++ )
        {
            hammer_client_step( &w, client, &ch );
            hammer_server_step( &w, slot, &sh );
        }
        if ( ch.open )
        {
            rocketnet_client_abort_message( client );
            ch.open = false;
        }
        if ( sh.open )
        {
            rocketnet_server_abort_message( w.server );
            sh.open = false;
        }
        send_room_frame( w.server, w.clients, 2 );
        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_server_client_connected( w.server, slot ) );
        check( rocketnet_server_disconnect_reason( w.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    check( ch.given > 0 && ch.room_refusals > 0 );
    check( sh.given > 0 && sh.room_refusals > 0 );
    check( refuse_client_refused( client ) == ch.room_refusals );
    check( refuse_slot_refused( w.server, slot ) == sh.room_refusals );
    check( refuse_client_refused( w.clients[1] ) == 0 );
    check( refuse_slot_refused( w.server, w.slots[0] ) == 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_state( w.clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_num_connected_clients( w.server ) == 2 );

    // after a disconnect: client 0 ends its connection; the hammer goes on
    // against it and against the server, whose other client stays connected

    rocketnet_client_disconnect( client );
    const uint64_t given_before = ch.given;
    for ( int i = 0; i < 500; i++ )
    {
        check( rocketnet_client_state( client ) <= ROCKETNET_CLIENT_STATE_DISCONNECTED );
        hammer_client_step( &w, client, &ch );
        hammer_server_step( &w, slot, &sh );
    }
    check( ch.given == given_before );
    if ( sh.open )
    {
        rocketnet_server_abort_message( w.server );
        sh.open = false;
    }
    send_room_frame( w.server, w.clients, 2 );

    hammer_null_handles();

    check( rocketnet_client_state( w.clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( w.server, slot ) );
    check( rocketnet_server_disconnect_reason( w.server, slot ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( refuse_slot_refused( w.server, slot ) == sh.room_refusals );

    refuse_world_destroy( &w );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_send_hammer_every_send_call_every_state );
// Skip/defer policy, drain-and-retry, preflight race and finish-without-start:
// fill until can-send is false, never start while false, prove the connection
// stays healthy, drain through frames, then send successfully; then prove a
// true preflight still requires guarding the later start, and pin the
// documented finish-without-start error through the assert harness.
static int send_room_count_server_received( rocketnet_server_t * server, uint8_t marker )
{
    int found = 0;
    const struct rocketnet_message_batch_t * batch;
    while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            if ( batch->bytes[i] > 0 && batch->data[i][0] == marker )
            {
                found++;
            }
        }
    }
    return found;
}

static int send_room_count_client_received( rocketnet_client_t * client, uint8_t marker )
{
    int found = 0;
    const struct rocketnet_message_batch_t * batch;
    while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            if ( batch->bytes[i] > 0 && batch->data[i][0] == marker )
            {
                found++;
            }
        }
    }
    return found;
}

static void test_send_room_skip_defer_drain_retry_and_preflight_race()
{
    rocketnet_quiet( true );
    rocketnet_assert_function( send_room_count_assert );
    g_send_room_asserts = 0;
    g_send_room_now = 1000.0;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0x6B ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );

    server_config.protocol_id = SEND_ROOM_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = 1;
    client_config.time_callback = send_room_clock;
    create_status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rn_test_server_token_string( backend_secret, server, SEND_ROOM_PROTOCOL, addresses, 1,
                                        (uint64_t) 0x77, 1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    rocketnet_client_t * clients[] = { client };

    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        send_room_frame( server, clients, 1 );
    }

    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    const int slot = rocketnet_client_index( client );
    check( slot >= 0 && slot < 1 );

    const int bytes = 32768;

    // client fills reliable room until the query says no; skip policy: no start while false

    int client_filled = 0;
    for ( ;; )
    {
        if ( !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
        {
            break;
        }
        uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x11, (size_t) bytes );
            check( rocketnet_client_finish_message( client, bytes ) != 0 );
        }

        client_filled++;
        check( client_filled < 100000 );
    }

    check( client_filled > 0 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    // deferral keeps the connection healthy while frames drain the queue

    int server_got_filled = 0;
    for ( int i = 0; i < 20000 && !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ); i++ )
    {
        // one manual frame that also counts deliveries, mirroring send_room_frame

        rocketnet_client_update( client, g_send_room_now );
        while ( rocketnet_client_receive_messages( client ) != NULL )
        {
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, g_send_room_now );
        server_got_filled += send_room_count_server_received( server, 0x11 );
        rocketnet_server_send_packets( server );
        g_send_room_now += 0.01;

        check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    }

    check( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( server_got_filled > 0 );
    // retry after drain succeeds and is delivered
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x22, (size_t) bytes );
            check( rocketnet_client_finish_message( client, bytes ) != 0 );
        }
    }
    int client_retry_delivered = 0;
    for ( int i = 0; i < 20000 && !client_retry_delivered; i++ )
    {
        rocketnet_client_update( client, g_send_room_now );
        while ( rocketnet_client_receive_messages( client ) != NULL )
        {
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, g_send_room_now );
        client_retry_delivered += send_room_count_server_received( server, 0x22 );
        rocketnet_server_send_packets( server );
        g_send_room_now += 0.01;
    }

    check( client_retry_delivered == 1 );

    // server fills its room to the slot until the query says no

    int server_filled = 0;
    for ( ;; )
    {
        if ( !rocketnet_server_can_send_message( server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
        {
            break;
        }
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x33, (size_t) bytes );
            check( rocketnet_server_finish_message( server, bytes ) != 0 );
        }

        server_filled++;
        check( server_filled < 100000 );
    }

    check( server_filled > 0 );
    check( rocketnet_server_client_connected( server, slot ) );

    int client_got_filled = 0;
    for ( int i = 0; i < 20000 && !rocketnet_server_can_send_message( server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ); i++ )
    {
        rocketnet_client_update( client, g_send_room_now );
        client_got_filled += send_room_count_client_received( client, 0x33 );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, g_send_room_now );
        while ( rocketnet_server_receive_messages( server ) != NULL )
        {
        }

        rocketnet_server_send_packets( server );
        g_send_room_now += 0.01;

        check( rocketnet_server_client_connected( server, slot ) );
    }

    check( rocketnet_server_can_send_message( server, slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( client_got_filled > 0 );
    {
        uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p != NULL );
        if ( p )
        {
            memset( p, 0x44, (size_t) bytes );
            check( rocketnet_server_finish_message( server, bytes ) != 0 );
        }
    }
    int server_retry_delivered = 0;
    for ( int i = 0; i < 20000 && !server_retry_delivered; i++ )
    {
        rocketnet_client_update( client, g_send_room_now );
        server_retry_delivered += send_room_count_client_received( client, 0x44 );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, g_send_room_now );
        while ( rocketnet_server_receive_messages( server ) != NULL )
        {
        }

        rocketnet_server_send_packets( server );
        g_send_room_now += 0.01;
    }

    check( server_retry_delivered == 1 );

    // preflight true, then a state transition: disconnect makes the later start return NULL.
    // The canonical branch writes and finishes only on non-NULL, so nothing is written or finished here.

    check( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );

    rocketnet_client_disconnect( client );
    {
        uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
        check( p == NULL );
        if ( p )
        {
            memset( p, 0x55, (size_t) bytes );
            check( rocketnet_client_finish_message( client, bytes ) != 0 );
        }
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );

    // finish without a successful start, only through the existing assert harness:
    // returns 0 and asserts exactly in assertion builds (documented behavior, not a silent no-op).

    g_send_room_asserts = 0;

    check( rocketnet_client_finish_message( client, 2 ) == 0 );
    check( g_send_room_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    g_send_room_asserts = 0;

    check( rocketnet_server_finish_message( server, 2 ) == 0 );
    check( g_send_room_asserts == ( ROCKETNET_ASSERTS ? 1 : 0 ) );

    g_send_room_asserts = 0;

    rn_test_client_stop_and_destroy( client );
    rn_test_server_stop_and_destroy( server );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}

REGISTER_UNIT_TEST( test_send_room_skip_defer_drain_retry_and_preflight_race );
