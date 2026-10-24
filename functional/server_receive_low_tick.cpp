/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    A full server updating at a game tick rate: 1000 portable clients each
    sending 256 kbps of payload to a portable-transport server that updates
    every 0.1 s. Between two updates the server's receive socket must hold
    one update's worth of inbound packets (USAGE.md, "Receive buffer
    sizing"); the case reads the kernel's drop count for the server socket
    from /proc/net/udp before and after, and requires zero drops and zero
    client timeouts. Linux only: the drop count is read from procfs.
*/

#include "../tests/native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_platform.h"
#include "../tests/time_scale.h"
#include "../tests/suite_runner.h"
#include "../tests/portable_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

static const int server_receive_low_tick_max_clients = 1000;
static const int server_receive_low_tick_payload_bytes_per_second = 32000; // 256 kbps of payload per client
static const int server_receive_low_tick_packets_per_client_update = 4;    // 4 x 800 bytes per 0.1 s at 10 Hz
static const int server_receive_low_tick_slices = 10;                 // clients are updated in 10 slices of 100
static const double server_receive_low_tick_traffic_seconds = 10.0;

static void server_receive_low_tick_check( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "server_receive_low_tick check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define server_receive_low_tick_require( condition ) server_receive_low_tick_check( (condition), #condition, __LINE__ )

static long server_receive_low_tick_rss_bound_mb()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = server_receive_low_tick_max_clients;
    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const uint64_t server_bytes = rocketnet_server_transport_required_memory( &server_config, "portable" );
    const uint64_t client_bytes = rocketnet_client_transport_required_memory( &client_config, "portable" );
    if ( !server_bytes || !client_bytes )
        return 0; // a refused plan keeps the strict suite-wide bound
    // 1000 client sockets and their workers: 256 MiB covers the process,
    // thread stacks and allocator rounding
    return suite_rss_bound_mb( server_bytes + (uint64_t) server_receive_low_tick_max_clients * client_bytes, 256 );
}

SUITE_CASE_RSS_BOUND( test_server_receive_1000_clients_256kbps_10hz, server_receive_low_tick_rss_bound_mb );

#if defined(__linux__)

#include <sys/resource.h>

struct server_receive_low_tick_udp_row_t
{
    bool found;
    uint64_t receive_queue_bytes;
    uint64_t drops;
};

// The server socket's row in /proc/net/udp or /proc/net/udp6: the bytes the
// kernel holds for it now (rx_queue) and the datagrams it dropped since the
// socket opened (drops, the last column).
static server_receive_low_tick_udp_row_t server_receive_low_tick_udp_row( uint16_t port )
{
    server_receive_low_tick_udp_row_t row = { false, 0, 0 };
    const char * files[] = { "/proc/net/udp", "/proc/net/udp6" };
    for ( const char * file : files )
    {
        FILE * f = fopen( file, "r" );
        if ( !f )
            continue;
        char line[512];
        bool header = true;
        while ( fgets( line, sizeof( line ), f ) )
        {
            if ( header ) { header = false; continue; }
            char local[128], remote[128];
            unsigned int state = 0;
            unsigned long tx_queue = 0, rx_queue = 0;
            if ( sscanf( line, " %*d: %127s %127s %x %lx:%lx", local, remote, &state, &tx_queue, &rx_queue ) != 5 )
                continue;
            const char * colon = strrchr( local, ':' );
            if ( !colon || (uint16_t) strtoul( colon + 1, NULL, 16 ) != port )
                continue;
            const size_t length = strlen( line );
            size_t end = length;
            while ( end > 0 && ( line[end - 1] == '\n' || line[end - 1] == ' ' ) ) --end;
            size_t start = end;
            while ( start > 0 && line[start - 1] != ' ' ) --start;
            row.found = true;
            row.receive_queue_bytes += rx_queue;
            row.drops += strtoull( line + start, NULL, 10 );
        }
        fclose( f );
    }
    return row;
}

static int server_receive_low_tick_update_hz()
{
    // measurement only: ROCKETNET_LOW_TICK_HZ runs the same load at another
    // update rate (20 or 30) to report inbound bytes per update; the case
    // itself runs at 10 Hz
    char value[32];
    if ( rocketnet_test_getenv( "ROCKETNET_LOW_TICK_HZ", value, sizeof( value ) ) )
    {
        const int hz = atoi( value );
        if ( hz >= 1 && hz <= 1000 )
            return hz;
    }
    return 10;
}

