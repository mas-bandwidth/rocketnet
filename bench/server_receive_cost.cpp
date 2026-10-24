/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// The server's worst-case receive update: max_clients connected clients on the
// loopback transport, each delivering ROCKETNET_RECEIVE_PACKETS_PER_CLIENT
// payload packets, so one update processes the whole receive budget
// (ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE, 16,000 at 1000 clients).
//
// The loopback queue holds less than a full budget, so the packets the
// clients seal are captured first, outside the timed window, by a receive
// that stores them and hands the server nothing; the timed update then
// receives the whole capture in one budget, as a backlogged socket would
// hand it over. Every allocation comes from one arena reserved before
// rocketnet_init, and the arena counts any allocation or free inside a timed
// window (the run fails on one). Time is simulated, from 1000.0.
//
// server_receive_cost [clients] [packets_per_client] [updates] [payload_bytes] [arena_gib]
// prints update_us p50, p99 and max over the timed updates.

#include "../tests/native_shutdown.h"
#include "../sdk/rocketnet_server_transport.cpp"
#include "rocketnet_connect_token.h"

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "server receive cost bench failed: %s, line %d\n", #x, __LINE__); exit(1); } } while (0)

// ---- the arena -------------------------------------------------------------

struct receive_cost_arena_t
{
    uint8_t * memory;
    size_t capacity;
    size_t used;
    bool in_window;
    uint64_t window_allocations;
    uint64_t window_frees;
};

static receive_cost_arena_t receive_cost_arena;

static void * receive_cost_allocate( void *, size_t bytes )
{
    if ( receive_cost_arena.in_window )
        receive_cost_arena.window_allocations++;
    const size_t aligned = ( bytes + 63 ) & ~(size_t) 63;
    if ( receive_cost_arena.used + aligned > receive_cost_arena.capacity )
    {
        fprintf( stderr, "server receive cost bench: arena exhausted at %zu bytes; raise arena_gib\n", receive_cost_arena.used );
        exit( 1 );
    }
    void * p = receive_cost_arena.memory + receive_cost_arena.used;
    receive_cost_arena.used += aligned;
    return p;
}

static void receive_cost_free( void *, void * p )
{
    if ( p && receive_cost_arena.in_window )
        receive_cost_arena.window_frees++;
}

// ---- the capture and replay receive ------------------------------------------

typedef int (*receive_cost_receive_batch_t)( void *, rocketnet_address_t *, uint8_t *, int, int *, int );

struct receive_cost_capture_t
{
    receive_cost_receive_batch_t loopback_receive_batch;
    bool replay;
    int capacity;
    int count;
    int cursor;
    rocketnet_address_t * from;
    int * packet_bytes;
    uint8_t * data;
};

static receive_cost_capture_t receive_cost_capture;

static int receive_cost_receive_batch( void * instance, rocketnet_address_t * from, uint8_t * data, int stride, int * packet_bytes, int capacity )
{
    receive_cost_capture_t * capture = &receive_cost_capture;
    if ( !capture->replay )
    {
        const int received = capture->loopback_receive_batch( instance, from, data, stride, packet_bytes, capacity );
        for ( int i = 0; i < received && capture->count < capture->capacity; i++ )
        {
            CHECK( packet_bytes[i] >= 0 && packet_bytes[i] <= ROCKETNET_RECEIVE_CEILING );
            capture->from[capture->count] = from[i];
            capture->packet_bytes[capture->count] = packet_bytes[i];
            memcpy( capture->data + (size_t) capture->count * ROCKETNET_RECEIVE_CEILING, data + (size_t) i * (size_t) stride, (size_t) packet_bytes[i] );
            capture->count++;
        }
        return 0;
    }
    int n = capture->count - capture->cursor;
    if ( n > capacity )
        n = capacity;
    for ( int i = 0; i < n; i++ )
    {
        const int index = capture->cursor + i;
        from[i] = capture->from[index];
        packet_bytes[i] = capture->packet_bytes[index];
        memcpy( data + (size_t) i * (size_t) stride, capture->data + (size_t) index * ROCKETNET_RECEIVE_CEILING, (size_t) capture->packet_bytes[index] );
    }
    capture->cursor += n;
    return n;
}

