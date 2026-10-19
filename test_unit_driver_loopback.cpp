/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Unit tests for the loopback driver backend, called directly through the
// driver open helpers with the backend's operations; no transport, no thread,
// no sleep, no clock.

#include "test_unit_registry.h"

#include "rocketnet_driver.h"
#include "rocketnet_address.h"
#include "rocketnet_config.h"
#include "rocketnet_platform.h"
#include "rocketnet_driver_loopback.h"
#include "rocketnet.h"

#include <stdio.h>
#include <string.h>

// The staged finish-path wire-total fold: single-threaded, no sockets, no
// real time (the clock is an argument, starting at 1000.0). Compiling the
// transport here gives the test the internal layout it swaps the fake staged
// driver into, exactly as test_transport_control.cpp does; the loopback
// backend's in-memory create is used so no real socket is ever opened.
#include "sdk/rocketnet_server_transport.cpp"

extern const rocketnet_driver_backend_t rocketnet_loopback_driver;

static void set_ipv4_loopback( rocketnet_address_t & address )
{
    memset( &address.data, 0, sizeof( address.data ) );
    address.type = ROCKETNET_ADDRESS_IPV4;
    address.data.ipv4[0] = 127;
    address.data.ipv4[3] = 1;
}

static void driver_loopback_binds_and_discovers_a_port()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t setup = { "loopback", &server_config, NULL };

    rocketnet_address_t a;
    rocketnet_unit_check( rocketnet_address_parse( &a, "127.0.0.1:0" ) );
    rocketnet_server_driver_t sa = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &setup, &a, &sa ) );
    rocketnet_unit_check( a.port != 0 );

    rocketnet_address_t b = a;
    rocketnet_server_driver_t sb = {};
    rocketnet_unit_check( !rocketnet_server_driver_open( backend->server, NULL, &setup, &b, &sb ) );

    rocketnet_address_t c;
    rocketnet_unit_check( rocketnet_address_parse( &c, "[::]:0" ) );
    rocketnet_server_driver_t sc = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &setup, &c, &sc ) );
    rocketnet_unit_check( c.port != 0 );
    rocketnet_unit_check( c.port != a.port );

    rocketnet_server_driver_close( &sa );
    rocketnet_server_driver_close( &sc );
    printf( "driver_loopback_binds_and_discovers_a_port\n" );
}

static void driver_loopback_exchanges_both_ways()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };

    rocketnet_address_t server_address, client_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "0.0.0.0:0" ) );
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_server_driver_t server = {};
    rocketnet_client_driver_t client = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    const uint8_t data[] = { 10, 20, 30, 40 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    client.operations->send( client.instance, &to, data, sizeof( data ) );

    rocketnet_address_t from;
    uint8_t buf[64];
    int bytes = 0;
    const int received = server.operations->receive_batch( server.instance, &from, buf, 64, &bytes, 1 );
    rocketnet_unit_check( received == 1 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
    rocketnet_unit_check( from.port == client_address.port );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.data.ipv4[0] == 127 && from.data.ipv4[3] == 1 );

    rocketnet_platform_send_t packet = { &from, data, (int) sizeof( data ) };
    server.operations->send_batch( server.instance, &packet, 1 );

    bytes = client.operations->receive( client.instance, &from, buf, 64 );
    rocketnet_unit_check( bytes == (int) sizeof( data ) );
    rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
    rocketnet_unit_check( from.port == server_address.port );
    rocketnet_unit_check( from.type == ROCKETNET_ADDRESS_IPV4 );
    rocketnet_unit_check( from.data.ipv4[0] == 127 && from.data.ipv4[3] == 1 );

    rocketnet_unit_check( client.operations->receive( client.instance, &from, buf, 64 ) == 0 );

    rocketnet_server_driver_close( &server );
    rocketnet_client_driver_close( &client );
    printf( "driver_loopback_exchanges_both_ways\n" );
}

