/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "../native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_platform.h"
#include "../time_scale.h"
#include "../suite_runner.h"
#include "../portable_crt.h"

#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// tests/functional/harness.inc: the token stamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

#include "../../sdk/rocketnet_config.h"
#include "../loopback_hub.h"

#if defined( _WIN32 )
#include <stdlib.h>
#else
#include <unistd.h>
#endif // #if defined( _WIN32 )

uint64_t rocketnet_functional_token_timestamp( uint64_t token_epoch_unix_seconds, double time );

static void server_receive_backlog_check( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "server_receive_backlog check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define server_receive_backlog_require( condition ) server_receive_backlog_check( (condition), #condition, __LINE__ )

static void server_receive_backlog_set_env( const char * name, const char * value )
{
#if defined( _WIN32 )
    _putenv_s( name, value ? value : "" );
#else
    if ( value )
    {
        setenv( name, value, 1 );
    }
    else
    {
        unsetenv( name );
    }
#endif // #if defined( _WIN32 )
}

struct server_receive_backlog_env_t
{
    const char * name;
    bool had_value;
    std::string value;

    explicit server_receive_backlog_env_t( const char * env_name )
        : name( env_name ), had_value( false ), value()
    {
        char buffer[4096];
        had_value = rocketnet_test_getenv( env_name, buffer, sizeof( buffer ) ) != 0;
        value = buffer;
        server_receive_backlog_set_env( name, NULL );
    }

    ~server_receive_backlog_env_t()
    {
        server_receive_backlog_set_env( name, had_value ? value.c_str() : NULL );
    }
};

static int server_receive_backlog_connected_clients( const std::vector<struct rocketnet_client_transport_t *> & clients )
{
    int connected = 0;

    for ( size_t i = 0; i < clients.size(); ++i )
        if ( rocketnet_client_transport_state( clients[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            ++connected;
        }

    return connected;
}

static bool server_receive_backlog_all_connected( const std::vector<struct rocketnet_client_transport_t *> & clients )
{
    return server_receive_backlog_connected_clients( clients ) == (int) clients.size();
}

// Only this case deliberately instantiates 48 full client transports. Budget
// their fixed arrays using the SDK's memory plans, rather than bypassing the
// Debug CRT. The plans currently accept portable, not loopback; using portable
// conservatively retains its small socket/wake allocation, then adds each
// actual loopback queue. The 32 MiB margin covers the executable, CRT bookkeeping,
// process-wide transport table and page rounding. The native Windows Debug
// negative control must catch an extra touched 64 MiB server allocation.
// All other functional cases retain the default RSS bound.
static const int server_receive_backlog_max_clients = 48;

static long rocketnet_server_receive_backlog_rss_bound_mb()
{
    const char * name = "test_server_async_delivery_with_max_clients";
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = server_receive_backlog_max_clients;

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );

    const uint64_t server_bytes = rocketnet_server_transport_required_memory( &server_config, "portable" );
    const uint64_t client_bytes = rocketnet_client_transport_required_memory( &client_config, "portable" );

    const uint64_t queues = rocketnet_loopback_hub_bytes( server_receive_backlog_max_clients );
    if ( !server_bytes || !client_bytes )
        return 0; // A missing plan falls back to the strict default bound.

    const uint64_t bytes = server_bytes + server_receive_backlog_max_clients * client_bytes + queues;
    const long bound = suite_rss_bound_mb( bytes, 32 );
    char verbose[256];
    if ( rocketnet_test_getenv( "ROCKETNET_TEST_VERBOSE", verbose, sizeof( verbose ) ) )
        printf( "%s rss_budget server_bytes=%llu client_bytes=%llu clients=%d queue_bytes=%llu margin_mb=32 bound_mb=%ld\n",
                name, (unsigned long long) server_bytes, (unsigned long long) client_bytes,
                server_receive_backlog_max_clients, (unsigned long long) queues, bound );
    return bound;
}

SUITE_CASE_RSS_BOUND( test_server_async_delivery_with_max_clients, rocketnet_server_receive_backlog_rss_bound_mb );

void rocketnet_test_server_async_delivery_with_max_clients()
{
    server_receive_backlog_env_t server_env( "ROCKETNET_SERVER_TRANSPORT" );
    server_receive_backlog_env_t client_env( "ROCKETNET_CLIENT_TRANSPORT" );

    const double frame_step = 1.0 / 60.0;
    double time = 1000.0;

    // Every configured slot is filled, so "max clients" keeps its meaning while
    // the case stays small: one client transport is about 7.5 MB of fixed
    // compile-time arrays, by design (one client per game process). The
    // thousand-client scale is a load measurement (bin/load), not this case.

    const int max_clients = server_receive_backlog_max_clients;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = max_clients;

    // Token validity uses the same synthetic clock at both endpoints. The
    // token helper takes creation time, not a distant expiration timestamp.

    server_config.token_epoch_unix_seconds = 1700000000ULL;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );
    rocketnet_test_loopback_hub_clients( max_clients );
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_current_ex( NULL, "127.0.0.1:0", &server_config, time, &status );
    server_receive_backlog_require( server != NULL );
    server_receive_backlog_require( status == ROCKETNET_STATUS_OK );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_transport_port( server ) );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config_current( &client_config );
    client_config.token_epoch_unix_seconds = server_config.token_epoch_unix_seconds;
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
    client_config.loopback_hub = rocketnet_test_loopback_hub();

    std::vector<struct rocketnet_client_transport_t *> clients;
    clients.reserve( max_clients );
    for ( int i = 0; i < max_clients; ++i )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        server_receive_backlog_require( rocketnet_write_test_connect_token( server_address, (uint64_t) i + 1, rocketnet_functional_token_timestamp( server_config.token_epoch_unix_seconds, time ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create_current( NULL, token, &client_config, time );
        server_receive_backlog_require( client != NULL );
        clients.push_back( client );
    }

    const double connect_start = rocketnet_platform_time();
    const double connect_deadline = 10.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE;
    bool connected = false;
    int attempts = 0;

    while ( true )
    {
        ++attempts;

        for ( size_t i = 0; i < clients.size(); ++i )
        {
            rocketnet_client_transport_update( clients[i], time );
        }

        rocketnet_server_transport_update( server, time );

        const double elapsed = rocketnet_platform_time() - connect_start;
        if ( attempts <= 30 || time - 1000.0 < elapsed )
        {
            time += frame_step;
        }

        if ( server_receive_backlog_all_connected( clients ) &&
             rocketnet_server_transport_num_connected_clients( server ) == max_clients )
        {
            connected = true;
            break;
        }

        if ( attempts >= 600 && elapsed >= connect_deadline )
        {
            printf( "server_receive_backlog connect deadline: attempts=%d connections_accepted=%d server_connected=%d elapsed=%.6fs\n",
                    attempts,
                    server_receive_backlog_connected_clients( clients ),
                    rocketnet_server_transport_num_connected_clients( server ),
                    elapsed );
            fflush( stdout );
            break;
        }
    }

    server_receive_backlog_require( connected );

    // Native sends complete asynchronously, including loopback. Prove data
    // delivery using application identities, not queued-send counters or a
    // fixed number of settling updates. Protocol packets may remain in flight.

    const int frames = 300;
    uint64_t delivered = 0;
    uint64_t updates = 0;
    int read_min = INT_MAX;
    int read_max = 0;
    double worst_frame_seconds = 0.0;
    for ( int frame = 0; frame < frames; ++frame )
    {
        std::vector<bool> seen( max_clients, false );

        int frame_received = 0;

        for ( size_t i = 0; i < clients.size(); ++i )
        {
            struct rocketnet_packet_context_t context;
            uint8_t * frame_data = rocketnet_client_transport_start_packet( clients[i], 8, &context );
            server_receive_backlog_require( frame_data != NULL );

            int capacity = 0;
            uint8_t * payload = rocketnet_client_transport_packet_payload( clients[i], &context, &capacity );
            server_receive_backlog_require( payload != NULL && capacity >= 8 );
            for ( int byte = 0; byte < 4; ++byte )
            {
                payload[byte] = (uint8_t) ( ( (uint32_t) i >> ( byte * 8 ) ) & 0xff );
                payload[4 + byte] = (uint8_t) ( ( (uint32_t) frame >> ( byte * 8 ) ) & 0xff );
            }

            server_receive_backlog_require( rocketnet_client_transport_finish_packet( clients[i], &context, 8 ) > 0 );
        }

        for ( size_t i = 0; i < clients.size(); ++i )
        {
            rocketnet_client_transport_send_packets( clients[i] );
        }

        const double frame_start = rocketnet_platform_time();
        const double deadline = frame_start + 5.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE;

        do
        {
            for ( size_t i = 0; i < clients.size(); ++i )
            {
                rocketnet_client_transport_update( clients[i], time );
            }

            rocketnet_server_transport_update( server, time );
            ++updates;

            const int num_payloads = rocketnet_server_transport_num_payloads( server );
            if ( num_payloads < read_min )
            {
                read_min = num_payloads;
            }

            if ( num_payloads > read_max )
            {
                read_max = num_payloads;
            }

            for ( int packet = 0; packet < num_payloads; ++packet )
            {
                int client_index = -1, payload_bytes = 0;
                const uint8_t * payload = rocketnet_server_transport_payload( server, packet, &client_index, &payload_bytes );
                server_receive_backlog_require( payload && payload_bytes == 8 );
                server_receive_backlog_require( client_index >= 0 && client_index < max_clients );

                uint32_t identity = 0, sequence = 0;

                for ( int byte = 0; byte < 4; ++byte )
                {
                    identity |= uint32_t( payload[byte] ) << ( byte * 8 );
                    sequence |= uint32_t( payload[4 + byte] ) << ( byte * 8 );
                }

                server_receive_backlog_require( identity < (uint32_t) max_clients && sequence == (uint32_t) frame );
                server_receive_backlog_require( !seen[identity] );
                seen[identity] = true;
                ++frame_received;
                ++delivered;
            }

            if ( frame_received == max_clients )
                break;
            rocketnet_platform_sleep( 0.0001 );
        } while ( rocketnet_platform_time() < deadline );

        if ( frame_received != max_clients )
        {
            printf( "server_receive_backlog deadline: frame=%d received=%d expected=%d\n", frame, frame_received, max_clients );
        }

        server_receive_backlog_require( frame_received == max_clients );

        const double elapsed = rocketnet_platform_time() - frame_start;
        if ( elapsed > worst_frame_seconds )
        {
            worst_frame_seconds = elapsed;
        }

        time += frame_step;
    }

    server_receive_backlog_require( delivered == uint64_t( max_clients ) * frames );
    printf( "server_receive_backlog clients=%d frames=%d delivered=%llu updates=%llu payloads_per_update_min=%d payloads_per_update_max=%d worst_frame_seconds=%.6f status=PASS\n",
            max_clients, frames, (unsigned long long) delivered, (unsigned long long) updates,
            read_min, read_max, worst_frame_seconds );
    fflush( stdout );

    for ( size_t i = 0; i < clients.size(); ++i )
    {
        rn_native_client_transport_stop_and_destroy( clients[i] );
    }

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    rn_native_server_transport_stop_and_destroy( server );
}