// ---- the run ---------------------------------------------------------------

static double receive_cost_clock = 1000.0;

static double receive_cost_time_callback( void * )
{
    return receive_cost_clock;
}

static double percentile( std::vector<double> samples, double p )
{
    std::sort( samples.begin(), samples.end() );
    const size_t n = samples.size();
    if ( n == 0 ) return 0.0;
    const double rank = ( p / 100.0 ) * (double) n;
    size_t index = (size_t) rank;
    if ( (double) index < rank ) index++;
    if ( index > 0 ) index--;
    return samples[index < n ? index : n - 1];
}

int main( int argc, char ** argv )
{
    const int num_clients = argc > 1 ? atoi( argv[1] ) : ROCKETNET_MAX_CLIENTS;
    const int packets_per_client = argc > 2 ? atoi( argv[2] ) : ROCKETNET_RECEIVE_PACKETS_PER_CLIENT;
    const int updates = argc > 3 ? atoi( argv[3] ) : 50;
    const int payload_bytes = argc > 4 ? atoi( argv[4] ) : 100;
    const int arena_gib = argc > 5 ? atoi( argv[5] ) : 16;
    CHECK( num_clients >= 1 && num_clients <= ROCKETNET_MAX_CLIENTS );
    CHECK( packets_per_client >= 1 && num_clients * packets_per_client <= ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE );
    CHECK( updates >= 1 );
    CHECK( payload_bytes >= 1 && payload_bytes <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    CHECK( arena_gib >= 1 );

    receive_cost_arena.capacity = (size_t) arena_gib << 30;
    receive_cost_arena.memory = (uint8_t *) malloc( receive_cost_arena.capacity );
    CHECK( receive_cost_arena.memory );

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    CHECK( rocketnet_init( NULL, receive_cost_allocate, receive_cost_free ) == ROCKETNET_STATUS_OK );

    const int budget = num_clients * packets_per_client;
    receive_cost_capture.capacity = 2 * ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE;
    receive_cost_capture.from = (rocketnet_address_t *) receive_cost_allocate( NULL, sizeof( rocketnet_address_t ) * (size_t) receive_cost_capture.capacity );
    receive_cost_capture.packet_bytes = (int *) receive_cost_allocate( NULL, sizeof( int ) * (size_t) receive_cost_capture.capacity );
    receive_cost_capture.data = (uint8_t *) receive_cost_allocate( NULL, (size_t) ROCKETNET_RECEIVE_CEILING * (size_t) receive_cost_capture.capacity );

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = num_clients;
    snprintf( config.transport, sizeof( config.transport ), "%s", "loopback" );
    int status = 0;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, receive_cost_clock, &status );
    CHECK( server );
    CHECK( !rocketnet_server_transport_staged( server ) );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", rocketnet_server_transport_port( server ) );

    std::vector<rocketnet_client_transport_t *> clients( (size_t) num_clients, NULL );
    for ( int i = 0; i < num_clients; i++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        CHECK( rocketnet_write_test_connect_token( address, (uint64_t) ( 1000 + i ), (uint64_t) time( NULL ), token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
        rocketnet_client_transport_config_t client_config;
        rocketnet_client_transport_default_config( &client_config );
        snprintf( client_config.transport, sizeof( client_config.transport ), "%s", "loopback" );
        client_config.step_driven = 1;
        client_config.time_callback = receive_cost_time_callback;
        clients[(size_t) i] = rocketnet_client_transport_create( NULL, token, &client_config, receive_cost_clock );
        CHECK( clients[(size_t) i] );
    }

    // connect every client, stepped by 10 ms of simulated time
    int connected = 0;
    for ( int step = 0; step < 3000 && connected < num_clients; step++ )
    {
        receive_cost_clock += 0.01;
        rocketnet_server_transport_update( server, receive_cost_clock );
        rocketnet_server_transport_send_packets( server );
        connected = 0;
        for ( int i = 0; i < num_clients; i++ )
        {
            rocketnet_client_transport_update( clients[(size_t) i], receive_cost_clock );
            rocketnet_client_transport_send_packets( clients[(size_t) i] );
            if ( rocketnet_client_transport_state( clients[(size_t) i] ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                connected++;
        }
    }
    CHECK( connected == num_clients );
    CHECK( rocketnet_server_transport_num_connected_clients( server ) == num_clients );

    // the capture receive in front of the loopback transport's own
    static rocketnet_server_transport_io_operations_t operations;
    const rocketnet_server_transport_io_operations_t * loopback_operations = server->io.operations;
    operations = *loopback_operations;
    receive_cost_capture.loopback_receive_batch = operations.receive_batch;
    operations.receive_batch = receive_cost_receive_batch;
    server->io.operations = &operations;

    std::vector<double> update_us;
    update_us.reserve( (size_t) updates );
    uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    memset( payload, 0x5a, sizeof( payload ) );

    // the loopback queue holds ROCKETNET_LOOPBACK_QUEUE_PACKETS: clients send
    // in groups that fit it, each group captured by one server update
    const int clients_per_group = std::max( 1, ( ROCKETNET_LOOPBACK_QUEUE_PACKETS / 2 ) / packets_per_client );
    const int warmup = 5;
    int short_updates = 0;

    for ( int u = -warmup; u < updates; u++ )
    {
        receive_cost_capture.replay = false;
        receive_cost_capture.count = 0;
        receive_cost_capture.cursor = 0;
        for ( int first = 0; first < num_clients; first += clients_per_group )
        {
            const int last = std::min( num_clients, first + clients_per_group );
            for ( int i = first; i < last; i++ )
                for ( int p = 0; p < packets_per_client; p++ )
                    rocketnet_client_transport_send_packet( clients[(size_t) i], payload, payload_bytes );
            receive_cost_clock += 0.0001;
            rocketnet_server_transport_update( server, receive_cost_clock );
        }

        receive_cost_capture.replay = true;
        receive_cost_clock += 0.01;
        receive_cost_arena.in_window = true;
        const auto start = std::chrono::steady_clock::now();
        rocketnet_server_transport_update( server, receive_cost_clock );
        const auto end = std::chrono::steady_clock::now();
        receive_cost_arena.in_window = false;

        const int * client_index;
        uint8_t * const * payload_data;
        const int * received_bytes;
        const int delivered = rocketnet_server_transport_payloads( server, &client_index, &payload_data, &received_bytes );
        if ( delivered < budget )
            short_updates++;
        if ( u >= 0 )
            update_us.push_back( std::chrono::duration<double, std::micro>( end - start ).count() );

        // drain, outside the window: the server's acks and keep-alives to
        // the clients, and theirs back into the next capture
        rocketnet_server_transport_send_packets( server );
        for ( int i = 0; i < num_clients; i++ )
            rocketnet_client_transport_update( clients[(size_t) i], receive_cost_clock );
    }

    CHECK( receive_cost_arena.window_allocations == 0 );
    CHECK( receive_cost_arena.window_frees == 0 );
    CHECK( rocketnet_server_transport_num_connected_clients( server ) == num_clients );

    printf( "server_receive_cost clients=%d packets_per_client=%d packets_per_update=%d payload_bytes=%d updates=%d"
            " update_us_p50=%.1f update_us_p99=%.1f update_us_max=%.1f short_updates=%d window_allocations=%llu arena_mib=%zu\n",
            num_clients, packets_per_client, budget, payload_bytes, updates,
            percentile( update_us, 50.0 ), percentile( update_us, 99.0 ), *std::max_element( update_us.begin(), update_us.end() ),
            short_updates, (unsigned long long) receive_cost_arena.window_allocations, receive_cost_arena.used >> 20 );

    server->io.operations = loopback_operations;
    for ( int i = 0; i < num_clients; i++ )
        rn_native_client_transport_stop_and_destroy( clients[(size_t) i] );
    rn_native_server_transport_stop_and_destroy( server );
    rocketnet_term();
    free( receive_cost_arena.memory );
    return 0;
}
