/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The server's per-frame update on the main thread with the ack shape of the
// 10 Gb/s bar: N connected clients on the loopback transport, the server
// sending each of them S unreliable messages of B bytes a frame (one packet
// each), every client receiving them and sending K packets back, each with
// its ack block. Only rocketnet_server_update is measured: its wall time per
// frame, and on Linux its user-mode instructions and cycles from the
// thread's own hardware counters, enabled around the call alone. Under
// callgrind, --toggle-collect=rocketnet_server_update counts the same window.
//
// Time is simulated from 1000.0 at 60 Hz; nothing sleeps. Every client sends
// the same K packets each frame and the server the same S, so a run is the
// same workload before and after a change: compare the counts per frame.
//
// server_update_ack_cost [clients] [server_packets] [client_packets] [frames] [warmup] [server_bytes] [client_bytes]
// defaults: 1000 17 1 300 60 1200 64

#include "rocketnet.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_base64.h"

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#if defined( __linux__ )
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#define ACK_COST_INSTRUCTIONS PERF_COUNT_HW_INSTRUCTIONS
#define ACK_COST_CYCLES PERF_COUNT_HW_CPU_CYCLES
#else
#define ACK_COST_INSTRUCTIONS 0
#define ACK_COST_CYCLES 0
#endif // #if defined( __linux__ )

#define CHECK( x ) do { if ( !( x ) ) { fprintf( stderr, "server update ack cost bench failed: %s, line %d\n", #x, __LINE__ ); exit( 1 ); } } while ( 0 )

#define ACK_COST_PROTOCOL 0x51E7D41A92B3C6D9ULL

static double ack_cost_now = 1000.0;

static double ack_cost_clock( void * )
{
    return ack_cost_now;
}

static double percentile( std::vector<double> samples, double p )
{
    std::sort( samples.begin(), samples.end() );

    const size_t n = samples.size();
    if ( n == 0 )
    {
        return 0.0;
    }

    const double rank = ( p / 100.0 ) * (double) n;
    size_t index = (size_t) rank;
    if ( (double) index < rank )
    {
        index++;
    }

    if ( index > 0 )
    {
        index--;
    }

    return samples[index < n ? index : n - 1];
}

// ---- the thread's own counters, user mode, enabled around update alone -----

struct ack_cost_counter_t
{
    int fd;
    uint64_t total;
};

static void ack_cost_counter_open( ack_cost_counter_t * counter, uint64_t config )
{
    counter->fd = -1;
    counter->total = 0;
#if defined( __linux__ )
    struct perf_event_attr attr;
    memset( &attr, 0, sizeof( attr ) );
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof( attr );
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    counter->fd = (int) syscall( SYS_perf_event_open, &attr, 0, -1, -1, 0 );
#else
    (void) config;
#endif // #if defined( __linux__ )
}

static void ack_cost_counter_enable( ack_cost_counter_t * counter )
{
#if defined( __linux__ )
    if ( counter->fd >= 0 )
    {
        ioctl( counter->fd, PERF_EVENT_IOC_RESET, 0 );
        ioctl( counter->fd, PERF_EVENT_IOC_ENABLE, 0 );
    }
#else
    (void) counter;
#endif // #if defined( __linux__ )
}

static uint64_t ack_cost_counter_disable( ack_cost_counter_t * counter )
{
#if defined( __linux__ )
    if ( counter->fd >= 0 )
    {
        ioctl( counter->fd, PERF_EVENT_IOC_DISABLE, 0 );
        uint64_t value = 0;
        if ( read( counter->fd, &value, sizeof( value ) ) == (ssize_t) sizeof( value ) )
        {
            counter->total += value;
            return value;
        }
    }
#else
    (void) counter;
#endif // #if defined( __linux__ )
    return 0;
}

