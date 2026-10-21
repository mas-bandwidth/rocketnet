/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Public step-driven clients use only rocketnet.h for their success paths.
// The loopback transport carries packets in memory; callers provide every
// update time from 1000.0, and no test sleeps or opens a socket.

#include "test_unit_registry.h"
#include "sdk/rocketnet_connect_token.h"
#include "sdk/rocketnet_base64.h"
#include "test/fake_client_driver.h"
#include "test/portable_crt.h"
#include "sdk/rocketnet.h"
#include "sdk/rocketnet_config.h"
#include "rocketnet_client_io.h"       // refusal fixture for a transport with no non-blocking step

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
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
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
    config->transport = "loopback";
}

static void public_step_token( uint16_t port, const uint8_t * server_public, const public_step_identity_t * identity, uint8_t * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, server_public,
        PUBLIC_STEP_PROTOCOL, addresses, 1, 0x5EED, 1000, 1ULL << 40, 300, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
}

// The string token for a running server, sealed to the key the server made.
static void public_step_token_string( const rocketnet_server_t * server, const public_step_identity_t * identity, char * token_string )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_step_token( rocketnet_server_port( server ), server_public, identity, token );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

static double public_step_now = 1000.0;
static double public_step_clock( void * ) { return public_step_now; }

static void public_step_client_config( rocketnet_client_test_config_t * config )
{
    rocketnet_client_default_config( config );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = public_step_clock;
}

static void public_step_pair_create( public_step_pair_t * pair, const public_step_identity_t * identity,
                                     uint16_t fixed_port = 0 )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->time = 1000.0;
    rocketnet_quiet( 1 );

    rocketnet_server_config_t server_config;
    public_step_server_config( &server_config, identity );
    char bind[64];
    snprintf( bind, sizeof( bind ), "127.0.0.1:%u", (unsigned) fixed_port );
    pair->server = rocketnet_server_create( NULL, bind, NULL, &server_config, NULL );
    check( pair->server != NULL );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    public_step_token_string( pair->server, identity, token );
    rocketnet_client_test_config_t client_config;
    public_step_client_config( &client_config );
    pair->threads_before_client = public_step_thread_count();
    pair->client = rocketnet_client_create( NULL, &client_config, NULL );
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
    rocketnet_client_update( pair->client, pair->time );
    rocketnet_client_send_packets( pair->client );
    rocketnet_server_update( pair->server, pair->time );
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

static void test_public_step_driven_client_connects_without_a_thread()
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
REGISTER_UNIT_TEST( test_public_step_driven_client_connects_without_a_thread );

static void test_public_step_driven_client_delivers_both_ways()
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
        uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        check( out != NULL ); memcpy( out, client_messages[i], 4 ); check( rocketnet_client_finish_message( pair.client, 4 ) != 0 );
        out = rocketnet_server_start_message( pair.server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        check( out != NULL ); memcpy( out, server_messages[i], 4 ); check( rocketnet_server_finish_message( pair.server, 4 ) != 0 );
    }

    int at_server = 0, at_client = 0;
    for ( int step = 0; step < 400 && ( at_server < 2 || at_client < 2 ); step++ )
    {
        public_step_pump( &pair );
        const uint8_t * data = NULL; int bytes = 0;
        uint64_t sender = ~0ULL;
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( pair.server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                data = batch->data[i];
                bytes = batch->bytes[i];
                sender = batch->client_index[i];
                check( sender == 0 );
                check( at_server < 2 && bytes == 4 && memcmp( data, client_messages[at_server], 4 ) == 0 );
                at_server++;
            }
        }
        const struct rocketnet_message_batch_t * incoming = NULL;
        while ( ( incoming = rocketnet_client_receive_messages( pair.client ) ) != NULL )
        {
            for ( int i = 0; i < incoming->num_messages; i++ )
            {
                data = incoming->data[i];
                bytes = incoming->bytes[i];
                check( at_client < 2 && bytes == 4 && memcmp( data, server_messages[at_client], 4 ) == 0 );
                at_client++;
            }
        }
    }
    check( at_server == 2 && at_client == 2 );
    public_step_pair_destroy( &pair );
}
REGISTER_UNIT_TEST( test_public_step_driven_client_delivers_both_ways );

