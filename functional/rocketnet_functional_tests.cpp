/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional suite: every test here opens a socket or starts a real driver.
    The assertions are the ones the unit suite used to run. A test that cannot
    run without a socket stays here. The unit suite drives the same rules
    through the state machines and the in-memory simulator.
*/

#include "harness.inc"

void rocketnet_test_server_refuses_keyless_config()
{
    // the no-default-keys gate: a config with no keys is refused, never
    // silently defaulted -- default keys are how every deployment of a
    // protocol ends up sharing one identity. the dev posture is explicit.

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config, 0.0 ) == NULL );

    // and out-of-range client counts are refused at the same gate

    rocketnet_server_transport_dev_config( &config );
    config.max_clients = 0;
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config, 0.0 ) == NULL );
    config.max_clients = ROCKETNET_MAX_CLIENTS + 1;
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40888", &config, 0.0 ) == NULL );
}

struct rocketnet_test_hammer_args_t
{
    struct rocketnet_server_transport_t * server;
    int thread_id;
    int packets;
};

static void rocketnet_test_hammer_thread( void * data )
{
    struct rocketnet_test_hammer_args_t * args = (struct rocketnet_test_hammer_args_t*) data;
    for ( int i = 0; i < args->packets; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( args->server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        rocketnet_check( frame );
        uint8_t * payload = frame + context.payload_index;
        payload[0] = (uint8_t) args->thread_id;
        payload[1] = (uint8_t) i;
        for ( int j = 2; j < 16; j++ )
            payload[j] = (uint8_t) ( args->thread_id * 31 + i * 7 + j );
        rocketnet_server_transport_finish_packet( args->server, &context, 16 );
    }
}

void rocketnet_test_transport_disconnect_delivery()
{
    // the disconnect burst through the TRANSPORT, not handed to the client
    // protocol by hand -- the reviewer found the transport's disconnect
    // producer writing at the wrong frame offset while the protocol-level
    // test stayed green, because it never crossed the transport. this one
    // crosses it: the client must reach DISCONNECTED long before its 10s
    // timeout could explain it.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    rocketnet_server_transport_disconnect_client( server, 0 );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 3.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

static int rocketnet_test_fill_server_ring( rocketnet_server_transport_t * server, int completion_capacity )
{
    // IOCP may still own earlier sends. Keep every successful reservation in
    // this fill, without flushing, while completion workers return the rest.
    // A lost frame still fails the exact-capacity check after a bounded wait.
    const double deadline = rocketnet_platform_time() + 5.0;
    int count = 0;
    while ( count < 20000 )
    {
        rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        if ( !frame )
        {
            if ( count < completion_capacity && rocketnet_platform_time() < deadline )
            {
                rocketnet_platform_sleep( 0.001 );
                continue;
            }
            break;
        }
        frame[context.payload_index] = (uint8_t) count;
        rocketnet_check( rocketnet_server_transport_finish_packet( server, &context, 1 ) > 0 );
        count++;
    }
    if ( completion_capacity && count != completion_capacity )
        printf( "ring fill: driver=%s accepted=%d expected=%d\n", rocketnet_server_transport_driver( server ), count, completion_capacity );
    return count;
}

void rocketnet_test_transport_ring_exhaustion()
{
    // fill past the ring, and the overflow refuses cleanly: starts beyond
    // capacity return NULL, the counter names the reason, and the full
    // capacity returns after flush and any asynchronous send completions.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // Synchronous drivers are measured at their first refusal. IOCP's pool
    // has a known size, but even handshake sends may still be retiring at
    // the first fill. Wait for that whole pool, not an incidental snapshot.
    int completion_capacity = 0;
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    if ( strcmp( rocketnet_server_transport_driver( server ), "iocp" ) == 0 )
        completion_capacity = ROCKETNET_IOCP_SEND_QUEUE_SIZE;
#endif
    const int capacity = rocketnet_test_fill_server_ring( server, completion_capacity );
    rocketnet_check( capacity >= 256 );
    rocketnet_check( capacity < 20000 );
    rocketnet_check( completion_capacity == 0 || capacity == completion_capacity );

    // refusal holds until the flush -- 44 more starts all refuse, and the
    // counter attributes every refusal exactly (the fill already incurred
    // refusals, so the baseline is read here)

    const uint64_t ring_full_before = rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_RING_FULL];
    for ( int i = 0; i < 44; i++ )
    {
        struct rocketnet_packet_context_t context;
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context ) == NULL );
    }
    rocketnet_check( rocketnet_server_transport_counters( server )->values[ROCKETNET_COUNTER_SEND_RING_FULL] == ring_full_before + 44 );

    rocketnet_server_transport_send_packets( server );

    // Flush submits IOCP sends; it does not synchronously reclaim them.
    // Accumulate returned frames in this one fill until the same exact
    // capacity is reached. No flush or transport update assists the refill.
    const int refill = rocketnet_test_fill_server_ring( server, completion_capacity );
    if ( refill != capacity )
        printf( "ring refill: driver=%s first=%d second=%d\n", rocketnet_server_transport_driver( server ), capacity, refill );
    rocketnet_check( refill == capacity );

    rocketnet_server_transport_send_packets( server );

    // the next frame is whole: fresh starts succeed and one arrives. the
    // payload is unreliable and the flood may have left the client's kernel
    // receive buffer overflowing -- a driver with deep staging floods far
    // past what the buffer holds, and packets lost there are priced as lost
    // on the wire -- so recovery sends a fresh "again" every iteration
    // until one is observed, the transport's own retry idiom.

    rocketnet_server_transport_update( server, rocketnet_platform_time() );

    int recovered = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        rocketnet_check( frame || completion_capacity );
        if ( frame )
        {
            memcpy( frame + context.payload_index, "again", 5 );
            rocketnet_check( rocketnet_server_transport_finish_packet( server, &context, 5 ) > 0 );
        }
        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_client_transport_payloads( client, &payload_data, &payload_bytes );
        for ( int i = 0; i < num_payloads; i++ )
        {
            if ( payload_bytes[i] == 5 && memcmp( payload_data[i], "again", 5 ) == 0 )
                recovered = 1;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( recovered );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

struct rocketnet_test_client_hammer_args_t
{
    struct rocketnet_client_transport_t * client;
    int thread_id;
    int packets;
};

static void rocketnet_test_client_hammer_thread( void * data )
{
    struct rocketnet_test_client_hammer_args_t * args = (struct rocketnet_test_client_hammer_args_t*) data;
    for ( int i = 0; i < args->packets; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( args->client, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
        rocketnet_check( frame );
        uint8_t * payload = frame + context.payload_index;
        payload[0] = (uint8_t) args->thread_id;
        payload[1] = (uint8_t) i;
        rocketnet_check( rocketnet_client_transport_finish_packet( args->client, &context, 2 ) > 0 );
    }
}

void rocketnet_test_transport_client_concurrent_fill()
{
    // the client's fill phase under threads, mirrored from the server's
    // hammer: four fillers, one flush, every packet arrives distinct.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int NUM_THREADS = 4;
    const int PACKETS_PER_THREAD = 40;

    struct rocketnet_test_client_hammer_args_t args[NUM_THREADS];
    rocketnet_platform_thread_t * threads[NUM_THREADS];

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        args[t].client = client;
        args[t].thread_id = t;
        args[t].packets = PACKETS_PER_THREAD;
        threads[t] = rocketnet_platform_thread_create( NULL, rocketnet_test_client_hammer_thread, &args[t] );
        rocketnet_check( threads[t] );
    }

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        rocketnet_platform_thread_join( threads[t] );
        rocketnet_platform_thread_destroy( threads[t] );
    }

    rocketnet_client_transport_send_packets( client );

    int seen[NUM_THREADS][PACKETS_PER_THREAD];
    memset( seen, 0, sizeof(seen) );
    int received = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && received < NUM_THREADS * PACKETS_PER_THREAD )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int * client_index;
        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_server_transport_payloads( server, &client_index, &payload_data, &payload_bytes );

        for ( int i = 0; i < num_payloads; i++ )
        {
            rocketnet_check( client_index[i] == 0 );
            rocketnet_check( payload_bytes[i] == 2 );
            const int thread_id = payload_data[i][0];
            const int index = payload_data[i][1];
            rocketnet_check( thread_id >= 0 && thread_id < NUM_THREADS );
            rocketnet_check( index >= 0 && index < PACKETS_PER_THREAD );
            rocketnet_check( !seen[thread_id][index] );
            seen[thread_id][index] = 1;
            received++;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( received == NUM_THREADS * PACKETS_PER_THREAD );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_concurrent_fill()
{
    // the lock-free claim under real threads: four fillers acquire, write
    // and seal concurrently in one fill phase, the main thread flushes, and
    // every packet arrives distinct -- which also proves every sequence was
    // unique, because the client replay window would eat a duplicate.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    const int NUM_THREADS = 4;
    const int PACKETS_PER_THREAD = 50;

    struct rocketnet_test_hammer_args_t args[NUM_THREADS];
    rocketnet_platform_thread_t * threads[NUM_THREADS];

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        args[t].server = server;
        args[t].thread_id = t;
        args[t].packets = PACKETS_PER_THREAD;
        threads[t] = rocketnet_platform_thread_create( NULL, rocketnet_test_hammer_thread, &args[t] );
        rocketnet_check( threads[t] );
    }

    for ( int t = 0; t < NUM_THREADS; t++ )
    {
        rocketnet_platform_thread_join( threads[t] );
        rocketnet_platform_thread_destroy( threads[t] );
    }

    rocketnet_server_transport_send_packets( server );

    // every one of the two hundred distinct packets arrives

    int seen[NUM_THREADS][PACKETS_PER_THREAD];
    memset( seen, 0, sizeof(seen) );
    int received = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && received < NUM_THREADS * PACKETS_PER_THREAD )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        uint8_t * const * payload_data;
        const int * payload_bytes;
        int num_payloads = rocketnet_client_transport_payloads( client, &payload_data, &payload_bytes );

        for ( int i = 0; i < num_payloads; i++ )
        {
            rocketnet_check( payload_bytes[i] == 16 );
            const int thread_id = payload_data[i][0];
            const int index = payload_data[i][1];
            rocketnet_check( thread_id >= 0 && thread_id < NUM_THREADS );
            rocketnet_check( index >= 0 && index < PACKETS_PER_THREAD );
            for ( int j = 2; j < 16; j++ )
                rocketnet_check( payload_data[i][j] == (uint8_t) ( thread_id * 31 + index * 7 + j ) );
            rocketnet_check( !seen[thread_id][index] );
            seen[thread_id][index] = 1;
            received++;
        }

        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_check( received == NUM_THREADS * PACKETS_PER_THREAD );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_start_finish_shape()
{
    // the acquire/commit surface, proven over loopback: aligned roots, the
    // documented layout, seal in place, oversize refused, aborts harmless.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // a packet before connect for a bogus slot is refused at start

    struct rocketnet_packet_context_t context;
    rocketnet_check( rocketnet_server_transport_start_packet( server, 3, ROCKETNET_MAX_PAYLOAD_BYTES, &context ) == NULL );

    // acquire: the root and the payload pointer are both 16-aligned, the
    // layout is the documented one, and the capacity is qword-clean

    uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_check( frame );
    rocketnet_check( ( (uintptr_t) frame % 16 ) == 0 );
    rocketnet_check( context.payload_index == ROCKETNET_FRAME_PAYLOAD_OFFSET );
    rocketnet_check( ( (uintptr_t) ( frame + context.payload_index ) % 16 ) == 0 );
    rocketnet_check( context.payload_capacity == ROCKETNET_MAX_PAYLOAD_BYTES );
    rocketnet_check( context.payload_capacity % 8 == 0 );
    rocketnet_check( context.sequence > 0 );

    // Packet metadata is a view, never authority over native storage.
    int checked_capacity = 0;
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &context, &checked_capacity) == frame + context.payload_index);
    rocketnet_check(checked_capacity == context.payload_capacity);
    rocketnet_check(rocketnet_client_transport_packet_payload(client, &context, &checked_capacity) == NULL && checked_capacity == 0);
    for (int field = 0; field < 6; ++field)
    {
        auto changed = context;
        switch(field)
        {
            case 0: changed.frame++; break;
            case 1: changed.packet_id++; break;
            case 2: changed.sequence++; break;
            case 3: changed.client_index = -1; break;
            case 4: changed.payload_index = -1; break;
            case 5: changed.payload_capacity = INT_MAX; break;
        }
        rocketnet_check(rocketnet_server_transport_packet_payload(server, &changed, &checked_capacity) == NULL);
        auto aborted = changed;
        rocketnet_server_transport_abort_packet(server, &aborted);
        rocketnet_check(aborted.frame == NULL);
        rocketnet_check(rocketnet_server_transport_finish_packet(server, &changed, 1) == 0);
    }
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &context, &checked_capacity) == frame + context.payload_index);
    auto expired = context;

    // oversize is refused at finish and consumes nothing visible

    rocketnet_server_transport_finish_packet( server, &context, context.payload_capacity + 1 );
    rocketnet_check(rocketnet_server_transport_packet_payload(server, &expired, &checked_capacity) == NULL);
    rocketnet_check(rocketnet_server_transport_finish_packet(server, &expired, 1) == 0);

    // an aborted packet is harmless

    frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context );
    rocketnet_check( frame );
    rocketnet_server_transport_abort_packet( server, &context );

    for (int arena = 0; arena < 2; ++arena)
    {
    // commit: write at the payload pointer, finish, flush, and the client
    // receives exactly those bytes

    frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES + arena, &context );
    rocketnet_check( frame );
    rocketnet_check( context.payload_index == (arena ? 0 : ROCKETNET_FRAME_PAYLOAD_OFFSET) );
    memcpy( frame + context.payload_index, "shape", 5 );
    rocketnet_server_transport_finish_packet( server, &context, 5 );
    rocketnet_server_transport_send_packets( server );

    int received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "shape", 5 ) == 0 );
            received = 1;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( received );

    struct rocketnet_packet_context_t reply;
    uint8_t * reply_frame = rocketnet_client_transport_start_packet(client, ROCKETNET_MAX_PAYLOAD_BYTES + arena, &reply);
    rocketnet_check(reply_frame);
    rocketnet_check(reply.payload_index == (arena ? 0 : ROCKETNET_FRAME_PAYLOAD_OFFSET));
    memcpy(reply_frame + reply.payload_index, "reply", 5);
    rocketnet_check(rocketnet_client_transport_finish_packet(client, &reply, 5) > 0);
    rocketnet_client_transport_send_packets(client);
    bool replied = false;
    start = rocketnet_platform_time();
    while (!replied && rocketnet_platform_time() - start < 5.0)
    {
        rocketnet_client_transport_update(client, rocketnet_platform_time());
        rocketnet_server_transport_update(server, rocketnet_platform_time());
        if (rocketnet_server_transport_num_payloads(server))
        {
            int index, bytes;
            uint8_t * payload = rocketnet_server_transport_payload(server, 0, &index, &bytes);
            rocketnet_check(payload && index == 0 && bytes == 5 && memcmp(payload, "reply", 5) == 0);
            rocketnet_check(rocketnet_server_transport_num_payloads(server) == 1);
            replied = true;
        }
        if (!replied) rocketnet_platform_sleep(0.001);
    }
    rocketnet_check(replied);


    }

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_udp_loopback()
{
    // the first packets on a real socket: the whole connect over UDP through
    // loopback, on the transports -- the IO carrying the connection
    // protocol, development keys, exactly the out-of-the-box path. real
    // clocks here; the protocol underneath is the one proven on mocked time.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t loopback_connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), loopback_connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, loopback_connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    // pump both until connected, bounded by the wall clock
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_client_transport_client_index( client ) == 0 );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );
    rocketnet_check( rocketnet_server_transport_is_client_connected( server, 0 ) );

    // payloads both ways through the transport surfaces
    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello", 5 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world", 5 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5 );
            rocketnet_check( memcmp( payload, "hello", 5 ) == 0 );
            server_received++;
        }

        // out-of-range indices answer safely rather than trusting the caller
        int oob_client_index, oob_payload_bytes;
        rocketnet_check( rocketnet_server_transport_payload( server, num_payloads, &oob_client_index, &oob_payload_bytes ) == NULL );
        rocketnet_check( rocketnet_server_transport_payload( server, -1, &oob_client_index, &oob_payload_bytes ) == NULL );

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 );
            rocketnet_check( memcmp( packet, "world", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 10 );
    rocketnet_check( client_received >= 10 );

    // the deliberate disconnect crosses the wire and the server empties
    rocketnet_client_transport_disconnect( client );
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_server_transport_num_connected_clients( server ) == 0 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 0 );
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    // Stop may need to drain asynchronous I/O. The public contract requires
    // waiting for STOPPED, not a particular number of updates.
    start = rocketnet_platform_time();
    while ( rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED &&
            rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_server_transport_state( server ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_stats()
{
    // the bucket rings on both transports over UDP loopback, on a synthetic
    // game clock: the published send rates equal an independent recount of
    // the sends this test made under the same trailing-one-second rule, and
    // after a second of idle game time every published rate is zero --
    // stale buckets drop out

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    double game_time = 100.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, game_time );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, game_time );
    rocketnet_check( client );

    // connect on the synthetic clock, bounded by the wall clock

    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // two simulated seconds of traffic, one payload each way per frame. the
    // send times are recorded so the published numbers can be recounted
    // independently; two seconds ages the handshake's packets out of the
    // window, so the window holds payload traffic alone.

    const int num_traffic_frames = 120;

    double send_times[num_traffic_frames];

    uint8_t payload_data[100];
    memset( payload_data, 0x5A, sizeof(payload_data) );

    for ( int i = 0; i < num_traffic_frames; i++ )
    {
        game_time += 1.0 / 60.0;

        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );

        rocketnet_client_transport_send_packet( client, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );

        send_times[i] = game_time;

        uint8_t scratch[ROCKETNET_RECEIVE_CEILING];
        while ( rocketnet_client_transport_receive_packet( client, scratch, (int) sizeof(scratch) ) > 0 ) {}

        rocketnet_platform_sleep( 0.001 );
    }

    // one more update pair at the final time publishes the window sums

    rocketnet_client_transport_update( client, game_time );
    rocketnet_server_transport_update( server, game_time );

    // the independent recount, under the published numbers' own rule: a
    // send contributes while its bucket is younger than one second

    int expected_sends = 0;
    for ( int i = 0; i < num_traffic_frames; i++ )
    {
        if ( send_times[i] + 1.0 >= game_time )
            expected_sends++;
    }
    rocketnet_check( expected_sends > 0 );

    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( client, &client_stats );

    rocketnet_check( client_stats.packets_sent_per_second == expected_sends );
    rocketnet_check( client_stats.packets_received_per_second > 0 );
    rocketnet_check( client_stats.bandwidth_sent_kbps > 0 );
    rocketnet_check( client_stats.bandwidth_received_kbps > 0 );
    rocketnet_check( client_stats.rtt > 0.0f );
    rocketnet_check( client_stats.rtt < 1000.0f );
    rocketnet_check( client_stats.jitter >= 0.0f );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    struct rocketnet_server_transport_stats_t server_stats;
    rocketnet_server_transport_stats( server, &server_stats );

    rocketnet_check( server_stats.packets_sent_per_second == (uint64_t) expected_sends );
    rocketnet_check( server_stats.packets_received_per_second > 0 );
    rocketnet_check( server_stats.bandwidth_sent_kbps > 0.0f );
    rocketnet_check( server_stats.bandwidth_received_kbps > 0.0f );

    // stale bucket exclusion: disconnect, then a second and a half of idle
    // game time. nothing is sent or received, the traffic buckets age past
    // the window, and every published rate returns to zero.

    rocketnet_client_transport_disconnect( client );

    for ( int i = 0; i < 90; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packets_sent_per_second == 0 );
    rocketnet_check( client_stats.packets_received_per_second == 0 );
    rocketnet_check( client_stats.bandwidth_sent_kbps == 0 );
    rocketnet_check( client_stats.bandwidth_received_kbps == 0 );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    rocketnet_server_transport_stats( server, &server_stats );
    rocketnet_check( server_stats.packets_sent_per_second == 0 );
    rocketnet_check( server_stats.packets_received_per_second == 0 );
    rocketnet_check( server_stats.bandwidth_sent_kbps == 0.0f );
    rocketnet_check( server_stats.bandwidth_received_kbps == 0.0f );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, game_time );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_packet_loss_stats()
{
    // packet loss measured under a lossy sequence, deterministically: a
    // sequence started and aborted on the server is burned unsent, so the
    // client sees a gap in the session sequence over a perfect wire. clean
    // traffic first proves zero; the burns prove loss shows up.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    double game_time = 100.0;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, game_time );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, game_time );
    rocketnet_check( client );

    // This handshake uses real sockets and a real receive thread. Advancing
    // virtual time by 1/60 per 1 ms wait gives that thread only ~300 ms before
    // a five-second protocol deadline. Use actual elapsed time for connection;
    // the controlled virtual cadence below still drives the statistics case.
    const double connect_time = game_time;
    double wall_start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - wall_start < 5.0 )
    {
        game_time = connect_time + rocketnet_platform_time() - wall_start;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    if (rocketnet_client_transport_state(client) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED)
        printf("packet loss test connect failed: %s, server clients %d\n",
            rocketnet_client_transport_state_name(rocketnet_client_transport_state(client)),
            rocketnet_server_transport_num_connected_clients(server));
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    uint8_t payload_data[64];
    memset( payload_data, 0xA5, sizeof(payload_data) );

    // clean traffic: no gaps, so no loss. 45 frames on purpose: enough session
    // packets that the tracker's sweep crosses its safety margin, which is where
    // a phantom loss from a mis-based sequence feed becomes visible.

    for ( int i = 0; i < 45; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packet_loss == 0.0f );

    // burn ten sequences unsent, then enough delivered traffic to carry the
    // loss tracker's sweep past its safety margin

    for ( int i = 0; i < 10; i++ )
    {
        struct rocketnet_packet_context_t context;
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PAYLOAD_BYTES, &context ) );
        rocketnet_server_transport_abort_packet( server, &context );
    }

    for ( int i = 0; i < ROCKETNET_PACKET_LOSS_TRACKER_SAFETY + 20; i++ )
    {
        game_time += 1.0 / 60.0;
        rocketnet_client_transport_update( client, game_time );
        rocketnet_server_transport_update( server, game_time );
        rocketnet_server_transport_send_packet( server, 0, payload_data, sizeof(payload_data) );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    rocketnet_client_transport_stats( client, &client_stats );
    rocketnet_check( client_stats.packet_loss > 0.0f );
    rocketnet_check( client_stats.packet_loss < 100.0f );

    rocketnet_client_transport_disconnect( client );
    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, game_time );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_platform_socket()
{
    // non-blocking socket (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with timeout (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with no timeout (ipv4)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "0.0.0.0" );
        rocketnet_address_parse( &local_address, "127.0.0.1" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) );
        rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        rocketnet_platform_socket_destroy( socket );
    }

