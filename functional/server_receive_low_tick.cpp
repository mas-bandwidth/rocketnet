/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The low-tick receive proof: a portable-transport server held at 1000
    clients offering their 256-kbps budget while the server updates at 10 Hz
    must not overflow the kernel receive socket or time a client out. The
    receive socket is sized from max_clients by
    rocketnet_server_receive_buffer_bytes, so this case also pins the grant
    the kernel actually gives on the gate host.
*/

#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "../tests/suite_runner.h"
#include "../tests/portable_crt.h"

#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

uint64_t rocketnet_functional_token_timestamp( uint64_t game_clock_epoch, double time );

static void low_tick_require( bool condition, const char * expression, int line )
{
    if ( condition )
        return;
    printf( "server_receive_low_tick check failed: %s at line %d\n", expression, line );
    fflush( stdout );
    exit( 1 );
}

#define low_tick_require( condition ) low_tick_require( (condition), #condition, __LINE__ )

#if defined(__linux__)
// The drops column of the server socket's /proc/net/udp row. A port match on
// the local address is unique for this process's wildcard bind.
static unsigned long long low_tick_udp_drops( uint16_t port, bool * found )
{
    *found = false;
    FILE * file = fopen( "/proc/net/udp", "r" );
    if ( !file )
        return 0;
    char needle[8];
    snprintf( needle, sizeof( needle ), ":%04X", (unsigned) port );
    char line[512];
    unsigned long long drops = 0;
    if ( !fgets( line, sizeof( line ), file ) )
    {
        fclose( file );
        return 0;
    }
    while ( fgets( line, sizeof( line ), file ) )
    {
        char sl[32], local[128], remote[128], state[64], queues[64], timer[64], retr[32], uid[32], timeout[32], inode[64], ref[32], pointer[64];
        unsigned long long row_drops = 0;
        const int fields = sscanf( line, "%31s %127s %127s %63s %63s %63s %31s %31s %31s %63s %31s %63s %llu",
            sl, local, remote, state, queues, timer, retr, uid, timeout, inode, ref, pointer, &row_drops );
        if ( fields != 13 )
            continue;
        const size_t length = strlen( local );
        if ( length < strlen( needle ) || strcmp( local + length - strlen( needle ), needle ) != 0 )
            continue;
        *found = true;
        drops = row_drops;
        break;
    }
    fclose( file );
    return drops;
}
#endif