// the client's counters, in the stats struct's order
#define PUBLIC_STEP_NUM_COUNTERS 11

static void public_step_trace( const public_step_identity_t * identity, uint16_t port,
                               uint64_t * counters )
{
    public_step_pair_t pair;
    public_step_pair_create( &pair, identity, port );
    public_step_connect( &pair );
    for ( int i = 0; i < 8; i++ )
    {
        uint8_t * out = rocketnet_client_start_message( pair.client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 1 );
        check( out != NULL ); out[0] = (uint8_t) i; check( rocketnet_client_finish_message( pair.client, 1 ) != 0 );
        public_step_pump( &pair );
    }
    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( pair.client, &stats );
    const uint64_t values[PUBLIC_STEP_NUM_COUNTERS] = { stats.packets_sent, stats.packets_received, stats.bytes_sent,
        stats.bytes_received, stats.payloads_received, stats.messages_resent, stats.messages_duplicated,
        stats.receipts_acked, stats.receipts_lost, stats.messages_dropped_over_budget, stats.messages_dropped_not_connected };
    memcpy( counters, values, sizeof( values ) );
    public_step_pair_destroy( &pair );
}

static void test_public_step_driven_client_same_inputs_same_trace()
{
    public_step_identity_t identity;
    public_step_identity( &identity );
    const uint16_t port = 52341;
    uint64_t first[PUBLIC_STEP_NUM_COUNTERS] = {};
    uint64_t second[PUBLIC_STEP_NUM_COUNTERS] = {};
    public_step_trace( &identity, port, first );
    public_step_trace( &identity, port, second );
    check( memcmp( first, second, sizeof( first ) ) == 0 );
}
REGISTER_UNIT_TEST( test_public_step_driven_client_same_inputs_same_trace );

static void test_public_step_driven_client_refused_on_a_threaded_only_transport()
{
    public_step_identity_t identity;
    public_step_identity( &identity );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_step_token( 40000, identity.server_public, &identity, token );
    rocketnet_fake_client_transport_io_t fake = {};
    rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    config.step_driven = true;
    int status = ROCKETNET_STATUS_OK;
    rocketnet_client_transport_t * transport = rocketnet_client_transport_create_step_driven_ex(
        &fake, token, &config, 1000.0, &rocketnet_fake_client_operations_base, &fake, &status );
    check( transport == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
    check( fake.destroy_calls == 1 );
}
REGISTER_UNIT_TEST( test_public_step_driven_client_refused_on_a_threaded_only_transport );

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

static void test_public_step_driven_client_env_selects_the_mode()
{
    char saved[64] = {};
    const bool present = rocketnet_test_getenv( "ROCKETNET_CLIENT_STEP_DRIVEN", saved, sizeof( saved ) ) && saved[0] != '\0';

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
REGISTER_UNIT_TEST( test_public_step_driven_client_env_selects_the_mode );

static void test_public_step_driven_client_loss_100_drops_everything()
{
    rocketnet_public_link_simulator_test_development( 1 );
    public_step_identity_t identity;
    public_step_identity( &identity );

    rocketnet_quiet( 1 );
    rocketnet_server_config_t server_config;
    public_step_server_config( &server_config, &identity );
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    public_step_token_string( server, &identity, token );

    rocketnet_client_test_config_t client_config;
    memset( &client_config, 0, sizeof( client_config ) );
    public_step_client_config( &client_config );

    rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    rocketnet_client_network_simulator( client, 0, 0, 100, 0 );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    double now = 1000.0;
    for ( int i = 0; i < 50; i++ )
    {
        public_step_now = now;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        now += 0.01;
    }

    const uint64_t packets_received = rn_client_stats_of( client ).packets_received;
    check( packets_received == 0 );

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_public_link_simulator_test_development( 0 );
}
REGISTER_UNIT_TEST( test_public_step_driven_client_loss_100_drops_everything );
