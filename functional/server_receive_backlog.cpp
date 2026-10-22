/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "../tests/native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet_platform.h"
#include "../tests/time_scale.h"
#include "../tests/suite_runner.h"
#include "../tests/portable_crt.h"

#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// functional/harness.inc: the token stamp from the clock that judges it
uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

#if defined(_WIN32)
#include <stdlib.h>
#else
#include <unistd.h>
#endif

uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

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
#if defined(_WIN32)
    _putenv_s( name, value ? value : "" );
#else
    if ( value )
        setenv( name, value, 1 );
    else
        unsetenv( name );
#endif
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

static uint64_t server_receive_backlog_client_packets_sent( const std::vector<struct rocketnet_client_transport_t*> & clients )
{
    uint64_t packets = 0;
    for ( size_t i = 0; i < clients.size(); ++i )
    {
        struct rocketnet_client_transport_stats_t stats;
        memset( &stats, 0, sizeof( stats ) );
        rocketnet_client_transport_stats( clients[i], &stats );
        packets += stats.packets_sent;
    }
    return packets;
}

static int server_receive_backlog_connected_clients( const std::vector<struct rocketnet_client_transport_t*> & clients )
{
    int connected = 0;
    for ( size_t i = 0; i < clients.size(); ++i )
        if ( rocketnet_client_transport_state( clients[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            ++connected;
    return connected;
}

static bool server_receive_backlog_all_connected( const std::vector<struct rocketnet_client_transport_t*> & clients )
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
    const char * name = "test_server_receive_keeps_up_with_max_clients";
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = server_receive_backlog_max_clients;
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const uint64_t server_bytes = rocketnet_server_transport_required_memory( &server_config, "portable" );
    const uint64_t client_bytes = rocketnet_client_transport_required_memory( &client_config, "portable" );
    const uint64_t queues = uint64_t( server_receive_backlog_max_clients + 1 ) *
        rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS );
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

SUITE_CASE_RSS_BOUND( test_server_receive_keeps_up_with_max_clients, rocketnet_server_receive_backlog_rss_bound_mb );

void rocketnet_test_server_receive_keeps_up_with_max_clients()
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
    server_config.game_clock_epoch = 1700000000ULL;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "loopback" );

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, time, &status );
    server_receive_backlog_require( server != NULL );
    server_receive_backlog_require( status == ROCKETNET_STATUS_OK );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_transport_port( server ) );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.game_clock_epoch = server_config.game_clock_epoch;
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );

    std::vector<struct rocketnet_client_transport_t*> clients;
    clients.reserve( max_clients );
    for ( int i = 0; i < max_clients; ++i )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        server_receive_backlog_require( rocketnet_write_test_connect_token( server_address, (uint64_t) i + 1, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, time ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, time );
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
            rocketnet_client_transport_update( clients[i], time );
        rocketnet_server_transport_update( server, time );
        const double elapsed = rocketnet_platform_time() - connect_start;
        if ( attempts <= 30 || time - 1000.0 < elapsed )
            time += frame_step;

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

    // Let the last handshake acknowledgements drain before measuring data packets.
    for ( int settle = 0; settle < 8; ++settle )
    {
        for ( size_t i = 0; i < clients.size(); ++i )
            rocketnet_client_transport_update( clients[i], time );
        rocketnet_server_transport_update( server, time );
        time += frame_step;
    }

    struct rocketnet_server_transport_stats_t server_stats;
    memset( &server_stats, 0, sizeof( server_stats ) );
    rocketnet_server_transport_stats( server, &server_stats );
    const uint64_t starting_received = server_stats.packets_received;
    const uint64_t starting_sent = server_receive_backlog_client_packets_sent( clients );
    server_receive_backlog_require( starting_sent == starting_received );

    uint64_t sent_total = 0;
    uint64_t received_total = 0;
    uint64_t payload_total = 0;
    uint64_t dropped = 0;
    uint64_t queue_depth = 0;
    uint64_t read_sum = 0;
    int read_min = INT_MAX;
    int read_max = 0;
    bool growing = false;
    bool accounting_mismatch = false;
    uint64_t previous_depth = 0;

    for ( int frame = 0; frame < 300; ++frame )
    {
        for ( size_t i = 0; i < clients.size(); ++i )
            rocketnet_client_transport_update( clients[i], time );
        rocketnet_server_transport_update( server, time );

        memset( &server_stats, 0, sizeof( server_stats ) );
        rocketnet_server_transport_stats( server, &server_stats );
        const uint64_t received_now = server_stats.packets_received - starting_received;
        const int packets_read = (int) ( received_now - received_total );
        received_total = received_now;
        if ( packets_read < read_min )
            read_min = packets_read;
        if ( packets_read > read_max )
            read_max = packets_read;
        read_sum += (uint64_t) packets_read;

        // The frame order reads before it queues, so the first measured frame has
        // nothing to deliver; every frame after it carries one payload per client
        // unless the server is behind, which the summary's backlog line reports.
        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        payload_total += (uint64_t) num_payloads;
        for ( int payload_index = 0; payload_index < num_payloads; ++payload_index )
        {
            int client_index = -1;
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_server_transport_payload( server, payload_index, &client_index, &payload_bytes );
            server_receive_backlog_require( payload != NULL );
            server_receive_backlog_require( client_index >= 0 && client_index < max_clients );
            server_receive_backlog_require( payload_bytes == 8 );
        }

        // Simulation has no game state; queue one fixed-size unreliable packet per client after receive.
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
                payload[byte] = (uint8_t) ( ((uint32_t) i >> ( byte * 8 )) & 0xff );
                payload[4 + byte] = (uint8_t) ( ((uint32_t) frame >> ( byte * 8 )) & 0xff );
            }
            server_receive_backlog_require( rocketnet_client_transport_finish_packet( clients[i], &context, 8 ) > 0 );
        }
        for ( size_t i = 0; i < clients.size(); ++i )
            rocketnet_client_transport_send_packets( clients[i] );

        // The public cumulative counters include application and protocol-control packets.
        const uint64_t actual_sent = server_receive_backlog_client_packets_sent( clients );
        if ( actual_sent < starting_sent )
            accounting_mismatch = true;
        else
            sent_total = actual_sent - starting_sent;
        if ( received_total > sent_total )
            accounting_mismatch = true;
        // All post-settle client packets target the server loopback endpoint, so the exact
        // sent-minus-received remainder is queued or dropped; capacity splits those counts.
        const uint64_t outstanding = sent_total >= received_total ? sent_total - received_total : 0;
        const uint64_t queue_capacity = (uint64_t) ROCKETNET_LOOPBACK_QUEUE_PACKETS;
        queue_depth = outstanding < queue_capacity ? outstanding : queue_capacity;
        dropped = outstanding - queue_depth;
        if ( frame > 0 && queue_depth > previous_depth )
            growing = true;
        previous_depth = queue_depth;
        time += frame_step;
    }

    if ( payload_total != (uint64_t) max_clients * 299 )
        accounting_mismatch = true;
    const double read_mean = (double) read_sum / 300.0;
    const bool failed = growing || dropped != 0 || accounting_mismatch;
    printf( "server_receive_backlog clients=%d frames=300 packets_read_min=%d packets_read_mean=%.2f packets_read_max=%d queue_depth_last=%llu dropped=%llu growing=%d accounting_ok=%d status=%s\n",
            max_clients, read_min, read_mean, read_max,
            (unsigned long long) queue_depth, (unsigned long long) dropped, growing ? 1 : 0,
            accounting_mismatch ? 0 : 1, failed ? "FAIL" : "PASS" );
    fflush( stdout );

    for ( size_t i = 0; i < clients.size(); ++i )
        rn_native_client_transport_stop_and_destroy( clients[i] );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    rn_native_server_transport_stop_and_destroy( server );

    server_receive_backlog_require( !failed );
}
