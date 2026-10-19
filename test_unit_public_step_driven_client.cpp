/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public step-driven client: rocketnet_client_transport_config_t's
// step_driven field runs the I/O step inside update and send_packets on the
// caller's thread. The loopback transport carries the connection in memory;
// time is an argument from 1000.0, nothing sleeps and no socket opens.

#include "test_unit_registry.h"
#include "test/fake_client_driver.h"

#include "sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_client_io.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_counters.h"
#include "rocketnet_test_vectors.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static double public_step_now = 1000.0;

static double public_step_time( void * context )
{
    (void) context;
    return public_step_now;
}

static int public_step_thread_count()
{
    DIR * dir = opendir( "/proc/self/task" );
    if ( !dir )
        return 0;
    int count = 0;
    while ( readdir( dir ) )
        count++;
    closedir( dir );
    return count;
}

static uint8_t public_step_backend_public[32];
static uint8_t public_step_backend_secret[64];
static uint8_t public_step_server_public[32];
static uint8_t public_step_server_secret[32];

static void public_step_keys()
{
    if ( public_step_server_public[0] != 0 )
        return;
    check( rocketnet_generate_backend_keypair( public_step_backend_public, public_step_backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( public_step_server_public, public_step_server_secret ) == ROCKETNET_STATUS_OK );
}

static void public_step_server_config( rocketnet_server_transport_config_t * config )
{
    public_step_keys();
    rocketnet_server_transport_default_config( config );
    config->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( config->filter_value, rocketnet_test_filter_value, sizeof( config->filter_value ) );
    memcpy( config->backend_public_key, public_step_backend_public, sizeof( config->backend_public_key ) );
    memcpy( config->server_public_key, public_step_server_public, sizeof( config->server_public_key ) );
    memcpy( config->server_secret_key, public_step_server_secret, sizeof( config->server_secret_key ) );
    snprintf( config->driver, sizeof( config->driver ), "loopback" );
}

static void public_step_client_config( rocketnet_client_transport_config_t * config, void * unused )
{
    rocketnet_client_transport_default_config( config );
    memcpy( config->filter_value, rocketnet_test_filter_value, sizeof( config->filter_value ) );
    snprintf( config->driver, sizeof( config->driver ), "loopback" );
    config->step_driven = 1;
    config->time_callback = public_step_time;
    (void) unused;
}

static bool public_step_token( uint16_t port, uint8_t * token )
{
    public_step_keys();
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = 1000;
    signed_fields.expire_timestamp = 1ULL << 40;
    signed_fields.timeout_seconds = 300;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    signed_fields.ipv4_address[0] = 127;
    signed_fields.ipv4_address[1] = 0;
    signed_fields.ipv4_address[2] = 0;
    signed_fields.ipv4_address[3] = 1;
    signed_fields.ipv4_address[4] = (uint8_t) ( port & 0xFF );
    signed_fields.ipv4_address[5] = (uint8_t) ( port >> 8 );
    memcpy( signed_fields.server_public_key, public_step_server_public, ROCKETNET_KEY_BYTES );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 0x5EED;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = 1ULL << 40;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );
    memset( token, 0, ROCKETNET_CONNECT_TOKEN_BYTES );
    return rocketnet_write_connect_token( &signed_fields, &private_fields, public_step_backend_secret, token ) != 0;
}

// one client update, the server's update and flush, at the same clock value
static void public_step_pump( rocketnet_client_transport_t * client, rocketnet_server_transport_t * server, double time )
{
    public_step_now = time;
    rocketnet_client_transport_update( client, time );
    rocketnet_server_transport_update( server, time );
    rocketnet_server_transport_send_packets( server );
}

static bool public_step_connect( rocketnet_client_transport_t * client, rocketnet_server_transport_t * server )
{
    for ( int i = 0; i < 400; i++ )
    {
        public_step_pump( client, server, 1000.0 + i * 0.01 );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            return true;
    }
    return false;
}

// The client reaches CONNECTED against a loopback server by update calls
// alone, and creating it starts no thread: the process's task count is the
// same before and after the create.
static void public_step_driven_client_connects_without_a_thread()
{
    rocketnet_server_transport_config_t server_config;
    public_step_server_config( &server_config );
    server_config.max_clients = 4;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config );
    check( server != NULL );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( public_step_token( rocketnet_server_transport_port( server ), token ) );

    rocketnet_client_transport_config_t client_config;
    public_step_client_config( &client_config, NULL );

    const int threads_before = public_step_thread_count();
    rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, 1000.0 );
    check( client != NULL );
    const int threads_after = public_step_thread_count();
    check( threads_after == threads_before );
    check( rocketnet_client_transport_client_io( client ) != NULL );

    check( public_step_connect( client, server ) );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}
REGISTER_UNIT_TEST( public_step_driven_client_connects_without_a_thread );