static void driver_loopback_full_queue_drops_and_counts()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_driver_t server = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &server_address, &server ) );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };
    rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_client_driver_t client = {};
    rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    const int total = ROCKETNET_LOOPBACK_QUEUE_PACKETS + 5;
    uint8_t data[4];
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    for ( int i = 0; i < total; ++i )
    {
        data[0] = (uint8_t) ( i & 0xff );
        data[1] = (uint8_t) ( ( i >> 8 ) & 0xff );
        data[2] = (uint8_t) ( ( i >> 16 ) & 0xff );
        data[3] = 0;
        client.operations->send( client.instance, &to, data, 4 );
    }
    rocketnet_unit_check( rocketnet_loopback_endpoint_dropped( server.instance ) == 5 );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_QUEUE_PACKETS; ++i )
    {
        uint8_t buf[4];
        int bytes = 0;
        rocketnet_address_t from;
        const int n = server.operations->receive_batch( server.instance, &from, buf, 4, &bytes, 1 );
        rocketnet_unit_check( n == 1 );
        rocketnet_unit_check( bytes == 4 );
        const uint32_t v = (uint32_t) buf[0] | ( (uint32_t) buf[1] << 8 ) | ( (uint32_t) buf[2] << 16 );
        rocketnet_unit_check( v == (uint32_t) i );
        rocketnet_unit_check( from.port == client_address.port );
    }

    rocketnet_address_t from;
    uint8_t buf[4];
    int bytes = 0;
    rocketnet_unit_check( server.operations->receive_batch( server.instance, &from, buf, 4, &bytes, 1 ) == 0 );

    rocketnet_server_driver_close( &server );
    rocketnet_client_driver_close( &client );
    printf( "driver_loopback_full_queue_drops_and_counts\n" );
}

static void driver_loopback_destroy_frees_the_port()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    const int live_before = rocketnet_loopback_endpoints_live();

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_driver_t server = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    const uint16_t port = server_address.port;

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };
    rocketnet_address_t client_address;
    rocketnet_unit_check( rocketnet_address_parse( &client_address, "0.0.0.0:0" ) );
    rocketnet_client_driver_t client = {};
    rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &client_address, &client ) );

    rocketnet_server_driver_close( &server );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + 1 );

    uint8_t data[4] = { 1, 2, 3, 4 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    client.operations->send( client.instance, &to, data, 4 );
    rocketnet_unit_check( client.operations->receive( client.instance, &to, data, 4 ) == 0 );

    rocketnet_address_t again;
    rocketnet_unit_check( rocketnet_address_parse( &again, "127.0.0.1:0" ) );
    again.port = port;
    rocketnet_server_driver_t server2 = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &again, &server2 ) );
    rocketnet_unit_check( again.port == port );

    rocketnet_server_driver_close( &server2 );
    rocketnet_client_driver_close( &client );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before );
    printf( "driver_loopback_destroy_frees_the_port\n" );
}


// The test binary does not link the vendored sodium directly (the production
// archive carries it under renamed symbols), so the transport compiled into
// this TU needs its two original-name sodium entry points. Route them to the
// renamed implementations the archive does provide; the test never exercises
// real randomness or cleanup past the fake driver's staging.
extern "C" void rocketnet_sodium_randombytes_buf( void * const buf, const size_t size );
extern "C" void rocketnet_sodium_sodium_memzero( void * const pnt, const size_t len );
void randombytes_buf( void * const buf, const size_t size ) { rocketnet_sodium_randombytes_buf( buf, size ); }
void sodium_memzero( void * const pnt, const size_t len ) { rocketnet_sodium_sodium_memzero( pnt, len ); }

struct wire_totals_staged_fixture_t
{
    rocketnet_address_t reserved_to;
    uint8_t frame[ROCKETNET_FRAME_STRIDE];
    int used;
    int capacity;
};

static void wt_staged_destroy(void *) {}
static bool wt_staged_stop(void *) { return true; }
static void wt_staged_send_batch(void *, const rocketnet_platform_send_t *, int) {}
static int wt_staged_receive_batch(void * p, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int capacity)
{
    (void)p; (void)from; (void)data; (void)stride; (void)bytes; (void)capacity;
    return 0;
}
static rocketnet_server_socket_process_packets_t * wt_staged_receive(void *) { return NULL; }
static uint8_t * wt_staged_start_packet(void * p, const rocketnet_address_t * to, uint64_t * id)
{
    wire_totals_staged_fixture_t * f = (wire_totals_staged_fixture_t*) p;
    if ( f->used == f->capacity ) return NULL;
    f->reserved_to = *to;
    *id = (uint64_t) f->used;
    return f->frame;
}
static void wt_staged_finish_packet(void * p, uint64_t, uint8_t *, int)
{
    ++((wire_totals_staged_fixture_t*)p)->used;
}
static void wt_staged_abort_packet(void * p, uint64_t, uint8_t *) { (void)p; }
static void wt_staged_flush(void * p) { ((wire_totals_staged_fixture_t*)p)->used = 0; }

