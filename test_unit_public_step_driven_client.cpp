/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public step-driven clients use only rocketnet.h for their success paths.
// The loopback transport carries packets in memory; callers provide every
// update time from 1000.0, and no test sleeps or opens a socket.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_config.h"
#include "rocketnet_client_io.h"       // refusal fixture for a driver with no non-blocking step

#if defined(__linux__)
#include <dirent.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check
#define PUBLIC_STEP_PROTOCOL 0x51E7D41A92B3C6D8ULL

struct public_step_pair_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    double time;
    int threads_before_client;
    int threads_after_client;
};

struct public_step_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
};

static int public_step_thread_count()
{
#if defined(__linux__)
    DIR * dir = opendir( "/proc/self/task" );
    if ( !dir )
        return -1;
    int count = 0;
    while ( readdir( dir ) )
        count++;
    closedir( dir );
    return count;
#else
    return -1;
#endif
}

static void public_step_identity( public_step_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0xA5 ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static void public_step_server_config( rocketnet_server_config_t * config, const public_step_identity_t * identity )
{
    rocketnet_server_default_config( config );
    config->protocol_id = PUBLIC_STEP_PROTOCOL;
    config->max_clients = 1;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    memcpy( config->server_public_key, identity->server_public, sizeof( config->server_public_key ) );
    memcpy( config->server_secret_key, identity->server_secret, sizeof( config->server_secret_key ) );
    config->transport = "loopback";
}

static void public_step_token( uint16_t port, const public_step_identity_t * identity, uint8_t * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
        PUBLIC_STEP_PROTOCOL, addresses, 1, 0x5EED, 1000, 1ULL << 40, 300, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
}

static double public_step_now = 1000.0;
static double public_step_clock( void * ) { return public_step_now; }

static void public_step_client_config( rocketnet_client_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->step_driven = 1;
    config->time_callback = public_step_clock;
}

static void public_step_pair_create( public_step_pair_t * pair, const public_step_identity_t * identity,
                                     const uint8_t * existing_token = NULL, uint16_t fixed_port = 0 )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    rocketnet_quiet( 1 );

    rocketnet_server_config_t server_config;
    public_step_server_config( &server_config, identity );
    char bind[64];
    snprintf( bind, sizeof( bind ), "127.0.0.1:%u", (unsigned) fixed_port );
    pair->server = rocketnet_server_create( NULL, bind, &server_config, NULL, 0 );
    check( pair->server != NULL );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    uint8_t local_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    const uint8_t * token = existing_token;
    if ( !token )
    {
        public_step_token( rocketnet_server_port( pair->server ), identity, local_token );
        token = local_token;
    }
    rocketnet_client_config_t client_config;
    public_step_client_config( &client_config );
    pair->threads_before_client = public_step_thread_count();
    pair->client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( pair->client != NULL );
    check( rocketnet_client_connect( pair->client, token ) == ROCKETNET_STATUS_OK );
    pair->threads_after_client = public_step_thread_count();
}

static void public_step_pair_destroy( public_step_pair_t * pair )
{
    rocketnet_client_destroy( pair->client );
    rocketnet_server_destroy( pair->server );
}

// RNClientIo CallerStep: update and send_packets execute the non-blocking I/O
// step on this caller's thread; no I/O-thread action exists in this mode.
static void public_step_pump( public_step_pair_t * pair )
{
    public_step_now = pair->time;
    check( rocketnet_client_update( pair->client, pair->time ) == ROCKETNET_MESSAGE_OK );
    rocketnet_client_send_packets( pair->client );
    check( rocketnet_server_update( pair->server, pair->time ) == ROCKETNET_MESSAGE_OK );
    rocketnet_server_send_packets( pair->server );
    pair->time += 0.01;
}