// A reliable ordered message each way arrives, in order, without a socket.
static void public_step_driven_client_delivers_both_ways()
{
    rocketnet_server_transport_config_t server_config;
    public_step_server_config( &server_config );
    server_config.max_clients = 4;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config );
    check( server != NULL );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( public_step_token( rocketnet_server_transport_port( server ), token ) );

    rocketnet_client_transport_config_t client_config;
    public_step_client_config( &client_config, NULL );
    rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, 1000.0 );
    check( client != NULL );
    check( public_step_connect( client, server ) );

    const uint8_t first[] = "first";
    const uint8_t second[] = "second";
    rocketnet_client_transport_send_packet( client, first, (int) sizeof( first ) - 1 );
    rocketnet_client_transport_send_packet( client, second, (int) sizeof( second ) - 1 );

    const uint8_t reply_first[] = "reply-one";
    const uint8_t reply_second[] = "reply-two";
    rocketnet_server_transport_send_packet( server, 0, reply_first, (int) sizeof( reply_first ) - 1 );
    rocketnet_server_transport_send_packet( server, 0, reply_second, (int) sizeof( reply_second ) - 1 );

    int server_seen = 0;
    int client_seen = 0;
    uint8_t server_got_first[32] = {};
    uint8_t server_got_second[32] = {};
    uint8_t client_got_first[32] = {};
    uint8_t client_got_second[32] = {};
    int server_first_bytes = 0, server_second_bytes = 0;
    int client_first_bytes = 0, client_second_bytes = 0;

    for ( int i = 0; i < 300 && ( server_seen < 2 || client_seen < 2 ); i++ )
    {
        public_step_pump( client, server, 1000.0 + i * 0.01 );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int p = 0; p < num_payloads; p++ )
        {
            int client_index = -1, payload_bytes = 0;
            uint8_t * payload = rocketnet_server_transport_payload( server, p, &client_index, &payload_bytes );
            check( client_index == 0 );
            if ( server_seen == 0 )
            {
                memcpy( server_got_first, payload, (size_t) payload_bytes );
                server_first_bytes = payload_bytes;
            }
            else if ( server_seen == 1 )
            {
                memcpy( server_got_second, payload, (size_t) payload_bytes );
                server_second_bytes = payload_bytes;
            }
            server_seen++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes = 0;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof( packet ) ) ) > 0 )
        {
            if ( client_seen == 0 )
            {
                memcpy( client_got_first, packet, (size_t) packet_bytes );
                client_first_bytes = packet_bytes;
            }
            else if ( client_seen == 1 )
            {
                memcpy( client_got_second, packet, (size_t) packet_bytes );
                client_second_bytes = packet_bytes;
            }
            client_seen++;
        }
    }

    check( server_seen == 2 );
    check( client_seen == 2 );
    check( server_first_bytes == 5 && memcmp( server_got_first, first, 5 ) == 0 );
    check( server_second_bytes == 6 && memcmp( server_got_second, second, 6 ) == 0 );
    check( client_first_bytes == 9 && memcmp( client_got_first, reply_first, 9 ) == 0 );
    check( client_second_bytes == 9 && memcmp( client_got_second, reply_second, 9 ) == 0 );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}
REGISTER_UNIT_TEST( public_step_driven_client_delivers_both_ways );

// Two runs with the same token and update times produce the same counters.
static void public_step_run_trace( uint16_t port, const uint8_t * token, uint64_t * counters )
{
    rocketnet_server_transport_config_t server_config;
    public_step_server_config( &server_config );
    server_config.max_clients = 4;
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, address, &server_config );
    check( server != NULL );

    rocketnet_client_transport_config_t client_config;
    public_step_client_config( &client_config, NULL );
    rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, 1000.0 );
    check( client != NULL );
    check( public_step_connect( client, server ) );

    for ( int i = 0; i < 8; i++ )
    {
        const uint8_t payload[] = "trace";
        rocketnet_client_transport_send_packet( client, payload, (int) sizeof( payload ) - 1 );
        public_step_pump( client, server, 2000.0 + i * 0.01 );
    }

    const struct rocketnet_counters_t * source = rocketnet_client_transport_counters( client );
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        counters[i] = source->values[i];

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

static void public_step_driven_client_same_inputs_same_trace()
{
    rocketnet_server_transport_config_t probe_config;
    public_step_server_config( &probe_config );
    rocketnet_server_transport_t * probe = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &probe_config );
    check( probe != NULL );
    const uint16_t port = rocketnet_server_transport_port( probe );
    rocketnet_server_transport_destroy( probe );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( public_step_token( port, token ) );

    uint64_t first[ROCKETNET_NUM_COUNTERS] = {};
    uint64_t second[ROCKETNET_NUM_COUNTERS] = {};
    public_step_run_trace( port, token, first );
    public_step_run_trace( port, token, second );
    check( memcmp( first, second, sizeof( first ) ) == 0 );
}
REGISTER_UNIT_TEST( public_step_driven_client_same_inputs_same_trace );

// A transport whose driver has no non-blocking step is refused at create with
// the config status, and the log line names the field. The base fake driver
// has no wait, wake or batched receive.
static void public_step_driven_client_refused_on_a_threaded_only_transport()
{
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( public_step_token( 40000, token ) );

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