static void wire_totals_staged_finish_counts_exactly()
{
    // the staged producer path seals into the fake driver's staging, bumps
    // server_stats and the per-client totals with the relaxed atomics, and
    // the public stats readers fold them back; a non-atomic add either loses
    // an increment here (without a thread) or trips the tsan gate upstream.

    wire_totals_staged_fixture_t fx;
    memset( &fx, 0, sizeof(fx) );
    fx.capacity = 1024;

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    config.max_clients = 4;
    config.protocol_id = 1;
    snprintf( config.driver, sizeof(config.driver), "%s", "loopback" );
    // production create refuses dev keys and zero keys; any non-zero, non-dev
    // bytes satisfy the key check for this fake-driver fixture, which never
    // runs a real handshake
    memset( config.backend_public_key, 0xAB, 32 );
    memset( config.server_public_key, 0xCD, 32 );
    memset( config.server_secret_key, 0xEF, 32 );

    rocketnet_server_driver_operations_t operations;
    int create_status = -1;
    rocketnet_server_transport_t * server = rocketnet_server_transport_create_ex( NULL, "127.0.0.1:0", &config, 1000.0, &create_status );
    if ( !server ) printf( "create_ex status=%d\n", create_status );
    rocketnet_unit_check( server );
    rocketnet_server_driver_close( &server->io );
    memset( &operations, 0, sizeof(operations) );
    operations.destroy = wt_staged_destroy;
    operations.stop = wt_staged_stop;
    operations.send_batch = wt_staged_send_batch;
    operations.receive_batch = wt_staged_receive_batch;
    operations.flags = ROCKETNET_DRIVER_STAGED | ROCKETNET_DRIVER_FLUSH_ON_UPDATE;
    operations.start_packet = wt_staged_start_packet;
    operations.finish_packet = wt_staged_finish_packet;
    operations.abort_packet = wt_staged_abort_packet;
    operations.flush = wt_staged_flush;
    operations.receive = wt_staged_receive;
    server->io.instance = &fx;
    server->io.operations = &operations;

    for ( int i = 0; i < 4; i++ )
    {
        rocketnet_address_t address;
        rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
        address.port = (uint16_t) ( 1000 + i );
        const int index = rocketnet_encryption_map_add( server->protocol.encryption_map, &address, 100.0 );
        rocketnet_unit_check( index >= 0 );
        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( server->protocol.encryption_map, index );
        uint8_t user_data[ROCKETNET_USER_DATA_BYTES] = {};
        rocketnet_unit_check( rocketnet_client_slots_assign( server->protocol.client_slots, i + 1, index, user_data ) == i );
        mapping->client_index = i;
        mapping->client_id = i + 1;
        mapping->established = 1;
        mapping->timeout_seconds = 1000;
        memset( &mapping->keys, i + 1, sizeof(mapping->keys) );
        server->protocol.acks->endpoints[i].rtt_history[0] = 17.0f;
    }

    const int N = 200;
    uint64_t total_wire = 0;
    for ( int i = 0; i < N; i++ )
    {
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, 16, &context );
        rocketnet_unit_check( frame );
        memset( frame + context.payload_index, i & 0xff, 16 );
        const int wire_bytes = rocketnet_server_transport_finish_packet( server, &context, 16 );
        rocketnet_unit_check( wire_bytes > 0 );
        total_wire += (uint64_t) wire_bytes;
    }

    rocketnet_server_transport_send_packets( server );

    const uint64_t overhead = (uint64_t)( ROCKETNET_IPV4_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES );

    struct rocketnet_server_transport_stats_t sstats;
    memset( &sstats, 0, sizeof(sstats) );
    rocketnet_server_transport_stats( server, &sstats );
    rocketnet_unit_check( sstats.packets_sent == (uint64_t) N );
    rocketnet_unit_check( sstats.bytes_sent == total_wire + (uint64_t) N * overhead );

    struct rocketnet_server_transport_client_stats_t cstats;
    memset( &cstats, 0, sizeof(cstats) );
    rocketnet_server_transport_client_stats( server, 0, &cstats );
    rocketnet_unit_check( cstats.packets_sent == (uint64_t) N );
    rocketnet_unit_check( cstats.bytes_sent == total_wire + (uint64_t) N * overhead );

    rocketnet_server_transport_destroy( server );
    printf( "wire_totals_staged_finish_counts_exactly\n" );
}

