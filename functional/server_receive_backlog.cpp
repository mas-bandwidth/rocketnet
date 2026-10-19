/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"
#include "rocketnet_driver_loopback.h"

#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <stdlib.h>
#else
#include <unistd.h>
#endif

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
        : name( env_name ), had_value( getenv( env_name ) != NULL ), value( had_value ? getenv( env_name ) : "" )
    {
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

static bool server_receive_backlog_all_connected( const std::vector<struct rocketnet_client_transport_t*> & clients )
{
    for ( size_t i = 0; i < clients.size(); ++i )
        if ( rocketnet_client_transport_state( clients[i] ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            return false;
    return true;
}

void rocketnet_test_server_receive_keeps_up_with_max_clients()
{
    server_receive_backlog_env_t driver_env( "ROCKETNET_DRIVER" );
    server_receive_backlog_env_t server_env( "ROCKETNET_SERVER_TRANSPORT" );
    server_receive_backlog_env_t client_env( "ROCKETNET_CLIENT_TRANSPORT" );

    const double frame_step = 1.0 / 60.0;
    double time = 1000.0;

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = ROCKETNET_MAX_CLIENTS;
    snprintf( server_config.driver, sizeof( server_config.driver ), "%s", "loopback" );

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, time, &status );
    server_receive_backlog_require( server != NULL );
    server_receive_backlog_require( status == ROCKETNET_STATUS_OK );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_transport_port( server ) );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    snprintf( client_config.driver, sizeof( client_config.driver ), "%s", "loopback" );

    std::vector<struct rocketnet_client_transport_t*> clients;
    clients.reserve( ROCKETNET_MAX_CLIENTS );
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        server_receive_backlog_require( rocketnet_write_dev_connect_token( server_address, (uint64_t) i + 1, 4102444800ull, token ) != 0 );
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, time );
        server_receive_backlog_require( client != NULL );
        clients.push_back( client );
    }

    bool connected = false;
    for ( int attempt = 0; attempt < 600; ++attempt )
    {
        for ( size_t i = 0; i < clients.size(); ++i )
            rocketnet_client_transport_update( clients[i], time );
        rocketnet_server_transport_update( server, time );
        time += frame_step;

        if ( server_receive_backlog_all_connected( clients ) &&
             rocketnet_server_transport_num_connected_clients( server ) == ROCKETNET_MAX_CLIENTS )
        {
            connected = true;
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

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        server_receive_backlog_require( num_payloads == ROCKETNET_MAX_CLIENTS );
        payload_total += (uint64_t) num_payloads;
        for ( int payload_index = 0; payload_index < num_payloads; ++payload_index )
        {
            int client_index = -1;
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_server_transport_payload( server, payload_index, &client_index, &payload_bytes );
            server_receive_backlog_require( payload != NULL );
            server_receive_backlog_require( client_index >= 0 && client_index < ROCKETNET_MAX_CLIENTS );
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
        queue_depth = outstanding < ROCKETNET_LOOPBACK_QUEUE_PACKETS ? outstanding : ROCKETNET_LOOPBACK_QUEUE_PACKETS;
        dropped = outstanding - queue_depth;
        if ( frame > 0 && queue_depth > previous_depth )
            growing = true;
        previous_depth = queue_depth;
        time += frame_step;
    }

    if ( payload_total != (uint64_t) ROCKETNET_MAX_CLIENTS * 300 )
        accounting_mismatch = true;
    const double read_mean = (double) read_sum / 300.0;
    const bool failed = growing || dropped != 0 || accounting_mismatch;
    printf( "server_receive_backlog clients=%d frames=300 packets_read_min=%d packets_read_mean=%.2f packets_read_max=%d queue_depth_last=%llu dropped=%llu growing=%d accounting_ok=%d status=%s\n",
            ROCKETNET_MAX_CLIENTS, read_min, read_mean, read_max,
            (unsigned long long) queue_depth, (unsigned long long) dropped, growing ? 1 : 0,
            accounting_mismatch ? 0 : 1, failed ? "FAIL" : "PASS" );
    fflush( stdout );

    for ( size_t i = 0; i < clients.size(); ++i )
        rocketnet_client_transport_destroy( clients[i] );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    rocketnet_server_transport_destroy( server );

    server_receive_backlog_require( !failed );
}
