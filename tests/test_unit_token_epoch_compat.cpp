/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Connects the way an application following the v1.0.0 header could: a
// configured token_epoch_unix_seconds is "the Unix time, in seconds, at zero
// of the time the caller passes to rocketnet_client_update" (and to
// rocketnet_server_update), set on both configs, and the backend mints the
// token at its real Unix time, which is that epoch plus the caller's time.
// Each case must reach CONNECTED on v1.0.0 and on every later point release.
// The default epoch 0 reads the wall clock at connect, unchanged since
// v1.0.0. The last case is the usage docs/SECURITY.md excludes ("an explicit epoch
// must represent Unix time at that clock's zero, not Unix time at the current
// update"), and pins what connect does with it now. Public API only, step
// driven over the in-memory loopback hub; time is an argument from 1000.0,
// with no socket, sleep or thread.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"

#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#include "loopback_hub.h"

#define check rocketnet_unit_check

#define EPOCH_COMPAT_EPOCH      1700000000ULL
#define EPOCH_COMPAT_PROTOCOL   0x3C0A7E5B91D2F468ULL
#define EPOCH_COMPAT_LIFETIME   30

static double epoch_compat_now = 1000.0;
static double epoch_compat_clock( void * ) { return epoch_compat_now; }

struct epoch_compat_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
};

// a started loopback server on server_epoch and a step-driven client on
// client_epoch, neither updated yet