static void driver_loopback_holds_max_clients_plus_server()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    const int live_before = rocketnet_loopback_endpoints_live();

    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    const rocketnet_server_driver_config_t server_setup = { "loopback", &server_config, NULL };
    rocketnet_address_t server_address;
    rocketnet_unit_check( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    rocketnet_server_driver_t server = {};
    rocketnet_unit_check( rocketnet_server_driver_open( backend->server, NULL, &server_setup, &server_address, &server ) );
    rocketnet_unit_check( server_address.port != 0 );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };
    rocketnet_client_driver_t * clients = (rocketnet_client_driver_t*) calloc( ROCKETNET_MAX_CLIENTS, sizeof( rocketnet_client_driver_t ) );
    rocketnet_address_t * client_addresses = (rocketnet_address_t*) calloc( ROCKETNET_MAX_CLIENTS, sizeof( rocketnet_address_t ) );
    rocketnet_unit_check( clients && client_addresses );

    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        rocketnet_unit_check( rocketnet_address_parse( &client_addresses[i], "127.0.0.1:0" ) );
        rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &client_addresses[i], &clients[i] ) );
        rocketnet_unit_check( client_addresses[i].port != 0 );
    }
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS );
    if ( live_before == 0 )
        rocketnet_unit_check( rocketnet_loopback_endpoints_live() == ROCKETNET_LOOPBACK_MAX_ENDPOINTS );

    const uint8_t data[] = { 1, 2, 3, 4 };
    rocketnet_address_t to = server_address;
    set_ipv4_loopback( to );
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
        clients[i].operations->send( clients[i].instance, &to, data, sizeof( data ) );

    const int capacity = 64;
    const int stride = ROCKETNET_RECEIVE_CEILING;
    uint8_t * batch = (uint8_t*) malloc( (size_t) stride * capacity );
    rocketnet_address_t * froms = (rocketnet_address_t*) malloc( sizeof( rocketnet_address_t ) * capacity );
    int * batch_bytes = (int*) malloc( sizeof( int ) * capacity );
    bool * seen = (bool*) calloc( ROCKETNET_MAX_CLIENTS, sizeof( bool ) );
    rocketnet_unit_check( batch && froms && batch_bytes && seen );

    int total = 0;
    while ( total < ROCKETNET_MAX_CLIENTS )
    {
        const int received = server.operations->receive_batch( server.instance, froms, batch, stride, batch_bytes, capacity );
        rocketnet_unit_check( received > 0 );
        for ( int i = 0; i < received; ++i )
        {
            rocketnet_unit_check( batch_bytes[i] == (int) sizeof( data ) );
            rocketnet_unit_check( memcmp( batch + (size_t) i * stride, data, sizeof( data ) ) == 0 );
            int index = -1;
            for ( int c = 0; c < ROCKETNET_MAX_CLIENTS; ++c )
                if ( client_addresses[c].port == froms[i].port )
                    { index = c; break; }
            rocketnet_unit_check( index >= 0 );
            rocketnet_unit_check( !seen[index] );
            seen[index] = true;
            ++total;
        }
    }
    rocketnet_unit_check( total == ROCKETNET_MAX_CLIENTS );

    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        rocketnet_platform_send_t packet = { &client_addresses[i], data, (int) sizeof( data ) };
        server.operations->send_batch( server.instance, &packet, 1 );
    }
    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
    {
        rocketnet_address_t from;
        uint8_t buf[64];
        const int bytes = clients[i].operations->receive( clients[i].instance, &from, buf, 64 );
        rocketnet_unit_check( bytes == (int) sizeof( data ) );
        rocketnet_unit_check( memcmp( buf, data, sizeof( data ) ) == 0 );
        rocketnet_unit_check( from.port == server_address.port );
    }

    for ( int i = 0; i < ROCKETNET_MAX_CLIENTS; ++i )
        rocketnet_client_driver_close( &clients[i] );
    rocketnet_server_driver_close( &server );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before );

    free( batch );
    free( froms );
    free( batch_bytes );
    free( seen );
    free( clients );
    free( client_addresses );
    printf( "driver_loopback_holds_max_clients_plus_server\n" );
}