#if ROCKETNET_PLATFORM_HAS_IPV6

    // non-blocking socket (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with timeout (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, 0.01f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        while ( rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) ) )
        {
            rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        }
        rocketnet_platform_socket_destroy( socket );
    }

    // blocking socket with no timeout (ipv6)
    {
        rocketnet_address_t bind_address;
        rocketnet_address_t local_address;
        rocketnet_address_parse( &bind_address, "[::]" );
        rocketnet_address_parse( &local_address, "[::1]" );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_BLOCKING, -1.0f, 64*1024, 64*1024 );
        local_address.port = bind_address.port;
        rocketnet_check( socket );
        uint8_t packet[256];
        memset( packet, 0, sizeof(packet) );
        rocketnet_platform_socket_send_packet( socket, &local_address, packet, sizeof(packet) );
        rocketnet_address_t from;
        rocketnet_platform_socket_receive_packet( socket, &from, packet, sizeof(packet) );
        rocketnet_check( rocketnet_address_equal( &from, &local_address ) );
        rocketnet_platform_socket_destroy( socket );
    }

#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
}

// the batch pump seam (#100): every platform serves it -- portable loops
// underneath on most, darwin's batched syscalls when the runtime probe
// admits them -- and these tests pin the seam contract itself, so they run
// identically over either implementation. packets are verified by content
// (index and pattern), never by arrival order.

