/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public connect's token expire check (standard/TRANSPORT.md t11 rule 2) on
// the caller's clock: with a configured token_epoch_unix_seconds it reads the
// epoch plus the time of the client's most recent update, zero if it has
// never been updated; a disconnect hands the ended connection's time to the
// client, so a reconnect reads it. With the field zero it reads the wall
// clock (here the suite's fixed ROCKETNET_UNIT_WALL_CLOCK) and the update
// time does not enter it. The client is driven through rocketnet.h only; time
// is an argument from 1000.0, step driven over the in-memory loopback
// transport, with no socket, sleep or thread.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_connect_token.h"

#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV

#include "loopback_hub.h"

#define check rocketnet_unit_check

#define EPOCH_CONNECT_EPOCH     1700000000ULL
#define EPOCH_CONNECT_PROTOCOL  0x7E0C4A51D2B39F68ULL

static double epoch_connect_now = 1000.0;
static double epoch_connect_clock( void * ) { return epoch_connect_now; }

struct epoch_connect_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    rocketnet_server_t * server;
    rocketnet_client_t * client;
};

// a started loopback server and a step-driven client on token epoch epoch,
// not yet updated

static void epoch_connect_create( epoch_connect_t * t, uint64_t epoch )
{
    memset( t, 0, sizeof( *t ) );
    rocketnet_quiet( 1 );
    epoch_connect_now = 1000.0;

    check( rocketnet_generate_backend_keypair( t->backend_public, t->backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = EPOCH_CONNECT_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, t->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    t->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( t->server != NULL );
    check( rocketnet_server_start( t->server ) == ROCKETNET_STATUS_OK );

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = epoch_connect_clock;
    client_config.token_epoch_unix_seconds = epoch;
    t->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( t->client != NULL );
}

static void epoch_connect_destroy( epoch_connect_t * t )
{
    rn_test_client_stop_and_destroy( t->client );
    rn_test_server_stop_and_destroy( t->server );
    rocketnet_quiet( 0 );
}

// a token for the server, minted with the public minter thirty seconds before
// it expires at expire

static void epoch_connect_token( const epoch_connect_t * t, uint64_t expire, char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES] )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( t->server ) );

    struct rocketnet_connect_token_config_t token_config;
    check( rocketnet_connect_token_default_config( &token_config ) == ROCKETNET_STATUS_OK );
    memcpy( token_config.backend_secret_key, t->backend_secret, sizeof( token_config.backend_secret_key ) );
    rocketnet_server_public_key( t->server, token_config.server_public_key );
    token_config.server_address_ipv4 = address;
    token_config.client_id = 0x5EED;
    token_config.protocol_id = EPOCH_CONNECT_PROTOCOL;
    token_config.expire_seconds = 30;
    token_config.timeout_seconds = 10;
    token_config.unix_timestamp = expire - 30;
    check( rocketnet_create_connect_token( &token_config, token ) == ROCKETNET_STATUS_OK );
}

// connect with a token that expires at expire, and check the status and
// state; an admitted attempt is disconnected and retired again without an
// update, so the client's time is unchanged

static void epoch_connect_expect( epoch_connect_t * t, uint64_t expire, enum rocketnet_status_t expected )
{
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    epoch_connect_token( t, expire, token );

    const enum rocketnet_status_t status = rocketnet_client_connect( t->client, token );
    check( status == expected );

    if ( expected == ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED )
        check( rocketnet_client_state( t->client ) == ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED );

    if ( status == ROCKETNET_STATUS_OK )
    {
        rocketnet_client_disconnect( t->client );
        rn_test_client_wait_disconnected( t->client );
    }
}

// a client updated at 1000.0 and then at 8200.0 checks the token at
// epoch + 8200: a token that expired at epoch + 3630 is refused, one that
// expires at epoch + 8210 connects, and one that expires at epoch + 8200 is
// refused (expire must be later than now)

static void test_token_epoch_public_connect_reads_the_last_update()
{
    epoch_connect_t t;
    epoch_connect_create( &t, EPOCH_CONNECT_EPOCH );

    rocketnet_client_update( t.client, 1000.0 );
    rocketnet_client_update( t.client, 8200.0 );

    epoch_connect_expect( &t, EPOCH_CONNECT_EPOCH + 3630, ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    epoch_connect_expect( &t, EPOCH_CONNECT_EPOCH + 8210, ROCKETNET_STATUS_OK );
    epoch_connect_expect( &t, EPOCH_CONNECT_EPOCH + 8200, ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );

    epoch_connect_destroy( &t );
}

// a client that has never been updated has been passed no time: the check
// reads the epoch alone

static void test_token_epoch_public_connect_never_updated_reads_the_epoch()
{
    epoch_connect_t t;
    epoch_connect_create( &t, EPOCH_CONNECT_EPOCH );

    epoch_connect_expect( &t, EPOCH_CONNECT_EPOCH + 10, ROCKETNET_STATUS_OK );
    epoch_connect_expect( &t, EPOCH_CONNECT_EPOCH, ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );

    epoch_connect_destroy( &t );
}

// a connection updated to 1005.0 and disconnected hands its time to the
// client: the reconnect reads epoch + 1005

static void test_token_epoch_public_connect_reconnect_reads_the_ended_connection()
{
    epoch_connect_t t;
    epoch_connect_create( &t, EPOCH_CONNECT_EPOCH );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    epoch_connect_token( &t, EPOCH_CONNECT_EPOCH + 100000, token );
    check( rocketnet_client_connect( t.client, token ) == ROCKETNET_STATUS_OK );

    for ( double time = 1000.0; time <= 1005.0; time += 1.0 )
    {
        epoch_connect_now = time;
        rocketnet_client_update( t.client, time );
        rocketnet_client_send_packets( t.client );
        rocketnet_server_update( t.server, time );
        rocketnet_server_send_packets( t.server );
    }

    rocketnet_client_disconnect( t.client );
    rn_test_client_wait_disconnected( t.client );

    epoch_connect_expect( &t, EPOCH_CONNECT_EPOCH + 1004, ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );
    epoch_connect_expect( &t, EPOCH_CONNECT_EPOCH + 1006, ROCKETNET_STATUS_OK );

    epoch_connect_destroy( &t );
}

// with the epoch zero the check reads the wall clock once, at connect, and
// the client's update time is not added to it

static void test_token_epoch_public_connect_zero_reads_the_wall_clock()
{
    epoch_connect_t t;
    epoch_connect_create( &t, 0 );

    rocketnet_client_update( t.client, 1000.0 );
    rocketnet_client_update( t.client, 8200.0 );

    epoch_connect_expect( &t, ROCKETNET_UNIT_WALL_CLOCK + 10, ROCKETNET_STATUS_OK );
    epoch_connect_expect( &t, ROCKETNET_UNIT_WALL_CLOCK + 1, ROCKETNET_STATUS_OK );
    epoch_connect_expect( &t, ROCKETNET_UNIT_WALL_CLOCK, ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED );

    epoch_connect_destroy( &t );
}

REGISTER_UNIT_TEST( test_token_epoch_public_connect_reads_the_last_update );
REGISTER_UNIT_TEST( test_token_epoch_public_connect_never_updated_reads_the_epoch );
REGISTER_UNIT_TEST( test_token_epoch_public_connect_reconnect_reads_the_ended_connection );
REGISTER_UNIT_TEST( test_token_epoch_public_connect_zero_reads_the_wall_clock );

#endif // #if ROCKETNET_DEV
