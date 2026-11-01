/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every packet and message finish requires a live reservation from the matching
// start: calling finish before start, after a refused or dropped start, a
// second time after finish, after abort, on a different slot, with a mismatched
// reservation mode asserts in a debug build and returns 0 safely in a release
// build. A server packet whose lease update, send_packets or stop already
// reset is caller misuse: finish returns 0 in every build, counts one
// api_misuse and does not assert. The client transport still asserts for
// that sequence. Out-of-range byte counts on a live reservation still abort the
// reservation and return 0 without firing the missing-start assertion, and a
// subsequent valid start and finish succeeds.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "token_string.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_allocator.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_message_object.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#define check rocketnet_unit_check

#define FINISH_REQUIRES_START_PROTOCOL 0x7A11F1615B000001ULL
#define FINISH_REQUIRES_START_MAX_CLIENTS 2

static double g_finish_test_now = 1000.0;
static double finish_test_clock( void * ) { return g_finish_test_now; }

static int g_finish_test_asserts = 0;
static void finish_test_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    g_finish_test_asserts++;
}

struct finish_assert_scope_t
{
    rocketnet_assert_function_t saved;

    finish_assert_scope_t()
    {
        saved = rn_assert_function;
        g_finish_test_asserts = 0;
        rocketnet_assert_function( finish_test_assert_hook );
    }

    ~finish_assert_scope_t()
    {
        rocketnet_assert_function( saved );
    }
};

static void check_asserted_once()
{
#if ROCKETNET_ASSERTS
    check( g_finish_test_asserts == 1 );
#else
    check( g_finish_test_asserts == 0 );
#endif
    g_finish_test_asserts = 0;
}

static void check_not_asserted()
{
    check( g_finish_test_asserts == 0 );
}

struct finish_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    rocketnet_loopback_hub_t * hub;     // the world's own: test_finish_requires_start_transport_packets runs two servers at once
    rocketnet_server_t * server;
    rocketnet_client_t * clients[FINISH_REQUIRES_START_MAX_CLIENTS];
    int num_clients;
    double time;
};

static void finish_world_create( finish_world_t * world, int max_clients, int num_clients, uint8_t seed_tag = 0x51 )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    g_finish_test_now = world->time;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( seed_tag ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );

    world->hub = rocketnet_loopback_hub_create( NULL, max_clients, NULL );
    check( world->hub != NULL );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = FINISH_REQUIRES_START_PROTOCOL;
    server_config.max_clients = max_clients;
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = world->hub;

    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( world->server != NULL );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    world->num_clients = num_clients;
    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_config_t client_config;
        rocketnet_client_default_config( &client_config );
        client_config.transport = "loopback";
        client_config.loopback_hub = world->hub;
        client_config.step_driven = true;
        client_config.time_callback = finish_test_clock;
        world->clients[i] = rocketnet_client_create( NULL, &client_config, NULL );
        check( world->clients[i] != NULL );
    }
}

static void finish_world_destroy( finish_world_t * world )
{
    for ( int i = 0; i < world->num_clients; i++ )
        rn_test_client_stop_and_destroy( world->clients[i] );
    rn_test_server_stop_and_destroy( world->server );
    check( rocketnet_loopback_hub_destroy( world->hub ) == ROCKETNET_STATUS_OK );
}