static char g_loopback_refusal_log[1024];
static int g_loopback_refusal_log_count;

static void loopback_refusal_log_capture( void *, int, const char * message, size_t length )
{
    g_loopback_refusal_log_count++;
    const size_t copy = length < sizeof( g_loopback_refusal_log ) - 1 ? length : sizeof( g_loopback_refusal_log ) - 1;
    memcpy( g_loopback_refusal_log, message, copy );
    g_loopback_refusal_log[copy] = '\0';
}

static void driver_loopback_refuses_past_max_endpoints()
{
    const rocketnet_driver_backend_t * backend = &rocketnet_loopback_driver;
    const int live_before = rocketnet_loopback_endpoints_live();
    const int saved_level = rocketnet_get_log_level();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    rocketnet_set_log_callback( loopback_refusal_log_capture, NULL );

    rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    const rocketnet_client_driver_config_t client_setup = { "loopback", &client_config };
    rocketnet_client_driver_t * clients = (rocketnet_client_driver_t*) calloc( ROCKETNET_LOOPBACK_MAX_ENDPOINTS, sizeof( rocketnet_client_driver_t ) );
    rocketnet_unit_check( clients );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
    {
        rocketnet_address_t address;
        rocketnet_unit_check( rocketnet_address_parse( &address, "127.0.0.1:0" ) );
        rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &address, &clients[i] ) );
    }
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS );

    g_loopback_refusal_log_count = 0;
    g_loopback_refusal_log[0] = '\0';
    rocketnet_address_t extra;
    rocketnet_unit_check( rocketnet_address_parse( &extra, "127.0.0.1:0" ) );
    rocketnet_client_driver_t refused = {};
    rocketnet_unit_check( !rocketnet_client_driver_open( backend->client, NULL, &client_setup, &extra, &refused ) );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS );

    char value[32];
    snprintf( value, sizeof( value ), "%d", (int) ROCKETNET_LOOPBACK_MAX_ENDPOINTS );
    rocketnet_unit_check( g_loopback_refusal_log_count == 1 );
    rocketnet_unit_check( strstr( g_loopback_refusal_log, "ROCKETNET_LOOPBACK_MAX_ENDPOINTS" ) != NULL );
    rocketnet_unit_check( strstr( g_loopback_refusal_log, value ) != NULL );

    rocketnet_client_driver_close( &clients[0] );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before + ROCKETNET_LOOPBACK_MAX_ENDPOINTS - 1 );

    rocketnet_address_t again;
    rocketnet_unit_check( rocketnet_address_parse( &again, "127.0.0.1:0" ) );
    rocketnet_unit_check( rocketnet_client_driver_open( backend->client, NULL, &client_setup, &again, &clients[0] ) );

    for ( int i = 0; i < ROCKETNET_LOOPBACK_MAX_ENDPOINTS; ++i )
        rocketnet_client_driver_close( &clients[i] );
    rocketnet_unit_check( rocketnet_loopback_endpoints_live() == live_before );

    rocketnet_set_log_callback( NULL, NULL );
    rocketnet_log_level( saved_level );
    free( clients );
    printf( "driver_loopback_refuses_past_max_endpoints\n" );
}

REGISTER_UNIT_TEST( driver_loopback_binds_and_discovers_a_port );
REGISTER_UNIT_TEST( driver_loopback_exchanges_both_ways );
REGISTER_UNIT_TEST( driver_loopback_full_queue_drops_and_counts );
REGISTER_UNIT_TEST( driver_loopback_destroy_frees_the_port );
REGISTER_UNIT_TEST( driver_loopback_holds_max_clients_plus_server );
REGISTER_UNIT_TEST( driver_loopback_refuses_past_max_endpoints );
REGISTER_UNIT_TEST( wire_totals_staged_finish_counts_exactly );