static void epoch_compat_create( epoch_compat_t * t, uint64_t server_epoch, uint64_t client_epoch )
{
    memset( t, 0, sizeof( *t ) );
    rocketnet_quiet( 1 );
    epoch_compat_now = 1000.0;

    check( rocketnet_generate_backend_keypair( t->backend_public, t->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = EPOCH_COMPAT_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, t->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    server_config.token_epoch_unix_seconds = server_epoch;
    t->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( t->server != NULL );
    check( rocketnet_server_start( t->server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = epoch_compat_clock;
    client_config.token_epoch_unix_seconds = client_epoch;
    t->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( t->client != NULL );
}

static void epoch_compat_destroy( epoch_compat_t * t )
{
    rn_test_client_stop_and_destroy( t->client );
    rn_test_server_stop_and_destroy( t->server );
    rocketnet_quiet( 0 );
}

// the backend's token, created at create (Unix seconds) and living
// EPOCH_COMPAT_LIFETIME seconds

static void epoch_compat_token( const epoch_compat_t * t, uint64_t create, char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES] )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( t->server ) );

    struct rocketnet_connect_token_config_t token_config;
    check( rocketnet_connect_token_default_config( &token_config ) == ROCKETNET_STATUS_OK );
    memcpy( token_config.backend_secret_key, t->backend_secret, sizeof( token_config.backend_secret_key ) );
    rocketnet_server_public_key( t->server, token_config.server_public_key );
    token_config.server_address_ipv4 = address;
    token_config.client_id = 0xC0FFEE;
    token_config.protocol_id = EPOCH_COMPAT_PROTOCOL;
    token_config.expire_seconds = EPOCH_COMPAT_LIFETIME;
    token_config.timeout_seconds = 10;
    token_config.unix_timestamp = create;
    check( rocketnet_create_connect_token( &token_config, token ) == ROCKETNET_STATUS_OK );
}

// one frame of both endpoints at time

static void epoch_compat_frame( epoch_compat_t * t, double time, bool client_too )
{
    epoch_compat_now = time;
    if ( client_too )
    {
        rocketnet_client_update( t->client, time );
        rocketnet_client_send_packets( t->client );
    }
    rocketnet_server_update( t->server, time );
    rocketnet_server_send_packets( t->server );
}

// frames from time in 0.1 s steps until the client is connected, at most
// five seconds; returns the client's final state

static enum rocketnet_client_state_t epoch_compat_pump( epoch_compat_t * t, double time )
{
    for ( int i = 0; i < 50; i++ )
    {
        epoch_compat_frame( t, time + 0.1 * i, true );
        if ( rocketnet_client_state( t->client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            break;
    }
    return rocketnet_client_state( t->client );
}

// a game clock that starts at 1000: both endpoints run a frame at 1000.0,
// then the client connects with a token the backend minted at Unix now,
// which is the epoch plus 1000

static void test_token_epoch_compat_clock_starting_at_1000_connects()
{
    epoch_compat_t t;
    epoch_compat_create( &t, EPOCH_COMPAT_EPOCH, EPOCH_COMPAT_EPOCH );

    epoch_compat_frame( &t, 1000.0, true );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    epoch_compat_token( &t, EPOCH_COMPAT_EPOCH + 1000, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_OK );
    check( epoch_compat_pump( &t, 1000.1 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    epoch_compat_destroy( &t );
}

// a process that has run three days, far past the token's lifetime: the
// token is minted at Unix now, the epoch plus the caller's time

static void test_token_epoch_compat_long_running_process_connects()
{
    epoch_compat_t t;
    epoch_compat_create( &t, EPOCH_COMPAT_EPOCH, EPOCH_COMPAT_EPOCH );

    const double later = 1000.0 + 3.0 * 86400.0;
    epoch_compat_frame( &t, 1000.0, true );
    epoch_compat_frame( &t, later, true );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    epoch_compat_token( &t, EPOCH_COMPAT_EPOCH + (uint64_t) later, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_OK );
    check( epoch_compat_pump( &t, later + 0.1 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    epoch_compat_destroy( &t );
}

// a client connected before its first update while the game clock reads
// 1000 (the server has been running): the token minted at Unix now connects

static void test_token_epoch_compat_connect_before_first_update_connects()
{
    epoch_compat_t t;
    epoch_compat_create( &t, EPOCH_COMPAT_EPOCH, EPOCH_COMPAT_EPOCH );

    epoch_compat_frame( &t, 1000.0, false );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    epoch_compat_token( &t, EPOCH_COMPAT_EPOCH + 1000, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_OK );
    check( epoch_compat_pump( &t, 1000.1 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    epoch_compat_destroy( &t );
}

// the default epoch 0 on both ends: connect reads the wall clock (the suite's
// fixed clock) and the client's update time does not enter it, as in v1.0.0.
// A token that expires one second after the wall clock connects at a game
// clock of 8200; one that expires at the wall clock is refused

static void test_token_epoch_compat_epoch_zero_reads_the_wall_clock()
{
    epoch_compat_t t;
    epoch_compat_create( &t, 0, 0 );

    rocketnet_client_update( t.client, 1000.0 );
    epoch_compat_frame( &t, 8200.0, true );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    epoch_compat_token( &t, ROCKETNET_UNIT_WALL_CLOCK - EPOCH_COMPAT_LIFETIME, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );

    epoch_compat_token( &t, ROCKETNET_UNIT_WALL_CLOCK + 1 - EPOCH_COMPAT_LIFETIME, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_OK );
    check( epoch_compat_pump( &t, 8200.1 ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    epoch_compat_destroy( &t );
}

// outside the v1.0.0 contract: the client's epoch is set to Unix now (here
// the suite's fixed wall clock) while its game clock reads 8200, against a
// server on epoch 0 that started at 8200, so it reads the same wall clock.
// v1.0.0 checked the epoch plus zero and connected; connect now checks the
// epoch plus 8200 and refuses the token

static void test_token_epoch_compat_epoch_set_to_now_is_refused()
{
    epoch_compat_t t;
    epoch_compat_create( &t, 0, ROCKETNET_UNIT_WALL_CLOCK );

    rocketnet_client_update( t.client, 1000.0 );
    epoch_compat_frame( &t, 8200.0, true );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    epoch_compat_token( &t, ROCKETNET_UNIT_WALL_CLOCK, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_state( t.client ) == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );

    epoch_compat_destroy( &t );
}

REGISTER_UNIT_TEST( test_token_epoch_compat_clock_starting_at_1000_connects );
REGISTER_UNIT_TEST( test_token_epoch_compat_long_running_process_connects );
REGISTER_UNIT_TEST( test_token_epoch_compat_connect_before_first_update_connects );
REGISTER_UNIT_TEST( test_token_epoch_compat_epoch_zero_reads_the_wall_clock );
REGISTER_UNIT_TEST( test_token_epoch_compat_epoch_set_to_now_is_refused );

#endif // #if ROCKETNET_DEV