#define ROCKETNET_TEST_PUMP_PACKETS 100         // several pump chunks, with a short final chunk
#define ROCKETNET_TEST_PUMP_STRIDE  1426        // the transport's own receive ceiling

// the pump each platform must be running when nothing forces portable: on
// macOS the msgx pump, everywhere else the portable loop. asserted inside
// the exchange so a silently-degraded driver can never pass the suite as
// "tested" -- the batch tests would otherwise green over the fallback.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#define ROCKETNET_TEST_EXPECTED_PUMP "msgx"
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
#define ROCKETNET_TEST_EXPECTED_PUMP "portable"
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

static void rocketnet_test_pump_exchange( const char * bind_string, const char * local_string, const char * expected_pump )
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_parse( &bind_address, bind_string );
    rocketnet_address_parse( &local_address, local_string );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), expected_pump ) == 0 );
    local_address.port = bind_address.port;

    // batch send: varied sizes, a content pattern keyed by index

    static uint8_t send_data[ROCKETNET_TEST_PUMP_PACKETS][512];
    struct rocketnet_platform_send_t sends[ROCKETNET_TEST_PUMP_PACKETS];
    for ( int i = 0; i < ROCKETNET_TEST_PUMP_PACKETS; i++ )
    {
        const int bytes = 32 + i * 4;
        send_data[i][0] = (uint8_t) i;
        for ( int j = 1; j < bytes; j++ )
            send_data[i][j] = (uint8_t) ( i + j );
        sends[i].to = &local_address;
        sends[i].packet_data = send_data[i];
        sends[i].packet_bytes = bytes;
    }
    rocketnet_platform_socket_send_packets( socket, sends, ROCKETNET_TEST_PUMP_PACKETS );

    // batch receive: drain until everything sent has arrived. loopback
    // delivery can trickle, so poll under a deadline rather than assume
    // one call sees the batch whole.

    static rocketnet_address_t from[ROCKETNET_TEST_PUMP_PACKETS];
    static int packet_bytes[ROCKETNET_TEST_PUMP_PACKETS];
    static uint8_t receive_data[ROCKETNET_TEST_PUMP_PACKETS*ROCKETNET_TEST_PUMP_STRIDE];

    int received = 0;
    double start_time = rocketnet_platform_time();
    while ( received < ROCKETNET_TEST_PUMP_PACKETS && rocketnet_platform_time() - start_time < 5.0 )
    {
        received += rocketnet_platform_socket_receive_packets( socket,
                                                               from + received,
                                                               receive_data + (size_t) received * ROCKETNET_TEST_PUMP_STRIDE,
                                                               ROCKETNET_TEST_PUMP_STRIDE,
                                                               packet_bytes + received,
                                                               ROCKETNET_TEST_PUMP_PACKETS - received );
    }
    rocketnet_check( received == ROCKETNET_TEST_PUMP_PACKETS );

    // every packet arrived whole, correctly sized, correctly attributed --
    // and exactly once

    bool seen[ROCKETNET_TEST_PUMP_PACKETS];
    memset( seen, 0, sizeof(seen) );
    for ( int i = 0; i < ROCKETNET_TEST_PUMP_PACKETS; i++ )
    {
        rocketnet_check( rocketnet_address_equal( &from[i], &local_address ) );
        const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
        const int index = packet[0];
        rocketnet_check( index < ROCKETNET_TEST_PUMP_PACKETS );
        rocketnet_check( !seen[index] );
        seen[index] = true;
        rocketnet_check( packet_bytes[i] == 32 + index * 4 );
        for ( int j = 1; j < packet_bytes[i]; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( index + j ) );
    }

    // a drained socket returns zero

    rocketnet_check( rocketnet_platform_socket_receive_packets( socket, from, receive_data, ROCKETNET_TEST_PUMP_STRIDE, packet_bytes, ROCKETNET_TEST_PUMP_PACKETS ) == 0 );

    // a partial batch returns short without blocking: send 3, ask for many

    for ( int i = 0; i < 3; i++ )
        rocketnet_platform_socket_send_packet( socket, &local_address, send_data[i], 32 + i * 4 );

    received = 0;
    start_time = rocketnet_platform_time();
    while ( received < 3 && rocketnet_platform_time() - start_time < 5.0 )
    {
        received += rocketnet_platform_socket_receive_packets( socket,
                                                               from + received,
                                                               receive_data + (size_t) received * ROCKETNET_TEST_PUMP_STRIDE,
                                                               ROCKETNET_TEST_PUMP_STRIDE,
                                                               packet_bytes + received,
                                                               ROCKETNET_TEST_PUMP_PACKETS - received );
    }
    rocketnet_check( received == 3 );

    // the pump is still the expected one AFTER the exchange, not merely at
    // create. the create-time assertion pins only the probe (which
    // exercises recvmsg_x alone); a send-half-only breakage -- the
    // realistic ABI-drift regression -- would pause the pump mid-exchange
    // and finish portable, and the review demonstrated the whole suite
    // greening over exactly that. the pause outlives any test by design
    // (60 s), so a mid-exchange degradation cannot hide from this line.

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), expected_pump ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_platform_batch_pump()
{
    rocketnet_test_pump_exchange( "0.0.0.0", "127.0.0.1", ROCKETNET_TEST_EXPECTED_PUMP );

#if ROCKETNET_PLATFORM_HAS_IPV6
    rocketnet_test_pump_exchange( "[::]", "[::1]", ROCKETNET_TEST_EXPECTED_PUMP );
#endif // #if ROCKETNET_PLATFORM_HAS_IPV6
}

void rocketnet_test_platform_batch_pump_degraded()
{
    // ROCKETNET_PUMP=portable pins the degradation contract: the forced
    // portable pump answers the same seam with the same behavior, so a
    // darwin machine runs this test over the exact code path a runtime
    // refusal would degrade to. on platforms whose pump is portable
    // already, the override is a no-op and the exchange still holds.

    setenv( "ROCKETNET_PUMP", "portable", 1 );

    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 64*1024, 64*1024 );
    rocketnet_check( socket );
    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), "portable" ) == 0 );
    rocketnet_platform_socket_destroy( socket );

    rocketnet_test_pump_exchange( "0.0.0.0", "127.0.0.1", "portable" );

    unsetenv( "ROCKETNET_PUMP" );
}

#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

// zero-length chaff injection (#100's review): a zero-length datagram is
// legal UDP, and before this round the portable path treated one as
// "socket drained" -- byte-less chaff capped every drain at one packet, a
// standing DoS lever. both pumps now share skip-and-continue semantics,
// pinned here by injecting chaff between real packets from one raw socket
// (one source socket, so loopback preserves the interleaving) and
// requiring every real packet back, in order, contents intact -- which
// also proves the msgx compaction memmove, since kept packets after a
// mid-chunk skip must move down a slot.

static void rocketnet_test_pump_chaff_exchange( void )
{
    rocketnet_address_t bind_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );

    const char * pump_at_create = rocketnet_platform_socket_pump( socket );

    int injector = ::socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    rocketnet_check( injector >= 0 );

    struct sockaddr_in to;
    memset( &to, 0, sizeof(to) );
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    to.sin_port = htons( bind_address.port );

    // interleave: chaff leads, trails, and lands between every couple of
    // real packets, crossing msgx chunk boundaries (48 real > one chunk)

    const int num_real = 48;
    uint8_t real_packet[256];
    int sent_real = 0;
    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    for ( int i = 0; i < num_real; i++ )
    {
        const int bytes = 24 + i;
        real_packet[0] = (uint8_t) i;
        for ( int j = 1; j < bytes; j++ )
            real_packet[j] = (uint8_t) ( 3 * i + j );
        rocketnet_check( sendto( injector, real_packet, (size_t) bytes, 0, (struct sockaddr*) &to, sizeof(to) ) == bytes );
        sent_real++;
        if ( ( i % 2 ) == 1 )
            rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    }
    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );

    // drain through the batch pump: every real packet, in order, intact --
    // and in ONE receive_packets call, because a retry loop here greens
    // over the exact bug this test names (the review measured it: an
    // unfixed path consumes chaff between calls, so retries make
    // progress). loopback arrival is asynchronous (also measured: a
    // recvfrom immediately after sendto answers EAGAIN), so the wait is
    // on ARRIVAL -- readability, then a settle window orders of magnitude
    // past loopback flight time -- and never on the drain itself. the
    // injector socket's FIFO means readability plus the settle has every
    // datagram above queued before the one drain call runs.

    {
        double deadline = rocketnet_platform_time() + 5.0;
        for ( ;; )
        {
            fd_set readable;
            FD_ZERO( &readable );
            FD_SET( socket->handle, &readable );
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            if ( select( socket->handle + 1, &readable, NULL, NULL, &tv ) > 0 )
                break;
            rocketnet_check( rocketnet_platform_time() < deadline );
        }
        usleep( 200000 );
    }

    static rocketnet_address_t from[ROCKETNET_TEST_PUMP_PACKETS];
    static int packet_bytes[ROCKETNET_TEST_PUMP_PACKETS];
    static uint8_t receive_data[ROCKETNET_TEST_PUMP_PACKETS*ROCKETNET_TEST_PUMP_STRIDE];

    const int received = rocketnet_platform_socket_receive_packets( socket, from, receive_data,
                                                                    ROCKETNET_TEST_PUMP_STRIDE, packet_bytes,
                                                                    ROCKETNET_TEST_PUMP_PACKETS );
    rocketnet_check( received == sent_real );

    for ( int i = 0; i < received; i++ )
    {
        const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
        rocketnet_check( packet[0] == (uint8_t) i );
        rocketnet_check( packet_bytes[i] == 24 + i );
        for ( int j = 1; j < packet_bytes[i]; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( 3 * i + j ) );
    }

    // and the per-packet primitive itself skips chaff: one zero-length
    // then one real packet, and the FIRST single-packet call must hand
    // back the real one -- a retry loop here would green over a
    // per-packet path that still reads chaff as drain-end

    rocketnet_check( sendto( injector, real_packet, 0, 0, (struct sockaddr*) &to, sizeof(to) ) == 0 );
    uint8_t single[64];
    memset( single, 0x5a, sizeof(single) );
    rocketnet_check( sendto( injector, single, sizeof(single), 0, (struct sockaddr*) &to, sizeof(to) ) == (ssize_t) sizeof(single) );

    // the same arrival wait, then the FIRST per-packet call must hand
    // back the real packet through the chaff

    {
        double deadline = rocketnet_platform_time() + 5.0;
        for ( ;; )
        {
            fd_set readable;
            FD_ZERO( &readable );
            FD_SET( socket->handle, &readable );
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            if ( select( socket->handle + 1, &readable, NULL, NULL, &tv ) > 0 )
                break;
            rocketnet_check( rocketnet_platform_time() < deadline );
        }
        usleep( 200000 );
    }

    uint8_t single_received[ROCKETNET_TEST_PUMP_STRIDE];
    rocketnet_address_t single_from;
    int single_bytes = rocketnet_platform_socket_receive_packet( socket, &single_from, single_received, sizeof(single_received) );
    rocketnet_check( single_bytes == (int) sizeof(single) );
    for ( int j = 0; j < single_bytes; j++ )
        rocketnet_check( single_received[j] == 0x5a );

    // chaff exercised the pump it claims to have exercised: no silent
    // mid-run degradation

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), pump_at_create ) == 0 );

    close( injector );
    rocketnet_platform_socket_destroy( socket );
}

// a refused destination mid-batch (#100's review, the blocker): xnu can
// deliver a prefix of a sendmsg_x batch and still return -1 with no
// count, so the send path's law is one bounded per-packet finish of the
// chunk. this pins the survivable half of that law on the real kernel: a
// batch with a refused destination in the middle must deliver every other
// packet AT LEAST once (duplicates are legal, priced, and eaten by replay
// protection above; spurious loss is not legal).

