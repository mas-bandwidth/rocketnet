/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public server_secret_key and step-driven client fields
// (sdk/rocketnet.h). A fixed server key round-trips through
// rocketnet_server_public_key and a token sealed to it connects; the
// published development server key is refused at start in a production
// build; a public step-driven client's send arrives in the next update over
// the loopback hub; a production build refuses step_driven and time_callback.
// The loopback transport carries packets in memory; callers provide every
// time, and no case sleeps or opens a socket.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_connect_token.h"
#include "loopback_hub.h"

#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define SERVER_KEY_PROTOCOL 0x5E4BE7C0FFEE1234ULL

// the published development server secret key, written out: a production
// library does not carry rocketnet_dev_server_secret_key

static const uint8_t rocketnet_server_key_dev_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES] = {
    0x6f, 0xfb, 0x8f, 0x73, 0x37, 0xec, 0x37, 0x76, 0x97, 0xf5, 0x68, 0xde,
    0x31, 0x18, 0xd8, 0x39, 0x59, 0x56, 0x0b, 0x25, 0x92, 0x8d, 0x22, 0x70,
    0x7b, 0xa2, 0x29, 0x75, 0x89, 0xd0, 0x8b, 0x97
};

static void server_key_config( rocketnet_server_config_t * config )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = SERVER_KEY_PROTOCOL;
    config->max_clients = 1;
    memset( config->backend_public_key, 0x52, sizeof( config->backend_public_key ) );
    const uint8_t filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x13, 0x57, 0x9B, 0xDF };
    memcpy( config->packet_filter, filter, sizeof( filter ) );
    config->transport = "loopback";
    config->loopback_hub = rocketnet_test_loopback_hub();
}

// the default all-zero key: create makes a fresh pair; a supplied key: create
// derives its public key, and rocketnet_server_public_key returns it

