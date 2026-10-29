/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public key generators and rocketnet_create_connect_token (sdk/rocketnet.h),
// and the token clock's epoch (token_epoch_unix_seconds). Every time is an
// argument: the tokens carry fixed Unix timestamps, the pair runs on caller
// time from 1000.0 over the in-memory loopback transport, and no case reads
// a clock, sleeps or opens a socket.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_connect_token.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define CREATE_TOKEN_PROTOCOL       0x6A1B2C3D4E5F7081ULL
#define CREATE_TOKEN_CLIENT_ID      0x0123456789ABCDEFULL
#define CREATE_TOKEN_EPOCH          1700000000ULL
#define CREATE_TOKEN_TIMESTAMP      ( CREATE_TOKEN_EPOCH + 1000 )

struct create_token_keys_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
};

static void create_token_keys( create_token_keys_t * keys )
{
    check( rocketnet_generate_backend_keypair( keys->backend_public, keys->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( keys->server_public, keys->server_secret ) == ROCKETNET_STATUS_OK );
}

static void create_token_config( struct rocketnet_connect_token_config_t * config, const create_token_keys_t * keys )
{
    check( rocketnet_connect_token_default_config( config ) == ROCKETNET_STATUS_OK );
    memcpy( config->backend_secret_key, keys->backend_secret, sizeof( config->backend_secret_key ) );
    memcpy( config->server_public_key, keys->server_public, sizeof( config->server_public_key ) );
    config->server_address_ipv4 = "203.0.113.10:40000";
    config->server_address_ipv6 = "[2001:db8::1]:65535";
    config->client_id = CREATE_TOKEN_CLIENT_ID;
    config->protocol_id = CREATE_TOKEN_PROTOCOL;
    for ( int i = 0; i < ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES; i++ )
        config->user_data[i] = (uint8_t) ( i * 5 + 1 );
    config->expire_seconds = 30;
    config->timeout_seconds = 10;
    config->unix_timestamp = CREATE_TOKEN_TIMESTAMP;
}

// the defaults: every key, address and id zero, the recommended lifetime

static void test_create_connect_token_defaults()
{
    struct rocketnet_connect_token_config_t config;
    memset( &config, 0xCD, sizeof( config ) );
    check( rocketnet_connect_token_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );
    check( config.server_address_ipv4 == NULL && config.server_address_ipv6 == NULL );
    check( config.client_id == 0 && config.protocol_id == 0 && config.unix_timestamp == 0 );
    check( config.expire_seconds == 30 && config.timeout_seconds == 10 );
    for ( size_t i = 0; i < sizeof( config.backend_secret_key ); i++ )
        check( config.backend_secret_key[i] == 0 );
    for ( size_t i = 0; i < sizeof( config.user_data ); i++ )
        check( config.user_data[i] == 0 );

    // a zeroed config was never initialized

    struct rocketnet_connect_token_config_t zeroed;
    memset( &zeroed, 0, sizeof( zeroed ) );
    check( rocketnet_connect_token_default_config_internal( &zeroed ) == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );

    printf( "test_create_connect_token_defaults\n" );
}

// the decoded token against the inputs: the signature verifies with the
// backend public key, the private block opens with the server secret key,
// and the signed fields before the sealed block are byte identical to the
// internal writer's for the same inputs (the sealed block and the token keys
// are drawn fresh by each, so they cannot match)

static void test_create_connect_token_decodes_to_its_inputs()
{
    create_token_keys_t keys;
    create_token_keys( &keys );

    struct rocketnet_connect_token_config_t config;
    create_token_config( &config, &keys );

    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_create_connect_token( &config, token_string ) == ROCKETNET_STATUS_OK );
    check( strlen( token_string ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_base64_decode_data( token_string, token, sizeof( token ) ) == ROCKETNET_CONNECT_TOKEN_BYTES );

    struct rocketnet_connect_token_t parsed;
    rocketnet_read_connect_token( &parsed, token );
    check( rocketnet_verify_signed_connect_data( parsed.signed_connect_data, parsed.signature, keys.backend_public ) == 1 );

    struct rocketnet_signed_connect_data_t signed_fields;
    rocketnet_read_signed_connect_data( &signed_fields, parsed.signed_connect_data );
    check( signed_fields.protocol_id == CREATE_TOKEN_PROTOCOL );
    check( signed_fields.create_timestamp == CREATE_TOKEN_TIMESTAMP );
    check( signed_fields.expire_timestamp == CREATE_TOKEN_TIMESTAMP + 30 );
    check( signed_fields.timeout_seconds == 10 );
    check( signed_fields.address_flags == ( ROCKETNET_ADDRESS_FLAG_IPV4 | ROCKETNET_ADDRESS_FLAG_IPV6 ) );
    check( memcmp( signed_fields.server_public_key, keys.server_public, sizeof( keys.server_public ) ) == 0 );

    struct rocketnet_connect_token_private_t private_fields;
    check( rocketnet_open_connect_token_private( &private_fields, parsed.signed_connect_data, keys.server_public, keys.server_secret ) == 1 );
    check( private_fields.client_id == CREATE_TOKEN_CLIENT_ID );
    check( memcmp( private_fields.user_data, config.user_data, sizeof( config.user_data ) ) == 0 );
    check( memcmp( private_fields.client_to_server_key, parsed.client_to_server_key, sizeof( parsed.client_to_server_key ) ) == 0 );
    check( memcmp( private_fields.server_to_client_key, parsed.server_to_client_key, sizeof( parsed.server_to_client_key ) ) == 0 );

    const char * const addresses[] = { config.server_address_ipv4, config.server_address_ipv6 };
    uint8_t reference[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( keys.backend_secret, keys.server_public, CREATE_TOKEN_PROTOCOL, addresses, 2,
                                                     CREATE_TOKEN_CLIENT_ID, CREATE_TOKEN_TIMESTAMP, CREATE_TOKEN_TIMESTAMP + 30, 10,
                                                     config.user_data, reference, sizeof( reference ) ) == ROCKETNET_STATUS_OK );
    check( memcmp( reference, token, ROCKETNET_SIGNED_OFFSET_SEALED_BLOCK ) == 0 );

    printf( "test_create_connect_token_decodes_to_its_inputs\n" );
}

// every refusal returns its status and leaves the output unchanged

static void create_token_refused( const struct rocketnet_connect_token_config_t * config, enum rocketnet_status_t expected )
{
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    memset( token_string, 'Z', sizeof( token_string ) );
    check( rocketnet_create_connect_token( config, token_string ) == expected );
    for ( size_t i = 0; i < sizeof( token_string ); i++ )
        check( token_string[i] == 'Z' );
}

static void test_create_connect_token_refusals()
{
    rocketnet_quiet( 1 );

    create_token_keys_t keys;
    create_token_keys( &keys );

    struct rocketnet_connect_token_config_t base;
    create_token_config( &base, &keys );
    struct rocketnet_connect_token_config_t config;

    config = base;
    config.config_bytes = 0;
    create_token_refused( &config, ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );

    config = base;
    config.config_bytes = sizeof( size_t ) + 1;
    create_token_refused( &config, ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );

    config = base;
    config.server_address_ipv4 = NULL;
    config.server_address_ipv6 = NULL;
    create_token_refused( &config, ROCKETNET_STATUS_INVALID_ARGUMENT );

    const char * const bad_ipv4[] = { "not an address", "[2001:db8::1]:40000", "203.0.113.10:0", "" };
    for ( size_t i = 0; i < sizeof( bad_ipv4 ) / sizeof( bad_ipv4[0] ); i++ )
    {
        config = base;
        config.server_address_ipv4 = bad_ipv4[i];
        create_token_refused( &config, ROCKETNET_STATUS_INVALID_ADDRESS );
    }

    const char * const bad_ipv6[] = { "[2001:db8::zz]:40000", "203.0.113.10:40000", "[2001:db8::1]:0" };
    for ( size_t i = 0; i < sizeof( bad_ipv6 ) / sizeof( bad_ipv6[0] ); i++ )
    {
        config = base;
        config.server_address_ipv6 = bad_ipv6[i];
        create_token_refused( &config, ROCKETNET_STATUS_INVALID_ADDRESS );
    }

    char overlong[ROCKETNET_MAX_ADDRESS_STRING_BYTES + 8];
    memset( overlong, '1', sizeof( overlong ) - 1 );
    overlong[sizeof( overlong ) - 1] = '\0';
    config = base;
    config.server_address_ipv4 = overlong;
    create_token_refused( &config, ROCKETNET_STATUS_INVALID_ADDRESS );

    config = base;
    config.protocol_id = 0;
    create_token_refused( &config, ROCKETNET_STATUS_INVALID_ARGUMENT );

    const int bad_seconds[] = { 0, -1 };
    for ( size_t i = 0; i < 2; i++ )
    {
        config = base;
        config.expire_seconds = bad_seconds[i];
        create_token_refused( &config, ROCKETNET_STATUS_INVALID_ARGUMENT );

        config = base;
        config.timeout_seconds = bad_seconds[i];
        create_token_refused( &config, ROCKETNET_STATUS_INVALID_ARGUMENT );
    }

    config = base;
    config.unix_timestamp = UINT64_MAX - 5;
    create_token_refused( &config, ROCKETNET_STATUS_INVALID_ARGUMENT );

    // one address of either family is enough

    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    config = base;
    config.server_address_ipv6 = NULL;
    check( rocketnet_create_connect_token( &config, token_string ) == ROCKETNET_STATUS_OK );
    config = base;
    config.server_address_ipv4 = NULL;
    check( rocketnet_create_connect_token( &config, token_string ) == ROCKETNET_STATUS_OK );

    rocketnet_quiet( 0 );

    printf( "test_create_connect_token_refusals\n" );
}

// the published development keys: a production build refuses them, by the
// rule server start applies; a development build mints with them (the
// development token is this call over the published backend key)

static void test_create_connect_token_development_keys()
{
    rocketnet_quiet( 1 );

    create_token_keys_t keys;
    create_token_keys( &keys );

    struct rocketnet_connect_token_config_t dev_backend;
    create_token_config( &dev_backend, &keys );

    // the Ed25519 secret's second half is its public key: that is what is checked

    memset( dev_backend.backend_secret_key, 0, sizeof( dev_backend.backend_secret_key ) );
    memcpy( dev_backend.backend_secret_key + ROCKETNET_BACKEND_SECRET_KEY_BYTES - ROCKETNET_BACKEND_PUBLIC_KEY_BYTES, rocketnet_dev_backend_public_key, ROCKETNET_BACKEND_PUBLIC_KEY_BYTES );

    struct rocketnet_connect_token_config_t dev_server;
    create_token_config( &dev_server, &keys );
    memcpy( dev_server.server_public_key, rocketnet_dev_server_public_key, sizeof( dev_server.server_public_key ) );

#if ROCKETNET_DEV
    memcpy( dev_backend.backend_secret_key, rocketnet_dev_backend_secret_key, sizeof( dev_backend.backend_secret_key ) );

    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_create_connect_token( &dev_backend, token_string ) == ROCKETNET_STATUS_OK );
    check( rocketnet_create_connect_token( &dev_server, token_string ) == ROCKETNET_STATUS_OK );
#else  // #if ROCKETNET_DEV
    create_token_refused( &dev_backend, ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
    create_token_refused( &dev_server, ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
#endif // #if ROCKETNET_DEV

    rocketnet_quiet( 0 );

    printf( "test_create_connect_token_development_keys\n" );
}

REGISTER_UNIT_TEST( test_create_connect_token_defaults );
REGISTER_UNIT_TEST( test_create_connect_token_decodes_to_its_inputs );
REGISTER_UNIT_TEST( test_create_connect_token_refusals );
REGISTER_UNIT_TEST( test_create_connect_token_development_keys );

// ---------------------------------------------------------------------------
// a token minted with fresh keys connects, and the token clock is the epoch
// plus the caller's time: the step-driven client and the loopback transport
// are development seams, so these cases run in a development build
// ---------------------------------------------------------------------------

#if ROCKETNET_DEV

#include "../sdk/rocketnet_config.h"
#include "loopback_hub.h"

static double create_token_now = 1000.0;
static double create_token_clock( void * ) { return create_token_now; }

struct create_token_pair_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * client;
};

// a server on token epoch server_epoch and a client on client_epoch, both on
// the loopback hub, the client connecting with a token minted at
// create_timestamp that lives expire_seconds. Nothing is updated yet.

static void create_token_pair( create_token_pair_t * pair, const create_token_keys_t * keys, uint64_t server_epoch, uint64_t client_epoch,
                               uint64_t create_timestamp, int expire_seconds )
{
    memset( pair, 0, sizeof( *pair ) );
    rocketnet_quiet( 1 );

    rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.protocol_id = CREATE_TOKEN_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, keys->backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = rocketnet_test_loopback_hub();
    server_config.token_epoch_unix_seconds = server_epoch;

    pair->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( pair->server != NULL );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( pair->server ) );

    struct rocketnet_connect_token_config_t token_config;
    check( rocketnet_connect_token_default_config( &token_config ) == ROCKETNET_STATUS_OK );
    memcpy( token_config.backend_secret_key, keys->backend_secret, sizeof( token_config.backend_secret_key ) );
    rocketnet_server_public_key( pair->server, token_config.server_public_key );
    token_config.server_address_ipv4 = address;
    token_config.client_id = CREATE_TOKEN_CLIENT_ID;
    token_config.protocol_id = CREATE_TOKEN_PROTOCOL;
    token_config.expire_seconds = expire_seconds;
    token_config.timeout_seconds = 10;
    token_config.unix_timestamp = create_timestamp;

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_create_connect_token( &token_config, token ) == ROCKETNET_STATUS_OK );

    create_token_now = 1000.0;

    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    client_config.step_driven = true;
    client_config.time_callback = create_token_clock;
    client_config.token_epoch_unix_seconds = client_epoch;
    pair->client = rocketnet_client_create( NULL, &client_config, NULL );
    check( pair->client != NULL );
    check( rocketnet_client_connect( pair->client, token ) == ROCKETNET_STATUS_OK );
}

