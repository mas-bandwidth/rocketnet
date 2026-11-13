/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The token clock, token_epoch_unix_seconds plus the whole seconds of the
// caller's time, stops at the largest timestamp: an epoch so near it that
// the sum would pass it, or a finite time of 2^64 seconds or more, reads as
// UINT64_MAX, and every token has expired. The update refuses a time above
// 2^52 seconds, so only the epoch can push a public update past the largest
// timestamp. The server's check reads it at each update, and public
// connect's at connect. Time is an argument, step driven over the in-memory
// loopback transport, with no socket, sleep or thread.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#include "loopback_hub.h"

#define check rocketnet_unit_check

#define TOKEN_CLOCK_PROTOCOL    0x3C51A0E7B2D49F86ULL
#define TOKEN_CLOCK_EPOCH       1700000000ULL

struct token_clock_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
};

// a started loopback server and a step-driven client, each on its own token
// epoch, not yet updated

static void token_clock_create( token_clock_t * t, uint64_t server_epoch, uint64_t client_epoch )
{
    memset( t, 0, sizeof( *t ) );
    rocketnet_quiet( 1 );

    check( rocketnet_generate_backend_keypair( t->backend_public, t->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = TOKEN_CLOCK_PROTOCOL;
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
    client_config.token_epoch_unix_seconds = client_epoch;
    t->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( t->client != NULL );
}

static void token_clock_destroy( token_clock_t * t )
{
    rn_test_client_stop_and_destroy( t->client );
    rn_test_server_stop_and_destroy( t->server );
    rocketnet_quiet( 0 );
}

// a token for the server, minted with the public minter thirty seconds before
// it expires at expire

static void token_clock_token( const token_clock_t * t, uint64_t expire, char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES] )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( t->server ) );

    struct rocketnet_connect_token_config_t token_config;
    check( rocketnet_connect_token_default_config( &token_config ) == ROCKETNET_STATUS_OK );
    memcpy( token_config.backend_secret_key, t->backend_secret, sizeof( token_config.backend_secret_key ) );
    rocketnet_server_public_key( t->server, token_config.server_public_key );
    token_config.server_address_ipv4 = address;
    token_config.client_id = 0xC10C;
    token_config.protocol_id = TOKEN_CLOCK_PROTOCOL;
    token_config.expire_seconds = 30;
    token_config.timeout_seconds = 10;
    token_config.unix_timestamp = expire - 30;
    check( rocketnet_create_connect_token( &token_config, token ) == ROCKETNET_STATUS_OK );
}

// the client's clock admits a token that expires at expire, and the server's,
// stepped from server_time, refuses it as expired: no connection is accepted

static void token_clock_server_refuses( uint64_t server_epoch, double server_time, uint64_t client_epoch, double client_time, uint64_t expire )
{
    token_clock_t t;
    token_clock_create( &t, server_epoch, client_epoch );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    token_clock_token( &t, expire, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_OK );

    for ( int step = 0; step < 12; ++step )
    {
        rocketnet_client_update( t.client, client_time + 0.1 * step );
        rocketnet_client_send_packets( t.client );
        rocketnet_server_update( t.server, server_time + 0.1 * step );
        rocketnet_server_send_packets( t.server );
    }

    const struct rocketnet_counters_t * counters = rocketnet_server_transport_counter_block( t.server );
    check( counters->values[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] > 0 );
    check( rn_server_stats_of( t.server ).connections_accepted == 0 );
    check( rocketnet_client_state( t.client ) != ROCKETNET_CLIENT_STATE_CONNECTED );

    token_clock_destroy( &t );
}

// a client updated at client_time refuses a token that expires at expire

static void token_clock_client_refuses( uint64_t client_epoch, double client_time, uint64_t expire )
{
    token_clock_t t;
    token_clock_create( &t, 0, client_epoch );

    rocketnet_client_update( t.client, client_time );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    token_clock_token( &t, expire, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    check( rocketnet_client_state( t.client ) == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );

    token_clock_destroy( &t );
}

// the server: an epoch five seconds short of the largest timestamp, updated
// at 10.0; update times just under the largest the clock accepts
// (ROCKETNET_UPDATE_TIME_MAX_SECONDS, 2^52 seconds: stepped up from two
// seconds short of it, the last step rounds to a second short) on a
// configured epoch; and the same at the first update of a server whose
// epoch is read from the wall clock. The client's
// own clock admits the token each time. An update time of 2^64 seconds or
// more no longer reaches the token clock (the update refuses a time above
// 2^52 seconds), so its saturation there is checked on
// rocketnet_platform_token_clock itself.

static void test_token_clock_saturates_server_past_the_largest_timestamp()
{
    token_clock_server_refuses( UINT64_MAX - 5, 10.0, 15, 10.0, 40 );
}

static void test_token_clock_saturates_server_time_at_the_update_ceiling()
{
    check( rocketnet_platform_token_clock( TOKEN_CLOCK_EPOCH, 1.0e20 ) == UINT64_MAX );
    check( rocketnet_platform_token_clock( TOKEN_CLOCK_EPOCH, ROCKETNET_UPDATE_TIME_MAX_SECONDS ) == TOKEN_CLOCK_EPOCH + ( 1ULL << 52 ) );
    token_clock_server_refuses( TOKEN_CLOCK_EPOCH, ROCKETNET_UPDATE_TIME_MAX_SECONDS - 2.0, TOKEN_CLOCK_EPOCH - 1000, 1000.0, TOKEN_CLOCK_EPOCH + 20 );
}

static void test_token_clock_saturates_server_wall_clock_epoch()
{
    token_clock_server_refuses( 0, ROCKETNET_UPDATE_TIME_MAX_SECONDS - 2.0, ROCKETNET_UNIT_WALL_CLOCK - 1000, 1000.0, ROCKETNET_UNIT_WALL_CLOCK + 20 );
}

// public connect: an epoch five seconds short of the largest timestamp,
// updated at 1000.0, and the largest update time the clock accepts on a
// configured epoch

static void test_token_clock_saturates_client_past_the_largest_timestamp()
{
    token_clock_client_refuses( UINT64_MAX - 5, 1000.0, 2000 );
}

static void test_token_clock_saturates_client_time_at_the_update_ceiling()
{
    token_clock_client_refuses( TOKEN_CLOCK_EPOCH, ROCKETNET_UPDATE_TIME_MAX_SECONDS, TOKEN_CLOCK_EPOCH + 30 );
}

REGISTER_UNIT_TEST( test_token_clock_saturates_server_past_the_largest_timestamp );
REGISTER_UNIT_TEST( test_token_clock_saturates_server_time_at_the_update_ceiling );
REGISTER_UNIT_TEST( test_token_clock_saturates_server_wall_clock_epoch );
REGISTER_UNIT_TEST( test_token_clock_saturates_client_past_the_largest_timestamp );
REGISTER_UNIT_TEST( test_token_clock_saturates_client_time_at_the_update_ceiling );

#endif // #if ROCKETNET_DEV
