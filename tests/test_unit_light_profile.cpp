/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The public light-client profile, pinned. A 1000-slot server and a client
// size their create blocks within the figures USAGE.md records, and both
// endpoints create and destroy on the profile without opening a socket.

#include "test_unit_registry.h"
#include "rocketnet.h"

#include <stddef.h>

#define check rocketnet_unit_check

// USAGE.md "Server memory footprint" and docs/LOAD.md record these figures.
// required_memory is the lifecycle peak the caller's allocator sees (create,
// connect or start, stop or disconnect, again, destroy) for the largest
// transport the default ladder can grant, so it differs by platform and by
// ROCKETNET_DEV (a development endpoint also owns its link simulator). Each
// bound is the figure this test read, Release, for
// rocketnet_server_required_memory at max_clients 1000 and
// rocketnet_client_required_memory under the profile: on Linux x86-64 with
// io_uring and XDP built, on macOS arm64, and on Windows x64 with MSVC. A
// host whose ladder grants fewer transports reports less; the check is at
// most.
#if defined(_WIN32)
#if ROCKETNET_DEV
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1523801281 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22745549 )
#else
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1517404917 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 21959745 )
#endif
#elif defined(__APPLE__)
#if ROCKETNET_DEV
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1523801417 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22745629 )
#else
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 1517405053 )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 21959825 )
#endif
#else
#if ROCKETNET_DEV
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 3233438146ULL )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 22745621 )
#else
#define LIGHT_SERVER_1000_MAX_BYTES ( (size_t) 3227041782ULL )
#define LIGHT_CLIENT_MAX_BYTES ( (size_t) 21959817 )
#endif
#endif

static void light_message_fields_server( const struct rocketnet_server_config_t * config )
{
    check( config->max_fragmented_messages == 1 );
    check( config->per_client_send_kbps == 256 );
    check( config->num_channels == 8 );
    check( config->reliable_messages_in_flight == 256 );
    check( config->receive_message_buffer_bytes == 142400 );
    check( config->max_message_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 );
    check( config->send_message_buffer_bytes == 141500 );
    check( config->send_queue_messages == 128 );
    check( config->packets_in_flight == 256 );
    check( config->packets_per_update == 4 );
    check( config->unreliable_messages_per_update == 4 );
    check( config->sequenced_messages_held == 100 );
    check( config->max_message_receipts == 128 );
    check( config->reliable_message_timeout_ms == 10000 );
}

static void light_message_fields_client( const struct rocketnet_client_config_t * config )
{
    check( config->max_fragmented_messages == 1 );
    check( config->send_kbps == 256 );
    check( config->num_channels == 8 );
    check( config->reliable_messages_in_flight == 256 );
    check( config->receive_message_buffer_bytes == 142400 );
    check( config->max_message_bytes == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 );
    check( config->send_message_buffer_bytes == 141500 );
    check( config->send_queue_messages == 128 );
    check( config->packets_in_flight == 256 );
    check( config->packets_per_update == 4 );
    check( config->unreliable_messages_per_update == 4 );
    check( config->sequenced_messages_held == 100 );
    check( config->max_message_receipts == 128 );
    check( config->reliable_message_timeout_ms == 10000 );
}

static void test_light_profile_server_1000_required_memory()
{
    struct rocketnet_server_config_t config;
    check( rocketnet_server_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );
    check( config.max_clients == 1000 );
    light_message_fields_server( &config );

    config.max_clients = 1000;
    const size_t bytes = rocketnet_server_required_memory( &config );
    check( bytes > 0 );
    check( bytes <= LIGHT_SERVER_1000_MAX_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_server_1000_required_memory );

static void test_light_profile_client_required_memory()
{
    struct rocketnet_client_config_t config;
    check( rocketnet_client_light_config( &config ) == ROCKETNET_STATUS_OK );
    check( config.config_bytes == sizeof( config ) );
    light_message_fields_client( &config );

    const size_t bytes = rocketnet_client_required_memory( &config );
    check( bytes > 0 );
    check( bytes <= LIGHT_CLIENT_MAX_BYTES );
}

REGISTER_UNIT_TEST( test_light_profile_client_required_memory );

static void test_light_profile_configs_create()
{
    // create does not bind, and it takes no clock. update at 1000.0 on a
    // stopped server and a disconnected client opens no socket.
    const double now = 1000.0;

    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_light_config( &server_config ) == ROCKETNET_STATUS_OK );

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_server_stopped( server ) );
    rocketnet_server_update( server, now );
    check( rocketnet_server_stopped( server ) );
    rocketnet_server_destroy( server );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_light_config( &client_config ) == ROCKETNET_STATUS_OK );

    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_update( client, now );
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_destroy( client );
}

REGISTER_UNIT_TEST( test_light_profile_configs_create );