static void create_token_step( create_token_pair_t * pair, double time )
{
    create_token_now = time;
    rocketnet_client_update( pair->client, time );
    rocketnet_client_send_packets( pair->client );
    rocketnet_server_update( pair->server, time );
    rocketnet_server_send_packets( pair->server );
}

static void create_token_pair_destroy( create_token_pair_t * pair )
{
    rn_test_client_stop_and_destroy( pair->client );
    rocketnet_server_stop( pair->server );
    for ( int step = 0; step < 32 && !rocketnet_server_stopped( pair->server ); step++ )
    {
    }
    check( rocketnet_server_stopped( pair->server ) );
    rocketnet_server_destroy( pair->server );
    rocketnet_quiet( 0 );
}

// fresh keys from the public generators, a token from the public minter: the
// client connects at epoch + 1000, inside the token's lifetime

static void test_create_connect_token_fresh_keys_connect()
{
    create_token_keys_t keys;
    create_token_keys( &keys );

    create_token_pair_t pair;
    create_token_pair( &pair, &keys, CREATE_TOKEN_EPOCH, CREATE_TOKEN_EPOCH, CREATE_TOKEN_TIMESTAMP, 30 );

    double time = 1000.0;
    for ( int i = 0; i < 400 && rocketnet_client_state( pair.client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        create_token_step( &pair, time );
        time += 0.01;
    }

    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair.server, 0 ) != 0 );
    check( rocketnet_server_client_id( pair.server, 0 ) == CREATE_TOKEN_CLIENT_ID );

    create_token_pair_destroy( &pair );

    printf( "test_create_connect_token_fresh_keys_connect\n" );
}