void rocketnet_test_platform_batch_pump_refused_destination()
{
    rocketnet_address_t bind_address;
    rocketnet_address_t local_address;
    rocketnet_address_t refused_address;
    rocketnet_address_parse( &bind_address, "0.0.0.0" );
    rocketnet_address_parse( &local_address, "127.0.0.1" );
    rocketnet_address_parse( &refused_address, "0.0.0.0" );        // port 0: the kernel refuses it at send
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 1024*1024, 1024*1024 );
    rocketnet_check( socket );
    local_address.port = bind_address.port;

    const int num_packets = 8;
    const int refused_index = 5;

    static uint8_t send_data[8][64];
    struct rocketnet_platform_send_t sends[8];
    for ( int i = 0; i < num_packets; i++ )
    {
        memset( send_data[i], i, sizeof(send_data[i]) );
        sends[i].to = i == refused_index ? &refused_address : &local_address;
        sends[i].packet_data = send_data[i];
        sends[i].packet_bytes = (int) sizeof(send_data[i]);
    }
    rocketnet_platform_socket_send_packets( socket, sends, num_packets );

    bool seen[8];
    memset( seen, 0, sizeof(seen) );
    int distinct = 0;

    double start_time = rocketnet_platform_time();
    while ( distinct < num_packets - 1 && rocketnet_platform_time() - start_time < 5.0 )
    {
        rocketnet_address_t from[8];
        int packet_bytes[8];
        uint8_t receive_data[8*ROCKETNET_TEST_PUMP_STRIDE];
        int received = rocketnet_platform_socket_receive_packets( socket, from, receive_data, ROCKETNET_TEST_PUMP_STRIDE, packet_bytes, 8 );
        for ( int i = 0; i < received; i++ )
        {
            const uint8_t * packet = receive_data + (size_t) i * ROCKETNET_TEST_PUMP_STRIDE;
            const int index = packet[0];
            rocketnet_check( index < num_packets );
            rocketnet_check( index != refused_index );
            rocketnet_check( packet_bytes[i] == 64 );
            for ( int j = 0; j < 64; j++ )
                rocketnet_check( packet[j] == (uint8_t) index );
            if ( !seen[index] )
            {
                seen[index] = true;
                distinct++;
            }
        }
    }

    rocketnet_check( distinct == num_packets - 1 );

    // the refused destination is weather, never a pause: the pump must
    // still be the platform's expected one after the error path ran

    rocketnet_check( strcmp( rocketnet_platform_socket_pump( socket ), ROCKETNET_TEST_EXPECTED_PUMP ) == 0 );

    rocketnet_platform_socket_destroy( socket );
}

void rocketnet_test_platform_pump_zero_length_chaff()
{
    // both pumps: whatever the platform runs by default, and the portable
    // pump forced -- identical semantics is the seam law under review

    rocketnet_test_pump_chaff_exchange();

    setenv( "ROCKETNET_PUMP", "portable", 1 );
    rocketnet_test_pump_chaff_exchange();
    unsetenv( "ROCKETNET_PUMP" );
}

#endif // #if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS

void rocketnet_test_transport_acks_and_client_stats()
{
    // The transports end to end over UDP loopback: eight clients fill every
    // slot of an eight-slot server, payloads flow on the highest slot, and
    // the delivery notifications surface the packet ids from start_packet on
    // both ends -- the join key, through real sockets. The per-client stats
    // for the HIGH slot must move after traffic: the round-robin recompute
    // covers all slots, a sixth per update, which is exactly the test the
    // reference's literal i == update_count % 6 predicate fails --
    // that form never updates any slot past five.

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 8;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    struct rocketnet_client_transport_t * clients[8];

    for ( int i = 0; i < 8; i++ )
    {
        uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
        rocketnet_check( rocketnet_write_dev_connect_token( server_address, 0x77000ULL + (uint64_t) i, (uint64_t) time( NULL ), token ) );
        clients[i] = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
        rocketnet_check( clients[i] );
    }

    double start = rocketnet_platform_time();
    int all_connected = 0;
    while ( rocketnet_platform_time() - start < 10.0 && !all_connected )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        all_connected = 1;
        for ( int i = 0; i < 8; i++ )
            if ( rocketnet_client_transport_state( clients[i] ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                all_connected = 0;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( all_connected );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 8 );

    // the client holding the highest slot -- past every index the broken
    // reference amortization would ever touch

    int high_slot = -1;
    struct rocketnet_client_transport_t * high_client = NULL;
    for ( int i = 0; i < 8; i++ )
    {
        int index = rocketnet_client_transport_client_index( clients[i] );
        if ( index > high_slot )
        {
            high_slot = index;
            high_client = clients[i];
        }
    }
    rocketnet_check( high_slot >= 6 );

    // one payload each way through acquire/commit, keeping the packet ids

    struct rocketnet_packet_context_t up_context;
    uint8_t * frame = rocketnet_client_transport_start_packet( high_client, ROCKETNET_MAX_PAYLOAD_BYTES, &up_context );
    rocketnet_check( frame );
    memcpy( frame + up_context.payload_index, "ping", 4 );
    rocketnet_check( rocketnet_client_transport_finish_packet( high_client, &up_context, 4 ) > 0 );
    rocketnet_client_transport_send_packets( high_client );

    struct rocketnet_packet_context_t down_context;
    frame = rocketnet_server_transport_start_packet( server, high_slot, ROCKETNET_MAX_PAYLOAD_BYTES, &down_context );
    rocketnet_check( frame );
    memcpy( frame + down_context.payload_index, "pong", 4 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 4 ) > 0 );

    // pump until both notifications surface: the server sees its id come
    // back from the high slot, the client sees its own -- each exactly the
    // id start_packet handed out

    int server_saw_ack = 0;
    int client_saw_ack = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( server_saw_ack && client_saw_ack ) )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int * acked_client_index = NULL;
        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_server_transport_acked_packets( server, &acked_client_index, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
        {
            if ( acked_ids[i] == down_context.packet_id )
            {
                rocketnet_check( acked_client_index[i] == high_slot );
                server_saw_ack = 1;
            }
        }

        const uint64_t * client_acked_ids = NULL;
        int client_num_acked = rocketnet_client_transport_acked_packets( high_client, &client_acked_ids );
        for ( int i = 0; i < client_num_acked; i++ )
        {
            if ( client_acked_ids[i] == up_context.packet_id )
                client_saw_ack = 1;
        }

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_saw_ack );
    rocketnet_check( client_saw_ack );

    // keep traffic flowing long enough for every round-robin phase to pass
    // several times, then the high slot's published stats have moved

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 1.0 )
    {
        for ( int i = 0; i < 8; i++ )
            rocketnet_client_transport_update( clients[i], rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        rocketnet_platform_sleep( 0.001 );
    }

    struct rocketnet_server_transport_client_stats_t stats;
    rocketnet_server_transport_client_stats( server, high_slot, &stats );
    rocketnet_check( stats.bandwidth_received_kbps > 0.0f );
    rocketnet_check( stats.rtt >= 0.0f && stats.rtt < 1000.0f );
    rocketnet_check( stats.packet_loss >= 0.0f && stats.packet_loss <= 100.0f );

    // an out-of-range slot answers zeros, never a crash
    rocketnet_server_transport_client_stats( server, 12345, &stats );
    rocketnet_check( stats.rtt == 0.0f && stats.bandwidth_received_kbps == 0.0f );

    // and the client's rtt is live from the acks: nonzero and sane on a
    // loopback wire
    struct rocketnet_client_transport_stats_t client_stats;
    rocketnet_client_transport_stats( high_client, &client_stats );
    rocketnet_check( client_stats.rtt > 0.0f && client_stats.rtt < 1000.0f );

    for ( int i = 0; i < 8; i++ )
    {
        rocketnet_client_transport_disconnect( clients[i] );
        rocketnet_client_transport_destroy( clients[i] );
    }
    rocketnet_server_transport_stop( server );
    for ( int i = 0; i < 100 && rocketnet_server_transport_state( server ) != ROCKETNET_SERVER_TRANSPORT_STOPPED; i++ )
    {
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
    }
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_transport_fragmentation()
{
    // the declared-capacity surface end to end over UDP loopback: a
    // request above the wire's capacity routes to the arena, splits into
    // fragments at finish, crosses real sockets, and reassembles
    // byte-exact into the ordinary payload batch on the far side -- both
    // directions -- with the payload's one packet id surfacing exactly
    // once when every fragment's wire packet is acknowledged

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, 0xF6A9ULL, (uint64_t) time( NULL ), token ) );
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    static uint8_t pattern[ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES];
    for ( int i = 0; i < (int) sizeof(pattern); i++ )
        pattern[i] = (uint8_t) ( 0xC3 + i );

    // client to server: a 5000-byte payload written into the arena frame

    struct rocketnet_packet_context_t up_context;
    uint8_t * frame = rocketnet_client_transport_start_packet( client, 5000, &up_context );
    rocketnet_check( frame );
    rocketnet_check( up_context.payload_index == 0 );
    rocketnet_check( up_context.payload_capacity == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );
    memcpy( frame, pattern, 5000 );
    rocketnet_check( rocketnet_client_transport_finish_packet( client, &up_context, 5000 ) > 0 );
    rocketnet_client_transport_send_packets( client );

    int server_received = 0;
    int up_acks_seen = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( server_received && up_acks_seen ) )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 5000 );
            rocketnet_check( memcmp( payload, pattern, 5000 ) == 0 );
            server_received++;
        }

        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_client_transport_acked_packets( client, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
            if ( acked_ids[i] == up_context.packet_id )
                up_acks_seen++;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received == 1 );
    rocketnet_check( up_acks_seen == 1 );                   // the payload's one id, exactly once

    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( server );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == 5 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED] == 5 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    // server to client: a 9000-byte payload the other way

    struct rocketnet_packet_context_t down_context;
    frame = rocketnet_server_transport_start_packet( server, 0, 9000, &down_context );
    rocketnet_check( frame );
    rocketnet_check( down_context.payload_index == 0 );
    memcpy( frame, pattern, 9000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 9000 ) > 0 );

    int client_received = 0;
    int down_acks_seen = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !( client_received && down_acks_seen ) )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_client_transport_num_payloads( client );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_bytes;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            rocketnet_check( payload_bytes == 9000 );
            rocketnet_check( memcmp( payload, pattern, 9000 ) == 0 );
            client_received++;
        }

        const int * acked_client_index = NULL;
        const uint64_t * acked_ids = NULL;
        int num_acked = rocketnet_server_transport_acked_packets( server, &acked_client_index, &acked_ids );
        for ( int i = 0; i < num_acked; i++ )
        {
            if ( acked_ids[i] == down_context.packet_id )
            {
                rocketnet_check( acked_client_index[i] == 0 );
                down_acks_seen++;
            }
        }

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client_received == 1 );
    rocketnet_check( down_acks_seen == 1 );

    const struct rocketnet_counters_t * client_counters = rocketnet_client_transport_counters( client );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED] == 8 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == 1 );

    // an over-capacity request whose write turns out small collapses to
    // one ordinary payload packet -- the fragment counters do not move

    frame = rocketnet_client_transport_start_packet( client, 5000, &up_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 64 );
    rocketnet_check( rocketnet_client_transport_finish_packet( client, &up_context, 64 ) > 0 );
    rocketnet_client_transport_send_packets( client );

    server_received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !server_received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_bytes == 64 );
            rocketnet_check( memcmp( payload, pattern, 64 ) == 0 );
            server_received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received == 1 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == 5 );        // unchanged

    // the configured maximum is readable, so callers can size buffers
    rocketnet_check( rocketnet_client_transport_max_payload_bytes( client ) == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );

    // the copying cursor carries reassembled payloads too, sized by the
    // configured maximum
    frame = rocketnet_server_transport_start_packet( server, 0, 4000, &down_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 4000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 4000 ) > 0 );

    static uint8_t cursor_buffer[ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES];
    int cursor_received = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 && !cursor_received )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        int cursor_bytes;
        while ( ( cursor_bytes = rocketnet_client_transport_receive_packet( client, cursor_buffer, (int) sizeof(cursor_buffer) ) ) > 0 )
        {
            rocketnet_check( cursor_bytes == 4000 );
            rocketnet_check( memcmp( cursor_buffer, pattern, 4000 ) == 0 );
            cursor_received++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( cursor_received == 1 );

    // NEGATIVE: a cursor buffer smaller than a reassembled payload refuses
    // it whole -- counted, never truncated, never delivered
    frame = rocketnet_server_transport_start_packet( server, 0, 3000, &down_context );
    rocketnet_check( frame );
    memcpy( frame, pattern, 3000 );
    rocketnet_check( rocketnet_server_transport_finish_packet( server, &down_context, 3000 ) > 0 );

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 10.0 &&
            client_counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );
        uint8_t small_buffer[ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES];
        rocketnet_check( rocketnet_client_transport_receive_packet( client, small_buffer, (int) sizeof(small_buffer) ) == 0 );
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_RECEIVE_CURSOR_TOO_SMALL] == 1 );

    // the reviewer's burst: twelve fragmented payloads in one fill phase --
    // 36 wire packets, ordinary load -- can all complete in ONE client
    // update, which is more completions than the client has reassembly
    // slots. the delivered mirror sizes to the drain bound, so every one
    // is delivered and BATCH_FULL stays exactly zero.
    {
        struct rocketnet_packet_context_t burst_context;
        for ( int i = 0; i < 12; i++ )
        {
            frame = rocketnet_server_transport_start_packet( server, 0, 2500, &burst_context );
            rocketnet_check( frame );
            memcpy( frame, pattern, 2500 );
            rocketnet_check( rocketnet_server_transport_finish_packet( server, &burst_context, 2500 ) > 0 );
        }
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_send_packets( server );

        rocketnet_platform_sleep( 0.1 );                    // let the whole burst queue on the client's receive thread

        int burst_received = 0;
        start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 10.0 && burst_received < 12 )
        {
            rocketnet_client_transport_update( client, rocketnet_platform_time() );
            rocketnet_server_transport_update( server, rocketnet_platform_time() );
            rocketnet_server_transport_send_packets( server );

            const int num_payloads = rocketnet_client_transport_num_payloads( client );
            for ( int i = 0; i < num_payloads; i++ )
            {
                int payload_bytes;
                uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
                rocketnet_check( payload_bytes == 2500 );
                rocketnet_check( memcmp( payload, pattern, 2500 ) == 0 );
                burst_received++;
            }
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( burst_received == 12 );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == 15 );    // 9000 + 4000 + 3000 + the twelve
    }

    // NEGATIVE: the arena is a fixed fill-phase resource -- exhaustion
    // drops at start_packet on its own counter, and the flush reclaims it
    {
        struct rocketnet_packet_context_t contexts[ROCKETNET_SEND_ARENA_SLOTS];
        for ( int i = 0; i < ROCKETNET_SEND_ARENA_SLOTS; i++ )
            rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &contexts[i] ) );
        struct rocketnet_packet_context_t overflow_context;
        rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &overflow_context ) == NULL );
        rocketnet_check( client_counters->values[ROCKETNET_COUNTER_SEND_ARENA_FULL] == 1 );
        for ( int i = 0; i < ROCKETNET_SEND_ARENA_SLOTS; i++ )
            rocketnet_client_transport_abort_packet( client, &contexts[i] );
        rocketnet_client_transport_send_packets( client );
        rocketnet_check( rocketnet_client_transport_start_packet( client, 2000, &overflow_context ) != NULL );
        rocketnet_client_transport_abort_packet( client, &overflow_context );
    }

    // NEGATIVE: a request past the configured maximum refuses outright
    {
        struct rocketnet_packet_context_t oversize_context;
        rocketnet_check( rocketnet_client_transport_start_packet( client, ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES + 1, &oversize_context ) == NULL );
        rocketnet_check( rocketnet_server_transport_start_packet( server, 0, ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES + 1, &oversize_context ) == NULL );
    }

    // nothing died on the clean wire
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
    rocketnet_check( server_counters->values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );
    rocketnet_check( client_counters->values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL] == 0 );

    rocketnet_client_transport_disconnect( client );
    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );
}