static void public_step_connect( public_step_pair_t * pair )
{
    for ( int i = 0; i < 400 && rocketnet_client_state( pair->client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        public_step_pump( pair );
    check( rocketnet_client_state( pair->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair->server, 0 ) != 0 );
}

static void public_step_driven_client_connects_without_a_thread()
{
    public_step_identity_t identity;
    public_step_identity( &identity );
    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity );
    if ( pair.threads_before_client >= 0 && pair.threads_after_client >= 0 )
        check( pair.threads_after_client == pair.threads_before_client );
    public_step_connect( &pair );
    public_step_pair_destroy( &pair );
}
REGISTER_UNIT_TEST( public_step_driven_client_connects_without_a_thread );

static void public_step_driven_client_delivers_both_ways()
{
    public_step_identity_t identity;
    public_step_identity( &identity );
    public_step_pair_t pair;
    public_step_pair_create( &pair, &identity );
    public_step_connect( &pair );

    const uint8_t client_messages[2][4] = { {1,2,3,4}, {5,6,7,8} };
    const uint8_t server_messages[2][4] = { {9,10,11,12}, {13,14,15,16} };
    for ( int i = 0; i < 2; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABLE_ORDERED, 4 );
        check( out != NULL ); memcpy( out, client_messages[i], 4 ); check( rocketnet_client_finish_message( pair.client, 4 ) != 0 );
        out = rocketnet_server_start_message( pair.server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 4 );
        check( out != NULL ); memcpy( out, server_messages[i], 4 ); check( rocketnet_server_finish_message( pair.server, 0, 4 ) != 0 );
    }

    int at_server = 0, at_client = 0;
    for ( int step = 0; step < 400 && ( at_server < 2 || at_client < 2 ); step++ )
    {
        public_step_pump( &pair );
        const uint8_t * data = NULL; int bytes = 0;
        while ( rocketnet_server_receive_message( pair.server, 0, 0, &data, &bytes ) )
        {
            check( at_server < 2 && bytes == 4 && memcmp( data, client_messages[at_server], 4 ) == 0 );
            rocketnet_server_release_message( pair.server, 0, data ); at_server++;
        }
        while ( rocketnet_client_receive_message( pair.client, 0, &data, &bytes ) )
        {
            check( at_client < 2 && bytes == 4 && memcmp( data, server_messages[at_client], 4 ) == 0 );
            rocketnet_client_release_message( pair.client, data ); at_client++;
        }
    }
    check( at_server == 2 && at_client == 2 );
    public_step_pair_destroy( &pair );
}
REGISTER_UNIT_TEST( public_step_driven_client_delivers_both_ways );

static void public_step_trace( const public_step_identity_t * identity, const uint8_t * token, uint16_t port,
                               uint64_t * counters )
{
    public_step_pair_t pair;
    public_step_pair_create( &pair, identity, token, port );
    public_step_connect( &pair );
    for ( int i = 0; i < 8; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABLE_ORDERED, 1 );
        check( out != NULL ); out[0] = (uint8_t) i; check( rocketnet_client_finish_message( pair.client, 1 ) != 0 );
        public_step_pump( &pair );
    }
    for ( int i = 0; i < ROCKETNET_CLIENT_COUNTER_COUNT; i++ )
        counters[i] = rocketnet_client_counter( pair.client, (enum rocketnet_client_counter_t) i );
    public_step_pair_destroy( &pair );
}

static void public_step_driven_client_same_inputs_same_trace()
{
    public_step_identity_t identity;
    public_step_identity( &identity );
    const uint16_t port = 52341;
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_step_token( port, &identity, token );
    uint64_t first[ROCKETNET_CLIENT_COUNTER_COUNT] = {};
    uint64_t second[ROCKETNET_CLIENT_COUNTER_COUNT] = {};
    public_step_trace( &identity, token, port, first );
    public_step_trace( &identity, token, port, second );
    check( memcmp( first, second, sizeof( first ) ) == 0 );
}
REGISTER_UNIT_TEST( public_step_driven_client_same_inputs_same_trace );

static void public_step_driven_client_refused_on_a_threaded_only_transport()
{
    public_step_identity_t identity;
    public_step_identity( &identity );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_step_token( 40000, &identity, token );
    rocketnet_fake_client_driver_t fake = {};
    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.step_driven = 1;
    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_ex(
        &fake, token, &config, 1000.0, &rocketnet_fake_client_operations_base, &fake, &status );
    check( transport == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    check( fake.destroy_calls == 1 );
}
REGISTER_UNIT_TEST( public_step_driven_client_refused_on_a_threaded_only_transport );

static int public_step_setenv( const char * name, const char * value )
{
#ifdef _WIN32
    return _putenv_s( name, value );
#else
    return setenv( name, value, 1 );
#endif
}
static int public_step_unsetenv( const char * name )
{
#ifdef _WIN32
    return _putenv_s( name, "" );
#else
    return unsetenv( name );
#endif
}

static void public_step_driven_client_env_selects_the_mode()
{
    const char * prior = getenv( "ROCKETNET_CLIENT_STEP_DRIVEN" );
    char saved[64] = {};
    const bool present = prior && prior[0] != '\0';
    if ( present ) strncpy( saved, prior, sizeof( saved ) - 1 );

    rocketnet_client_transport_config_t config;
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &num_fields );
    check( public_step_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", "1" ) == 0 );
    rocketnet_client_transport_default_config( &config );
    check( rocketnet_config_apply_env( "client", &config, fields, num_fields ) );
    check( config.step_driven == 1 );
    check( public_step_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", "0" ) == 0 );
    rocketnet_client_transport_default_config( &config );
    check( rocketnet_config_apply_env( "client", &config, fields, num_fields ) );
    check( config.step_driven == 0 );
    if ( present ) check( public_step_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", saved ) == 0 );
    else check( public_step_unsetenv( "ROCKETNET_CLIENT_STEP_DRIVEN" ) == 0 );
}
REGISTER_UNIT_TEST( public_step_driven_client_env_selects_the_mode );