void rocketnet_test_server_receive_1000_clients_256kbps_10hz()
{
#if !defined(__linux__)
    // The proof reads the Linux kernel's UDP drop counter.
    printf( "server_receive_low_tick skipped: /proc/net/udp is Linux only\n" );
    fflush( stdout );
    return;
#else
    const int max_clients = 1000;
    const double game_clock_epoch = (double) 1700000000ULL;
    double time = 1000.0;

    low_tick_require( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    low_tick_require( rocketnet_server_light_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = max_clients;
    server_config.protocol_id = 0x1122334455667788ULL;
    static const uint8_t dev_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    memcpy( server_config.backend_public_key, dev_key, sizeof( dev_key ) );
    server_config.transport = "portable";

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    low_tick_require( server != NULL && status == ROCKETNET_STATUS_OK );
    low_tick_require( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    low_tick_require( rocketnet_server_ipv4_address( server, address, sizeof( address ) ) );
    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, public_key );
    const uint16_t port = rocketnet_server_port( server );

    struct rocketnet_client_config_t client_config;
    low_tick_require( rocketnet_client_light_config( &client_config ) == ROCKETNET_STATUS_OK );
    client_config.transport = "portable";

    std::vector<struct rocketnet_client_t*> clients;
    clients.reserve( max_clients );
    for ( int i = 0; i < max_clients; ++i )
    {
        char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        low_tick_require( rocketnet_create_dev_connect_token( address, NULL, public_key, 60, 60, (uint64_t) i + 1,
            server_config.protocol_id, rocketnet_functional_token_timestamp( (uint64_t) game_clock_epoch, time ),
            NULL, token ) == ROCKETNET_STATUS_OK );
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
        low_tick_require( client != NULL && status == ROCKETNET_STATUS_OK );
        low_tick_require( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );
        clients.push_back( client );
    }

    int connected = 0;
    int failed = 0;
    const double connect_deadline = rocketnet_platform_time() + 60.0;
    while ( rocketnet_platform_time() < connect_deadline )
    {
        connected = 0;
        failed = 0;
        for ( size_t i = 0; i < clients.size(); ++i )
        {
            rocketnet_client_update( clients[i], time );
            const struct rocketnet_message_batch_t * batch = NULL;
            while ( ( batch = rocketnet_client_receive_messages( clients[i] ) ) != NULL ) {}
            const enum rocketnet_client_state_t state = rocketnet_client_state( clients[i] );
            if ( state == ROCKETNET_CLIENT_STATE_CONNECTED ) ++connected;
            else if ( state < 0 ) ++failed;
        }
        rocketnet_server_update( server, time );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL ) {}
        if ( connected == max_clients )
            break;
        time += 1.0 / 100.0;
        rocketnet_sleep( 0.001 );
    }
    low_tick_require( connected == max_clients );
    low_tick_require( failed == 0 );

    bool drops_found_before = false, drops_found_after = false;
    const unsigned long long drops_before = low_tick_udp_drops( port, &drops_found_before );

    struct rocketnet_server_stats_t stats_before, stats_after;
    rocketnet_server_stats( server, &stats_before );

    // 10 Hz server, 100 Hz clients: ten client slices per server update. Each
    // client offers one unreliable message per slice; the 256-kbps send budget
    // is what actually reaches the wire, about 32,000 bytes/s per client.
    const double traffic_start = rocketnet_platform_time();
    const double traffic_seconds = 10.0;
    int server_updates = 0;
    const int slices_per_update = 10;
    while ( rocketnet_platform_time() - traffic_start < traffic_seconds )
    {
        for ( int slice = 0; slice < slices_per_update; ++slice )
        {
            for ( size_t i = 0; i < clients.size(); ++i )
            {
                rocketnet_client_update( clients[i], time );
                const struct rocketnet_message_batch_t * batch = NULL;
                while ( ( batch = rocketnet_client_receive_messages( clients[i] ) ) != NULL ) {}
                if ( rocketnet_client_state( clients[i] ) != ROCKETNET_CLIENT_STATE_CONNECTED )
                    continue;
                uint8_t * out = rocketnet_client_start_message( clients[i], 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1200 );
                if ( out )
                {
                    memset( out, (int) ( i & 0xff ), 1200 );
                    low_tick_require( rocketnet_client_finish_message( clients[i], 1200 ) != 0 );
                }
            }
            for ( size_t i = 0; i < clients.size(); ++i )
                rocketnet_client_send_packets( clients[i] );
            time += 0.001;
            rocketnet_sleep( 0.001 );
        }

        rocketnet_server_update( server, time );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL ) {}
        ++server_updates;
        time += 0.1;
    }

    const unsigned long long drops_after = low_tick_udp_drops( port, &drops_found_after );
    rocketnet_server_stats( server, &stats_after );

    int still_connected = 0;
    int timed_out = 0;
    for ( size_t i = 0; i < clients.size(); ++i )
    {
        rocketnet_client_update( clients[i], time );
        const enum rocketnet_client_state_t state = rocketnet_client_state( clients[i] );
        if ( state == ROCKETNET_CLIENT_STATE_CONNECTED ) ++still_connected;
        else if ( state < 0 ) ++timed_out;
    }

    const unsigned long long drops = drops_found_before && drops_found_after ? drops_after - drops_before : 0;
    const uint64_t bytes = stats_after.bytes_received - stats_before.bytes_received;
    const double seconds = rocketnet_platform_time() - traffic_start;
    const double bytes_per_update_10hz = seconds > 0.0 ? (double) bytes / seconds / 10.0 : 0.0;
    printf( "server_receive_low_tick clients=%d server_updates=%d drops=%llu rx_bytes=%llu seconds=%.3f bytes_per_update_10hz=%.0f bytes_per_update_20hz=%.0f bytes_per_update_30hz=%.0f connected=%d timed_out=%d status=%s\n",
        max_clients, server_updates, drops, (unsigned long long) bytes, seconds, bytes_per_update_10hz,
        bytes_per_update_10hz / 2.0, bytes_per_update_10hz / 3.0, still_connected, timed_out,
        ( drops == 0 && timed_out == 0 && still_connected == max_clients ) ? "PASS" : "FAIL" );
    fflush( stdout );

    low_tick_require( drops_found_before && drops_found_after );
    low_tick_require( drops == 0 );
    low_tick_require( timed_out == 0 );
    low_tick_require( still_connected == max_clients );

    for ( size_t i = 0; i < clients.size(); ++i )
    {
        rocketnet_client_disconnect( clients[i] );
        rocketnet_client_destroy( clients[i] );
    }
    rocketnet_server_stop( server );
    rocketnet_server_destroy( server );
    low_tick_require( rocketnet_term() == ROCKETNET_STATUS_OK );
#endif
}