int main( int argc, char ** argv )
{
    const int num_clients = argc > 1 ? atoi( argv[1] ) : 1000;
    const int server_packets = argc > 2 ? atoi( argv[2] ) : 17;
    const int client_packets = argc > 3 ? atoi( argv[3] ) : 1;
    const int frames = argc > 4 ? atoi( argv[4] ) : 300;
    const int warmup = argc > 5 ? atoi( argv[5] ) : 60;
    const int server_bytes = argc > 6 ? atoi( argv[6] ) : 1200;
    const int client_bytes = argc > 7 ? atoi( argv[7] ) : 64;
    CHECK( num_clients >= 1 && num_clients <= ROCKETNET_MAX_CLIENTS );
    CHECK( server_packets >= 0 && client_packets >= 0 && frames >= 1 && warmup >= 0 );
    CHECK( server_bytes >= 1 && client_bytes >= 1 );

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_quiet( 1 );

    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, num_clients, NULL );
    CHECK( hub );

    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
    {
        seed[i] = (uint8_t) ( 0xA5 ^ i );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    CHECK( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = ACK_COST_PROTOCOL;
    server_config.max_clients = num_clients;
    server_config.per_client_send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;

    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    CHECK( server );
    CHECK( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };

    std::vector<rocketnet_client_t *> clients( (size_t) num_clients, NULL );
    for ( int i = 0; i < num_clients; i++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        CHECK( rocketnet_write_production_connect_token( backend_secret, server_public, ACK_COST_PROTOCOL, addresses, 1,
                                                         (uint64_t) ( 0x5EED + i ), 1000, 1ULL << 40, 300, NULL,
                                                         token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        CHECK( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) > 0 );

        rocketnet_client_config_t client_config;
        rocketnet_client_default_config( &client_config );
        client_config.transport = "loopback";
        client_config.loopback_hub = hub;
        client_config.step_driven = true;
        client_config.time_callback = ack_cost_clock;
        clients[(size_t) i] = rocketnet_client_create( NULL, &client_config, NULL );
        CHECK( clients[(size_t) i] );
        CHECK( rocketnet_client_connect( clients[(size_t) i], token_string ) == ROCKETNET_STATUS_OK );
    }

    const double frame_seconds = 1.0 / 60.0;

    int clients_connected = 0;
    for ( int step = 0; step < 6000 && ( clients_connected < num_clients || rocketnet_server_num_connected_clients( server ) < num_clients ); step++ )
    {
        ack_cost_now += 0.01;
        clients_connected = 0;
        for ( int i = 0; i < num_clients; i++ )
        {
            rocketnet_client_update( clients[(size_t) i], ack_cost_now );
            rocketnet_client_send_packets( clients[(size_t) i] );
            if ( rocketnet_client_state( clients[(size_t) i] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                clients_connected++;
            }
        }
        rocketnet_server_update( server, ack_cost_now );
        rocketnet_server_send_packets( server );
    }

    CHECK( rocketnet_server_num_connected_clients( server ) == num_clients );

    std::vector<int> slot_of( (size_t) num_clients, -1 );
    for ( int i = 0; i < num_clients; i++ )
    {
        CHECK( rocketnet_client_state( clients[(size_t) i] ) == ROCKETNET_CLIENT_STATE_CONNECTED );
        slot_of[(size_t) i] = rocketnet_client_index( clients[(size_t) i] );
        CHECK( slot_of[(size_t) i] >= 0 && slot_of[(size_t) i] < num_clients );
    }

    ack_cost_counter_t instructions;
    ack_cost_counter_t cycles;
    ack_cost_counter_open( &instructions, ACK_COST_INSTRUCTIONS );
    ack_cost_counter_open( &cycles, ACK_COST_CYCLES );

    std::vector<double> update_us;
    std::vector<double> update_instructions;
    std::vector<double> update_cycles;
    update_us.reserve( (size_t) frames );

    uint64_t server_messages_received = 0;
    uint64_t client_messages_received = 0;
    uint64_t server_sends_refused = 0;
    uint64_t client_sends_refused = 0;

    for ( int frame = -warmup; frame < frames; frame++ )
    {
        ack_cost_now += frame_seconds;

        // the clients: receive the server's packets of the last frame, then
        // send theirs, each carrying an ack block

        for ( int i = 0; i < num_clients; i++ )
        {
            rocketnet_client_t * client = clients[(size_t) i];
            rocketnet_client_update( client, ack_cost_now );

            const struct rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_client_receive_messages( client ) ) )
            {
                client_messages_received += (uint64_t) batch->num_messages;
            }

            for ( int p = 0; p < client_packets; p++ )
            {
                uint8_t * data = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, client_bytes );
                if ( !data )
                {
                    client_sends_refused++;
                    continue;
                }
                memset( data, p, (size_t) client_bytes );
                rocketnet_client_finish_message( client, client_bytes );

                // one packet per message: each carries its own ack block

                rocketnet_client_send_packets( client );
            }
        }

        // the measured call: the server's update on the main thread

        const auto start = std::chrono::steady_clock::now();
        ack_cost_counter_enable( &instructions );
        ack_cost_counter_enable( &cycles );

        rocketnet_server_update( server, ack_cost_now );

        const uint64_t frame_cycles = ack_cost_counter_disable( &cycles );
        const uint64_t frame_instructions = ack_cost_counter_disable( &instructions );
        const auto end = std::chrono::steady_clock::now();

        if ( frame >= 0 )
        {
            update_us.push_back( std::chrono::duration<double, std::micro>( end - start ).count() );
            update_instructions.push_back( (double) frame_instructions );
            update_cycles.push_back( (double) frame_cycles );
        }
        else
        {
            instructions.total = 0;
            cycles.total = 0;
        }

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) )
        {
            server_messages_received += (uint64_t) batch->num_messages;
        }

        // the server's frame of sends: S packets to every client

        for ( int i = 0; i < num_clients; i++ )
        {
            for ( int p = 0; p < server_packets; p++ )
            {
                uint8_t * data = rocketnet_server_start_message( server, (uint64_t) slot_of[(size_t) i], 0, ROCKETNET_RELIABILITY_UNRELIABLE, server_bytes );
                if ( !data )
                {
                    server_sends_refused++;
                    continue;
                }
                memset( data, p, (size_t) server_bytes );
                rocketnet_server_finish_message( server, server_bytes );
            }
        }

        rocketnet_server_send_packets( server );
    }

    CHECK( rocketnet_server_num_connected_clients( server ) == num_clients );

    struct rocketnet_server_stats_t stats;
    rocketnet_server_stats( server, &stats );

    printf( "server_update_ack_cost clients=%d server_packets=%d client_packets=%d frames=%d server_bytes=%d client_bytes=%d"
            " update_us_p50=%.1f update_us_p99=%.1f update_us_max=%.1f"
            " instructions_per_frame=%.0f instructions_p50=%.0f cycles_per_frame=%.0f cycles_p50=%.0f"
            " server_messages_received=%llu client_messages_received=%llu server_sends_refused=%llu client_sends_refused=%llu\n",
            num_clients, server_packets, client_packets, frames, server_bytes, client_bytes,
            percentile( update_us, 50.0 ), percentile( update_us, 99.0 ), *std::max_element( update_us.begin(), update_us.end() ),
            (double) instructions.total / (double) frames, percentile( update_instructions, 50.0 ),
            (double) cycles.total / (double) frames, percentile( update_cycles, 50.0 ),
            (unsigned long long) server_messages_received, (unsigned long long) client_messages_received,
            (unsigned long long) server_sends_refused, (unsigned long long) client_sends_refused );

    for ( int i = 0; i < num_clients; i++ )
    {
        rocketnet_client_destroy( clients[(size_t) i] );
    }

    rocketnet_server_stop( server );
    for ( int step = 0; step < 64 && !rocketnet_server_stopped( server ); step++ )
    {
        ack_cost_now += 0.01;
        rocketnet_server_update( server, ack_cost_now );
        rocketnet_server_send_packets( server );
    }
    rocketnet_server_destroy( server );
    CHECK( rocketnet_loopback_hub_destroy( hub ) == ROCKETNET_STATUS_OK );
    rocketnet_term();

    return 0;
}