static void test_public_server_secret_key_round_trips()
{
    rocketnet_server_config_t config;
    server_key_config( &config );

    uint8_t zero[ROCKETNET_SERVER_SECRET_KEY_BYTES] = { 0 };
    check( memcmp( config.server_secret_key, zero, sizeof( zero ) ) == 0 );

    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    check( rocketnet_generate_server_keypair( server_public, server_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_t * fresh = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( fresh != NULL );
    uint8_t fresh_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( fresh, fresh_public );
    check( memcmp( fresh_public, zero, sizeof( zero ) ) != 0 );
    check( memcmp( fresh_public, server_public, sizeof( server_public ) ) != 0 );
    rocketnet_server_destroy( fresh );

    memcpy( config.server_secret_key, server_secret, sizeof( server_secret ) );
    for ( int i = 0; i < 2; i++ )
    {
        rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
        check( server != NULL );
        uint8_t read_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
        rocketnet_server_public_key( server, read_public );
        check( memcmp( read_public, server_public, sizeof( server_public ) ) == 0 );
        rocketnet_server_destroy( server );
    }

    // the published development pair: the derivation gives its public key

    memcpy( config.server_secret_key, rocketnet_server_key_dev_secret, sizeof( config.server_secret_key ) );
    rocketnet_server_t * dev = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( dev != NULL );
    uint8_t dev_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( dev, dev_public );
    check( memcmp( dev_public, rocketnet_dev_server_public_key, sizeof( dev_public ) ) == 0 );
    rocketnet_server_destroy( dev );
}

REGISTER_UNIT_TEST( test_public_server_secret_key_round_trips );

#if ROCKETNET_DEV

#include "../sdk/rocketnet_test_seams.h"
#include "public_step_pair.h"

// a failed key generation or derivation during create (the seam stands in
// for rocketnet_crypto_acquire / crypto_scalarmult_base failing) refuses
// the create: no server, and the status is CRYPTO_INIT_FAILED, on both the
// generated-pair path (all-zero secret) and the derived path (a supplied
// secret); with the seam cleared the same configs create

static void test_public_server_keygen_failure_create_fails()
{
    rocketnet_server_config_t config;
    server_key_config( &config );

    rocketnet_dev_set_server_keygen_failure( true );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    rocketnet_dev_set_server_keygen_failure( false );
    check( server == NULL );
    check( status == ROCKETNET_STATUS_CRYPTO_INIT_FAILED );

    uint8_t secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    check( rocketnet_generate_server_keypair( public_key, secret ) == ROCKETNET_STATUS_OK );
    memcpy( config.server_secret_key, secret, sizeof( config.server_secret_key ) );

    rocketnet_dev_set_server_keygen_failure( true );
    status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    rocketnet_dev_set_server_keygen_failure( false );
    check( server == NULL );
    check( status == ROCKETNET_STATUS_CRYPTO_INIT_FAILED );

    status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );
}

REGISTER_UNIT_TEST( test_public_server_keygen_failure_create_fails );

// a token the backend sealed to the fixed key, never read from the server,
// connects a public step-driven client

static void test_public_server_secret_key_token_connects()
{
    public_step_identity_t identity;
    public_step_identity( &identity );

    rocketnet_quiet( 1 );

    rocketnet_server_config_t server_config;
    public_step_server_config( &server_config, &identity );
    memcpy( server_config.server_secret_key, identity.server_secret, sizeof( server_config.server_secret_key ) );

    public_step_pair_t pair;
    memset( &pair, 0, sizeof( pair ) );
    pair.time = 1000.0;
    pair.server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( pair.server != NULL );
    check( rocketnet_server_start( pair.server ) == ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_step_token( rocketnet_server_port( pair.server ), identity.server_public, &identity, token );
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, sizeof( token_string ) ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    rocketnet_client_config_t client_config;
    public_step_client_config( &client_config );
    pair.client = rocketnet_client_create( NULL, &client_config, NULL );
    check( pair.client != NULL );
    check( rocketnet_client_connect( pair.client, token_string ) == ROCKETNET_STATUS_OK );

    public_step_connect( &pair );

    public_step_pair_destroy( &pair );
}

REGISTER_UNIT_TEST( test_public_server_secret_key_token_connects );

// one thread, no network thread: a message the client sends arrives in the
// server's next update, and a server message in the client's next update

static void test_public_step_driven_client_delivers_next_update()
{
    public_step_identity_t identity;
    public_step_identity( &identity );

    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity );
    public_step_connect( &pair );

    // drain whatever the handshake left

    for ( int i = 0; i < 4; i++ )
    {
        public_step_pump( &pair );
        while ( rocketnet_server_receive_messages( pair.server ) != NULL )
        {
        }
        while ( rocketnet_client_receive_messages( pair.client ) != NULL )
        {
        }
    }

    const uint8_t up[4] = { 1, 2, 3, 4 };
    uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( out != NULL );
    memcpy( out, up, 4 );
    check( rocketnet_client_finish_message( pair.client, 4 ) != 0 );
    rocketnet_client_send_packets( pair.client );

    rocketnet_server_update( pair.server, pair.time );
    const struct rocketnet_message_batch_t * batch = rocketnet_server_receive_messages( pair.server );
    check( batch != NULL && batch->num_messages == 1 );
    check( batch->bytes[0] == 4 && memcmp( batch->data[0], up, 4 ) == 0 && batch->client_index[0] == 0 );
    check( rocketnet_server_receive_messages( pair.server ) == NULL );

    const uint8_t down[4] = { 5, 6, 7, 8 };
    out = rocketnet_server_start_message( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
    check( out != NULL );
    memcpy( out, down, 4 );
    check( rocketnet_server_finish_message( pair.server, 4 ) != 0 );
    rocketnet_server_send_packets( pair.server );

    pair.time += 0.01;
    public_step_now = pair.time;
    rocketnet_client_update( pair.client, pair.time );
    const struct rocketnet_message_batch_t * incoming = rocketnet_client_receive_messages( pair.client );
    check( incoming != NULL && incoming->num_messages == 1 );
    check( incoming->bytes[0] == 4 && memcmp( incoming->data[0], down, 4 ) == 0 );
    check( rocketnet_client_receive_messages( pair.client ) == NULL );

    public_step_pair_destroy( &pair );
}

REGISTER_UNIT_TEST( test_public_step_driven_client_delivers_next_update );

#else // !ROCKETNET_DEV

// the published development server key is refused at start

static void test_public_server_dev_key_refused_in_production()
{
    rocketnet_quiet( 1 );

    rocketnet_server_config_t config;
    server_key_config( &config );
    memcpy( config.server_secret_key, rocketnet_server_key_dev_secret, sizeof( config.server_secret_key ) );

    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION );
    check( !rocketnet_server_running( server ) );
    rocketnet_server_destroy( server );
}

REGISTER_UNIT_TEST( test_public_server_dev_key_refused_in_production );

static double production_clock( void * ) { return 1000.0; }

// step_driven and time_callback are development only: each one set is
// refused at create; neither set creates

static void test_public_step_driven_refused_in_production()
{
    rocketnet_quiet( 1 );

    rocketnet_client_config_t config;
    check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    check( !config.step_driven && config.time_callback == NULL );

    config.step_driven = true;
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    config.step_driven = false;
    config.time_callback = production_clock;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    // the neither-set create needs a title filter: production refuses the test one

    config.time_callback = NULL;
    const uint8_t filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x13, 0x57, 0x9B, 0xDF };
    memcpy( config.packet_filter, filter, sizeof( filter ) );
    rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &status );
    check( client != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
}

REGISTER_UNIT_TEST( test_public_step_driven_refused_in_production );

#endif // #if ROCKETNET_DEV