// the server refuses a token once its epoch plus the caller's time reaches
// the expiry. The client's own epoch is earlier, so the client still holds
// the token live and keeps asking; only the server's clock has passed it.

static void test_token_epoch_server_refuses_after_expiry()
{
    create_token_keys_t keys;
    create_token_keys( &keys );

    // minted at epoch + 990, living 10 s: expired at the server's epoch + 1000

    create_token_pair_t pair;
    create_token_pair( &pair, &keys, CREATE_TOKEN_EPOCH, CREATE_TOKEN_EPOCH - 100, CREATE_TOKEN_EPOCH + 990, 10 );

    double time = 1000.0;
    for ( int i = 0; i < 400; i++ )
    {
        create_token_step( &pair, time );
        time += 0.01;
    }

    check( rocketnet_client_state( pair.client ) != ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_num_connected_clients( pair.server ) == 0 );

    create_token_pair_destroy( &pair );

    // the same token, one second earlier on the server's clock, connects

    create_token_pair( &pair, &keys, CREATE_TOKEN_EPOCH - 1, CREATE_TOKEN_EPOCH - 100, CREATE_TOKEN_EPOCH + 990, 10 );

    time = 1000.0;
    for ( int i = 0; i < 90 && rocketnet_client_state( pair.client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        create_token_step( &pair, time );
        time += 0.01;
    }

    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    create_token_pair_destroy( &pair );

    printf( "test_token_epoch_server_refuses_after_expiry\n" );
}

// a paused caller clock pauses token expiry: the token lives one second from
// epoch + 1000. With the caller's time held at 1000.0 no update expires it,
// however many run; when the time moves again the client connects inside the
// second that is left. Once the time is past the expiry the server refuses it.

static void test_token_epoch_no_expiry_while_time_is_paused()
{
    create_token_keys_t keys;
    create_token_keys( &keys );

    create_token_pair_t pair;
    create_token_pair( &pair, &keys, CREATE_TOKEN_EPOCH, CREATE_TOKEN_EPOCH, CREATE_TOKEN_TIMESTAMP, 1 );

    for ( int i = 0; i < 1000; i++ )
    {
        create_token_step( &pair, 1000.0 );
        check( rocketnet_client_state( pair.client ) >= 0 );
    }

    double time = 1000.0;
    for ( int i = 0; i < 90 && rocketnet_client_state( pair.client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        time += 0.01;
        create_token_step( &pair, time );
    }

    check( time < 1001.0 );
    check( rocketnet_client_state( pair.client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair.server, 0 ) != 0 );

    create_token_pair_destroy( &pair );

    // the same lifetime, with the caller's time past the expiry at the server
    // and the client on an earlier epoch: refused

    create_token_pair( &pair, &keys, CREATE_TOKEN_EPOCH, CREATE_TOKEN_EPOCH - 100, CREATE_TOKEN_TIMESTAMP, 1 );

    for ( int i = 0; i < 400; i++ )
        create_token_step( &pair, 1001.0 );

    check( rocketnet_server_num_connected_clients( pair.server ) == 0 );

    create_token_pair_destroy( &pair );

    printf( "test_token_epoch_no_expiry_while_time_is_paused\n" );
}

REGISTER_UNIT_TEST( test_create_connect_token_fresh_keys_connect );
REGISTER_UNIT_TEST( test_token_epoch_server_refuses_after_expiry );
REGISTER_UNIT_TEST( test_token_epoch_no_expiry_while_time_is_paused );

#endif // #if ROCKETNET_DEV