static void finish_world_pump( finish_world_t * world, double dt = 0.01 )
{
    g_finish_test_now = world->time;
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

static void finish_world_token( const finish_world_t * world, uint64_t client_id, char * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    check( rn_test_server_token_string( world->backend_secret, world->server,
                                        FINISH_REQUIRES_START_PROTOCOL, addresses, 1, client_id,
                                        1000, 1ULL << 40, 300, NULL, token ) == ROCKETNET_STATUS_OK );
}

static void finish_world_connect( finish_world_t * world, int client_slot, uint64_t client_id )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    finish_world_token( world, client_id, token );
    check( rn_test_client_connect_after_completion( world->clients[client_slot], token ) == ROCKETNET_STATUS_OK );
    for ( int i = 0; i < 400 && rocketnet_client_state( world->clients[client_slot] ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        finish_world_pump( world );
    check( rocketnet_client_state( world->clients[client_slot] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int index = rocketnet_client_index( world->clients[client_slot] );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( world->server, index ); i++ )
        finish_world_pump( world );
    check( rocketnet_server_client_connected( world->server, index ) );
}

static void finish_world_verify_round_trip( finish_world_t * world, int client_slot, uint8_t client_tag, uint8_t server_tag )
{
    rocketnet_client_t * client = world->clients[client_slot];
    const int index = rocketnet_client_index( client );
    check( index >= 0 );

    uint8_t * up = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( up != NULL );
    up[0] = client_tag;
    up[1] = 0x11;
    up[2] = 0x22;
    up[3] = 0x33;
    check( rocketnet_client_finish_message( client, 4 ) != 0 );

    uint8_t * down = rocketnet_server_start_message( world->server, (uint64_t) index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( down != NULL );
    down[0] = server_tag;
    down[1] = 0x44;
    down[2] = 0x55;
    down[3] = 0x66;
    check( rocketnet_server_finish_message( world->server, 4 ) != 0 );

    bool server_got = false;
    bool client_got = false;
    for ( int step = 0; step < 200 && !( server_got && client_got ); step++ )
    {
        finish_world_pump( world );
        const rocketnet_message_batch_t * server_batch = NULL;
        while ( ( server_batch = rocketnet_server_receive_messages( world->server ) ) != NULL )
        {
            for ( int i = 0; i < server_batch->num_messages; i++ )
            {
                if ( server_batch->client_index[i] == index && server_batch->bytes[i] == 4 &&
                     server_batch->data[i][0] == client_tag && server_batch->data[i][3] == 0x33 )
                    server_got = true;
            }
        }
        const rocketnet_message_batch_t * client_batch = NULL;
        while ( ( client_batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
            for ( int i = 0; i < client_batch->num_messages; i++ )
            {
                if ( client_batch->bytes[i] == 4 && client_batch->data[i][0] == server_tag &&
                     client_batch->data[i][3] == 0x66 )
                    client_got = true;
            }
        }
    }
    check( server_got );
    check( client_got );
}

// ---------------------------------------------------------------------------
// 1. Public client and server message finish
// ---------------------------------------------------------------------------

static void test_finish_requires_start_public_client_and_server()
{
    finish_world_t world;
    finish_world_create( &world, 2, 1 );
    rocketnet_client_t * client = world.clients[0];
    rocketnet_server_t * server = world.server;

    {
        finish_assert_scope_t scope;

        // Disconnected client: finish before start and after a dropped start
        check( rocketnet_client_finish_message( client, 4 ) == 0 );
        check_asserted_once();
        check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
        check_not_asserted();
        check( rocketnet_client_finish_message( client, 4 ) == 0 );
        check_asserted_once();

        // Stopped server: finish before start and after a dropped start
        rocketnet_server_stop( server );
        rn_test_server_wait_stopped( server );
        check( rocketnet_server_finish_message( server, 4 ) == 0 );
        check_asserted_once();
        check( rocketnet_server_start_message( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
        check_not_asserted();
        check( rocketnet_server_finish_message( server, 4 ) == 0 );
        check_asserted_once();
        check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

        // Connecting client (before handshake completes): start drops, finish asserts
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        finish_world_token( &world, 101, token );
        check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
        check( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
        check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
        check_not_asserted();
        check( rocketnet_client_finish_message( client, 4 ) == 0 );
        check_asserted_once();
    }

    // Complete the connection
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        finish_world_pump( &world );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    const int slot = rocketnet_client_index( client );
    for ( int i = 0; i < 400 && !rocketnet_server_client_connected( server, slot ); i++ )
        finish_world_pump( &world );
    check( rocketnet_server_client_connected( server, slot ) );
    const int empty_slot = 1 - slot;

    {
        finish_assert_scope_t scope;

        // Connected client & server: finish with no prior start
        check( rocketnet_client_finish_message( client, 4 ) == 0 );
        check_asserted_once();
        check( rocketnet_server_finish_message( server, 4 ) == 0 );
        check_asserted_once();

        // Refused / dropped start_message returning NULL leaves no reservation:
        // invalid channel, invalid reliability, invalid max_bytes, empty server slot, out-of-range slot
        check( rocketnet_client_start_message( client, -1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
        check_not_asserted();
        check( rocketnet_client_finish_message( client, 4 ) == 0 );
        check_asserted_once();

        const int invalid_rels[] = { INT_MIN, -1, 8, INT_MAX };
        for ( size_t i = 0; i < sizeof( invalid_rels ) / sizeof( invalid_rels[0] ); i++ )
        {
            const enum rocketnet_reliability_t invalid_rel = (enum rocketnet_reliability_t) invalid_rels[i];
            check( rocketnet_client_start_message( client, 0, invalid_rel, 4 ) == NULL );
            check_not_asserted();
            check( rocketnet_client_finish_message( client, 4 ) == 0 );
            check_asserted_once();
            check( rocketnet_server_start_message( server, (uint64_t) slot, 0, invalid_rel, 4 ) == NULL );
            check_not_asserted();
            check( rocketnet_server_finish_message( server, 4 ) == 0 );
            check_asserted_once();
        }

        check( rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 0 ) == NULL );
        check_not_asserted();
        check( rocketnet_client_finish_message( client, 4 ) == 0 );
        check_asserted_once();

        check( rocketnet_server_start_message( server, (uint64_t) empty_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
        check_not_asserted();
        check( rocketnet_server_finish_message( server, 4 ) == 0 );
        check_asserted_once();

        check( rocketnet_server_start_message( server, UINT64_MAX, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
        check_not_asserted();
        check( rocketnet_server_finish_message( server, 4 ) == 0 );
        check_asserted_once();

        check( rocketnet_server_start_message( server, 2, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) == NULL );
        check_not_asserted();
        check( rocketnet_server_finish_message( server, 4 ) == 0 );
        check_asserted_once();

        // Valid start -> finish consumes reservation; second finish asserts
        for ( int mode = 0; mode < 2; mode++ )
        {
            const enum rocketnet_reliability_t rel =
                mode == 0 ? ROCKETNET_RELIABILITY_RELIABLE_ORDERED : ROCKETNET_RELIABILITY_UNRELIABLE;

            uint8_t * c_out = rocketnet_client_start_message( client, 0, rel, 8 );
            check( c_out != NULL );
            memset( c_out, 0xA1, 8 );
            // A second start while one is open returns NULL without disturbing the open start
            check( rocketnet_client_start_message( client, 0, rel, 4 ) == NULL );
            check_not_asserted();
            check( rocketnet_client_finish_message( client, 8 ) != 0 );
            check_not_asserted();
            check( rocketnet_client_finish_message( client, 8 ) == 0 );
            check_asserted_once();

            uint8_t * s_out = rocketnet_server_start_message( server, (uint64_t) slot, 0, rel, 8 );
            check( s_out != NULL );
            memset( s_out, 0xB2, 8 );
            check( rocketnet_server_start_message( server, (uint64_t) slot, 0, rel, 4 ) == NULL );
            check_not_asserted();
            check( rocketnet_server_finish_message( server, 8 ) != 0 );
            check_not_asserted();
            check( rocketnet_server_finish_message( server, 8 ) == 0 );
            check_asserted_once();

            // Valid start -> abort cancels reservation; subsequent finish asserts
            c_out = rocketnet_client_start_message( client, 0, rel, 8 );
            check( c_out != NULL );
            rocketnet_client_abort_message( client );
            check_not_asserted();
            check( rocketnet_client_finish_message( client, 8 ) == 0 );
            check_asserted_once();

            s_out = rocketnet_server_start_message( server, (uint64_t) slot, 0, rel, 8 );
            check( s_out != NULL );
            rocketnet_server_abort_message( server );
            check_not_asserted();
            check( rocketnet_server_finish_message( server, 8 ) == 0 );
            check_asserted_once();

            // Out-of-range bytes_written (0 and max_bytes + 1) on a live reservation
            // aborts the reservation and returns 0 without asserting; a second finish asserts
            const int bad_sizes[2] = { 0, 9 };
            for ( int b = 0; b < 2; b++ )
            {
                c_out = rocketnet_client_start_message( client, 0, rel, 8 );
                check( c_out != NULL );
                memset( c_out, 0xC3, 8 );
                check( rocketnet_client_finish_message( client, bad_sizes[b] ) == 0 );
                check_not_asserted();
                check( rocketnet_client_finish_message( client, 4 ) == 0 );
                check_asserted_once();

                s_out = rocketnet_server_start_message( server, (uint64_t) slot, 0, rel, 8 );
                check( s_out != NULL );
                memset( s_out, 0xD4, 8 );
                check( rocketnet_server_finish_message( server, bad_sizes[b] ) == 0 );
                check_not_asserted();
                check( rocketnet_server_finish_message( server, 4 ) == 0 );
                check_asserted_once();
            }
        }
    }

    // Drain any delivered messages and verify clean round-trip delivery
    finish_world_verify_round_trip( &world, 0, 0x71, 0x72 );

    // Open server reservation invalidated by rocketnet_server_disconnect_client: finish asserts
    {
        uint8_t * s_out = rocketnet_server_start_message( server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        check( s_out != NULL );
        rocketnet_server_disconnect_client( server, slot );
        finish_assert_scope_t scope;
        check( rocketnet_server_finish_message( server, 8 ) == 0 );
        check_asserted_once();
    }

    // Reconnect client and test open client reservation invalidated by rocketnet_client_disconnect
    rocketnet_client_disconnect( client );
    rn_test_client_wait_disconnected( client );
    finish_world_pump( &world );
    finish_world_connect( &world, 0, 102 );
    {
        uint8_t * c_out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        check( c_out != NULL );
        rocketnet_client_disconnect( client );
        rn_test_client_wait_disconnected( client );
        finish_assert_scope_t scope;
        check( rocketnet_client_finish_message( client, 8 ) == 0 );
        check_asserted_once();
    }

    // Reconnect client and test open server reservation invalidated by rocketnet_server_stop
    finish_world_pump( &world );
    finish_world_connect( &world, 0, 103 );
    {
        const int connected_slot = rocketnet_client_index( client );
        uint8_t * s_out = rocketnet_server_start_message( server, (uint64_t) connected_slot, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8 );
        check( s_out != NULL );
        rocketnet_server_stop( server );
        rn_test_server_wait_stopped( server );
        finish_assert_scope_t scope;
        check( rocketnet_server_finish_message( server, 8 ) == 0 );
        check_asserted_once();
    }

    finish_world_destroy( &world );
}

// ---------------------------------------------------------------------------
// 2. Message-object and endpoint finish
// ---------------------------------------------------------------------------

static void test_finish_requires_start_message_objects_and_endpoint()
{
    finish_world_t world;
    finish_world_create( &world, 2, 2, 0x62 );
    finish_world_connect( &world, 0, 201 );
    finish_world_connect( &world, 1, 202 );

    rocketnet_message_client_t * msg_client = rocketnet_client_message_client( world.clients[0] );
    rocketnet_message_server_t * msg_server = rocketnet_server_message_server( world.server );
    check( msg_client != NULL && msg_server != NULL );
    rn_message_object_t * server_obj = &msg_server->object;
    const int slot0 = rocketnet_client_index( world.clients[0] );
    const int slot1 = rocketnet_client_index( world.clients[1] );
    check( slot0 != slot1 );

    {
        finish_assert_scope_t scope;

        // NULL guards on message-client, message-server and message-object finish
        check( rocketnet_message_client_finish( NULL, 4 ) == 0 );
        check_asserted_once();
        check( rocketnet_message_server_finish( NULL, 0, 4 ) == 0 );
        check_asserted_once();
        check( rn_message_object_finish( NULL, 0, 4 ) == 0 );
        check_asserted_once();

        // Finish with no pending start
        check( rocketnet_message_client_finish( msg_client, 4 ) == 0 );
        check_asserted_once();
        check( rocketnet_message_server_finish( msg_server, slot0, 4 ) == 0 );
        check_asserted_once();
        check( rn_message_object_finish( server_obj, -1, 4 ) == 0 );
        check_asserted_once();
        check( rn_message_object_finish( server_obj, 2, 4 ) == 0 );
        check_asserted_once();

        // Start on slot0, then attempt finish on slot1 or out-of-range index:
        // asserts in Debug, returns 0 in Release, and leaves slot0's reservation intact
        uint8_t * s0 = rocketnet_message_server_start( msg_server, slot0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 6 );
        check( s0 != NULL );
        memset( s0, 0x3A, 6 );
        check( rocketnet_message_server_finish( msg_server, slot1, 6 ) == 0 );
        check_asserted_once();
        check( rocketnet_message_server_finish( msg_server, -1, 6 ) == 0 );
        check_asserted_once();
        check( rocketnet_message_server_finish( msg_server, 2, 6 ) == 0 );
        check_asserted_once();
        // Now finish on slot0 succeeds without asserting, and a second finish on slot0 asserts
        check( rocketnet_message_server_finish( msg_server, slot0, 6 ) != 0 );
        check_not_asserted();
        check( rocketnet_message_server_finish( msg_server, slot0, 6 ) == 0 );
        check_asserted_once();

        // Client and server message object: start -> abort -> finish asserts
        uint8_t * c0 = rocketnet_message_client_start( msg_client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 6 );
        check( c0 != NULL );
        rocketnet_message_client_abort( msg_client );
        check_not_asserted();
        check( rocketnet_message_client_finish( msg_client, 6 ) == 0 );
        check_asserted_once();

        s0 = rocketnet_message_server_start( msg_server, slot0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 6 );
        check( s0 != NULL );
        rocketnet_message_server_abort( msg_server, slot0 );
        check_not_asserted();
        check( rocketnet_message_server_finish( msg_server, slot0, 6 ) == 0 );
        check_asserted_once();

        // Out-of-range bytes_written on both in-place and queued reservations aborts without asserting,
        // and the following finish on that consumed reservation asserts
        for ( int mode = 0; mode < 2; mode++ )
        {
            const int rel = mode == 0 ? ROCKETNET_RELIABILITY_UNRELIABLE : ROCKETNET_RELIABILITY_RELIABLE_ORDERED;
            c0 = rocketnet_message_client_start( msg_client, 0, rel, 6 );
            check( c0 != NULL );
            check( rocketnet_message_client_finish( msg_client, 0 ) == 0 );
            check_not_asserted();
            check( rocketnet_message_client_finish( msg_client, 6 ) == 0 );
            check_asserted_once();

            c0 = rocketnet_message_client_start( msg_client, 0, rel, 6 );
            check( c0 != NULL );
            check( rocketnet_message_client_finish( msg_client, 7 ) == 0 );
            check_not_asserted();
            check( rocketnet_message_client_finish( msg_client, 6 ) == 0 );
            check_asserted_once();
        }

        // Kick cancels an open reservation on slot1, and finish after kick asserts
        uint8_t * s1 = rocketnet_message_server_start( msg_server, slot1, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 6 );
        check( s1 != NULL );
        rn_message_object_kick( server_obj, slot1, ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );
        check_not_asserted();
        check( rocketnet_message_server_finish( msg_server, slot1, 6 ) == 0 );
        check_asserted_once();

        // In-place start closed by send_packets or update (in_place == -1 - index):
        // finish clears in_place, asserts in Debug, and returns 0 in Release
        c0 = rocketnet_message_client_start( msg_client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 6 );
        check( c0 != NULL );
        check( msg_client->object.in_place > 0 );
        rocketnet_message_client_send_packets( msg_client );
        check( msg_client->object.in_place == -1 );
        check_not_asserted();
        check( rocketnet_message_client_finish( msg_client, 6 ) == 0 );
        check_asserted_once();
        check( msg_client->object.in_place == 0 );
        check( rocketnet_message_client_finish( msg_client, 6 ) == 0 );
        check_asserted_once();

        s0 = rocketnet_message_server_start( msg_server, slot0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 6 );
        check( s0 != NULL );
        check( server_obj->in_place > 0 );
        rocketnet_message_server_send_packets( msg_server );
        check( server_obj->in_place == -1 - slot0 );
        check_not_asserted();
        check( rocketnet_message_server_finish( msg_server, slot0, 6 ) == 0 );
        check_asserted_once();
        check( server_obj->in_place == 0 );
        check( rocketnet_message_server_finish( msg_server, slot0, 6 ) == 0 );
        check_asserted_once();

        // Public API in-place start closed by send_packets / update also asserts on finish
        c0 = rocketnet_client_start_message( world.clients[0], 0, ROCKETNET_RELIABILITY_UNRELIABLE, 6 );
        check( c0 != NULL );
        rocketnet_client_send_packets( world.clients[0] );
        check_not_asserted();
        check( rocketnet_client_finish_message( world.clients[0], 6 ) == 0 );
        check_asserted_once();

        s0 = rocketnet_server_start_message( world.server, (uint64_t) slot0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 6 );
        check( s0 != NULL );
        rocketnet_server_send_packets( world.server );
        check_not_asserted();
        check( rocketnet_server_finish_message( world.server, 6 ) == 0 );
        check_asserted_once();
    }

    // Slot0 still exchanges messages cleanly
    finish_world_verify_round_trip( &world, 0, 0x81, 0x82 );
    finish_world_destroy( &world );

    // Direct Layer-7 endpoint tests (rn_endpoint_finish and rn_endpoint_in_place_finish)
    rocketnet_message_config_t msg_config;
    rocketnet_message_default_config( &msg_config );
    msg_config.reliable_messages_in_flight = 64;
    msg_config.packets_in_flight = 64;
    msg_config.send_queue_messages = 16;
    msg_config.receive_message_buffer_bytes = 4096;
    msg_config.max_message_bytes = 3 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    msg_config.send_message_buffer_bytes = 12288;
    msg_config.client_send_kbps = 0;
    msg_config.server_send_kbps = 0;

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rn_arena_t * arena = rn_arena_create( NULL, &msg_config, 1, &counters );
    check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    check( region != NULL );

    rn_endpoint_config_t ep_config;
    memset( &ep_config, 0, sizeof( ep_config ) );
    ep_config.config = &msg_config;
    ep_config.send_kbps = 0;
    ep_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, region, &ep_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    {
        finish_assert_scope_t scope;
        uint8_t frame_buf[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        memset( frame_buf, 0, sizeof( frame_buf ) );
        int payload_bytes = 99;

        // NULL endpoint or NULL arguments
        check( rn_endpoint_finish( NULL, 4, 0 ) == 0 );
        check_asserted_once();
        check( rn_endpoint_in_place_finish( NULL, frame_buf, 4, 1000.0, &payload_bytes ) == 0 );
        check_asserted_once();
        check( payload_bytes == 0 );

        // Unstarted endpoint: both finishes assert and return 0
        check( rn_endpoint_finish( &endpoint, 4, 0 ) == 0 );
        check_asserted_once();
        payload_bytes = 99;
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 4, 1000.0, &payload_bytes ) == 0 );
        check_asserted_once();
        check( payload_bytes == 0 );

        // Refused rn_endpoint_start returns NULL and leaves no reservation
        check( rn_endpoint_start( &endpoint, -1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == NULL );
        check_not_asserted();
        check( rn_endpoint_finish( &endpoint, 4, 0 ) == 0 );
        check_asserted_once();

        // Queued start open: calling rn_endpoint_in_place_finish asserts (mode mismatch),
        // then rn_endpoint_finish succeeds, and a second rn_endpoint_finish asserts
        uint8_t * ptr = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        check( ptr != NULL );
        memset( ptr, 0x55, 8 );
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 8, 1000.0, &payload_bytes ) == 0 );
        check_asserted_once();
        check( rn_endpoint_finish( &endpoint, 8, 0 ) != 0 );
        check_not_asserted();
        check( rn_endpoint_finish( &endpoint, 8, 0 ) == 0 );
        check_asserted_once();

        // Queued start -> abort -> finish asserts
        ptr = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        check( ptr != NULL );
        rn_endpoint_abort( &endpoint );
        check_not_asserted();
        check( rn_endpoint_finish( &endpoint, 8, 0 ) == 0 );
        check_asserted_once();

        // Queued start -> reset -> finish asserts
        ptr = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 8, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        check( ptr != NULL );
        rn_endpoint_reset( &endpoint, 1001.0 );
        check_not_asserted();
        check( rn_endpoint_finish( &endpoint, 8, 0 ) == 0 );
        check_asserted_once();

        // In-place start open: calling rn_endpoint_finish asserts (mode mismatch),
        // NULL payload or NULL payload_bytes asserts, valid rn_endpoint_in_place_finish succeeds,
        // and a second rn_endpoint_in_place_finish asserts
        const int header = rn_endpoint_in_place_header( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, 1001.0 );
        check( header >= 0 );
        rn_endpoint_in_place_begin( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8, header );
        check( rn_endpoint_finish( &endpoint, 8, 0 ) == 0 );
        check_asserted_once();
        check( rn_endpoint_in_place_finish( &endpoint, NULL, 8, 1001.0, &payload_bytes ) == 0 );
        check_asserted_once();
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 8, 1001.0, NULL ) == 0 );
        check_asserted_once();

        // Out-of-range bytes on in-place finish returns 0 without asserting (reservation stays open until aborted or finished)
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 0, 1001.0, &payload_bytes ) == 0 );
        check_not_asserted();
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 9, 1001.0, &payload_bytes ) == 0 );
        check_not_asserted();

        // Valid in-place finish consumes the reservation; a second call asserts
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 8, 1001.0, &payload_bytes ) != 0 );
        check_not_asserted();
        check( payload_bytes > 0 );
        rn_endpoint_in_place_sent( &endpoint, 1, payload_bytes, 1001.0 );
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 8, 1001.0, &payload_bytes ) == 0 );
        check_asserted_once();

        // In-place start -> abort -> finish asserts
        rn_endpoint_in_place_begin( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 8, header );
        rn_endpoint_abort( &endpoint );
        check_not_asserted();
        check( rn_endpoint_in_place_finish( &endpoint, frame_buf, 8, 1001.0, &payload_bytes ) == 0 );
        check_asserted_once();
    }

    rn_arena_destroy( arena );
}

// ---------------------------------------------------------------------------
// 3. Client and server transport packet finish
// ---------------------------------------------------------------------------

static void test_finish_requires_start_transport_packets()
{
    finish_world_t world_a;
    finish_world_t world_b;
    finish_world_create( &world_a, 2, 1, 0x73 );
    finish_world_create( &world_b, 2, 1, 0x84 );
    finish_world_connect( &world_a, 0, 301 );
    finish_world_connect( &world_b, 0, 302 );

    rocketnet_client_transport_t * client_a =
        rocketnet_message_client_transport( rocketnet_client_message_client( world_a.clients[0] ) );
    rocketnet_server_transport_t * server_a =
        rocketnet_message_server_transport( rocketnet_server_message_server( world_a.server ) );
    rocketnet_client_transport_t * client_b =
        rocketnet_message_client_transport( rocketnet_client_message_client( world_b.clients[0] ) );
    rocketnet_server_transport_t * server_b =
        rocketnet_message_server_transport( rocketnet_server_message_server( world_b.server ) );
    check( client_a != NULL && server_a != NULL && client_b != NULL && server_b != NULL );

    const int slot_a = rocketnet_client_index( world_a.clients[0] );
    const int empty_slot_a = 1 - slot_a;
    const int slot_b = rocketnet_client_index( world_b.clients[0] );

    {
        finish_assert_scope_t scope;

        // 3a. Zero-initialized / unstarted context asserts in Debug and returns 0 in Release
        rocketnet_packet_context_t zero_ctx;
        memset( &zero_ctx, 0, sizeof( zero_ctx ) );
        check( rocketnet_client_transport_finish_packet( client_a, &zero_ctx, 16 ) == 0 );
        check_asserted_once();
        memset( &zero_ctx, 0, sizeof( zero_ctx ) );
        check( rocketnet_server_transport_finish_packet( server_a, &zero_ctx, 16 ) == 0 );
        check_asserted_once();

        // 3b. Failed start_packet (invalid request_bytes or unconnected slot) zeroes context;
        // calling finish_packet on that context asserts in Debug and returns 0 in Release
        rocketnet_packet_context_t failed_ctx;
        check( rocketnet_client_transport_start_packet( client_a, 0, &failed_ctx ) == NULL );
        check_not_asserted();
        check( rocketnet_client_transport_finish_packet( client_a, &failed_ctx, 16 ) == 0 );
        check_asserted_once();

        check( rocketnet_client_transport_start_packet( client_a, ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES + 1, &failed_ctx ) == NULL );
        check_not_asserted();
        check( rocketnet_client_transport_finish_packet( client_a, &failed_ctx, 16 ) == 0 );
        check_asserted_once();

        check( rocketnet_server_transport_start_packet( server_a, slot_a, 0, &failed_ctx ) == NULL );
        check_not_asserted();
        check( rocketnet_server_transport_finish_packet( server_a, &failed_ctx, 16 ) == 0 );
        check_asserted_once();

        check( rocketnet_server_transport_start_packet( server_a, empty_slot_a, 16, &failed_ctx ) == NULL );
        check_not_asserted();
        check( rocketnet_server_transport_finish_packet( server_a, &failed_ctx, 16 ) == 0 );
        check_asserted_once();

        // 3c. Valid start_packet -> finish_packet succeeds; a second finish_packet on the consumed
        // context (or a saved copy of the context before finish zeroed frame) asserts and returns 0
        rocketnet_packet_context_t c_ctx;
        uint8_t * c_frame = rocketnet_client_transport_start_packet( client_a, 16, &c_ctx );
        check( c_frame != NULL );
        memset( c_frame + c_ctx.payload_index, 0x11, 16 );
        rocketnet_packet_context_t c_copy = c_ctx;
        check( rocketnet_client_transport_finish_packet( client_a, &c_ctx, 16 ) > 0 );
        check_not_asserted();
        check( rocketnet_client_transport_finish_packet( client_a, &c_ctx, 16 ) == 0 );
        check_asserted_once();
        check( rocketnet_client_transport_finish_packet( client_a, &c_copy, 16 ) == 0 );
        check_asserted_once();

        rocketnet_packet_context_t s_ctx;
        uint8_t * s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
        check( s_frame != NULL );
        memset( s_frame + s_ctx.payload_index, 0x22, 16 );
        rocketnet_packet_context_t s_copy = s_ctx;
        check( rocketnet_server_transport_finish_packet( server_a, &s_ctx, 16 ) > 0 );
        check_not_asserted();
        check( rocketnet_server_transport_finish_packet( server_a, &s_ctx, 16 ) == 0 );
        check_asserted_once();
        check( rocketnet_server_transport_finish_packet( server_a, &s_copy, 16 ) == 0 );
        check_asserted_once();

        // 3d. Valid start_packet -> abort_packet; subsequent finish_packet asserts and returns 0
        c_frame = rocketnet_client_transport_start_packet( client_a, 16, &c_ctx );
        check( c_frame != NULL );
        c_copy = c_ctx;
        rocketnet_client_transport_abort_packet( client_a, &c_ctx );
        check_not_asserted();
        check( rocketnet_client_transport_finish_packet( client_a, &c_ctx, 16 ) == 0 );
        check_asserted_once();
        check( rocketnet_client_transport_finish_packet( client_a, &c_copy, 16 ) == 0 );
        check_asserted_once();

        s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
        check( s_frame != NULL );
        s_copy = s_ctx;
        rocketnet_server_transport_abort_packet( server_a, &s_ctx );
        check_not_asserted();
        check( rocketnet_server_transport_finish_packet( server_a, &s_ctx, 16 ) == 0 );
        check_asserted_once();
        check( rocketnet_server_transport_finish_packet( server_a, &s_copy, 16 ) == 0 );
        check_asserted_once();

        // 3e. Mutated context fields and wrong-owner leases assert in Debug and return 0 in Release
        c_frame = rocketnet_client_transport_start_packet( client_a, 16, &c_ctx );
        check( c_frame != NULL );
        rocketnet_packet_context_t bad = c_ctx;
        bad.packet_id ^= 1ULL;
        check( rocketnet_client_transport_finish_packet( client_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = c_ctx;
        bad.frame += 16;
        check( rocketnet_client_transport_finish_packet( client_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = c_ctx;
        bad.payload_index += 16;
        check( rocketnet_client_transport_finish_packet( client_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = c_ctx;
        bad.payload_capacity -= 8;
        check( rocketnet_client_transport_finish_packet( client_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = c_ctx;
        bad.sequence += 1;
        check( rocketnet_client_transport_finish_packet( client_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = c_ctx;
        bad.client_index = 0;
        check( rocketnet_client_transport_finish_packet( client_a, &bad, 16 ) == 0 );
        check_asserted_once();
        // Wrong owner: pass client_a's lease to client_b
        bad = c_ctx;
        check( rocketnet_client_transport_finish_packet( client_b, &bad, 16 ) == 0 );
        check_asserted_once();
        // The unmodified lease on client_a is still open; abort it cleanly
        rocketnet_client_transport_abort_packet( client_a, &c_ctx );
        check_not_asserted();

        s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
        check( s_frame != NULL );
        bad = s_ctx;
        bad.packet_id ^= 1ULL;
        check( rocketnet_server_transport_finish_packet( server_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = s_ctx;
        bad.frame += 16;
        check( rocketnet_server_transport_finish_packet( server_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = s_ctx;
        bad.payload_index += 16;
        check( rocketnet_server_transport_finish_packet( server_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = s_ctx;
        bad.payload_capacity -= 8;
        check( rocketnet_server_transport_finish_packet( server_a, &bad, 16 ) == 0 );
        check_asserted_once();
        bad = s_ctx;
        bad.client_index = empty_slot_a;
        check( rocketnet_server_transport_finish_packet( server_a, &bad, 16 ) == 0 );
        check_asserted_once();
        // Wrong owner: pass server_a's lease to server_b
        bad = s_ctx;
        bad.client_index = slot_b;
        check( rocketnet_server_transport_finish_packet( server_b, &bad, 16 ) == 0 );
        check_asserted_once();
        rocketnet_server_transport_abort_packet( server_a, &s_ctx );
        check_not_asserted();

        // 3f. Out-of-range payload_bytes (0 and payload_capacity + 1) on a valid lease
        // aborts the lease and returns 0 WITHOUT firing the missing-start assertion,
        // and a second finish_packet on that consumed context DOES assert
        for ( int b = 0; b < 2; b++ )
        {
            c_frame = rocketnet_client_transport_start_packet( client_a, 16, &c_ctx );
            check( c_frame != NULL );
            const int bad_bytes = b == 0 ? 0 : c_ctx.payload_capacity + 1;
            c_copy = c_ctx;
            check( rocketnet_client_transport_finish_packet( client_a, &c_ctx, bad_bytes ) == 0 );
            check_not_asserted();
            check( rocketnet_client_transport_finish_packet( client_a, &c_copy, 16 ) == 0 );
            check_asserted_once();

            s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
            check( s_frame != NULL );
            const int s_bad_bytes = b == 0 ? 0 : s_ctx.payload_capacity + 1;
            s_copy = s_ctx;
            check( rocketnet_server_transport_finish_packet( server_a, &s_ctx, s_bad_bytes ) == 0 );
            check_not_asserted();
            check( rocketnet_server_transport_finish_packet( server_a, &s_copy, 16 ) == 0 );
            check_asserted_once();
        }

        // 3g. Open lease invalidated by send_packets or update. The client
        // finish asserts. The server lease was reset: finish returns 0, counts
        // one api_misuse, and does not assert.
        c_frame = rocketnet_client_transport_start_packet( client_a, 16, &c_ctx );
        check( c_frame != NULL );
        rocketnet_client_transport_send_packets( client_a );
        check( rocketnet_client_transport_finish_packet( client_a, &c_ctx, 16 ) == 0 );
        check_asserted_once();

        s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
        check( s_frame != NULL );
        rocketnet_server_transport_send_packets( server_a );
        {
            const uint64_t misuses = rocketnet_api_misuse_count();
            check( rocketnet_server_transport_finish_packet( server_a, &s_ctx, 16 ) == 0 );
            check_not_asserted();
            check( rocketnet_api_misuse_count() == misuses + 1 );
        }

        const double c_origin = rocketnet_client_message_client( world_a.clients[0] )->object.clock_origin;
        const double s_origin = rocketnet_server_message_server( world_a.server )->object.clock_origin;
        world_a.time += 0.01;
        g_finish_test_now = world_a.time;
        c_frame = rocketnet_client_transport_start_packet( client_a, 16, &c_ctx );
        check( c_frame != NULL );
        rocketnet_client_transport_update( client_a, world_a.time - c_origin );
        check( rocketnet_client_transport_finish_packet( client_a, &c_ctx, 16 ) == 0 );
        check_asserted_once();

        s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
        check( s_frame != NULL );
        rocketnet_server_transport_update( server_a, world_a.time - s_origin );
        {
            const uint64_t misuses = rocketnet_api_misuse_count();
            check( rocketnet_server_transport_finish_packet( server_a, &s_ctx, 16 ) == 0 );
            check_not_asserted();
            check( rocketnet_api_misuse_count() == misuses + 1 );
        }

        // Verify the two valid 16-byte packets from 3c arrived intact at the transport layer,
        // and advance one more transport tick so their payload batch is cleared before message-layer pumping
        const int * rx_clients = NULL;
        uint8_t * const * rx_data = NULL;
        const int * rx_bytes = NULL;
        const int s_got = rocketnet_server_transport_payloads( server_a, &rx_clients, &rx_data, &rx_bytes );
        check( s_got == 1 && rx_clients[0] == slot_a && rx_bytes[0] == 16 && rx_data[0][0] == 0x11 && rx_data[0][15] == 0x11 );
        const int c_got = rocketnet_client_transport_payloads( client_a, &rx_data, &rx_bytes );
        check( c_got == 1 && rx_bytes[0] == 16 && rx_data[0][0] == 0x22 && rx_data[0][15] == 0x22 );

        world_a.time += 0.01;
        g_finish_test_now = world_a.time;
        rocketnet_client_transport_update( client_a, world_a.time - c_origin );
        rocketnet_server_transport_update( server_a, world_a.time - s_origin );
        world_a.time += 0.01;

        // Consumed lease (finished or aborted in state 3) followed by reset
        // (send_packets or update) must still assert on finish of a saved copy,
        // not take the retired() misuse path.
        s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
        check( s_frame != NULL );
        memset( s_frame + s_ctx.payload_index, 0x23, 16 );
        s_copy = s_ctx;
        check( rocketnet_server_transport_finish_packet( server_a, &s_ctx, 16 ) > 0 );
        check_not_asserted();
        rocketnet_server_transport_send_packets( server_a );
        {
            const uint64_t misuses = rocketnet_api_misuse_count();
            check( rocketnet_server_transport_finish_packet( server_a, &s_copy, 16 ) == 0 );
            check_asserted_once();
            check( rocketnet_api_misuse_count() == misuses );
        }
        world_a.time += 0.01;
        g_finish_test_now = world_a.time;
        rocketnet_client_transport_update( client_a, world_a.time - c_origin );
        rocketnet_server_transport_update( server_a, world_a.time - s_origin );
        (void) rocketnet_client_transport_payloads( client_a, &rx_data, &rx_bytes );
        world_a.time += 0.01;
        g_finish_test_now = world_a.time;
        rocketnet_client_transport_update( client_a, world_a.time - c_origin );
        rocketnet_server_transport_update( server_a, world_a.time - s_origin );

        s_frame = rocketnet_server_transport_start_packet( server_a, slot_a, 16, &s_ctx );
        check( s_frame != NULL );
        s_copy = s_ctx;
        rocketnet_server_transport_abort_packet( server_a, &s_ctx );
        check_not_asserted();
        rocketnet_server_transport_send_packets( server_a );
        {
            const uint64_t misuses = rocketnet_api_misuse_count();
            check( rocketnet_server_transport_finish_packet( server_a, &s_copy, 16 ) == 0 );
            check_asserted_once();
            check( rocketnet_api_misuse_count() == misuses );
        }
        world_a.time += 0.01;
    }

    // 3h. After all invalid packet finishes, world_a still delivers messages end-to-end
    finish_world_verify_round_trip( &world_a, 0, 0x91, 0x92 );

    // 3i. Open lease invalidated by stop. The client finish asserts. The
    // server lease was reset: finish returns 0, counts one api_misuse, and
    // does not assert.
    {
        rocketnet_packet_context_t c_ctx;
        uint8_t * c_frame = rocketnet_client_transport_start_packet( client_b, 16, &c_ctx );
        check( c_frame != NULL );
        rocketnet_client_transport_stop( client_b );
        rn_test_client_transport_wait_stopped( client_b );

        rocketnet_packet_context_t s_ctx;
        uint8_t * s_frame = rocketnet_server_transport_start_packet( server_b, slot_b, 16, &s_ctx );
        check( s_frame != NULL );
        rocketnet_server_transport_stop( server_b );
        check( rocketnet_server_transport_stopped( server_b ) );

        finish_assert_scope_t scope;
        check( rocketnet_client_transport_finish_packet( client_b, &c_ctx, 16 ) == 0 );
        check_asserted_once();
        {
            const uint64_t misuses = rocketnet_api_misuse_count();
            check( rocketnet_server_transport_finish_packet( server_b, &s_ctx, 16 ) == 0 );
            check_not_asserted();
            check( rocketnet_api_misuse_count() == misuses + 1 );
        }
    }

    finish_world_destroy( &world_a );
    finish_world_destroy( &world_b );
}

REGISTER_UNIT_TEST( test_finish_requires_start_public_client_and_server );
REGISTER_UNIT_TEST( test_finish_requires_start_message_objects_and_endpoint );
REGISTER_UNIT_TEST( test_finish_requires_start_transport_packets );
