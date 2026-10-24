/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Established keep-alives are decided at the send_packets flush, not in
// the update sweep: a payload the fill phase sends to a client in the
// same update suppresses that client's keep-alive, an idle established
// client still receives keep-alives at the send rate, and a client that
// has not acknowledged a keep-alive is never deferred, so payload
// traffic cannot starve its establishment (#90). One step-driven client
// on the in-memory loopback transport, caller-supplied time from 1000.0;
// no socket, no thread, no sleep and no real time. Server-side packet
// counts and the keep_alive_acked latch are observed with bounded
// polling (keep driving updates until the value lands or the bound
// expires), never assumed synchronous.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include "suite_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#define KA_PROTOCOL_ID  0x6B11C1C1E5EED0F1ULL
#define KA_DT           0.1                 // one send interval per tick (send rate 10)
#define KA_TIMEOUT      3                   // connect token timeout, seconds

static double ka_now = 1000.0;
static double ka_clock( void * ) { return ka_now; }

struct ka_world_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    int server_index;
    double time;
};

static void ka_world_create( ka_world_t * world )
{
    memset( world, 0, sizeof( *world ) );
    rocketnet_quiet( 1 );
    world->time = 1000.0;
    ka_now = world->time;
    world->server_index = -1;

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, world->backend_public, world->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( world->server_public, world->server_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = KA_PROTOCOL_ID;
    server_config.max_clients = 4;
    memcpy( server_config.backend_public_key, world->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    world->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( world->server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rocketnet_server_public_key( world->server, world->server_public );
    check( rocketnet_server_start( world->server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_test_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = ka_clock;
    create_status = ROCKETNET_STATUS_OK;
    world->client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( world->client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
}

static void ka_world_destroy( ka_world_t * world )
{
    rn_test_client_stop_and_destroy( world->client );
    rn_test_server_stop_and_destroy( world->server );
}

static void ka_begin_connect( ka_world_t * world )
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( world->server ) );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( world->backend_secret, world->server_public,
        KA_PROTOCOL_ID, addresses, 1, 0x6B10ULL, 1000, 1ULL << 40, KA_TIMEOUT, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    check( rocketnet_client_connect( world->client, token_string ) == ROCKETNET_STATUS_OK );
}

static void ka_pump( ka_world_t * world )
{
    ka_now = world->time;
    rocketnet_client_update( world->client, world->time );
    rocketnet_client_send_packets( world->client );
    rocketnet_server_update( world->server, world->time );
    rocketnet_server_send_packets( world->server );
    world->time += KA_DT;
}

// Drive until the server has assigned the client's slot (the single
// client takes slot 0), stopping before the client is driven again:
// the first keep-alive is on its way but nothing the client has sent
// can acknowledge it (its last send was the handshake response, which
// carries no ack block), so the server's keep_alive_acked latch is
// unset -- the unestablished window the #90 tests guard.
static void ka_connect_until_server_slot( ka_world_t * world )
{
    ka_begin_connect( world );
    for ( int step = 0; step < 400 && !rocketnet_server_client_connected( world->server, 0 ); step++ )
        ka_pump( world );
    check( rocketnet_server_client_connected( world->server, 0 ) != 0 );
    world->server_index = 0;
}

static void ka_connect( ka_world_t * world )
{
    ka_begin_connect( world );

    for ( int step = 0; step < 400 && rocketnet_client_state( world->client ) != ROCKETNET_CLIENT_STATE_CONNECTED; step++ )
    {
        ka_now = world->time;
        rocketnet_client_update( world->client, world->time );
        rocketnet_client_send_packets( world->client );
        rocketnet_server_update( world->server, world->time );
        rocketnet_server_send_packets( world->server );
        world->time += KA_DT;
    }
    check( rocketnet_client_state( world->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    world->server_index = rocketnet_client_index( world->client );
    check( world->server_index >= 0 );
}

static void ka_queue_payload( ka_world_t * world, uint16_t value )
{
    uint8_t * out = rocketnet_server_start_message( world->server, (uint64_t) world->server_index, 1,
                                                    ROCKETNET_RELIABILITY_UNRELIABLE, 2 );
    check( out != NULL );
    if ( !out )
        return;
    out[0] = (uint8_t) ( value >> 8 );
    out[1] = (uint8_t) value;
    check( rocketnet_server_finish_message( world->server, 2 ) != 0 );
}

static int ka_keep_alive_acked( ka_world_t * world )
{
#if ROCKETNET_DEV
    struct rocketnet_delivery_capture_t capture;
    rocketnet_server_delivery_capture( world->server, world->server_index, &capture );
    return capture.keep_alive_acked;
#else
    (void) world;
    return 1;
#endif
}

// (b) then (a): with the latch unset, payload traffic in the same update
// does not suppress the keep-alive (server sends payload + keep-alive
// per tick); once the client has acknowledged a keep-alive, the same
// traffic suppresses it (the client receives exactly the payload
// packets, no second sealed packet per tick).
static void test_keep_alive_deferred_only_once_established()
{
    ka_world_t world;
    ka_world_create( &world );
    ka_connect_until_server_slot( &world );
    check( !ka_keep_alive_acked( &world ) );            // the window is real: no ack has reached the server

    // (b) server-only ticks: the client is never driven, so it sends
    // nothing and the latch stays unset. Bounded observation of the
    // server's per-client packets_sent: payload + keep-alive per tick.

    const uint64_t sent_at_start = rn_server_client_stats_of( world.server, world.server_index ).packets_sent;
    const int unacked_ticks = 12;
    for ( int tick = 0; tick < unacked_ticks; tick++ )
    {
        rocketnet_server_update( world.server, world.time );
        ka_queue_payload( &world, (uint16_t) tick );
        rocketnet_server_send_packets( world.server );
        world.time += KA_DT;
    }
    check( !ka_keep_alive_acked( &world ) );
    uint64_t sent = rn_server_client_stats_of( world.server, world.server_index ).packets_sent;
    for ( int poll = 0; poll < 15 && sent - sent_at_start < (uint64_t) ( 2 * unacked_ticks ); poll++ )
    {
        rocketnet_server_update( world.server, world.time );
        rocketnet_server_send_packets( world.server );
        world.time += KA_DT;
        sent = rn_server_client_stats_of( world.server, world.server_index ).packets_sent;
    }
    check( sent - sent_at_start >= (uint64_t) ( 2 * unacked_ticks ) );   // a keep-alive rode with every payload

    // The silent client's queue overflowed while it was not driven, so
    // its drain now reports messages not received and the connection
    // ends under the undrained-loss rule; phase (a) therefore runs on a
    // fresh connection rather than reviving this one.
    ka_world_destroy( &world );
    ka_world_create( &world );
    ka_connect( &world );

    // (a) drive the client again: within a bounded number of ticks its
    // acknowledgment lands, and from then on a payload tick delivers
    // exactly one packet to it -- the payload, keep-alive suppressed.

    int suppressed_streak = 0;
    for ( int tick = 0; tick < 60 && suppressed_streak < 5; tick++ )
    {
        ka_now = world.time;
        const uint64_t received_before = rn_client_stats_of( world.client ).packets_received;
        const uint64_t payloads_before = rn_client_stats_of( world.client ).payloads_received;
        rocketnet_client_update( world.client, world.time );
        rocketnet_client_send_packets( world.client );
        // drain the client's deliveries before the next update: an
        // undrained delivery ends the connection (undrained-loss rule)
        while ( rocketnet_client_receive_messages( world.client ) != NULL )
        {
        }
        const uint64_t received = rn_client_stats_of( world.client ).packets_received - received_before;
        const uint64_t payloads = rn_client_stats_of( world.client ).payloads_received - payloads_before;

        rocketnet_server_update( world.server, world.time );
        ka_queue_payload( &world, (uint16_t) ( 100 + tick ) );
        rocketnet_server_send_packets( world.server );
        world.time += KA_DT;

        if ( ka_keep_alive_acked( &world ) && payloads >= 1 && received == payloads )
            suppressed_streak++;
        else
            suppressed_streak = 0;
    }
    check( ka_keep_alive_acked( &world ) );
    check( suppressed_streak >= 5 );

    ka_world_destroy( &world );
}

// (c): an idle established client still receives keep-alives at the
// send rate, and a client that goes silent still times out on the
// server by the existing receive-based rule.
static void test_keep_alive_deferred_idle_cadence_and_timeout()
{
    ka_world_t world;
    ka_world_create( &world );
    ka_connect( &world );

    // establish the latch with bounded driving, payloads flowing
    for ( int tick = 0; tick < 60 && !ka_keep_alive_acked( &world ); tick++ )
    {
        ka_now = world.time;
        rocketnet_client_update( world.client, world.time );
        rocketnet_client_send_packets( world.client );
        rocketnet_server_update( world.server, world.time );
        ka_queue_payload( &world, (uint16_t) tick );
        rocketnet_server_send_packets( world.server );
        world.time += KA_DT;
    }
    check( ka_keep_alive_acked( &world ) );

    // idle: no payloads queued. Each tick the client must receive
    // exactly one packet -- the deferred keep-alive, still written.
    int idle_streak = 0;
    for ( int tick = 0; tick < 40 && idle_streak < 5; tick++ )
    {
        ka_now = world.time;
        const uint64_t received_before = rn_client_stats_of( world.client ).packets_received;
        rocketnet_client_update( world.client, world.time );
        rocketnet_client_send_packets( world.client );
        const uint64_t received = rn_client_stats_of( world.client ).packets_received - received_before;

        rocketnet_server_update( world.server, world.time );
        rocketnet_server_send_packets( world.server );
        world.time += KA_DT;

        if ( received == 1 )
            idle_streak++;
        else
            idle_streak = 0;
    }
    check( idle_streak >= 5 );
    check( rocketnet_client_state( world.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( world.server, world.server_index ) != 0 );

    // silence: the client stops sending entirely. The server's timeout
    // reads the client's last receive, which keep-alives never refresh,
    // so the slot must fall within the token timeout plus a bound.
    int disconnected = 0;
    for ( int tick = 0; tick < ( KA_TIMEOUT + 3 ) * 10 && !disconnected; tick++ )
    {
        rocketnet_server_update( world.server, world.time );
        rocketnet_server_send_packets( world.server );
        world.time += KA_DT;
        disconnected = !rocketnet_server_client_connected( world.server, world.server_index );
    }
    check( disconnected );

    ka_world_destroy( &world );
}

REGISTER_UNIT_TEST( test_keep_alive_deferred_only_once_established );
REGISTER_UNIT_TEST( test_keep_alive_deferred_idle_cadence_and_timeout );