// the transport-level ipv6 loopback: the same out-of-the-box connect and
// payload exchange as the udp loopback test, on [::1] -- ipv6 listen is
// REQUIRED of every server driver (#97: the gamelift gateway speaks ipv6 to
// servers), so this runs on every platform and every driver the suite
// builds against, with zero driver ifdefs.

void rocketnet_test_transport_ipv6_loopback()
{
    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "[::1]", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );

    char server_address[64];
    snprintf( server_address, sizeof(server_address), "[::1]:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent_to_server = 0;
    int sent_to_client = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent_to_server < 10 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "hello6", 6 );
            sent_to_server++;
        }
        if ( sent_to_client < 10 )
        {
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "world6", 6 );
            sent_to_client++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload_client_index == 0 );
            rocketnet_check( payload_bytes == 6 );
            rocketnet_check( memcmp( payload, "hello6", 6 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 6 );
            rocketnet_check( memcmp( packet, "world6", 6 ) == 0 );
            client_received++;
        }

        if ( server_received >= 10 && client_received >= 10 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 10 );
    rocketnet_check( client_received >= 10 );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );
}

void rocketnet_test_config_precedence()
{
    // DEFAULT < STRUCT < ENV, observed through the one config field with a
    // public query: max_payload_bytes

    struct rocketnet_test_env_guard_t payload_guard;
    rocketnet_test_env_clear( &payload_guard, "ROCKETNET_MAX_PAYLOAD_BYTES" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );

    // default layer

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES );
    rocketnet_server_transport_destroy( server );

    // struct displaces default

    config.max_payload_bytes = 18000;
    server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == 18000 );
    rocketnet_server_transport_destroy( server );

    // env displaces struct

    setenv( "ROCKETNET_MAX_PAYLOAD_BYTES", "20000", 1 );
    server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_max_payload_bytes( server ) == 20000 );
    rocketnet_server_transport_destroy( server );

    rocketnet_test_env_restore( &payload_guard );
}

// the rung-forcing exchange: pin a driver list on the server, expect the
// named grant, and pass real payloads both ways through it -- the matrix
// doctrine's runtime form. the pin rides the struct, so ROCKETNET_DRIVER
// is cleared for the duration (env would win, by design). port zero per
// the #107 law: the kernel grants the port and the token is minted for
// what was granted, so no other process on the box can race this test.

static void rocketnet_test_driver_pin_exchange( const char * driver_list, const char * expected_driver )
{
    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    snprintf( server_config.driver, sizeof(server_config.driver), "%s", driver_list );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &server_config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), expected_driver ) == 0 );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_LENGTH];
    snprintf( server_address, sizeof(server_address), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_platform_time() );
    rocketnet_check( client );

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED &&
             rocketnet_server_transport_num_connected_clients( server ) == 1 )
            break;
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    rocketnet_check( rocketnet_server_transport_num_connected_clients( server ) == 1 );

    int server_received = 0;
    int client_received = 0;
    int sent = 0;
    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 )
    {
        if ( sent < 5 )
        {
            rocketnet_client_transport_send_packet( client, (const uint8_t*) "rung!", 5 );
            rocketnet_server_transport_send_packet( server, 0, (const uint8_t*) "rung!", 5 );
            sent++;
        }

        rocketnet_server_transport_send_packets( server );

        rocketnet_client_transport_update( client, rocketnet_platform_time() );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );

        const int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            int payload_client_index, payload_bytes;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
            rocketnet_check( payload && payload_bytes == 5 && memcmp( payload, "rung!", 5 ) == 0 );
            server_received++;
        }

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes;
        while ( ( packet_bytes = rocketnet_client_transport_receive_packet( client, packet, (int) sizeof(packet) ) ) > 0 )
        {
            rocketnet_check( packet_bytes == 5 && memcmp( packet, "rung!", 5 ) == 0 );
            client_received++;
        }

        if ( server_received >= 5 && client_received >= 5 )
            break;

        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( server_received >= 5 );
    rocketnet_check( client_received >= 5 );

    rocketnet_client_transport_destroy( client );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );

    rocketnet_test_env_restore( &driver_guard );
}

void rocketnet_test_server_driver_pin_portable()
{
    // the portable rung carries traffic on every platform -- the floor
    // must never be the untested tier

    rocketnet_test_driver_pin_exchange( "portable", "portable" );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
void rocketnet_test_server_driver_pin_iocp()
{
    rocketnet_test_driver_pin_exchange( "iocp", "iocp" );
}
#endif

void rocketnet_test_server_port_zero_driver()
{
    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    int num_available = 0;
    const int * available = rocketnet_server_driver_availability( &num_available );

    // THE ACCEPT SIDE FIRST, and it is the load-bearing half: this rule
    // must never cost a port-zero create that a discovering driver can
    // serve. auto on every platform, plus every discovering driver this
    // platform has, pinned one at a time.

    for ( int i = -1; i < num_available; i++ )
    {
        const char * list = "auto";
        if ( i >= 0 )
        {
            if ( !rocketnet_driver_discovers_bound_port( available[i] ) )
                continue;                                   // the refuse side below owns those
            list = rocketnet_driver_name( available[i] );
        }

        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "%s", list );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
        rocketnet_check( server );
        rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_destroy( server );
    }

    // THE REFUSE SIDE, where this platform actually carries a
    // non-discovering driver. the loop body is the linux legs; elsewhere
    // the accept side above is the whole test, which is the honest state
    // of a rule about a driver that does not exist on this platform.

    for ( int i = 0; i < num_available; i++ )
    {
        if ( rocketnet_driver_discovers_bound_port( available[i] ) )
            continue;

        const char * name = rocketnet_driver_name( available[i] );

        // a PIN on a non-discovering rung refuses the create, and the log
        // names the rule and the driver. that naming IS the fix: without
        // it an operator debugs a cookie gate that never matches.
        //
        // captured at the ERROR level ON PURPOSE, because that is the
        // level a deployed binary, this suite and the C# smoke all run at:
        // the per-rung warn is invisible there, so if the rule were only a
        // warn a pinned port-zero create would return NULL with nothing
        // anywhere saying why. The exhausted-ladder path restates it as an
        // error, and this asserts that restatement rather than the warn.

        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "%s", name );

        rocketnet_test_captured_log[0] = '\0';
        const int saved_level = rocketnet_get_log_level();
        rocketnet_log_function( rocketnet_test_capture_log );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "cannot discover the port it was granted and hand it back" ) != NULL );

        // and at a level that shows warns, the RUNG's own refusal names
        // the rule and the driver together -- matched as one phrase, since
        // the driver name alone also appears in the exhausted-ladder error
        // and would pass this check without the rule being mentioned

        char expected_rung_refusal[128];
        snprintf( expected_rung_refusal, sizeof(expected_rung_refusal), "server driver %s refused: a port-zero public address", name );

        rocketnet_test_captured_log[0] = '\0';
        rocketnet_log_function( rocketnet_test_capture_log );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, expected_rung_refusal ) != NULL );

        // SCOPE, the other half of correct: the rule is about port ZERO
        // and nothing else. the same pin with a real port never meets it
        // and goes on into the driver's own create, whose FIRST act is the
        // commit barrier -- the suite never mutates a host. That refusal
        // is asserted by its message rather than merely tolerated: it is
        // the one path in this test that enters the xdp driver at all, and
        // it is what keeps the BARRIER exercised, since the rule
        // short-circuits the port-zero pins that would otherwise reach it.

        rocketnet_test_captured_log[0] = '\0';
        rocketnet_log_function( rocketnet_test_capture_log );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:40100", &config, rocketnet_platform_time() );

        rocketnet_log_level( saved_level );
        rocketnet_log_function( rocketnet_test_suite_log_function );

        rocketnet_check( server == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "port-zero public address" ) == NULL );
        rocketnet_check( strstr( rocketnet_test_captured_log, "forbids committing an XDP data plane" ) != NULL );

        // and the rung refusal is a RUNG refusal, not a create refusal:
        // put a discovering rung after it and the ladder falls, the port
        // is discovered, and the advertised address is the real one.

        snprintf( config.driver, sizeof(config.driver), "%s,portable", name );

        server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
        rocketnet_check( server );
        rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), "portable" ) == 0 );
        rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_destroy( server );
    }

    rocketnet_test_env_restore( &driver_guard );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
void rocketnet_test_server_driver_pin_gso()
{
    rocketnet_test_driver_pin_exchange( "gso", "gso" );

    // per-driver bounds: a queue count past the gso geometry refuses the
    // pinned rung -- and with nothing left to fall to, the create --
    // loudly, never clamped

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "gso" );
    config.num_xdp_queues = ROCKETNET_GSO_MAX_QUEUES + 1;

    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &driver_guard );
}