void rocketnet_test_server_receive_1000_clients_256kbps_10hz()
{
    const int max_clients = server_receive_low_tick_max_clients;
    const int update_hz = server_receive_low_tick_update_hz();
    const double update_seconds = 1.0 / update_hz;
    const double slice_seconds = update_seconds / server_receive_low_tick_slices;
    const int clients_per_slice = max_clients / server_receive_low_tick_slices;
    // each client updates once per server update and sends that update's
    // share of 256 kbps, so the rate is the same at every update rate
    const int packet_bytes = server_receive_low_tick_payload_bytes_per_second / ( update_hz * server_receive_low_tick_packets_per_client_update );
    server_receive_low_tick_require( packet_bytes >= 8 );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = max_clients;
    server_config.game_clock_epoch = 1700000000ULL;
    server_config.handshake_deadline_seconds = 30;
    snprintf( server_config.transport, sizeof( server_config.transport ), "%s", "portable" );

    // each portable client holds a socket and wake descriptors: raise the
    // soft descriptor limit toward the hard one, as bin/load does
    struct rlimit limit;
    server_receive_low_tick_require( getrlimit( RLIMIT_NOFILE, &limit ) == 0 );
    const rlim_t wanted = (rlim_t) max_clients * 8 + 1024;
    if ( limit.rlim_cur < wanted )
    {
        limit.rlim_cur = limit.rlim_max < wanted ? limit.rlim_max : wanted;
        server_receive_low_tick_require( setrlimit( RLIMIT_NOFILE, &limit ) == 0 );
    }

    const double start = rocketnet_platform_time();
    double time = 1000.0;

    int status = -1;
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &server_config, time, &status );
    server_receive_low_tick_require( server != NULL );
    server_receive_low_tick_require( status == ROCKETNET_STATUS_OK );
    const uint16_t port = rocketnet_server_transport_port( server );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) port );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.game_clock_epoch = server_config.game_clock_epoch;
    snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "portable" );

    std::vector<struct rocketnet_client_transport_t*> clients;
    clients.reserve( max_clients );
    for ( int i = 0; i < max_clients; ++i )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        server_receive_low_tick_require( rocketnet_write_test_connect_token( server_address, (uint64_t) i + 1, rocketnet_functional_token_timestamp( server_config.game_clock_epoch, time ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, &client_config, time );
        server_receive_low_tick_require( client != NULL );
        clients.push_back( client );
    }

    // connect every client; the clock is the wall clock from 1000.0
    const double connect_deadline = rocketnet_platform_time() + 30.0 * ROCKETNET_FUNCTIONAL_TIME_SCALE;
    int connected = 0;
    while ( true )
    {
        time = 1000.0 + ( rocketnet_platform_time() - start );
        for ( size_t i = 0; i < clients.size(); ++i )
            rocketnet_client_transport_update( clients[i], time );
        rocketnet_server_transport_update( server, time );
        connected = 0;
        for ( size_t i = 0; i < clients.size(); ++i )
            if ( rocketnet_client_transport_state( clients[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                ++connected;
        if ( connected == max_clients && rocketnet_server_transport_num_connected_clients( server ) == max_clients )
            break;
        if ( rocketnet_platform_time() >= connect_deadline )
        {
            printf( "server_receive_low_tick connect deadline: clients_connected=%d server_connected=%d\n",
                    connected, rocketnet_server_transport_num_connected_clients( server ) );
            fflush( stdout );
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    server_receive_low_tick_require( connected == max_clients );

    const server_receive_low_tick_udp_row_t before = server_receive_low_tick_udp_row( port );
    server_receive_low_tick_require( before.found );
    struct rocketnet_server_transport_stats_t stats_before;
    rocketnet_server_transport_stats( server, &stats_before );

    // traffic: every slice updates 100 clients, each sending one update's
    // worth of packets; every 10 slices the server updates once
    const double traffic_start = rocketnet_platform_time();
    double next_slice = traffic_start;
    int slice = 0;
    uint64_t server_updates = 0;
    uint64_t payloads_received = 0;
    uint64_t payload_bytes_received = 0;
    uint64_t receive_queue_peak_bytes = 0;
    while ( rocketnet_platform_time() - traffic_start < server_receive_low_tick_traffic_seconds )
    {
        const double now = rocketnet_platform_time();
        if ( now < next_slice )
        {
            rocketnet_platform_sleep( next_slice - now < 0.001 ? next_slice - now : 0.001 );
            continue;
        }
        next_slice += slice_seconds;
        time = 1000.0 + ( now - start );
        const int first = ( slice % server_receive_low_tick_slices ) * clients_per_slice;
        for ( int i = first; i < first + clients_per_slice; ++i )
        {
            struct rocketnet_client_transport_t * client = clients[(size_t) i];
            for ( int p = 0; p < server_receive_low_tick_packets_per_client_update; ++p )
            {
                struct rocketnet_packet_context_t context;
                uint8_t * packet = rocketnet_client_transport_start_packet( client, packet_bytes, &context );
                if ( !packet )
                    break;
                int capacity = 0;
                uint8_t * payload = rocketnet_client_transport_packet_payload( client, &context, &capacity );
                server_receive_low_tick_require( payload != NULL && capacity >= packet_bytes );
                memset( payload, i & 0xff, (size_t) packet_bytes );
                server_receive_low_tick_require( rocketnet_client_transport_finish_packet( client, &context, packet_bytes ) > 0 );
            }
            rocketnet_client_transport_send_packets( client );
            rocketnet_client_transport_update( client, time );
        }
        ++slice;
        if ( slice % server_receive_low_tick_slices == 0 )
        {
            // the kernel's hold for the socket just before the update reads it
            const server_receive_low_tick_udp_row_t row = server_receive_low_tick_udp_row( port );
            if ( row.receive_queue_bytes > receive_queue_peak_bytes )
                receive_queue_peak_bytes = row.receive_queue_bytes;
            rocketnet_server_transport_update( server, time );
            ++server_updates;
            const int num_payloads = rocketnet_server_transport_num_payloads( server );
            for ( int i = 0; i < num_payloads; ++i )
            {
                int client_index = -1, bytes = 0;
                if ( rocketnet_server_transport_payload( server, i, &client_index, &bytes ) )
                {
                    ++payloads_received;
                    payload_bytes_received += (uint64_t) bytes;
                }
            }
        }
    }

    const server_receive_low_tick_udp_row_t after = server_receive_low_tick_udp_row( port );
    struct rocketnet_server_transport_stats_t stats_after;
    rocketnet_server_transport_stats( server, &stats_after );

    int clients_connected = 0;
    for ( size_t i = 0; i < clients.size(); ++i )
        if ( rocketnet_client_transport_state( clients[i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            ++clients_connected;
    const int server_connected = rocketnet_server_transport_num_connected_clients( server );
    const uint64_t kernel_drops = after.drops - before.drops;
    const uint64_t wire_bytes = stats_after.bytes_received - stats_before.bytes_received;
    const uint64_t wire_packets = stats_after.packets_received - stats_before.packets_received;

    printf( "server_receive_low_tick update_hz=%d clients=%d traffic_seconds=%.1f server_updates=%llu "
            "wire_packets=%llu wire_bytes=%llu wire_bytes_per_update=%llu payloads=%llu payload_bytes=%llu "
            "per_client_wire_kbps=%.1f receive_queue_peak_bytes=%llu kernel_receive_drops=%llu "
            "clients_connected=%d server_connected=%d status=%s\n",
            update_hz, max_clients, server_receive_low_tick_traffic_seconds, (unsigned long long) server_updates,
            (unsigned long long) wire_packets, (unsigned long long) wire_bytes,
            (unsigned long long) ( server_updates ? wire_bytes / server_updates : 0 ),
            (unsigned long long) payloads_received, (unsigned long long) payload_bytes_received,
            (double) wire_bytes * 8.0 / ( server_receive_low_tick_traffic_seconds * 1000.0 * max_clients ),
            (unsigned long long) receive_queue_peak_bytes, (unsigned long long) kernel_drops,
            clients_connected, server_connected,
            ( kernel_drops == 0 && clients_connected == max_clients && server_connected == max_clients ) ? "PASS" : "FAIL" );
    fflush( stdout );

    server_receive_low_tick_require( kernel_drops == 0 );
    server_receive_low_tick_require( clients_connected == max_clients );
    server_receive_low_tick_require( server_connected == max_clients );

    for ( size_t i = 0; i < clients.size(); ++i )
        rn_native_client_transport_stop_and_destroy( clients[i] );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, time );
    rn_native_server_transport_stop_and_destroy( server );
}

#else // #if defined(__linux__)

void rocketnet_test_server_receive_1000_clients_256kbps_10hz()
{
    // the kernel drop count is read from /proc/net/udp
    printf( "server_receive_low_tick skipped: linux only\n" );
    fflush( stdout );
}

#endif // #if defined(__linux__)
