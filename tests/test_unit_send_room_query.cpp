/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public room query: rocketnet_client_can_send_message and
// rocketnet_server_can_send_message. A sender asks whether a message would
// fit; a start that finds no room still ends the connection with
// MESSAGE_MEMORY_EXHAUSTED. The query says no exactly when the start would
// exhaust, the start after a no is still fatal, and asking changes nothing.
// In memory over the loopback transport, step driven, no clock, sockets or
// network; time starts at 1000.0.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "token_string.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define SEND_ROOM_PROTOCOL 0x5E2D500D0E5A11CEULL

static double g_send_room_now = 1000.0;
static double send_room_clock( void * ) { return g_send_room_now; }

static int g_send_room_asserts = 0;
static void send_room_count_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition; (void) function; (void) file; (void) line;
    g_send_room_asserts++;
}

static void send_room_frame( rocketnet_server_t * server, rocketnet_client_t ** clients, int num_clients )
{
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_update( clients[i], g_send_room_now );
        while ( rocketnet_client_receive_messages( clients[i] ) != NULL ) {}
        rocketnet_client_send_packets( clients[i] );
    }
    rocketnet_server_update( server, g_send_room_now );
    while ( rocketnet_server_receive_messages( server ) != NULL ) {}
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
        (void) rocketnet_client_can_send_message( client, i % 4, (enum rocketnet_reliability_t) ( i % 8 ), i * 37 );
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
        (void) rocketnet_server_can_send_message( server, slot, i % 4, (enum rocketnet_reliability_t) ( i % 8 ), i * 37 );
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
        seed[i] = (uint8_t) ( 0x3A ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = SEND_ROOM_PROTOCOL;
    server_config.max_clients = 3;
    server_config.transport = "loopback";
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

    // client 0 exhausts its own memory, client 1's slot is where the server exhausts
    rocketnet_client_t * clients[2];
    for ( int i = 0; i < 2; i++ )
    {
        struct rocketnet_client_test_config_t client_config;
        check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
        client_config.transport = "loopback";
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
            break;
        send_room_frame( server, clients, 2 );
    }
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_client_state( clients[1] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int server_slot = rocketnet_client_index( clients[1] );
    const int other_slot = rocketnet_client_index( clients[0] );
    check( server_slot >= 0 && server_slot < 3 );
    int empty_slot = 0;
    while ( empty_slot == server_slot || empty_slot == other_slot )
        empty_slot++;

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
    // a reliability outside the enum is asked at the object (mo_room_ask_reliabilities,
    // -1 and 99): every value of the enum type is a reliability, so C++ cannot pass one here

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
            break;
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
            break;
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

    // a send after the no is still fatal: start returns NULL and the update
    // ends the connection with MESSAGE_MEMORY_EXHAUSTED
    check( rocketnet_client_start_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == NULL );
    check( rocketnet_server_start_message( server, (uint64_t) server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) == NULL );
    rocketnet_client_update( clients[0], g_send_room_now );
    check( rocketnet_client_state( clients[0] ) == ROCKETNET_CLIENT_STATE_MESSAGE_MEMORY_EXHAUSTED );
    check( !rocketnet_client_can_send_message( clients[0], 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    rocketnet_server_update( server, g_send_room_now );
    check( rocketnet_server_disconnect_reason( server, server_slot ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );
    check( !rocketnet_server_client_connected( server, server_slot ) );
    check( !rocketnet_server_can_send_message( server, server_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) );
    check( g_send_room_asserts == 0 );

    for ( int i = 0; i < 2; i++ )
        rn_test_client_stop_and_destroy( clients[i] );
    rn_test_server_stop_and_destroy( server );
    rocketnet_assert_function( NULL );
    rocketnet_quiet( false );
}
REGISTER_UNIT_TEST( test_send_room_query_public_api_client_and_server );
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
        for ( int i = 0; i < batch->num_messages; i++ )
            if ( batch->bytes[i] > 0 && batch->data[i][0] == marker )
                found++;
    return found;
}

static int send_room_count_client_received( rocketnet_client_t * client, uint8_t marker )
{
    int found = 0;
    const struct rocketnet_message_batch_t * batch;
    while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        for ( int i = 0; i < batch->num_messages; i++ )
            if ( batch->bytes[i] > 0 && batch->data[i][0] == marker )
                found++;
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
        seed[i] = (uint8_t) ( 0x6B ^ i );
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = SEND_ROOM_PROTOCOL;
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    struct rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
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
        send_room_frame( server, clients, 1 );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int slot = rocketnet_client_index( client );
    check( slot >= 0 && slot < 1 );

    const int bytes = 32768;
    // client fills reliable room until the query says no; skip policy: no start while false
    int client_filled = 0;
    for ( ;; )
    {
        if ( !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
            break;
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
        while ( rocketnet_client_receive_messages( client ) != NULL ) {}
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
        while ( rocketnet_client_receive_messages( client ) != NULL ) {}
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
            break;
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
        while ( rocketnet_server_receive_messages( server ) != NULL ) {}
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
        while ( rocketnet_server_receive_messages( server ) != NULL ) {}
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