void rocketnet_test_server_driver_pin_xdp()
{
    // the pre-flight is called DIRECTLY here, as it also is in
    // rocketnet_test_server_driver_ladder_fall below: those two direct
    // calls are how the suite reaches it. Inside the xdp driver's create
    // the pre-flight sits below the commit barrier (barrier at
    // rocketnet_server_socket_linux.cpp:982, pre-flight at :1015), and the
    // suite installs that barrier process-wide, so the in-create pre-flight
    // is not reached under this suite. What create refuses with here is the
    // BARRIER. That is the refusal the pin asserts: pin-refuses proven
    // through create, no machine touched. On a box the pre-flight passes
    // whole, this test asserts nothing more -- granting xdp attaches a data
    // plane, which is not a unit test's act.
    //
    // THE ADDRESS CARRIES A REAL PORT DELIBERATELY (#109). A port-zero
    // address is now refused by the ladder's port-zero rule before the xdp
    // rung's create is called at all, which would make this test pass
    // without entering the driver -- no barrier, nothing proven about the
    // pin. A real port takes the rule out of the way so the refusal proven
    // here is again the driver's own. Nothing binds: the barrier returns
    // before create's first committing act.

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    const bool provisioned = rocketnet_server_socket_preflight( "127.0.0.1:40111", 7 );

    if ( !provisioned )
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "xdp" );

        rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:40111", &config, rocketnet_platform_time() ) == NULL );
    }

    rocketnet_test_env_restore( &driver_guard );
}

#if ROCKETNET_XDP
#include <sys/wait.h>

static void rocketnet_xdp_server_stub( const char * port )
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    if ( !rocketnet_init() )
        exit( 1 );
    rocketnet_forbid_xdp_commit();

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "xdp" );

    char addr[64];
    snprintf( addr, sizeof(addr), "127.0.0.1:%s", port );
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, addr, &config, rocketnet_platform_time() );
    if ( !server )
        exit( 2 );

    rocketnet_platform_sleep( 5.0 );

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_destroy( server );
    rocketnet_term();
    exit( 0 );
}
#endif

void rocketnet_test_xdp_restart_after_kill_receives()
{
#if ROCKETNET_XDP
    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    const bool provisioned = rocketnet_server_socket_preflight( "127.0.0.1:40115", 7 );

    if ( !provisioned )
        return;

    const char * port = "40115";

    pid_t pid = fork();
    if ( pid == -1 )
        rocketnet_check( false );

    if ( pid == 0 )
    {
        rocketnet_xdp_server_stub( port );
        exit( 1 );
    }

    rocketnet_platform_sleep( 2.0 );

    kill( pid, SIGKILL );
    int status;
    waitpid( pid, &status, 0 );

    rocketnet_test_env_restore( &driver_guard );

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    if ( !rocketnet_init() )
        rocketnet_check( false );

    rocketnet_test_env_save( &driver_guard, "ROCKETNET_DRIVER" );
    setenv( "ROCKETNET_DRIVER", "xdp", 1 );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "xdp" );

    char addr[64];
    snprintf( addr, sizeof(addr), "127.0.0.1:%s", port );
    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, addr, &config, rocketnet_platform_time() );
    rocketnet_check( server );

    rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), "xdp" ) == 0 );

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_destroy( server );
    rocketnet_term();

    rocketnet_test_env_restore( &driver_guard );
#else
    (void)port;
#endif
}

void rocketnet_test_server_driver_ladder_fall()
{
    // THE FALL, proven to actually happen where it can be proven without
    // mutating the host: the auto ladder must land on gso -- the next rung
    // -- never portable, never failure. on a box the pre-flight passes
    // whole, the ladder would grant xdp by attaching a real data plane, so
    // the test stands down: the suite never commits the machine.
    //
    // WHY the xdp rung refuses here, stated exactly (#109): the address
    // carries port zero -- it must, because the gso rung below binds it
    // and a fixed port would race other processes on the box -- so the xdp
    // rung here is refused by the port-zero rule, which is checked before
    // the rung's create is called at all. The fall itself is what this
    // test asserts and that is unchanged.
    //
    // Note what this does NOT reach, so the two comments agree: neither
    // the commit barrier nor the in-create pre-flight runs on this path,
    // because the rule refuses above both. The barrier is walked by
    // rocketnet_test_server_driver_pin_xdp, which uses a real port for
    // exactly that reason; the in-create pre-flight is below the barrier
    // and so is not reached under this suite from either test. The direct
    // pre-flight call just below is this test's own, and is what decides
    // whether to stand down.

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    const bool provisioned = rocketnet_server_socket_preflight( "127.0.0.1:40112", 7 );

    if ( !provisioned )
    {
        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_dev_config( &config );
        snprintf( config.driver, sizeof(config.driver), "xdp,gso,portable" );

        struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
        rocketnet_check( server );
        rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), "gso" ) == 0 );
        rocketnet_server_transport_stop( server );
        rocketnet_server_transport_update( server, rocketnet_platform_time() );
        rocketnet_server_transport_destroy( server );
    }

    rocketnet_test_env_restore( &driver_guard );
}
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_server_driver_pin_msgx()
{
    rocketnet_test_driver_pin_exchange( "msgx", "msgx" );
}

void rocketnet_test_driver_pump_alias()
{
    // ROCKETNET_PUMP=portable keeps its exact pre-#106 meaning -- the
    // darwin batch pump is off -- absorbed into the ladder as an msgx
    // refusal, so the auto ladder lands on portable

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_test_env_guard_t pump_guard;
    rocketnet_test_env_save( &pump_guard, "ROCKETNET_PUMP" );
    setenv( "ROCKETNET_PUMP", "portable", 1 );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( strcmp( rocketnet_server_transport_driver( server ), "portable" ) == 0 );
    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );

    // and a pinned msgx under the same override is a refusal-exhausted
    // ladder: the pin refuses loudly, it never falls

    struct rocketnet_server_transport_config_t pinned_config;
    rocketnet_server_transport_dev_config( &pinned_config );
    snprintf( pinned_config.driver, sizeof(pinned_config.driver), "msgx" );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &pinned_config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &pump_guard );
    rocketnet_test_env_restore( &driver_guard );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

void rocketnet_test_driver_all_skipped_uses_auto()
{
    // a list whose every name is a platform- or endpoint-skip names no
    // driver for this endpoint at all -- a fleet-wide line read by an
    // endpoint it was not addressing -- so the endpoint falls to its own
    // auto order, loudly, instead of bricking (#110 review, ruled inside
    // the #106 ignore rule). a typo is still a typo: loud parse failure,
    // no create. refusal-exhausted lists still fail loudly -- the xdp pin
    // test and the pump-alias msgx pin prove that side.

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    snprintf( config.driver, sizeof(config.driver), "msgx" );
#else // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    snprintf( config.driver, sizeof(config.driver), "gso" );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );

    // the grant came from the endpoint's own auto order

    int num_available = 0;
    const int * available = rocketnet_server_driver_availability( &num_available );
    bool granted_from_auto = false;
    for ( int i = 0; i < num_available; i++ )
        if ( strcmp( rocketnet_server_transport_driver( server ), rocketnet_driver_name( available[i] ) ) == 0 )
            granted_from_auto = true;
    rocketnet_check( granted_from_auto );

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_platform_time() );
    rocketnet_server_transport_destroy( server );

    snprintf( config.driver, sizeof(config.driver), "protable" );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &driver_guard );
}

// the deprecated env aliases (the pre-#106 spellings): honored where the
// successor is unset, logged loudly naming the successor, yielding loudly
// beside it -- and the public address alias feeds the one resolved
// address create uses everywhere

void rocketnet_test_config_env_aliases()
{
    struct rocketnet_test_env_guard_t alias_guard;
    rocketnet_test_env_save( &alias_guard, "ROCKETNET_SERVER_NUM_QUEUES" );

    struct rocketnet_test_env_guard_t successor_guard;
    rocketnet_test_env_save( &successor_guard, "ROCKETNET_NUM_XDP_QUEUES" );

    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    // the alias is honored where the successor is unset...

    unsetenv( "ROCKETNET_NUM_XDP_QUEUES" );
    setenv( "ROCKETNET_SERVER_NUM_QUEUES", "3", 1 );

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "portable" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_server_transport_destroy( server );

    // ...parsed strictly...

    setenv( "ROCKETNET_SERVER_NUM_QUEUES", "3x", 1 );
    rocketnet_check( rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() ) == NULL );

    // ...and yields to the successor when both are set

    setenv( "ROCKETNET_SERVER_NUM_QUEUES", "not-even-a-number", 1 );
    setenv( "ROCKETNET_NUM_XDP_QUEUES", "2", 1 );
    server = rocketnet_server_transport_create( NULL, "127.0.0.1:0", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_server_transport_destroy( server );

    rocketnet_test_env_restore( &alias_guard );
    rocketnet_test_env_restore( &successor_guard );

    // the public address rides the table now, scoped first-create-consumes
    // for the env layer. proven deterministically, no fixed port: the
    // create PARAMETER is unparseable, so the create succeeds if and only
    // if the env override displaced it -- and the second create proves the
    // consumption, succeeding-by-refusal when the parameter (not the still
    // exported variable) is what create reads.

    struct rocketnet_test_env_guard_t address_guard;
    rocketnet_test_env_save( &address_guard, "ROCKETNET_PUBLIC_ADDRESS" );
    setenv( "ROCKETNET_PUBLIC_ADDRESS", "127.0.0.1:0", 1 );

    rocketnet_server_transport_dev_config( &config );
    snprintf( config.driver, sizeof(config.driver), "portable" );

    server = rocketnet_server_transport_create( NULL, "not.an.address", &config, rocketnet_platform_time() );
    rocketnet_check( server );
    rocketnet_check( rocketnet_server_transport_port( server ) != 0 );
    rocketnet_server_transport_destroy( server );

    rocketnet_check( rocketnet_server_transport_create( NULL, "not.an.address", &config, rocketnet_platform_time() ) == NULL );

    rocketnet_test_env_restore( &address_guard );
    rocketnet_test_env_restore( &driver_guard );
}

void rocketnet_test_client_driver()
{
    struct rocketnet_test_env_guard_t driver_guard;
    rocketnet_test_env_clear( &driver_guard, "ROCKETNET_DRIVER" );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    rocketnet_check( rocketnet_write_dev_connect_token( "127.0.0.1:41040", rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    // the portable pin grants everywhere and reports itself

    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config( &config );
    rocketnet_check( strcmp( config.driver, "auto" ) == 0 );

    snprintf( config.driver, sizeof(config.driver), "portable" );

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_driver( client ), "portable" ) == 0 );
    rocketnet_client_transport_destroy( client );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

    // the msgx pin grants on macOS and reports itself

    snprintf( config.driver, sizeof(config.driver), "msgx" );
    client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    rocketnet_check( strcmp( rocketnet_client_transport_driver( client ), "msgx" ) == 0 );
    rocketnet_client_transport_destroy( client );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

    // the staged linux drivers are not client transports: a list naming
    // only them is an all-skipped list for the client endpoint, which
    // falls to the client's auto order rather than bricking every client
    // in a process that shares the fleet's ROCKETNET_DRIVER value

    snprintf( config.driver, sizeof(config.driver), "gso" );
    client = rocketnet_client_transport_create( NULL, connect_token, &config, rocketnet_platform_time() );
    rocketnet_check( client );
    {
        int num_available = 0;
        const int * available = rocketnet_client_driver_availability( &num_available );
        bool granted_from_auto = false;
        for ( int i = 0; i < num_available; i++ )
            if ( strcmp( rocketnet_client_transport_driver( client ), rocketnet_driver_name( available[i] ) ) == 0 )
                granted_from_auto = true;
        rocketnet_check( granted_from_auto );
    }
    rocketnet_client_transport_destroy( client );

    rocketnet_test_env_restore( &driver_guard );
}

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

// The GSO driver's own tests (#97), linux only, run on every linux build
// whatever driver the transport compiled against -- the driver module is
// always built, so its probe, its wire paths, its degraded tiers and its
// overflow behaviour are covered even when the transport above it is
// portable. Everything here is over-the-wire on loopback through real
// sockets; the peer is the portable platform socket, which also proves the
// driver interoperates with a plain UDP endpoint.

#include "rocketnet_server_gso.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

#ifndef UDP_GRO
#define UDP_GRO 104
#endif

// does this HOST actually coalesce? the driver's feature bits prove only
// that the setsockopts were ACCEPTED -- with `ethtool -K lo
// tx-udp-segmentation off`, a legitimate configuration, a UDP_SEGMENT
// super-datagram is segmented at transmit and arrives as plain datagrams,
// no UDP_GRO cmsg ever appears, and a counter assertion gated on the
// feature bits alone would encode that host tunable as a protocol fact.
// so probe the host itself, once, with the exact shape the provocation
// phase sends: a scratch socket with UDP_GRO enabled mails itself one
// eight-segment super-datagram over loopback and watches for an arrival
// carrying the UDP_GRO stride cmsg below its length -- the kernel's own
// statement that it delivered coalesced.

static bool rocketnet_test_gso_host_coalesces()
{
    int fd = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( fd < 0 )
        return false;

    struct sockaddr_in address;
    memset( &address, 0, sizeof(address) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    address.sin_port = 0;
    socklen_t address_length = sizeof(address);
    if ( bind( fd, (struct sockaddr*) &address, sizeof(address) ) != 0 ||
         getsockname( fd, (struct sockaddr*) &address, &address_length ) != 0 )
    {
        close( fd );
        return false;
    }

    int one = 1;
    if ( setsockopt( fd, IPPROTO_UDP, UDP_GRO, &one, sizeof(one) ) != 0 )
    {
        close( fd );
        return false;
    }

    const int probe_segments = 8;
    const int probe_segment_bytes = 100;

    uint8_t super[probe_segments * probe_segment_bytes];
    memset( super, 0xAB, sizeof(super) );

    struct iovec iov = { super, sizeof(super) };
    char control[CMSG_SPACE(sizeof(uint16_t))];
    memset( control, 0, sizeof(control) );
    struct msghdr msg;
    memset( &msg, 0, sizeof(msg) );
    msg.msg_name = &address;
    msg.msg_namelen = sizeof(address);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
    cmsg->cmsg_level = IPPROTO_UDP;
    cmsg->cmsg_type = UDP_SEGMENT;
    cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
    uint16_t stride = (uint16_t) probe_segment_bytes;
    memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );

    if ( sendmsg( fd, &msg, 0 ) != (ssize_t) sizeof(super) )
    {
        close( fd );
        return false;
    }

    bool coalesced = false;
    int received_bytes = 0;
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 2.0 && received_bytes < (int) sizeof(super) && !coalesced )
    {
        uint8_t buffer[sizeof(super)];
        struct iovec receive_iov = { buffer, sizeof(buffer) };
        char receive_control[CMSG_SPACE(sizeof(int))];
        struct msghdr receive_msg;
        memset( &receive_msg, 0, sizeof(receive_msg) );
        receive_msg.msg_iov = &receive_iov;
        receive_msg.msg_iovlen = 1;
        receive_msg.msg_control = receive_control;
        receive_msg.msg_controllen = sizeof(receive_control);
        ssize_t bytes = recvmsg( fd, &receive_msg, MSG_DONTWAIT );
        if ( bytes <= 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        received_bytes += (int) bytes;
        for ( struct cmsghdr * c = CMSG_FIRSTHDR( &receive_msg ); c; c = CMSG_NXTHDR( &receive_msg, c ) )
        {
            if ( c->cmsg_level == IPPROTO_UDP && c->cmsg_type == UDP_GRO )
            {
                int segment_bytes = 0;
                memcpy( &segment_bytes, CMSG_DATA( c ), sizeof(int) );
                if ( segment_bytes > 0 && segment_bytes < (int) bytes )
                    coalesced = true;
            }
        }
    }

    close( fd );
    return coalesced;
}

void rocketnet_test_server_gso_probe()
{
    // create discovers an ephemeral port and reports it back; the queue
    // count and the reuseport group are facts, the segmentation features
    // are probes whose result belongs to the kernel

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );
    rocketnet_check( bind_address.port == 0 );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 2, 0 );
    rocketnet_check( gso );
    rocketnet_check( bind_address.port != 0 );
    rocketnet_check( rocketnet_server_gso_num_queues( gso ) == 2 );
    rocketnet_check( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_REUSEPORT );

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == 0 );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED] == 0 );

    rocketnet_server_gso_destroy( gso );

    // the disable flags force the degraded tiers, so the fallback paths are
    // testable on kernels where the probe would say yes

    rocketnet_address_t plain_address;
    rocketnet_check( rocketnet_address_parse( &plain_address, "127.0.0.1" ) );
    struct rocketnet_server_gso_t * plain = rocketnet_server_gso_create( NULL, &plain_address, 1,
                                                                         ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO );
    rocketnet_check( plain );
    rocketnet_check( ( rocketnet_server_gso_features( plain ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) == 0 );
    rocketnet_check( ( rocketnet_server_gso_features( plain ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) == 0 );
    rocketnet_server_gso_destroy( plain );
}

// the wire exchange, shared by the tiers and the families: a plain UDP peer
// sends tagged packets of every size the wire allows into the driver, the
// driver's process batch must surface each byte-exact with the right
// source, then the driver sends tagged packets back -- deliberately
// including equal-size same-destination runs, the shape GSO coalesces --
// and the peer must receive each byte-exact.

static void rocketnet_test_server_gso_wire_exchange( const char * bind_string, const char * peer_string, const char * server_host, int flags, int expect_ipv4_from )
{
    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, bind_string ) );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 2, flags );
    rocketnet_check( gso );

    rocketnet_address_t peer_address;
    rocketnet_check( rocketnet_address_parse( &peer_address, peer_string ) );
    struct rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create( NULL, &peer_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( peer );
    rocketnet_check( peer_address.port != 0 );

    rocketnet_address_t server_address;
    rocketnet_check( rocketnet_address_parse( &server_address, server_host ) );
    server_address.port = bind_address.port;

    #define ROCKETNET_GSO_TEST_PACKETS 200

    // peer -> driver

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    for ( int i = 0; i < ROCKETNET_GSO_TEST_PACKETS; i++ )
    {
        const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        packet[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            packet[j] = (uint8_t) ( i * 31 + j );
        rocketnet_platform_socket_send_packet( peer, &server_address, packet, packet_bytes );
    }

    bool seen[ROCKETNET_GSO_TEST_PACKETS];
    memset( seen, 0, sizeof(seen) );
    int num_seen = 0;

    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_GSO_TEST_PACKETS )
    {
        rocketnet_server_gso_receive_packets( gso );
        struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
        for ( int i = 0; i < received->num_packets; i++ )
        {
            const int tag = received->packet_data[i][0];
            const int expected_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 37 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
            rocketnet_check( tag < ROCKETNET_GSO_TEST_PACKETS );
            rocketnet_check( !seen[tag] );
            rocketnet_check( received->packet_bytes[i] == expected_bytes );
            for ( int j = 1; j < expected_bytes; j++ )
                rocketnet_check( received->packet_data[i][j] == (uint8_t) ( tag * 31 + j ) );

            // the source is the peer's real port, and a v4 peer arriving on
            // a dual stack listen is native ipv4 -- normalized exactly once
            // at the boundary, per the standard's law

            rocketnet_check( received->from[i].port == peer_address.port );
            if ( expect_ipv4_from )
                rocketnet_check( received->from[i].type == ROCKETNET_ADDRESS_IPV4 );

            seen[tag] = true;
            num_seen++;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( num_seen == ROCKETNET_GSO_TEST_PACKETS );

    // driver -> peer. the first half all one size -- adjacent equal-size
    // same-destination slots, the runs the GSO tier folds into
    // super-datagrams -- the second half cycling sizes

    for ( int i = 0; i < ROCKETNET_GSO_TEST_PACKETS; i++ )
    {
        const int packet_bytes = i < ROCKETNET_GSO_TEST_PACKETS / 2 ? 400
                               : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( i * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_gso_start_packet( gso, &peer_address, &packet_id );
        rocketnet_check( frame );
        uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
        wire[0] = (uint8_t) i;
        for ( int j = 1; j < packet_bytes; j++ )
            wire[j] = (uint8_t) ( i * 47 + j );
        rocketnet_server_gso_finish_packet( gso, packet_id, frame, packet_bytes );
    }

    rocketnet_server_gso_send_packets( gso );

    memset( seen, 0, sizeof(seen) );
    num_seen = 0;

    start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && num_seen < ROCKETNET_GSO_TEST_PACKETS )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( peer, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        const int tag = packet[0];
        const int expected_bytes = tag < ROCKETNET_GSO_TEST_PACKETS / 2 ? 400
                                 : ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + ( tag * 53 ) % ( ROCKETNET_RECEIVE_CEILING - ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 1 );
        rocketnet_check( tag < ROCKETNET_GSO_TEST_PACKETS );
        rocketnet_check( !seen[tag] );
        rocketnet_check( packet_bytes == expected_bytes );
        for ( int j = 1; j < expected_bytes; j++ )
            rocketnet_check( packet[j] == (uint8_t) ( tag * 47 + j ) );
        seen[tag] = true;
        num_seen++;
    }
    rocketnet_check( num_seen == ROCKETNET_GSO_TEST_PACKETS );

    // on the GSO tier those equal-size runs must actually have coalesced:
    // the counter is the proof the fast path ran, not just that packets
    // arrived

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == ROCKETNET_GSO_TEST_PACKETS );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_RECEIVED] >= ROCKETNET_GSO_TEST_PACKETS );
    if ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT )
        rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS] > 0 );

    // a runt and an oversize packet die at the driver's size pass -- one
    // below the smallest packet a server reads, one above the receive
    // ceiling, the pair covering both bounds of the check that guards the
    // staging memcpy -- counted, never surfaced

    const uint64_t dropped_before = counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE];
    uint8_t runt[ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1];
    memset( runt, 0xEE, sizeof(runt) );
    rocketnet_platform_socket_send_packet( peer, &server_address, runt, (int) sizeof(runt) );
    uint8_t oversize[ROCKETNET_RECEIVE_CEILING + 74];
    memset( oversize, 0xEF, sizeof(oversize) );
    rocketnet_platform_socket_send_packet( peer, &server_address, oversize, (int) sizeof(oversize) );

    start = rocketnet_platform_time();
    uint64_t dropped_after = dropped_before;
    while ( rocketnet_platform_time() - start < 5.0 && dropped_after < dropped_before + 2 )
    {
        rocketnet_server_gso_receive_packets( gso );
        rocketnet_check( rocketnet_server_gso_process_packets( gso )->num_packets == 0 );
        rocketnet_server_gso_counters( gso, counters );
        dropped_after = counters[ROCKETNET_SERVER_GSO_COUNTER_RECEIVE_DROPPED_SIZE];
        rocketnet_platform_sleep( 0.001 );
    }
    rocketnet_check( dropped_after == dropped_before + 2 );

    // provoke GRO for real: the peer sends one UDP_SEGMENT super-datagram,
    // which loopback segments and the driver's UDP_GRO socket receives
    // coalesced -- the multi-segment split path runs, proven by the
    // GRO_BATCHES counter, and every segment surfaces byte-exact. the
    // suite's ordinary traffic never repeats a size back to back, so
    // without this phase the split loop would never execute under test.
    // ipv4 peers only (the sockaddr is built inline), which covers every
    // tier this helper runs.

    if ( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_GRO ) &&
         ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) &&
         server_address.type == ROCKETNET_ADDRESS_IPV4 )
    {
        const bool host_coalesces = rocketnet_test_gso_host_coalesces();

        const int gro_segments = 8;
        const int gro_segment_bytes = 100;

        uint8_t super[gro_segments * gro_segment_bytes];
        for ( int i = 0; i < gro_segments; i++ )
        {
            super[i * gro_segment_bytes] = (uint8_t) ( 200 + i );
            for ( int j = 1; j < gro_segment_bytes; j++ )
                super[i * gro_segment_bytes + j] = (uint8_t) ( ( 200 + i ) * 13 + j );
        }

        struct sockaddr_in to;
        memset( &to, 0, sizeof(to) );
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = ( ( (uint32_t) server_address.data.ipv4[0] ) )       |
                             ( ( (uint32_t) server_address.data.ipv4[1] ) << 8 )  |
                             ( ( (uint32_t) server_address.data.ipv4[2] ) << 16 ) |
                             ( ( (uint32_t) server_address.data.ipv4[3] ) << 24 );
        to.sin_port = rocketnet_platform_htons( server_address.port );

        struct iovec iov = { super, sizeof(super) };
        char control[CMSG_SPACE(sizeof(uint16_t))];
        memset( control, 0, sizeof(control) );
        struct msghdr msg;
        memset( &msg, 0, sizeof(msg) );
        msg.msg_name = &to;
        msg.msg_namelen = sizeof(to);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        struct cmsghdr * cmsg = CMSG_FIRSTHDR( &msg );
        cmsg->cmsg_level = IPPROTO_UDP;
        cmsg->cmsg_type = UDP_SEGMENT;
        cmsg->cmsg_len = CMSG_LEN( sizeof(uint16_t) );
        uint16_t stride = (uint16_t) gro_segment_bytes;
        memcpy( CMSG_DATA( cmsg ), &stride, sizeof(stride) );

        rocketnet_check( sendmsg( peer->handle, &msg, 0 ) == (ssize_t) sizeof(super) );

        rocketnet_server_gso_counters( gso, counters );
        const uint64_t gro_before = counters[ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES];

        bool segment_seen[gro_segments];
        memset( segment_seen, 0, sizeof(segment_seen) );
        int num_segments_seen = 0;

        start = rocketnet_platform_time();
        while ( rocketnet_platform_time() - start < 5.0 && num_segments_seen < gro_segments )
        {
            rocketnet_server_gso_receive_packets( gso );
            struct rocketnet_server_socket_process_packets_t * received = rocketnet_server_gso_process_packets( gso );
            for ( int i = 0; i < received->num_packets; i++ )
            {
                const int tag = received->packet_data[i][0] - 200;
                rocketnet_check( tag >= 0 && tag < gro_segments );
                rocketnet_check( !segment_seen[tag] );
                rocketnet_check( received->packet_bytes[i] == gro_segment_bytes );
                for ( int j = 1; j < gro_segment_bytes; j++ )
                    rocketnet_check( received->packet_data[i][j] == (uint8_t) ( ( 200 + tag ) * 13 + j ) );
                rocketnet_check( received->from[i].port == peer_address.port );
                segment_seen[tag] = true;
                num_segments_seen++;
            }
            rocketnet_platform_sleep( 0.001 );
        }
        rocketnet_check( num_segments_seen == gro_segments );

        // the counter is the proof the coalesced path ran -- eight distinct
        // arrivals alone would also be produced by uncoalesced delivery. it
        // is asserted only where the host probe observed real coalescing:
        // the feature bits mean the setsockopts were accepted, and a host
        // with tx-udp-segmentation off on loopback delivers this same send
        // uncoalesced -- legitimate configuration, not a driver failure. on
        // such a host the byte-exact segment coverage above still ran, and
        // the skip says so out loud rather than passing silently.

        if ( host_coalesces )
        {
            rocketnet_server_gso_counters( gso, counters );
            rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_GRO_BATCHES] > gro_before );
        }
        else
        {
            rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "test_server_gso_super_datagram_split [SKIPPED GRO_BATCHES assertion: this host delivers UDP_SEGMENT super-datagrams uncoalesced over loopback (tx-udp-segmentation off?); segments still surfaced byte-exact above]" );
        }
    }

    rocketnet_platform_socket_destroy( peer );
    rocketnet_server_gso_destroy( gso );
}

void rocketnet_test_server_gso_wire_ipv4()
{
    rocketnet_test_server_gso_wire_exchange( "127.0.0.1", "127.0.0.1", "127.0.0.1", 0, 1 );
}

void rocketnet_test_server_gso_wire_plain()
{
    // the degraded tier over the same wire: no UDP_SEGMENT, no UDP_GRO,
    // plain sendmmsg/recvmmsg batching. the fallback path is never the
    // untested path.

    rocketnet_test_server_gso_wire_exchange( "127.0.0.1", "127.0.0.1", "127.0.0.1", ROCKETNET_SERVER_GSO_DISABLE_SEGMENT | ROCKETNET_SERVER_GSO_DISABLE_GRO, 1 );
}

void rocketnet_test_server_gso_wire_ipv6()
{
    rocketnet_test_server_gso_wire_exchange( "[::1]", "[::1]", "[::1]", 0, 0 );
}

void rocketnet_test_server_gso_wire_dual_stack()
{
    // an ipv6 wildcard listen accepts an ipv4 peer, whose source surfaces
    // as native ipv4 -- the v4-mapped normalization the standard makes law

    rocketnet_test_server_gso_wire_exchange( "[::]", "127.0.0.1", "127.0.0.1", 0, 1 );
}

void rocketnet_test_server_gso_send_overflow()
{
    // fill the send staging past capacity: starts beyond it refuse with
    // NULL, the flush sends what fit, and the next phase is whole -- the
    // driver-level face of the transport's ring exhaustion property

    rocketnet_address_t bind_address;
    rocketnet_check( rocketnet_address_parse( &bind_address, "127.0.0.1" ) );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_create( NULL, &bind_address, 1, 0 );
    rocketnet_check( gso );

    rocketnet_address_t peer_address;
    rocketnet_check( rocketnet_address_parse( &peer_address, "127.0.0.1" ) );
    struct rocketnet_platform_socket_t * peer = rocketnet_platform_socket_create( NULL, &peer_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                  ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    rocketnet_check( peer );

    for ( int i = 0; i < ROCKETNET_GSO_SEND_QUEUE_SIZE; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * frame = rocketnet_server_gso_start_packet( gso, &peer_address, &packet_id );
        rocketnet_check( frame );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, (uint8_t) i, 64 );
        rocketnet_server_gso_finish_packet( gso, packet_id, frame, 64 );
    }

    uint64_t overflow_id = 0;
    rocketnet_check( rocketnet_server_gso_start_packet( gso, &peer_address, &overflow_id ) == NULL );
    rocketnet_check( rocketnet_server_gso_start_packet( gso, &peer_address, &overflow_id ) == NULL );

    rocketnet_server_gso_send_packets( gso );

    // every staged packet was either sent or dropped, and nothing vanished
    // uncounted

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    rocketnet_check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] + counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == ROCKETNET_GSO_SEND_QUEUE_SIZE );

    // the next phase is whole: a fresh tagged packet flushes and arrives

    uint8_t tag[64];
    memset( tag, 0xAB, sizeof(tag) );

    uint64_t packet_id = 0;
    uint8_t * frame = rocketnet_server_gso_start_packet( gso, &peer_address, &packet_id );
    rocketnet_check( frame );
    memcpy( frame + ROCKETNET_FRAME_WIRE_OFFSET, tag, sizeof(tag) );
    rocketnet_server_gso_finish_packet( gso, packet_id, frame, (int) sizeof(tag) );
    rocketnet_server_gso_send_packets( gso );

    bool recovered = false;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    double start = rocketnet_platform_time();
    while ( rocketnet_platform_time() - start < 5.0 && !recovered )
    {
        rocketnet_address_t from;
        int packet_bytes = rocketnet_platform_socket_receive_packet( peer, &from, packet, (int) sizeof(packet) );
        if ( packet_bytes == 0 )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }
        if ( packet_bytes == (int) sizeof(tag) && memcmp( packet, tag, sizeof(tag) ) == 0 )
            recovered = true;
    }
    rocketnet_check( recovered );

    rocketnet_platform_socket_destroy( peer );
    rocketnet_server_gso_destroy( gso );
}

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX


extern void rocketnet_test_lifecycle_driver();

struct rocketnet_functional_test_t
{
    const char * name;
    void ( *function )();
};

static const struct rocketnet_functional_test_t rocketnet_functional_tests[] =
{
    { "test_server_refuses_keyless_config", rocketnet_test_server_refuses_keyless_config },
    { "test_transport_disconnect_delivery", rocketnet_test_transport_disconnect_delivery },
    { "test_transport_ring_exhaustion", rocketnet_test_transport_ring_exhaustion },
    { "test_transport_client_concurrent_fill", rocketnet_test_transport_client_concurrent_fill },
    { "test_transport_concurrent_fill", rocketnet_test_transport_concurrent_fill },
    { "test_transport_start_finish_shape", rocketnet_test_transport_start_finish_shape },
    { "test_transport_udp_loopback", rocketnet_test_transport_udp_loopback },
    { "test_transport_stats", rocketnet_test_transport_stats },
    { "test_transport_packet_loss_stats", rocketnet_test_transport_packet_loss_stats },
    { "test_platform_socket", rocketnet_test_platform_socket },
    { "test_platform_batch_pump", rocketnet_test_platform_batch_pump },
    { "test_platform_batch_pump_degraded", rocketnet_test_platform_batch_pump_degraded },
#if ROCKETNET_PLATFORM != ROCKETNET_PLATFORM_WINDOWS
    { "test_platform_batch_pump_refused_destination", rocketnet_test_platform_batch_pump_refused_destination },
    { "test_platform_pump_zero_length_chaff", rocketnet_test_platform_pump_zero_length_chaff },
#endif
    { "test_transport_acks_and_client_stats", rocketnet_test_transport_acks_and_client_stats },
    { "test_transport_fragmentation", rocketnet_test_transport_fragmentation },
    { "test_transport_ipv6_loopback", rocketnet_test_transport_ipv6_loopback },
    { "test_config_out_of_range_refuses_create", rocketnet_test_config_out_of_range_refuses_create },
    { "test_config_precedence", rocketnet_test_config_precedence },
    { "test_server_driver_pin_portable", rocketnet_test_server_driver_pin_portable },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    { "test_server_driver_pin_iocp", rocketnet_test_server_driver_pin_iocp },
#endif
    { "test_server_port_zero_driver", rocketnet_test_server_port_zero_driver },
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    { "test_server_driver_pin_gso", rocketnet_test_server_driver_pin_gso },
    { "test_server_driver_pin_xdp", rocketnet_test_server_driver_pin_xdp },
    { "test_xdp_restart_after_kill_receives", rocketnet_test_xdp_restart_after_kill_receives },
    { "test_server_driver_ladder_fall", rocketnet_test_server_driver_ladder_fall },
    { "test_server_gso_probe", rocketnet_test_server_gso_probe },
    { "test_server_gso_wire_ipv4", rocketnet_test_server_gso_wire_ipv4 },
    { "test_server_gso_wire_plain", rocketnet_test_server_gso_wire_plain },
    { "test_server_gso_wire_ipv6", rocketnet_test_server_gso_wire_ipv6 },
    { "test_server_gso_wire_dual_stack", rocketnet_test_server_gso_wire_dual_stack },
    { "test_server_gso_send_overflow", rocketnet_test_server_gso_send_overflow },
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    { "test_server_driver_pin_msgx", rocketnet_test_server_driver_pin_msgx },
    { "test_driver_pump_alias", rocketnet_test_driver_pump_alias },
#endif
    { "test_driver_all_skipped_uses_auto", rocketnet_test_driver_all_skipped_uses_auto },
    { "test_config_env_aliases", rocketnet_test_config_env_aliases },
    { "test_client_driver", rocketnet_test_client_driver },
    { "test_lifecycle_driver", rocketnet_test_lifecycle_driver },
};

static rocketnet_test_environment_t * rocketnet_functional_environment = NULL;

void rocketnet_functional_prepare()
{
    if ( rocketnet_platform_getenv( "ROCKETNET_TEST_VERBOSE" ) == NULL )
    {
        rocketnet_test_suite_log_function = rocketnet_test_log_capture;
        rocketnet_log_function( rocketnet_test_suite_log_function );
    }
    rocketnet_forbid_xdp_commit();
    if ( rocketnet_functional_environment == NULL )
        rocketnet_functional_environment = new rocketnet_test_environment_t();
}

int rocketnet_functional_test_count()
{
    return (int) ( sizeof( rocketnet_functional_tests ) / sizeof( rocketnet_functional_tests[0] ) );
}

const char * rocketnet_functional_test_name( int index )
{
    return rocketnet_functional_tests[index].name;
}

int rocketnet_functional_run_named( const char * name )
{
    for ( int i = 0; i < rocketnet_functional_test_count(); i++ )
    {
        if ( strcmp( rocketnet_functional_tests[i].name, name ) != 0 )
            continue;
        rocketnet_test_log_ring_clear();
        rocketnet_printf( ROCKETNET_LOG_LEVEL_NONE, "%s", name );
        fflush( stdout );
        rocketnet_functional_tests[i].function();
        return 0;
    }
    fprintf( stderr, "unknown functional test %s\n", name );
    return 2;
}
