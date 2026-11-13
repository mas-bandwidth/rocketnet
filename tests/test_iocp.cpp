/* RocketNet. Copyright 2026 Más Bandwidth LLC. */
#include "native_shutdown.h"
#include "rocketnet_internal.h"
#include "rocketnet_platform.h"
#include "rocketnet_server_iocp.h"
#include "rocketnet_server_rio.h"
#include "rocketnet_receive_prefilter.h"
#include "rocketnet_memory.h"
#include <ws2tcpip.h>
#include <mswsock.h>
#include <malloc.h>
#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The portable socket's don't-fragment options as the kernel reports them on
// a real socket made by rocketnet_platform_socket_create (the client's and
// the portable server's Windows create sets them inline, not through
// rocketnet_platform_socket_apply_dont_fragment, so a unit test of that helper
// does not see them): IP_DONTFRAGMENT always, IPV6_DONTFRAG on an IPv6
// socket. Returns false when the socket could not be made or read.

static bool platform_socket_dont_fragment( const char * bind_text, int * ipv4_enabled, int * ipv6_enabled )
{
    rocketnet_address_t bind;
    if ( !rocketnet_address_parse( &bind, bind_text ) )
    {
        return false;
    }
    rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                             ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    if ( !socket )
    {
        return false;
    }
    *ipv4_enabled = 0;
    *ipv6_enabled = 0;
    int bytes = (int) sizeof( *ipv4_enabled );
    bool read = getsockopt( (SOCKET) socket->handle, IPPROTO_IP, IP_DONTFRAGMENT, (char *) ipv4_enabled, &bytes ) == 0 &&
                bytes == (int) sizeof( *ipv4_enabled );
    if ( bind.type == ROCKETNET_ADDRESS_IPV6 )
    {
        bytes = (int) sizeof( *ipv6_enabled );
        read = read && getsockopt( (SOCKET) socket->handle, IPPROTO_IPV6, IPV6_DONTFRAG, (char *) ipv6_enabled, &bytes ) == 0 &&
               bytes == (int) sizeof( *ipv6_enabled );
    }
    else
    {
        *ipv6_enabled = 1;
    }
    rocketnet_platform_socket_destroy( socket );
    return read;
}

#if !ROCKETNET_DEV

// This translation unit also supplies the production-linked native transport
// smoke target. It uses no control seam: both constructors have their ordinary
// signatures, probe the host normally, exchange real datagrams and cancel
// their posted receives during bounded shutdown.
typedef void ( *production_receive_fn )( void * transport );
typedef rocketnet_server_socket_process_packets_t * ( *production_process_fn )( void * transport );
typedef uint8_t * ( *production_start_fn )( void * transport, const rocketnet_address_t * to, uint64_t * packet_id );
typedef void ( *production_finish_fn )( void * transport, uint64_t packet_id, uint8_t * frame, int packet_bytes );
typedef void ( *production_send_fn )( void * transport );

#define PRODUCTION_CHECK( x ) do { if (!(x)) { fprintf(stderr, "production Windows transport check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

static void production_exchange( void * transport, const rocketnet_address_t * bind_address,
                                 production_receive_fn receive_packets, production_process_fn process_packets,
                                 production_start_fn start_packet, production_finish_fn finish_packet, production_send_fn send_packets )
{
    rocketnet_address_t client_address;
    PRODUCTION_CHECK( rocketnet_address_parse( &client_address, "127.0.0.1:0" ) );

    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
    PRODUCTION_CHECK( client && client_address.port != 0 );

    rocketnet_address_t server_address;
    PRODUCTION_CHECK( rocketnet_address_parse( &server_address, "127.0.0.1:0" ) );
    server_address.port = bind_address->port;

    const int packet_bytes = ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES + 7;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    for ( int i = 0; i < packet_bytes; ++i )
    {
        packet[i] = (uint8_t) ( i * 13 + 5 );
    }
    rocketnet_platform_socket_send_packet( client, &server_address, packet, packet_bytes );

    bool received = false;
    double deadline = rocketnet_platform_time() + 5.0;
    while ( !received && rocketnet_platform_time() < deadline )
    {
        receive_packets( transport );

        rocketnet_server_socket_process_packets_t * batch = process_packets( transport );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            PRODUCTION_CHECK( batch->packet_bytes[i] == packet_bytes );
            PRODUCTION_CHECK( batch->from[i].type == ROCKETNET_ADDRESS_IPV4 && batch->from[i].port == client_address.port );
            PRODUCTION_CHECK( memcmp( batch->packet_data[i], packet, (size_t) packet_bytes ) == 0 );
            received = true;
        }

        if ( !received )
        {
            rocketnet_platform_sleep( 0.001 );
        }
    }

    PRODUCTION_CHECK( received );

    uint64_t packet_id = 0;
    uint8_t * frame = start_packet( transport, &client_address, &packet_id );
    PRODUCTION_CHECK( frame );

    uint8_t * wire = frame + ROCKETNET_FRAME_WIRE_OFFSET;
    for ( int i = 0; i < packet_bytes; ++i )
    {
        wire[i] = (uint8_t) ( i * 29 + 11 );
    }
    finish_packet( transport, packet_id, frame, packet_bytes );
    send_packets( transport );

    int reply_bytes = 0;
    rocketnet_address_t from = {};
    deadline = rocketnet_platform_time() + 5.0;
    while ( reply_bytes <= 0 && rocketnet_platform_time() < deadline )
    {
        // Flush is a nonblocking try-publication. Retry until the worker owns
        // the staged frame; after a successful handoff later calls are empty.
        send_packets( transport );
        reply_bytes = rocketnet_platform_socket_receive_packet( client, &from, packet, (int) sizeof( packet ) );
        if ( reply_bytes <= 0 )
        {
            rocketnet_platform_sleep( 0.001 );
        }
    }

    PRODUCTION_CHECK( reply_bytes == packet_bytes && from.port == bind_address->port );
    for ( int i = 0; i < packet_bytes; ++i )
    {
        PRODUCTION_CHECK( packet[i] == (uint8_t) ( i * 29 + 11 ) );
    }
    rocketnet_platform_socket_destroy( client );
}

static void production_iocp_receive( void * p ) { rocketnet_server_iocp_receive_packets( (rocketnet_server_iocp_t *) p ); }
static rocketnet_server_socket_process_packets_t * production_iocp_process( void * p ) { return rocketnet_server_iocp_process_packets( (rocketnet_server_iocp_t *) p ); }
static uint8_t * production_iocp_start( void * p, const rocketnet_address_t * to, uint64_t * id ) { return rocketnet_server_iocp_start_packet( (rocketnet_server_iocp_t *) p, to, id ); }
static void production_iocp_finish( void * p, uint64_t id, uint8_t * frame, int bytes ) { rocketnet_server_iocp_finish_packet( (rocketnet_server_iocp_t *) p, id, frame, bytes ); }
static void production_iocp_send( void * p ) { rocketnet_server_iocp_send_packets( (rocketnet_server_iocp_t *) p ); }

static void production_rio_receive( void * p ) { rocketnet_server_rio_receive_packets( (rocketnet_server_rio_t *) p ); }
static rocketnet_server_socket_process_packets_t * production_rio_process( void * p ) { return rocketnet_server_rio_process_packets( (rocketnet_server_rio_t *) p ); }
static uint8_t * production_rio_start( void * p, const rocketnet_address_t * to, uint64_t * id ) { return rocketnet_server_rio_start_packet( (rocketnet_server_rio_t *) p, to, id ); }
static void production_rio_finish( void * p, uint64_t id, uint8_t * frame, int bytes ) { rocketnet_server_rio_finish_packet( (rocketnet_server_rio_t *) p, id, frame, bytes ); }
static void production_rio_send( void * p ) { rocketnet_server_rio_send_packets( (rocketnet_server_rio_t *) p ); }

int main( int argc, char ** argv )
{
    // every case runs, always: an argument (a case name, a flag) is refused,
    // never ignored, so a caller that believes it selected something learns
    // it did not

    if ( argc != 1 )
    {
        fprintf( stderr, "usage: %s (no arguments: it runs every case)\n", argv[0] );
        return 2;
    }

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    PRODUCTION_CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    rocketnet_address_t bind;
    PRODUCTION_CHECK( rocketnet_address_parse( &bind, "127.0.0.1:0" ) );

    rocketnet_server_iocp_t * iocp = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
    PRODUCTION_CHECK( iocp && bind.port != 0 );

    const int features = rocketnet_server_iocp_features( iocp );
    production_exchange( iocp, &bind, production_iocp_receive, production_iocp_process,
                         production_iocp_start, production_iocp_finish, production_iocp_send );
    rocketnet_server_iocp_stop( iocp );

    double deadline = rocketnet_platform_time() + 5.0;
    while ( !rocketnet_server_iocp_stopped( iocp ) && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    PRODUCTION_CHECK( rocketnet_server_iocp_stopped( iocp ) && rocketnet_server_iocp_pending( iocp ) == 0 );
    rocketnet_server_iocp_destroy( iocp );
    printf( "test_iocp_production_exchange_shutdown features=%d\n", features );

    PRODUCTION_CHECK( rocketnet_server_rio_available() );
    PRODUCTION_CHECK( rocketnet_address_parse( &bind, "127.0.0.1:0" ) );

    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &bind, ROCKETNET_DEFAULT_MAX_CLIENTS );
    PRODUCTION_CHECK( rio && bind.port != 0 );
    production_exchange( rio, &bind, production_rio_receive, production_rio_process,
                         production_rio_start, production_rio_finish, production_rio_send );
    rocketnet_server_rio_stop( rio );
    deadline = rocketnet_platform_time() + 5.0;
    while ( !rocketnet_server_rio_stopped( rio ) && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    PRODUCTION_CHECK( rocketnet_server_rio_stopped( rio ) && rocketnet_server_rio_pending( rio ) == 0 );
    rocketnet_server_rio_destroy( rio );
    printf( "test_rio_production_exchange_shutdown\n" );

    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };
    for ( int family = 0; family < 2; ++family )
    {
        int ipv4_enabled = 0;
        int ipv6_enabled = 0;
        PRODUCTION_CHECK( platform_socket_dont_fragment( binds[family], &ipv4_enabled, &ipv6_enabled ) );
        PRODUCTION_CHECK( ipv4_enabled == 1 && ipv6_enabled == 1 );
    }
    printf( "test_platform_socket_production_sets_dont_fragment\n" );

    rocketnet_term();
    printf( "*** PRODUCTION WINDOWS TRANSPORTS PASSED ***\n" );

    return 0;
}

#else

#if defined( _WIN32 )
bool rocketnet_server_rio_test_receive_overflow( uint64_t * counters, int * packet_counts,
                                                 int * repost_calls, int * pending_receives, int * guard_hit );
#endif // #if defined( _WIN32 )

#define CHECK( x ) do { if (!(x)) { fprintf(stderr, "IOCP check failed: %s (%s:%d)\n", #x, __FILE__, __LINE__); exit(1); } } while (0)

// The library's default assert handler logs through rocketnet_error, which
// ROCKETNET_LOG_LEVEL_NONE silences, so a failed assert would end this binary
// with no message. main installs this handler: it prints the assert to stderr,
// then traps through the default handler.
static rocketnet_assert_function_t iocp_default_assert = NULL;

static void iocp_print_assert( const char * condition, const char * function, const char * file, int line )
{
    fprintf( stderr, "assert failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stderr );
    iocp_default_assert( condition, function, file, line );
}

// Native workers advance in real time. Wait for the asserted state, never for
// an assumed amount of work to fit inside one caller flush or a fixed sleep.

template <typename Predicate>
static void wait_until( const char * name, Predicate predicate, double seconds = 5.0 )
{
    const double deadline = rocketnet_platform_time() + seconds;

    while ( !predicate() )
    {
        if ( rocketnet_platform_time() >= deadline )
        {
            fprintf( stderr, "IOCP check failed: timed out waiting for %s\n", name );
            exit( 1 );
        }

        rocketnet_platform_sleep( 0.001 );
    }
}

// The TX worker sends published banks off the caller thread. A test that
// asserts on what one flush did first waits until the producer is published
// and every published bank is serviced (sends posted, aborted frames freed).

static void iocp_flush( rocketnet_server_iocp_t * transport )
{
    wait_until( "IOCP TX idle", [&]()
                {
        rocketnet_server_iocp_send_packets(transport);
        return rocketnet_server_iocp_test_tx_idle(transport); } );
}

static std::atomic<int> live_allocations{ 0 };
static std::atomic<int64_t> live_bytes{ 0 };
static std::atomic<int> allocations{ 0 };
static int fail_allocation = -1;

static void * allocate( void *, size_t bytes )
{
    const int index = allocations.fetch_add( 1 );
    if ( index == fail_allocation )
    {
        return NULL;
    }

    void * p = malloc( bytes );
    if ( p )
    {
        memset( p, 0xB5, bytes );
        live_allocations.fetch_add( 1 );
        live_bytes.fetch_add( (int64_t) _msize( p ) );
    }

    return p;
}

static void release( void *, void * p )
{
    if ( p )
    {
        live_allocations.fetch_sub( 1 );
        live_bytes.fetch_sub( (int64_t) _msize( p ) );
        free( p );
    }
}

static rocketnet_address_t address( const char * text )
{
    rocketnet_address_t result;
    CHECK( rocketnet_address_parse( &result, text ) );
    return result;
}

static void wait_stopped( rocketnet_server_iocp_t * transport )
{
    rocketnet_server_iocp_stop( transport );

    const double deadline = rocketnet_platform_time() + 5.0;
    while ( !rocketnet_server_iocp_stopped( transport ) && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    CHECK( rocketnet_server_iocp_stopped( transport ) );
    CHECK( rocketnet_server_iocp_pending( transport ) == 0 );
}

static void test_create_cleanup()
{
    const int baseline = live_allocations.load();
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    allocations.store( 0 );

    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 2, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport && bind.port != 0 );

    const int count = allocations.load();
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    CHECK( live_allocations.load() == baseline );

    DWORD handle_baseline;
    CHECK( GetProcessHandleCount( GetCurrentProcess(), &handle_baseline ) );

    for ( int i = 0; i < count; ++i )
    {
        bind = address( "127.0.0.1:0" );
        allocations.store( 0 );
        fail_allocation = i;
        transport = rocketnet_server_iocp_create( NULL, &bind, 2, ROCKETNET_DEFAULT_MAX_CLIENTS );
        fail_allocation = -1;
        CHECK( !transport );
        CHECK( live_allocations.load() == baseline );
    }

    for ( int repeat = 0; repeat < 3; ++repeat )
    {
        for ( int stage = 1; stage <= 4; ++stage )
        {
            bind = address( "127.0.0.1:0" );
            CHECK( !rocketnet_server_iocp_create( NULL, &bind, 2, ROCKETNET_DEFAULT_MAX_CLIENTS, stage ) );
            CHECK( live_allocations.load() == baseline );
        }
    }

    DWORD handles;
    CHECK( GetProcessHandleCount( GetCurrentProcess(), &handles ) );
    CHECK( handles <= handle_baseline + 2 ); // Repeated failure must not leak handles.
    CHECK( !rocketnet_server_iocp_create( NULL, &bind, ROCKETNET_IOCP_MAX_WORKERS + 1, ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( !rocketnet_server_iocp_create( NULL, &bind, -1, ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    printf( "test_iocp_transport_create_cleanup\n" );
}

static void test_completion_owns_frames()
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport );

    rocketnet_address_t client_address = address( "127.0.0.1:0" );
    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024 );
    CHECK( client );

    rocketnet_address_t invalid = {};
    uint64_t refused_id;
    CHECK( !rocketnet_server_iocp_start_packet( transport, &invalid, &refused_id ) );

    // the refused address is counted SEND_FAILED, once (an address the
    // socket cannot carry, never a full staging); the sends below add none

    uint64_t refused_counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( transport, refused_counters );
    CHECK( refused_counters[ROCKETNET_IOCP_SEND_FAILED] == 1 );

    // address refusal must leave a whole generation available below

    // exhaust and abort a complete phase. Every frame must be reclaimable.

    std::set<uint8_t *> frames;

    for ( int i = 0; i < rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ); ++i )
    {
        uint64_t id;
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && ( (uintptr_t) frame % 16 ) == 0 && frames.insert( frame ).second );
        rocketnet_server_iocp_abort_packet( transport, id, frame );
    }

    uint64_t id;
    CHECK( !rocketnet_server_iocp_start_packet( transport, &client_address, &id ) );
    iocp_flush( transport );

    // Deterministically hold retirement while the OS completes sends. These
    // buffers must remain unavailable across flushes until completion is read.

    rocketnet_server_iocp_test_hold_completions( transport, true );

    std::set<uint8_t *> pending;

    for ( int i = 0; i < 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && pending.insert( frame ).second );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, i, 32 );
        rocketnet_server_iocp_finish_packet( transport, id, frame, 32 );
    }

    iocp_flush( transport );

    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( transport, counters );
    CHECK( counters[ROCKETNET_IOCP_SEND_FAILED] == 1 );

    for ( int i = 0; i < rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) - 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && !pending.count( frame ) );
        rocketnet_server_iocp_abort_packet( transport, id, frame );
    }

    CHECK( !rocketnet_server_iocp_start_packet( transport, &client_address, &id ) );
    rocketnet_server_iocp_test_hold_completions( transport, false );

    const double deadline = rocketnet_platform_time() + 5.0;

    do
    {
        rocketnet_server_iocp_counters( transport, counters );
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] == 32 )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    } while ( rocketnet_platform_time() < deadline );

    CHECK( counters[ROCKETNET_IOCP_SEND_COMPLETED] == 32 );
    CHECK( counters[ROCKETNET_IOCP_SEND_FAILED] == 1 );          // the refused address alone

    // Counters are diagnostic reads, not completion fences. Join the
    // retirement batch before checking that every frame is back in the pool.

    rocketnet_server_iocp_test_hold_completions( transport, true );
    rocketnet_server_iocp_test_hold_completions( transport, false );

    // The failed pop above must not burn a staging slot. As the 32 in-flight
    // frames return, all 32 must be reservable in this SAME fill generation.

    for ( int i = 0; i < 32; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && pending.erase( frame ) == 1 );
        rocketnet_server_iocp_abort_packet( transport, id, frame );
    }

    CHECK( pending.empty() );
    iocp_flush( transport );
    frames.clear();

    for ( int i = 0; i < rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ); ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && frames.insert( frame ).second );
        rocketnet_server_iocp_abort_packet( transport, id, frame );
    }

    iocp_flush( transport );
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    rocketnet_platform_socket_destroy( client );
    printf( "test_iocp_transport_completion_owns_frames\n" );
}

static void test_uso_run_owns_frames()
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport );

    if ( !( rocketnet_server_iocp_features( transport ) & ROCKETNET_IOCP_FEATURE_USO ) )
    {
        wait_stopped( transport );
        rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
        return;
    }

    rocketnet_address_t client_address = address( "127.0.0.1:0" );
    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024 );
    CHECK( client );

    // One run of equal-size frames to one client is one segmented post. Hold
    // retirement: no frame of the run may come back before that completion.

    const int run = 16;
    rocketnet_server_iocp_test_hold_completions( transport, true );

    std::set<uint8_t *> pending;
    uint64_t id;

    for ( int i = 0; i < run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && pending.insert( frame ).second );
        memset( frame + ROCKETNET_FRAME_WIRE_OFFSET, i, 200 );
        rocketnet_server_iocp_finish_packet( transport, id, frame, 200 );
    }

    iocp_flush( transport );

    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    wait_until( "worker USO submission", [&]()
                {
        rocketnet_server_iocp_send_packets(transport);
        rocketnet_server_iocp_counters(transport, counters);
        return counters[ROCKETNET_IOCP_USO_SENDS] >= 1; } );
    rocketnet_server_iocp_counters( transport, counters );
    CHECK( counters[ROCKETNET_IOCP_USO_SENDS] == 1 );
    CHECK( counters[ROCKETNET_IOCP_SEND_FAILED] == 0 );
    CHECK( counters[ROCKETNET_IOCP_SEND_COMPLETED] == 0 );
#if ROCKETNET_DEV
    rocketnet_server_iocp_test_queue_stats_t run_stats;
    rocketnet_server_iocp_test_queue_stats( transport, &run_stats );
    CHECK( run_stats.live_run_refs == run );
#endif // #if ROCKETNET_DEV
    for ( int i = 0; i < rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) - run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && !pending.count( frame ) );
        rocketnet_server_iocp_abort_packet( transport, id, frame );
    }

    CHECK( !rocketnet_server_iocp_start_packet( transport, &client_address, &id ) );
    rocketnet_server_iocp_test_hold_completions( transport, false );

    const double deadline = rocketnet_platform_time() + 5.0;

    // The run is already published. Do not publish here: the aborted frames
    // staged above stay unpublished, so the pool holds only what the run's
    // completion returns, whichever of TX and the completion runs first.

    do
    {
        rocketnet_server_iocp_counters( transport, counters );
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] == (uint64_t) run )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    } while ( rocketnet_platform_time() < deadline );

    CHECK( counters[ROCKETNET_IOCP_SEND_COMPLETED] == (uint64_t) run );
    CHECK( counters[ROCKETNET_IOCP_SEND_FAILED] == 0 );
    rocketnet_server_iocp_test_hold_completions( transport, true );
    rocketnet_server_iocp_test_hold_completions( transport, false );
#if ROCKETNET_DEV
    rocketnet_server_iocp_test_queue_stats( transport, &run_stats );
    CHECK( run_stats.live_run_refs == 0 );
#endif // #if ROCKETNET_DEV
    // the one completion returned every frame of the run, and only those

    for ( int i = 0; i < run; ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &client_address, &id );
        CHECK( frame && pending.erase( frame ) == 1 );
        rocketnet_server_iocp_abort_packet( transport, id, frame );
    }

    CHECK( pending.empty() );
    iocp_flush( transport );
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    rocketnet_platform_socket_destroy( client );
    printf( "test_iocp_transport_uso_run_owns_frames\n" );
}

static void test_datagrams_and_producers()
{
    rocketnet_address_t bind = address( "[::]:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 2, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport && bind.port );

    rocketnet_address_t destination = address( "127.0.0.1:0" );
    destination.port = bind.port;

    rocketnet_address_t client_address = address( "127.0.0.1:0" );
    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024 );
    CHECK( client );

    const uint8_t marker[] = { 9, 8, 7, 6 };
    rocketnet_platform_socket_send_packet( client, &destination, marker, sizeof( marker ) );

    uint8_t * received = NULL;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline && !received )
    {
        rocketnet_server_iocp_receive_packets( transport );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
        if ( batch->num_packets )
        {
            CHECK( batch->num_packets == 1 );
            CHECK( batch->from[0].type == ROCKETNET_ADDRESS_IPV4 && batch->from[0].port == client_address.port );
            CHECK( batch->packet_bytes[0] == sizeof( marker ) && memcmp( batch->packet_data[0], marker, sizeof( marker ) ) == 0 );
            received = batch->packet_data[0];
        }

        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( received );
    CHECK( rocketnet_server_iocp_test_receive_view_published( transport, received ) );

    uint64_t before[ROCKETNET_IOCP_NUM_COUNTERS], after[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( transport, before );

    uint8_t extra[32];
    memset( extra, 0x44, sizeof( extra ) );
    for ( int i = 0; i < 100; ++i )
    {
        rocketnet_platform_socket_send_packet( client, &destination, extra, sizeof( extra ) );
    }

    const double receive_deadline = rocketnet_platform_time() + 5.0;

    do
    {
        rocketnet_server_iocp_counters( transport, after );
        if ( after[ROCKETNET_IOCP_RECEIVE_COMPLETED] > before[ROCKETNET_IOCP_RECEIVE_COMPLETED] )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    } while ( rocketnet_platform_time() < receive_deadline );

    CHECK( after[ROCKETNET_IOCP_RECEIVE_COMPLETED] > before[ROCKETNET_IOCP_RECEIVE_COMPLETED] );
    CHECK( rocketnet_server_iocp_test_receive_view_published( transport, received ) );
    CHECK( memcmp( received, marker, sizeof( marker ) ) == 0 ); // View lifetime until next receive.

    std::vector<std::thread> producers;
    for ( int t = 0; t < 4; ++t )
    {
        producers.emplace_back( [=]()
                                {
            for ( int i = 0; i < 64; ++i )
            {
                uint64_t id;
                uint8_t * frame = rocketnet_server_iocp_start_packet(transport, &client_address, &id);
                CHECK(frame);
                memset(frame + ROCKETNET_FRAME_WIRE_OFFSET, t * 64 + i, 32);
                rocketnet_server_iocp_finish_packet(transport, id, frame, 32);
            } } );
    }
    for ( auto & thread : producers )
    {
        thread.join();
    }
    iocp_flush( transport );

    bool seen[256] = {};
    int count = 0;
    const double send_deadline = rocketnet_platform_time() + 5.0;
    while ( count < 256 && rocketnet_platform_time() < send_deadline )
    {
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        rocketnet_address_t from;
        const int bytes = rocketnet_platform_socket_receive_packet( client, &from, packet, sizeof( packet ) );
        if ( !bytes )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }

        CHECK( bytes == 32 && from.port == bind.port && !seen[packet[0]] );
        for ( int i = 1; i < 32; ++i )
        {
            CHECK( packet[i] == packet[0] );
        }
        seen[packet[0]] = true;
        ++count;
    }

    CHECK( count == 256 );

    // Pause completion work. Stop must return while a worker is paused, but
    // completion stays false until the worker can drain cancellation.

    rocketnet_server_iocp_test_hold_completions( transport, true );

    std::atomic<bool> sending{ true };
    std::thread incoming( [&]()
                          {
        while (sending.load())
        {
            rocketnet_platform_socket_send_packet(client, &destination, extra, sizeof(extra));
        } } );

    std::atomic<bool> stop_returned{ false };
    std::thread stopping( [&]()
                          {
        rocketnet_server_iocp_stop(transport);
        stop_returned.store(true); } );

    const double stop_deadline = rocketnet_platform_time() + 5.0;
    while ( !stop_returned.load() && rocketnet_platform_time() < stop_deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    CHECK( stop_returned.load() );
    CHECK( rocketnet_server_iocp_test_stopping( transport ) );
    CHECK( !rocketnet_server_iocp_stopped( transport ) );
    rocketnet_server_iocp_test_hold_completions( transport, false );
    stopping.join();
    CHECK( stop_returned.load() );
    wait_stopped( transport );
    sending.store( false );
    incoming.join();

    uint64_t stopped_id;
    CHECK( !rocketnet_server_iocp_start_packet( transport, &client_address, &stopped_id ) );
    rocketnet_server_iocp_stop( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    rocketnet_platform_socket_destroy( client );
    printf( "test_iocp_transport_datagrams_and_producers\n" );
}

static void test_receive_publication_never_waits()
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport );

    rocketnet_address_t client_address = address( "127.0.0.1:0" );
    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024 );
    CHECK( client );

    const uint8_t marker = 0x5a;
    rocketnet_platform_socket_send_packet( client, &bind, &marker, 1 );

    uint8_t * view = NULL;
    const double view_deadline = rocketnet_platform_time() + 5.0;
    while ( !view && rocketnet_platform_time() < view_deadline )
    {
        rocketnet_server_iocp_receive_packets( transport );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
        if ( batch->num_packets == 1 )
        {
            CHECK( batch->packet_bytes[0] == 1 && batch->packet_data[0][0] == marker );
            view = batch->packet_data[0];
        }

        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( view );

    std::atomic<bool> held{ false };
    std::atomic<bool> release_lock{ false };
    std::atomic<bool> receive_returned{ false };
    std::thread holder( [&]()
                        {
        rocketnet_server_iocp_test_hold_publication(transport, true);
        held.store(true);
        while (!release_lock.load())
        {
            rocketnet_platform_sleep(0.001);
        }
        rocketnet_server_iocp_test_hold_publication(transport, false); } );

    const double hold_deadline = rocketnet_platform_time() + 5.0;
    while ( !held.load() && rocketnet_platform_time() < hold_deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    CHECK( held.load() );
    std::thread receiver( [&]()
                          {
        rocketnet_server_iocp_receive_packets(transport);
        receive_returned.store(true); } );

    const double return_deadline = rocketnet_platform_time() + 1.0;
    while ( !receive_returned.load() && rocketnet_platform_time() < return_deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }

    const bool returned_before_release = receive_returned.load();
    release_lock.store( true );
    receiver.join();
    holder.join();
    CHECK( returned_before_release );
    CHECK( rocketnet_server_iocp_process_packets( transport )->num_packets == 0 );
    CHECK( view[0] == marker );
    CHECK( rocketnet_server_iocp_test_receive_view_published( transport, view ) );
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    rocketnet_platform_socket_destroy( client );
    printf( "test_iocp_transport_receive_publication_never_waits\n" );
}

static void test_receive_ready_banks_fifo()
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport && bind.port );

    rocketnet_address_t client_address = address( "127.0.0.1:0" );
    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024 );
    CHECK( client );

    for ( int marker = 1; marker <= 2; ++marker )
    {
        const uint8_t payload = (uint8_t) marker;
        rocketnet_platform_socket_send_packet( client, &bind, &payload, 1 );

        const double deadline = rocketnet_platform_time() + 5.0;
        while ( rocketnet_server_iocp_test_ready_banks( transport, 0 ) < marker && rocketnet_platform_time() < deadline )
        {
            rocketnet_platform_sleep( 0.001 );
        }
        CHECK( rocketnet_server_iocp_test_ready_banks( transport, 0 ) == marker );
    }

    rocketnet_server_iocp_receive_packets( transport );

    rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
    CHECK( batch->num_packets == 2 );
    for ( int i = 0; i < 2; ++i )
    {
        CHECK( batch->packet_bytes[i] == 1 && batch->packet_data[i][0] == i + 1 );
    }
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    rocketnet_platform_socket_destroy( client );
    printf( "test_iocp_transport_receive_ready_banks_fifo\n" );
}

// A zero-length datagram and one past the receive ceiling are each dropped
// and counted once, the first too short and the second too large, on the
// per-datagram receive path (URO disabled) and on the URO path where the
// host offers it; the good datagram behind them arrives.

static void test_receive_boundaries( int flags )
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, flags );
    CHECK( transport );
    const bool uro = ( rocketnet_server_iocp_features( transport ) & ROCKETNET_IOCP_FEATURE_URO ) != 0;
    CHECK( !uro || ( flags & ROCKETNET_IOCP_DISABLE_URO ) == 0 );

    SOCKET client = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( client != INVALID_SOCKET );

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons( bind.port );
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    char oversized[ROCKETNET_RECEIVE_CEILING + 64] = {};
    CHECK( sendto( client, oversized, 0, 0, (sockaddr *) &to, sizeof( to ) ) == 0 );
    CHECK( sendto( client, oversized, sizeof( oversized ), 0, (sockaddr *) &to, sizeof( to ) ) == sizeof( oversized ) );
    CHECK( sendto( client, "good", 4, 0, (sockaddr *) &to, sizeof( to ) ) == 4 );

    int received = 0;
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS] = {};
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets( transport );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            CHECK( batch->packet_bytes[i] == 4 && memcmp( batch->packet_data[i], "good", 4 ) == 0 );
            ++received;
        }

        rocketnet_server_iocp_counters( transport, counters );
        if ( received == 1 && counters[ROCKETNET_IOCP_RECEIVE_DROPPED] == 2 &&
             rocketnet_server_iocp_pending( transport ) == ROCKETNET_IOCP_RECEIVES_PER_WORKER )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( received == 1 && counters[ROCKETNET_IOCP_RECEIVE_DROPPED] == 2 );
    CHECK( counters[ROCKETNET_IOCP_RECEIVE_TOO_SHORT] == 1 );
    CHECK( counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE] == 1 );
    CHECK( counters[ROCKETNET_IOCP_RECEIVE_COMPLETED] == 1 );
    CHECK( counters[ROCKETNET_IOCP_RECEIVE_REPOST_FAILED] == 0 );
    CHECK( counters[ROCKETNET_IOCP_SEND_COMPLETED] == 0 && counters[ROCKETNET_IOCP_SEND_FAILED] == 0 );
    CHECK( rocketnet_server_iocp_pending( transport ) == ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    wait_stopped( transport ); // Cancellation with idle, outstanding receives too.
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    closesocket( client );
    printf( "test_iocp_transport_receive_boundaries uro=%d\n", uro ? 1 : 0 );
}

// Each dequeue of the I/O worker that returns completions counts one batch;
// a dequeue that times out with none counts nothing. With every receive
// posted and nothing sent, one datagram at a time is one completion, so one
// datagram, received before the next is sent, is exactly one batch. Run on
// the per-datagram path and on the URO path where the host offers it.

static void test_completion_batches_counted( int flags )
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, flags );
    CHECK( transport );
    const bool uro = ( rocketnet_server_iocp_features( transport ) & ROCKETNET_IOCP_FEATURE_URO ) != 0;

    SOCKET client = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( client != INVALID_SOCKET );

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons( bind.port );
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    wait_until( "IOCP receives posted", [&]()
                { return rocketnet_server_iocp_pending( transport ) == ROCKETNET_IOCP_RECEIVES_PER_WORKER; } );

    uint64_t before[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( transport, before );
    CHECK( before[ROCKETNET_IOCP_COMPLETION_BATCHES] == 0 );

    for ( int i = 0; i < 3; ++i )
    {
        char data[4] = { 'b', 'a', 't', (char) ( '0' + i ) };
        CHECK( sendto( client, data, 4, 0, (sockaddr *) &to, sizeof( to ) ) == 4 );

        int received = 0;
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( received == 0 && rocketnet_platform_time() < deadline )
        {
            rocketnet_server_iocp_receive_packets( transport );

            rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
            for ( int j = 0; j < batch->num_packets; ++j )
            {
                CHECK( batch->packet_bytes[j] == 4 && memcmp( batch->packet_data[j], data, 4 ) == 0 );
                ++received;
            }
            if ( received == 0 )
            {
                rocketnet_platform_sleep( 0.001 );
            }
        }
        CHECK( received == 1 );

        // the batch is counted before its completion is published, so the
        // count is final once the datagram is seen
        uint64_t now[ROCKETNET_IOCP_NUM_COUNTERS];
        rocketnet_server_iocp_counters( transport, now );
        CHECK( now[ROCKETNET_IOCP_COMPLETION_BATCHES] == (uint64_t) ( i + 1 ) );
        CHECK( now[ROCKETNET_IOCP_RECEIVE_COMPLETED] == (uint64_t) ( i + 1 ) );
    }

    uint64_t after[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( transport, after );
    for ( int c = 0; c < ROCKETNET_IOCP_NUM_COUNTERS; ++c )
    {
        if ( c == ROCKETNET_IOCP_COMPLETION_BATCHES || c == ROCKETNET_IOCP_RECEIVE_COMPLETED )
        {
            CHECK( after[c] - before[c] == 3 );
        }
        else
        {
            CHECK( after[c] == before[c] );
        }
    }

    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    closesocket( client );
    printf( "test_iocp_transport_completion_batches_counted uro=%d\n", uro ? 1 : 0 );
}

static void test_receive_boundaries_both_paths()
{
    test_receive_boundaries( 0 );
    test_receive_boundaries( ROCKETNET_IOCP_DISABLE_URO );
}

static void test_completion_batches_both_paths()
{
    test_completion_batches_counted( 0 );
    test_completion_batches_counted( ROCKETNET_IOCP_DISABLE_URO );
}

// A receive posted while an oversized datagram already waits in the socket:
// held completions take every posted receive with a small datagram, then
// oversized ones (a normal 1472-byte UDP payload and the 65507-byte IPv4
// maximum) queue behind them, so each repost meets one inline. Every repost
// is accepted, each oversized datagram is counted dropped and too large once,
// the depth stays exact, the datagrams behind them arrive, and the stop
// drains pending to exactly zero: no receive is posted twice on one operation.

static void receive_oversize_queued_at_post( int flags )
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, flags );
    CHECK( transport );
    CHECK( rocketnet_server_iocp_pending( transport ) == ROCKETNET_IOCP_RECEIVES_PER_WORKER );

    SOCKET client = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( client != INVALID_SOCKET );
    int send_buffer = 4 * 1024 * 1024;
    CHECK( setsockopt( client, SOL_SOCKET, SO_SNDBUF, (const char *) &send_buffer, sizeof( send_buffer ) ) == 0 );

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons( bind.port );
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    static char oversized[65507];
    memset( oversized, 0x5A, sizeof( oversized ) );
    const int sizes[] = { 1472, 65507 };
    const int per_size = 8;
    const int num_oversized = per_size * 2;
    const int num_after = 16;

    rocketnet_server_iocp_test_hold_completions( transport, true );
    for ( int i = 0; i < ROCKETNET_IOCP_RECEIVES_PER_WORKER; ++i )
    {
        CHECK( sendto( client, "fill", 4, 0, (sockaddr *) &to, sizeof( to ) ) == 4 );
    }
    for ( int i = 0; i < num_oversized; ++i )
    {
        const int bytes = sizes[i % 2];
        CHECK( sendto( client, oversized, bytes, 0, (sockaddr *) &to, sizeof( to ) ) == bytes );
    }
    for ( int i = 0; i < num_after; ++i )
    {
        CHECK( sendto( client, "good", 4, 0, (sockaddr *) &to, sizeof( to ) ) == 4 );
    }
    rocketnet_server_iocp_test_hold_completions( transport, false );

    int fills = 0;
    int goods = 0;
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS] = {};
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets( transport );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            CHECK( batch->packet_bytes[i] == 4 );
            if ( memcmp( batch->packet_data[i], "fill", 4 ) == 0 )
            {
                ++fills;
            }
            else if ( memcmp( batch->packet_data[i], "good", 4 ) == 0 )
            {
                ++goods;
            }
            else
            {
                CHECK( false );
            }
        }

        rocketnet_server_iocp_counters( transport, counters );
        if ( fills == ROCKETNET_IOCP_RECEIVES_PER_WORKER && goods == num_after &&
             counters[ROCKETNET_IOCP_RECEIVE_DROPPED] >= (uint64_t) num_oversized &&
             rocketnet_server_iocp_pending( transport ) == ROCKETNET_IOCP_RECEIVES_PER_WORKER )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    }

    // one more pass of the worker, so a late completion is counted too
    rocketnet_server_iocp_test_hold_completions( transport, true );
    rocketnet_server_iocp_test_hold_completions( transport, false );
    rocketnet_server_iocp_counters( transport, counters );
    printf( "  flags=%d fills=%d goods=%d dropped=%llu too_large=%llu repost_failed=%llu pending=%d\n", flags, fills, goods,
            (unsigned long long) counters[ROCKETNET_IOCP_RECEIVE_DROPPED], (unsigned long long) counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE],
            (unsigned long long) counters[ROCKETNET_IOCP_RECEIVE_REPOST_FAILED], rocketnet_server_iocp_pending( transport ) );

    CHECK( fills == ROCKETNET_IOCP_RECEIVES_PER_WORKER && goods == num_after );
    CHECK( counters[ROCKETNET_IOCP_RECEIVE_REPOST_FAILED] == 0 );
    CHECK( counters[ROCKETNET_IOCP_RECEIVE_DROPPED] == (uint64_t) num_oversized );
    CHECK( counters[ROCKETNET_IOCP_RECEIVE_TOO_LARGE] == (uint64_t) num_oversized );
    CHECK( rocketnet_server_iocp_pending( transport ) == ROCKETNET_IOCP_RECEIVES_PER_WORKER );
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    closesocket( client );
}

static void test_receive_oversize_queued_at_post()
{
    receive_oversize_queued_at_post( ROCKETNET_IOCP_DISABLE_URO );
    receive_oversize_queued_at_post( 0 );
    printf( "test_iocp_transport_receive_oversize_queued_at_post\n" );
}

// What Windows does with an overlapped receive posted onto a datagram that
// already waits in the socket and is longer than the receive's buffer, on a
// socket bound to a completion port. Whether the call fails at once with
// WSAEMSGSIZE, and whether a completion is queued for it either way, decides
// what rocketnet_iocp_post_receive and the iocp client's post must do with
// its return. Four cells: WSARecvFrom and WSARecvMsg, each with and without
// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (the transports never set it). Each
// cell posts the receive the way the transport does (no byte count argument,
// a ROCKETNET_RECEIVE_CEILING + 1 buffer), drains the port for a fixed time
// and prints what it saw, then sends a small datagram and posts a second
// receive to see whether the first is still in the kernel.

struct oversize_post_receive_t
{
    OVERLAPPED overlapped;
    WSABUF buffer;
    WSAMSG message;
    sockaddr_storage from;
    int from_bytes;
    DWORD flags;
    char control[64];
    char data[ROCKETNET_RECEIVE_CEILING + 1];
};

struct oversize_post_tally_t
{
    int count[2];
    OVERLAPPED_ENTRY entry[2];
};

#define OVERSIZE_POST_DRAIN_SECONDS 0.25

static unsigned long windows_build_number()
{
    typedef LONG( WINAPI * rtl_get_version_t )( OSVERSIONINFOW * );
    HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    CHECK( ntdll );
    rtl_get_version_t rtl_get_version = (rtl_get_version_t) (void ( * )( void )) GetProcAddress( ntdll, "RtlGetVersion" );
    CHECK( rtl_get_version );
    OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof( version );
    CHECK( rtl_get_version( &version ) == 0 );
    return version.dwBuildNumber;
}

static int oversize_post_receive( SOCKET socket, LPFN_WSARECVMSG receive_message, oversize_post_receive_t * receive, int * error )
{
    memset( receive, 0, sizeof( *receive ) );
    receive->buffer.buf = receive->data;
    receive->buffer.len = (ULONG) sizeof( receive->data );
    receive->from_bytes = (int) sizeof( receive->from );

    int result;
    if ( receive_message )
    {
        receive->message.name = (LPSOCKADDR) &receive->from;
        receive->message.namelen = (INT) sizeof( receive->from );
        receive->message.lpBuffers = &receive->buffer;
        receive->message.dwBufferCount = 1;
        receive->message.Control.buf = receive->control;
        receive->message.Control.len = (ULONG) sizeof( receive->control );
        result = receive_message( socket, &receive->message, NULL, &receive->overlapped, NULL );
    }
    else
    {
        result = WSARecvFrom( socket, &receive->buffer, 1, NULL, &receive->flags,
                              (sockaddr *) &receive->from, &receive->from_bytes, &receive->overlapped, NULL );
    }

    *error = result == 0 ? 0 : WSAGetLastError();
    return result;
}

// every completion the port delivers within the drain time, counted per receive
static void oversize_post_drain( HANDLE port, oversize_post_receive_t * receives, oversize_post_tally_t * tally )
{
    memset( tally, 0, sizeof( *tally ) );

    const double deadline = rocketnet_platform_time() + OVERSIZE_POST_DRAIN_SECONDS;
    for ( ;; )
    {
        const double left = deadline - rocketnet_platform_time();
        if ( left <= 0.0 )
        {
            break;
        }

        OVERLAPPED_ENTRY entries[8];
        ULONG count = 0;
        if ( !GetQueuedCompletionStatusEx( port, entries, 8, &count, (DWORD) ( left * 1000.0 ) + 1, FALSE ) )
        {
            CHECK( GetLastError() == WAIT_TIMEOUT );
            break;
        }

        for ( ULONG i = 0; i < count; ++i )
        {
            for ( int k = 0; k < 2; ++k )
            {
                if ( entries[i].lpOverlapped == &receives[k].overlapped )
                {
                    tally->entry[k] = entries[i];
                    tally->count[k]++;
                }
            }
        }
    }
}

struct oversize_post_seen_t
{
    int result;            // the first receive call's return, posted onto the queued oversized datagram
    int error;             // its WSAGetLastError, 0 when the call returned 0
    int completions;       // completions the port then delivered for it
    unsigned long long status; // that completion's status
    int overlapped_error;  // WSAGetOverlappedResult's error for it
    int later_completions; // further completions for it after the small datagram and the second receive
};

// The kernel writes a receive record until its completion, so each cell keeps
// its two records for the life of the process.
static oversize_post_receive_t oversize_post_receives[4][2];

static void oversize_post_cell( int cell, bool message_api, bool skip_on_success, oversize_post_seen_t * seen )
{
    oversize_post_receive_t * receives = oversize_post_receives[cell];

    SOCKET server = WSASocketW( AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED );
    CHECK( server != INVALID_SOCKET );

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    CHECK( bind( server, (sockaddr *) &to, sizeof( to ) ) == 0 );
    int to_bytes = sizeof( to );
    CHECK( getsockname( server, (sockaddr *) &to, &to_bytes ) == 0 );

    HANDLE port = CreateIoCompletionPort( (HANDLE) server, NULL, 0, 1 );
    CHECK( port );
    if ( skip_on_success )
    {
        CHECK( SetFileCompletionNotificationModes( (HANDLE) server, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS ) );
    }

    LPFN_WSARECVMSG receive_message = NULL;
    if ( message_api )
    {
        GUID guid = WSAID_WSARECVMSG;
        DWORD returned = 0;
        CHECK( WSAIoctl( server, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof( guid ), &receive_message,
                         sizeof( receive_message ), &returned, NULL, NULL ) == 0 );
        CHECK( receive_message );
    }

    SOCKET client = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( client != INVALID_SOCKET );

    // a full-size UDP payload: 20 bytes longer than the receive ceiling
    static char datagram[1472];
    static_assert( sizeof( datagram ) > ROCKETNET_RECEIVE_CEILING + 1, "the datagram is longer than the receive buffer" );
    CHECK( sendto( client, datagram, (int) sizeof( datagram ), 0, (sockaddr *) &to, sizeof( to ) ) == (int) sizeof( datagram ) );
    wait_until( "the oversized datagram to queue", [&]()
                {
        u_long available = 0;
        return ioctlsocket( server, (long) FIONREAD, &available ) == 0 && available > 0; } );

    oversize_post_tally_t tally;
    seen->result = oversize_post_receive( server, receive_message, &receives[0], &seen->error );
    oversize_post_drain( port, receives, &tally );
    seen->completions = tally.count[0];

    DWORD transferred = 0;
    DWORD result_flags = 0;
    int overlapped_error = 0;
    if ( tally.count[0] > 0 && !WSAGetOverlappedResult( server, &receives[0].overlapped, &transferred, FALSE, &result_flags ) )
    {
        overlapped_error = WSAGetLastError();
    }
    seen->status = (unsigned long long) tally.entry[0].Internal;
    seen->overlapped_error = overlapped_error;

    printf( "  oversize at post: api=%s skip_on_success=%d: result=%d error=%d completions=%d status=0x%llx bytes=%lu overlapped_error=%d message_flags=0x%lx\n",
            message_api ? "WSARecvMsg" : "WSARecvFrom", skip_on_success ? 1 : 0, seen->result, seen->error, tally.count[0],
            (unsigned long long) tally.entry[0].Internal, (unsigned long) tally.entry[0].dwNumberOfBytesTransferred, overlapped_error,
            (unsigned long) receives[0].message.dwFlags );

    // a small datagram and a second receive: a first receive still in the
    // kernel would take the datagram and complete now

    CHECK( sendto( client, "good", 4, 0, (sockaddr *) &to, sizeof( to ) ) == 4 );
    wait_until( "the small datagram to queue or be taken", [&]()
                {
        u_long available = 0;
        return ( ioctlsocket( server, (long) FIONREAD, &available ) == 0 && available > 0 ) || HasOverlappedIoCompleted( &receives[0].overlapped ); } );

    int second_error = 0;
    const int second_result = oversize_post_receive( server, receive_message, &receives[1], &second_error );
    oversize_post_drain( port, receives, &tally );
    seen->later_completions = tally.count[0];

    printf( "    then 4 bytes and a second receive: result=%d error=%d completions=%d status=0x%llx bytes=%lu; further completions for the first receive=%d\n",
            second_result, second_error, tally.count[1], (unsigned long long) tally.entry[1].Internal,
            (unsigned long) tally.entry[1].dwNumberOfBytesTransferred, tally.count[0] );

    // closing aborts whatever is still posted; its completions are drained before the port closes

    closesocket( client );
    closesocket( server );
    oversize_post_drain( port, receives, &tally );
    CloseHandle( port );
}

static void test_windows_oversize_receive_at_post()
{
    printf( "  windows build %lu\n", windows_build_number() );

    oversize_post_seen_t seen[4];
    for ( int cell = 0; cell < 4; ++cell )
    {
        oversize_post_cell( cell, ( cell & 2 ) != 0, ( cell & 1 ) != 0, &seen[cell] );
    }

    // What Windows 11 does (build 26200, 2026-11-10), and what the post rule
    // of the IOCP server and the iocp client rests on: the call reports
    // WSA_IO_PENDING, never WSAEMSGSIZE, and exactly one completion follows,
    // carrying the truncation as STATUS_BUFFER_OVERFLOW, which
    // WSAGetOverlappedResult reads as WSAEMSGSIZE. So "any error but
    // WSA_IO_PENDING started no I/O" holds for an oversized datagram too, and
    // the receive is never in the kernel twice. The cells with
    // FILE_SKIP_COMPLETION_PORT_ON_SUCCESS saw the same and are printed
    // only: the transports never set it.

    for ( int cell = 0; cell < 4; cell += 2 )
    {
        CHECK( seen[cell].result == SOCKET_ERROR && seen[cell].error == WSA_IO_PENDING );
        CHECK( seen[cell].completions == 1 );
        CHECK( seen[cell].status == 0x80000005ull );
        CHECK( seen[cell].overlapped_error == WSAEMSGSIZE );
        CHECK( seen[cell].later_completions == 0 );
    }

    printf( "test_windows_oversize_receive_at_post\n" );
}

// ---------------------------------------------------------------------------
// Fault cases: each failure path of the IOCP and RIO transports, injected
// through the test-only fault seams (ROCKETNET_DEV)
// on a live loopback exchange. Each checks that every frame or slot returns,
// the counter moves, and traffic in both directions resumes after the fault.

#define FAULT_BYTES 100
static_assert( FAULT_BYTES >= ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES, "the RIO receive keeps the datagram" );

static int fault_round = 0;

static rocketnet_platform_socket_t * fault_client( rocketnet_address_t * client_address )
{
    *client_address = address( "127.0.0.1:0" );

    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 4 * 1024 * 1024, 4 * 1024 * 1024 );
    CHECK( client );

    return client;
}

static void fault_mark( uint8_t * data, int round, int index )
{
    data[0] = 0xA5;
    data[1] = (uint8_t) round;
    data[2] = (uint8_t) ( index >> 8 );
    data[3] = (uint8_t) index;
    for ( int i = 4; i < FAULT_BYTES; ++i )
    {
        data[i] = (uint8_t) ( index + i );
    }
}

// the index a marked datagram of this round carries, or -1

static int fault_index( const uint8_t * data, int bytes, int round )
{
    if ( bytes != FAULT_BYTES || data[0] != 0xA5 || data[1] != (uint8_t) round )
    {
        return -1;
    }

    const int index = ( data[2] << 8 ) | data[3];
    for ( int i = 4; i < FAULT_BYTES; ++i )
    {
        if ( data[i] != (uint8_t) ( index + i ) )
        {
            return -1;
        }
    }

    return index;
}

// distinct marked datagrams of this round the client receives, up to n

static int fault_client_receive( rocketnet_platform_socket_t * client, int round, int n )
{
    std::vector<bool> seen( (size_t) n, false );

    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        rocketnet_address_t from;
        const int bytes = rocketnet_platform_socket_receive_packet( client, &from, packet, sizeof( packet ) );
        if ( !bytes )
        {
            rocketnet_platform_sleep( 0.001 );
            continue;
        }

        const int index = fault_index( packet, bytes, round );
        if ( index >= 0 && index < n && !seen[(size_t) index] )
        {
            seen[(size_t) index] = true;
            ++count;
        }
    }

    return count;
}

static void fault_client_send( rocketnet_platform_socket_t * client, const rocketnet_address_t * to, int round, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        uint8_t data[FAULT_BYTES];
        fault_mark( data, round, i );
        rocketnet_platform_socket_send_packet( client, to, data, FAULT_BYTES );
    }
}

// ---- IOCP ----

static void iocp_stage( rocketnet_server_iocp_t * transport, const rocketnet_address_t * to, int round, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        uint64_t id;
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, to, &id );
        CHECK( frame );
        fault_mark( frame + ROCKETNET_FRAME_WIRE_OFFSET, round, i );
        rocketnet_server_iocp_finish_packet( transport, id, frame, FAULT_BYTES );
    }
}

// waits until sends completed plus sends failed reach total, then joins the
// retirement batch so every retired frame is back in the pool

static void iocp_wait_sends( rocketnet_server_iocp_t * transport, uint64_t total )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    const double deadline = rocketnet_platform_time() + 10.0;

    do
    {
        rocketnet_server_iocp_counters( transport, counters );
        if ( counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED] >= total )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    } while ( rocketnet_platform_time() < deadline );

    CHECK( counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED] == total );
    rocketnet_server_iocp_test_hold_completions( transport, true );
    rocketnet_server_iocp_test_hold_completions( transport, false );
}

static uint64_t iocp_sends_retired( rocketnet_server_iocp_t * transport )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( transport, counters );
    return counters[ROCKETNET_IOCP_SEND_COMPLETED] + counters[ROCKETNET_IOCP_SEND_FAILED];
}

static uint64_t iocp_counter( rocketnet_server_iocp_t * transport, int counter )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( transport, counters );
    return counters[counter];
}

// every frame of the pool is free: a whole generation reserves, each frame once

static void iocp_all_frames_free( rocketnet_server_iocp_t * transport, const rocketnet_address_t * to )
{
    std::set<uint8_t *> frames;
    uint64_t id;

    for ( int i = 0; i < rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ); ++i )
    {
        uint8_t * frame = rocketnet_server_iocp_start_packet( transport, to, &id );
        CHECK( frame && frames.insert( frame ).second );
        rocketnet_server_iocp_abort_packet( transport, id, frame );
    }

    CHECK( !rocketnet_server_iocp_start_packet( transport, to, &id ) );
    iocp_flush( transport );
}

// the receive depth is back to its configured count, with no send in flight

static void iocp_wait_depth( rocketnet_server_iocp_t * transport, int workers )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_server_iocp_pending( transport ) != workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER && rocketnet_platform_time() < deadline )
    {
        rocketnet_platform_sleep( 0.001 );
    }
    CHECK( rocketnet_server_iocp_pending( transport ) == workers * ROCKETNET_IOCP_RECEIVES_PER_WORKER );
}

// n datagrams each way, every one delivered: the exchange lives

static void iocp_exchange( rocketnet_server_iocp_t * transport, const rocketnet_address_t * bind,
                           rocketnet_platform_socket_t * client, const rocketnet_address_t * client_address, int n )
{
    const int round = ++fault_round & 0xFF;
    const uint64_t retired = iocp_sends_retired( transport );
    iocp_stage( transport, client_address, round, n );
    iocp_flush( transport );
    CHECK( fault_client_receive( client, round, n ) == n );
    iocp_wait_sends( transport, retired + (uint64_t) n );
    fault_client_send( client, bind, round, n );
    std::vector<bool> seen( (size_t) n, false );

    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets( transport );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index( batch->packet_data[i], batch->packet_bytes[i], round );
            if ( index >= 0 && index < n && !seen[(size_t) index] )
            {
                seen[(size_t) index] = true;
                ++count;
            }
        }

        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( count == n );
}

static void iocp_finish( rocketnet_server_iocp_t * transport, rocketnet_platform_socket_t * client )
{
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    rocketnet_platform_socket_destroy( client );
}

// Where asserts are on, finish and abort assert on a frame or generation the
// transport did not issue before they refuse it; an id out of range is refused
// before the assert. The case counts the asserts through the assert seam
// instead of letting the default handler trap.
static int iocp_refused_asserts = 0;

static void iocp_count_refused_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    iocp_refused_asserts++;
}

static void test_iocp_finish_refuses_unissued_packet_id()
{
    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    rocketnet_assert_function( iocp_count_refused_assert );
    iocp_refused_asserts = 0;

    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, ROCKETNET_IOCP_DISABLE_USO );
    CHECK( transport );

    const rocketnet_address_t to = address( "127.0.0.1:40000" );
    uint64_t first_id, second_id;
    uint8_t * first_frame = rocketnet_server_iocp_start_packet( transport, &to, &first_id );
    uint8_t * second_frame = rocketnet_server_iocp_start_packet( transport, &to, &second_id );
    CHECK( first_frame && second_frame && first_id != second_id );

    rocketnet_server_iocp_finish_packet( transport, second_id, first_frame, FAULT_BYTES );
    CHECK( iocp_refused_asserts == ROCKETNET_ASSERTS );
    CHECK( rocketnet_server_iocp_test_packet_bytes( transport, second_id ) == 0 );
    rocketnet_server_iocp_finish_packet( transport, second_id, second_frame, FAULT_BYTES );
    CHECK( rocketnet_server_iocp_test_packet_bytes( transport, second_id ) == FAULT_BYTES );
    rocketnet_server_iocp_abort_packet( transport, second_id, first_frame );
    CHECK( iocp_refused_asserts == 2 * ROCKETNET_ASSERTS );
    CHECK( rocketnet_server_iocp_test_packet_bytes( transport, second_id ) == FAULT_BYTES );
    rocketnet_server_iocp_finish_packet( transport, (uint64_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ), first_frame, FAULT_BYTES );
    rocketnet_server_iocp_abort_packet( transport, (uint64_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ), first_frame );
    CHECK( iocp_refused_asserts == 2 * ROCKETNET_ASSERTS );

    // recycle the exact physical operation, then prove its old packet handle
    // cannot finish the new generation (the ABA-shaped stale-handle case)

    rocketnet_server_iocp_abort_packet( transport, first_id, first_frame );
    rocketnet_server_iocp_abort_packet( transport, second_id, second_frame );
    iocp_flush( transport );
#if ROCKETNET_DEV
    rocketnet_server_iocp_test_queue_stats_t before, empty, after;
    rocketnet_server_iocp_test_queue_stats( transport, &before );
#endif // #if ROCKETNET_DEV
    std::vector<uint64_t> ids( (size_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    std::vector<uint8_t *> frames( (size_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );

    int reused = -1;

    for ( int i = 0; i < rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ); ++i )
    {
        frames[(size_t) i] = rocketnet_server_iocp_start_packet( transport, &to, &ids[(size_t) i] );
        CHECK( frames[(size_t) i] );
        if ( frames[(size_t) i] == first_frame )
        {
            reused = i;
        }
    }

    CHECK( reused >= 0 && ids[(size_t) reused] != first_id );
#if ROCKETNET_DEV
    rocketnet_server_iocp_test_queue_stats( transport, &empty );
    CHECK( empty.free_count == 0 );
    CHECK( empty.free_pops - before.free_pops == (uint64_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
#endif // #if ROCKETNET_DEV
    rocketnet_server_iocp_finish_packet( transport, first_id, frames[(size_t) reused], FAULT_BYTES );
    CHECK( iocp_refused_asserts == 3 * ROCKETNET_ASSERTS );
    CHECK( rocketnet_server_iocp_test_packet_bytes( transport, ids[(size_t) reused] ) == 0 );
    for ( int i = 0; i < rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ); ++i )
    {
        rocketnet_server_iocp_abort_packet( transport, ids[(size_t) i], frames[(size_t) i] );
    }
    iocp_flush( transport );
#if ROCKETNET_DEV
    rocketnet_server_iocp_test_queue_stats( transport, &after );
    CHECK( after.free_count == rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( after.free_pushes - before.free_pushes == (uint64_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( after.queue_probes - before.queue_probes <= 4 * (uint64_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( after.live_run_refs == 0 );
#endif // #if ROCKETNET_DEV
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    CHECK( iocp_refused_asserts == 3 * ROCKETNET_ASSERTS );
    rocketnet_assert_function( saved_assert );
    printf( "test_iocp_transport_finish_refuses_unissued_packet_id\n" );
}

// An out-of-range packet_bytes is refused in release builds too, as RIO
// finish_packet refuses it: the slot keeps zero bytes and SEND_FAILED counts it.
static void test_iocp_finish_refuses_bad_packet_bytes()
{
    const rocketnet_assert_function_t saved_assert = rn_assert_function;
    rocketnet_assert_function( iocp_count_refused_assert );
    iocp_refused_asserts = 0;

    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, ROCKETNET_IOCP_DISABLE_USO );
    CHECK( transport );

    const rocketnet_address_t to = address( "127.0.0.1:40000" );
    uint64_t id;
    uint8_t * frame = rocketnet_server_iocp_start_packet( transport, &to, &id );
    CHECK( frame );

    const uint64_t failed = iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED );
    const int bad_bytes[] = { 0, -1, ROCKETNET_RECEIVE_CEILING + 1 };
    for ( int i = 0; i < 3; ++i )
    {
        rocketnet_server_iocp_finish_packet( transport, id, frame, bad_bytes[i] );
        CHECK( iocp_refused_asserts == ( i + 1 ) * ROCKETNET_ASSERTS );
        CHECK( rocketnet_server_iocp_test_packet_bytes( transport, id ) == 0 );
        CHECK( iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED ) - failed == (uint64_t) ( i + 1 ) );
    }

    rocketnet_server_iocp_abort_packet( transport, id, frame );
    iocp_flush( transport );
    wait_stopped( transport );
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
    rocketnet_assert_function( saved_assert );
    printf( "test_iocp_transport_finish_refuses_bad_packet_bytes\n" );
}

static void test_iocp_stop_sends_published_burst()
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, ROCKETNET_IOCP_DISABLE_USO );
    CHECK( transport );

    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    const int round = ++fault_round & 0xFF;
    const uint64_t failed = iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED );
    rocketnet_server_iocp_test_hold_tx( transport, true );
    iocp_stage( transport, &client_address, round, 10 );
    rocketnet_server_iocp_send_packets( transport );
    CHECK( rocketnet_server_iocp_test_ready_tx_banks( transport ) == 1 );
    rocketnet_server_iocp_stop( transport );
    rocketnet_server_iocp_test_hold_tx( transport, false );
    wait_stopped( transport );

    const int received = fault_client_receive( client, round, 10 );
    const uint64_t failed_delta = iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED ) - failed;
    printf( "stop burst: received=%d/10 send_failed=%llu\n", received, (unsigned long long) failed_delta );
    CHECK( received == 10 );
    CHECK( failed_delta == 0 );
    iocp_finish( transport, client );
    printf( "test_iocp_transport_stop_sends_published_burst\n" );
}

static void test_iocp_send_completion_fails()
{
    // Per-datagram sends: one completion is one frame

    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, ROCKETNET_IOCP_DISABLE_USO );
    CHECK( transport );

    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    iocp_exchange( transport, &bind, client, &client_address, 16 );

    const int statuses[] = { WSAENOBUFS, ERROR_OPERATION_ABORTED };
    const uint64_t failed_before = iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED );

    for ( int s = 0; s < 2; ++s )
    {
        const uint64_t retired = iocp_sends_retired( transport );
        rocketnet_server_iocp_test_fail_next( transport, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION, 50, statuses[s] );
        iocp_stage( transport, &client_address, 0xEE, 50 );
        iocp_flush( transport );
        iocp_wait_sends( transport, retired + 50 );
        CHECK( rocketnet_server_iocp_test_faults_left( transport, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION ) == 0 );
        iocp_all_frames_free( transport, &client_address );
        iocp_exchange( transport, &bind, client, &client_address, 16 );
    }

    CHECK( iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED ) - failed_before == 100 );
    iocp_wait_depth( transport, 1 );
    iocp_finish( transport, client );

    // A USO run: one failed completion returns every frame of the run

    bind = address( "127.0.0.1:0" );
    transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport );
    client = fault_client( &client_address );

    if ( rocketnet_server_iocp_features( transport ) & ROCKETNET_IOCP_FEATURE_USO )
    {
        const uint64_t retired = iocp_sends_retired( transport );
        rocketnet_server_iocp_test_fail_next( transport, ROCKETNET_IOCP_TEST_FAULT_SEND_COMPLETION, 1, WSAENOBUFS );
        iocp_stage( transport, &client_address, 0xEE, 16 );
        iocp_flush( transport );
        iocp_wait_sends( transport, retired + 16 );
        CHECK( iocp_counter( transport, ROCKETNET_IOCP_USO_SENDS ) == 1 );
        CHECK( iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED ) == 16 );
        iocp_all_frames_free( transport, &client_address );
    }

    iocp_exchange( transport, &bind, client, &client_address, 16 );
    iocp_finish( transport, client );
    printf( "test_iocp_transport_send_completion_fails\n" );
}

static void test_iocp_send_post_fails()
{
    // a refused post has no completion: the frame returns at the post

    for ( int uso = 0; uso < 2; ++uso )
    {
        rocketnet_address_t bind = address( "127.0.0.1:0" );
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, uso ? 0 : ROCKETNET_IOCP_DISABLE_USO );
        CHECK( transport );

        rocketnet_address_t client_address;
        rocketnet_platform_socket_t * client = fault_client( &client_address );
        const bool runs = uso && ( rocketnet_server_iocp_features( transport ) & ROCKETNET_IOCP_FEATURE_USO );
        if ( uso && !runs )
        {
            iocp_finish( transport, client );
            continue;
        }

        // per datagram: 100 refused posts of one frame; with USO: 2 refused
        // runs of 16 equal frames

        const int posts = runs ? 2 : 100;
        const int frames = runs ? 32 : 100;
        rocketnet_server_iocp_test_fail_next( transport, ROCKETNET_IOCP_TEST_FAULT_SEND_POST, posts, WSAENOBUFS );

        for ( int r = 0; r < ( runs ? 2 : 1 ); ++r )
        {
            iocp_stage( transport, &client_address, 0xEE, runs ? 16 : frames );
            iocp_flush( transport );
        }

        iocp_wait_sends( transport, (uint64_t) frames );
        CHECK( rocketnet_server_iocp_test_faults_left( transport, ROCKETNET_IOCP_TEST_FAULT_SEND_POST ) == 0 );
        CHECK( iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED ) == (uint64_t) frames );
        CHECK( iocp_counter( transport, ROCKETNET_IOCP_SEND_COMPLETED ) == 0 );
        CHECK( rocketnet_server_iocp_pending( transport ) == ROCKETNET_IOCP_RECEIVES_PER_WORKER );
        iocp_all_frames_free( transport, &client_address );
        iocp_exchange( transport, &bind, client, &client_address, 32 );
        iocp_finish( transport, client );
    }

    printf( "test_iocp_transport_send_post_fails\n" );
}

static void test_iocp_receive_completion_fails()
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, ROCKETNET_IOCP_DISABLE_URO );
    CHECK( transport );

    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    iocp_exchange( transport, &bind, client, &client_address, 16 );

    // 100 failed receive completions: each is counted dropped and its
    // operation is re-posted, so the depth holds

    const int statuses[] = { WSAENOBUFS, ERROR_OPERATION_ABORTED };

    for ( int s = 0; s < 2; ++s )
    {
        const uint64_t dropped = iocp_counter( transport, ROCKETNET_IOCP_RECEIVE_DROPPED );
        rocketnet_server_iocp_test_fail_next( transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION, 50, statuses[s] );
        fault_client_send( client, &bind, 0xEE, 50 );

        const double deadline = rocketnet_platform_time() + 5.0;
        while ( iocp_counter( transport, ROCKETNET_IOCP_RECEIVE_DROPPED ) - dropped < 50 && rocketnet_platform_time() < deadline )
        {
            rocketnet_platform_sleep( 0.001 );
        }
        CHECK( iocp_counter( transport, ROCKETNET_IOCP_RECEIVE_DROPPED ) - dropped == 50 );
        CHECK( rocketnet_server_iocp_test_faults_left( transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_COMPLETION ) == 0 );
        iocp_wait_depth( transport, 1 );
        iocp_exchange( transport, &bind, client, &client_address, 16 );
    }

    // refused re-posts: each completion delivers its datagram, its re-post is
    // refused and counted, and the retry queue re-posts it once the refusals
    // end: the depth recovers to its configured count

    const uint64_t repost_failed = iocp_counter( transport, ROCKETNET_IOCP_RECEIVE_REPOST_FAILED );
#if ROCKETNET_DEV
    rocketnet_server_iocp_test_queue_stats_t retry_before, retry_after;
    rocketnet_server_iocp_test_queue_stats( transport, &retry_before );
#endif // #if ROCKETNET_DEV
    rocketnet_server_iocp_test_fail_next( transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST, 300, WSAENOBUFS );

    const int round = ++fault_round & 0xFF;
    fault_client_send( client, &bind, round, 64 );
    std::vector<bool> seen( 64, false );

    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( ( count < 64 || rocketnet_server_iocp_test_faults_left( transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST ) > 0 ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_server_iocp_receive_packets( transport );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_iocp_process_packets( transport );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index( batch->packet_data[i], batch->packet_bytes[i], round );
            if ( index >= 0 && index < 64 && !seen[(size_t) index] )
            {
                seen[(size_t) index] = true;
                ++count;
            }
        }

        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( count == 64 );
    CHECK( rocketnet_server_iocp_test_faults_left( transport, ROCKETNET_IOCP_TEST_FAULT_RECEIVE_POST ) == 0 );
    iocp_wait_depth( transport, 1 );
    CHECK( iocp_counter( transport, ROCKETNET_IOCP_RECEIVE_REPOST_FAILED ) - repost_failed == 300 );
#if ROCKETNET_DEV
    rocketnet_server_iocp_test_queue_stats( transport, &retry_after );
    CHECK( retry_after.retry_count == 0 );
    CHECK( retry_after.retry_pushes - retry_before.retry_pushes == 300 );
    CHECK( retry_after.retry_pops - retry_before.retry_pops == 300 );
    CHECK( retry_after.queue_probes - retry_before.queue_probes <= 1200 );
#endif // #if ROCKETNET_DEV
    iocp_exchange( transport, &bind, client, &client_address, 64 );
    iocp_finish( transport, client );
    printf( "test_iocp_transport_receive_completion_fails\n" );
}

static void test_iocp_send_queue_full_recovers()
{
    rocketnet_address_t bind = address( "127.0.0.1:0" );
    rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
    CHECK( transport );

    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    iocp_exchange( transport, &bind, client, &client_address, 16 );

    // Held completions: the whole queue is staged and posted, and nothing
    // can retire it. start_packet refuses until the completions are read.

    const uint64_t retired = iocp_sends_retired( transport );
    rocketnet_server_iocp_test_hold_completions( transport, true );
    iocp_stage( transport, &client_address, 0xEE, rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );

    uint64_t id;
    CHECK( !rocketnet_server_iocp_start_packet( transport, &client_address, &id ) );
    iocp_flush( transport );
    CHECK( !rocketnet_server_iocp_start_packet( transport, &client_address, &id ) );
    wait_until( "held IOCP send submission", [&]()
                {
        rocketnet_server_iocp_send_packets(transport);
        return rocketnet_server_iocp_pending(transport) > ROCKETNET_IOCP_RECEIVES_PER_WORKER ||
               iocp_counter(transport, ROCKETNET_IOCP_SEND_FAILED) > 0; } );
    CHECK( rocketnet_server_iocp_pending( transport ) > ROCKETNET_IOCP_RECEIVES_PER_WORKER ||
           iocp_counter( transport, ROCKETNET_IOCP_SEND_FAILED ) > 0 );
    iocp_flush( transport );
    CHECK( !rocketnet_server_iocp_start_packet( transport, &client_address, &id ) );
    rocketnet_server_iocp_test_hold_completions( transport, false );

    // Released: every send drains, every frame is free, the exchange lives

    iocp_wait_sends( transport, retired + (uint64_t) rocketnet_server_iocp_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    iocp_wait_depth( transport, 1 );
    iocp_all_frames_free( transport, &client_address );
    iocp_exchange( transport, &bind, client, &client_address, 64 );
    printf( "test_iocp_transport_send_queue_full_recovers\n" );
    iocp_finish( transport, client );
}

// A watchdog over one call: the process exits 1 naming the call when it
// runs past its bound, so a destroy that never returns is a failure, never a
// hang.
struct case_watchdog_t
{
    std::atomic<bool> done{ false };
    std::thread thread;
    case_watchdog_t( const char * name, double seconds )
    {
        thread = std::thread( [this, name, seconds]()
                              {
            const double deadline = rocketnet_platform_time() + seconds;
            while ( !done.load() )
            {
                if ( rocketnet_platform_time() > deadline )
                {
                    fprintf( stderr, "IOCP check failed: %s ran past its %.0f s watchdog (%s:%d)\n", name, seconds, __FILE__, __LINE__ );
                    fflush( stderr );
                    _Exit( 1 );
                }
                rocketnet_platform_sleep( 0.01 );
            } } );
    }
    ~case_watchdog_t()
    {
        done.store( true );
        thread.join();
    }
};

// Destroy with I/O in flight: after TX drains, completion workers cancel
// each outstanding receive by its OVERLAPPED without cancelling sends.
// Destroy joins the workers and only then closes the socket.
// 100 idle servers are created and destroyed, then 100
// more after datagrams completed, so at least one receive was re-posted by a
// worker thread rather than the destroying one. Each destroy runs under its
// own 10 s watchdog.

static void iocp_destroy_watched( rocketnet_server_iocp_t * transport, double * slowest )
{
    case_watchdog_t watchdog( "rocketnet_server_iocp_destroy", 10.0 );

    const double start = rocketnet_platform_time();
    rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );

    const double elapsed = rocketnet_platform_time() - start;
    if ( elapsed > *slowest )
    {
        *slowest = elapsed;
    }
}

static void test_iocp_destroy_never_hangs()
{
    double slowest = 0.0;

    for ( int i = 0; i < 100; ++i )
    {
        rocketnet_address_t bind = address( "127.0.0.1:0" );
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 4, ROCKETNET_DEFAULT_MAX_CLIENTS );
        CHECK( transport );
        iocp_destroy_watched( transport, &slowest );
    }

    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );

    for ( int i = 0; i < 100; ++i )
    {
        rocketnet_address_t bind = address( "127.0.0.1:0" );
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 4, ROCKETNET_DEFAULT_MAX_CLIENTS );
        CHECK( transport );
        fault_client_send( client, &bind, 0xEE, 4 * ROCKETNET_IOCP_RECEIVES_PER_WORKER );

        const double deadline = rocketnet_platform_time() + 5.0;
        while ( iocp_counter( transport, ROCKETNET_IOCP_RECEIVE_COMPLETED ) == 0 && rocketnet_platform_time() < deadline )
        {
            rocketnet_platform_sleep( 0.001 );
        }
        CHECK( iocp_counter( transport, ROCKETNET_IOCP_RECEIVE_COMPLETED ) > 0 );
        iocp_destroy_watched( transport, &slowest );
    }

    rocketnet_platform_socket_destroy( client );
    printf( "test_iocp_transport_destroy_never_hangs\n" );
}

// ---- RIO ----

static uint64_t rio_counter( rocketnet_server_rio_t * rio, int counter )
{
    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( rio, counters );
    return counters[counter];
}

// The I/O worker returns a posted phase idle only after its completions are
// retired, so the fill threads may be refused until a later flush opens the
// other phase. Every reservation made so far is finished, so each retry may
// flush.

static uint8_t * rio_reserve( rocketnet_server_rio_t * rio, const rocketnet_address_t * to, uint64_t * id )
{
    uint8_t * frame = rocketnet_server_rio_start_packet( rio, to, id );
    if ( frame )
    {
        return frame;
    }
    wait_until( "RIO fill phase open", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        frame = rocketnet_server_rio_start_packet(rio, to, id);
        return frame != NULL; } );

    return frame;
}

static void rio_stage( rocketnet_server_rio_t * rio, const rocketnet_address_t * to, int round, int n )
{
    for ( int i = 0; i < n; ++i )
    {
        uint64_t id;
        uint8_t * frame = rio_reserve( rio, to, &id );
        CHECK( frame );
        fault_mark( frame + ROCKETNET_FRAME_WIRE_OFFSET, round, i );
        rocketnet_server_rio_finish_packet( rio, id, frame, FAULT_BYTES );
    }
}

// flushes until the worker has taken every handed phase, no send is
// outstanding and every receive slice is posted

static void rio_settle( rocketnet_server_rio_t * rio )
{
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( ( rocketnet_server_rio_pending( rio ) != rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) || rocketnet_server_rio_test_retry_receives( rio ) != 0 ||
              !rocketnet_server_rio_test_send_idle( rio ) ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_send_packets( rio );
        rocketnet_server_rio_receive_packets( rio );
        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( rocketnet_server_rio_pending( rio ) == rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( rocketnet_server_rio_test_retry_receives( rio ) == 0 );
    CHECK( rocketnet_server_rio_test_send_idle( rio ) );
}

// flushes until everything staged is handed, posted and retired

static void rio_flush( rocketnet_server_rio_t * rio )
{
    wait_until( "RIO sends handed and retired", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        return rocketnet_server_rio_test_send_idle(rio); } );
}

// every slot of every phase reserves: no phase is held by a send that never
// retires

static void rio_all_slots_free( rocketnet_server_rio_t * rio, const rocketnet_address_t * to )
{
    for ( int cycle = 0; cycle < 3; ++cycle )
    {
        uint64_t id;

        for ( int i = 0; i < rocketnet_server_rio_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ); ++i )
        {
            uint8_t * frame = i == 0 ? rio_reserve( rio, to, &id ) : rocketnet_server_rio_start_packet( rio, to, &id );
            CHECK( frame );
            rocketnet_server_rio_abort_packet( rio, id, frame );
        }

        CHECK( !rocketnet_server_rio_start_packet( rio, to, &id ) );
        rocketnet_server_rio_send_packets( rio );
    }
}

static void rio_exchange( rocketnet_server_rio_t * rio, const rocketnet_address_t * bind,
                          rocketnet_platform_socket_t * client, const rocketnet_address_t * client_address, int n )
{
    const int round = ++fault_round & 0xFF;
    rio_stage( rio, client_address, round, n );
    rio_flush( rio );
    CHECK( fault_client_receive( client, round, n ) == n );
    fault_client_send( client, bind, round, n );
    std::vector<bool> seen( (size_t) n, false );

    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets( rio );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_rio_process_packets( rio );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index( batch->packet_data[i], batch->packet_bytes[i], round );
            if ( index >= 0 && index < n && !seen[(size_t) index] )
            {
                seen[(size_t) index] = true;
                ++count;
            }
        }

        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( count == n );
    rio_settle( rio );
}

// One lane, asked for explicitly: these cases drive one lane's worker through
// its test seams, which act on lane 0, so ROCKETNET_TEST_RIO_LANES leaves
// them alone; the lane cases below open lanes themselves.
static rocketnet_server_rio_t * rio_open( rocketnet_address_t * bind )
{
    *bind = address( "127.0.0.1:0" );

    rocketnet_server_rio_options_t options;
    options.lanes = 1;
    options.packet_filter = NULL;
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, bind, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, NULL, &options );
    CHECK( rio && bind->port );
    CHECK( rocketnet_server_rio_lanes( rio ) == 1 );
    return rio;
}

static void rio_finish( rocketnet_server_rio_t * rio, rocketnet_platform_socket_t * client )
{
    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
    rocketnet_platform_socket_destroy( client );
}

static void test_rio_send_post_fails()
{
    CHECK( rocketnet_server_rio_available() );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open( &bind );
    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    rio_exchange( rio, &bind, client, &client_address, 16 );

    // 100 refused RIOSendEx posts of 150: each refused slot is free at once,
    // the other 50 are sent

    const uint64_t failed = rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
    const uint64_t sent = rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT );
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST, 100, WSAENOBUFS );
    rio_stage( rio, &client_address, 0xEE, 150 );
    wait_until( "RIO refused posts", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        return rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST) == 0 &&
               rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED) - failed == 100; } );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED ) - failed == 100 );
    rio_settle( rio );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT ) - sent == 50 );
    rio_all_slots_free( rio, &client_address );
    rio_exchange( rio, &bind, client, &client_address, 32 );

    // 100 failed send completions: each retires its slot and counts

    const uint64_t failed2 = rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_SEND_COMPLETION, 100, WSAENOBUFS );
    rio_stage( rio, &client_address, 0xEE, 100 );
    rocketnet_server_rio_send_packets( rio );
    rio_settle( rio );
    CHECK( rocketnet_server_rio_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_SEND_COMPLETION ) == 0 );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED ) - failed2 == 100 );
    rio_all_slots_free( rio, &client_address );

    // refused commits leave the deferred sends for the next flush

    const int round = ++fault_round & 0xFF;
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, 3, WSAENOBUFS );
    rio_stage( rio, &client_address, round, 10 );
    rocketnet_server_rio_send_packets( rio );

    // the worker retries a refused commit on its own short wait

    wait_until( "RIO refused commits retried", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        return rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_COMMIT) == 0; } );
    CHECK( fault_client_receive( client, round, 10 ) == 10 );
    rio_settle( rio );
    rio_exchange( rio, &bind, client, &client_address, 32 );
    rio_finish( rio, client );
    printf( "test_rio_transport_send_post_fails\n" );
}

static void test_rio_request_queue_full()
{
    CHECK( rocketnet_server_rio_available() );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open( &bind );
    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    rio_exchange( rio, &bind, client, &client_address, 16 );

    // The request queue refuses with WSAENOBUFS, as a full queue does: 200
    // receive re-posts and 200 send posts. Every datagram that completed is
    // delivered, no slice is lost, no slot is held.

    const uint64_t repost_failed = rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED );
    const uint64_t failed = rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST, 200, WSAENOBUFS );
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST, 200, WSAENOBUFS );

    const int round = ++fault_round & 0xFF;
    fault_client_send( client, &bind, round, 100 );
    std::vector<bool> seen( 100, false );

    int count = 0;
    double deadline = rocketnet_platform_time() + 5.0;
    while ( ( count < 100 || rocketnet_server_rio_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST ) > 0 ) &&
            rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets( rio );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_rio_process_packets( rio );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index( batch->packet_data[i], batch->packet_bytes[i], round );
            if ( index >= 0 && index < 100 && !seen[(size_t) index] )
            {
                seen[(size_t) index] = true;
                ++count;
            }
        }

        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( count == 100 );
    CHECK( rocketnet_server_rio_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_POST ) == 0 );
    rio_stage( rio, &client_address, 0xEE, 300 );
    wait_until( "RIO refused posts", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        return rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_SEND_POST) == 0 &&
               rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_FAILED) - failed == 200; } );
    rio_settle( rio );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_REPOST_FAILED ) - repost_failed == 200 );
    rio_all_slots_free( rio, &client_address );

    // failed receive completions: counted, and the slice is re-posted

    const uint64_t receive_failed = rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED );
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_RECEIVE_COMPLETION, 50, WSAENOBUFS );
    fault_client_send( client, &bind, 0xEE, 50 );
    deadline = rocketnet_platform_time() + 5.0;
    while ( rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED ) - receive_failed < 50 && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets( rio );
        rocketnet_platform_sleep( 0.001 );
    }

    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_FAILED ) - receive_failed == 50 );
    rio_settle( rio );
    rio_exchange( rio, &bind, client, &client_address, 64 );
    rio_finish( rio, client );
    printf( "test_rio_transport_request_queue_full\n" );
}

static void test_rio_completion_queue_full()
{
#if defined( _WIN32 )
    // Keep the CQ full through a guard beyond two banks. The old unbounded
    // loop reaches the corrupt-CQ sentinel; bounded receive returns at one
    // bank and leaves the still-full queue for the next update.

    uint64_t seam_counters[2 * ROCKETNET_RIO_NUM_COUNTERS] = {};
    int seam_packets[2] = {};
    int seam_reposts = 0;
    int seam_pending = 0;
    int seam_guard_hit = 0;
    CHECK( rocketnet_server_rio_test_receive_overflow( seam_counters, seam_packets, &seam_reposts, &seam_pending, &seam_guard_hit ) );
    CHECK( seam_guard_hit == 0 );
    CHECK( seam_packets[0] == rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( seam_packets[1] == rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( seam_counters[ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] == (uint64_t) rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( seam_counters[ROCKETNET_RIO_NUM_COUNTERS + ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED] == 2 * (uint64_t) rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( seam_reposts == 2 * rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( seam_pending == rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
#endif // #if defined( _WIN32 )
    CHECK( rocketnet_server_rio_available() );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open( &bind );
    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    rio_exchange( rio, &bind, client, &client_address, 16 );

    // Unpolled by main, the I/O worker keeps dequeuing: every slice
    // completes, the worker copies each datagram into a bank and re-posts the
    // slice, and when every bank is full it stops dequeuing (completions wait
    // in the queue) rather than dropping. A burst past the slices with main
    // idle, then polls until every datagram the worker received has been
    // exposed: nothing is dropped as overflow, each exposure fits the view.

    const uint64_t overflow = rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW );
    const uint64_t received = rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED );
    fault_client_send( client, &bind, 0xEE, rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) + 256 );
    rocketnet_platform_sleep( 0.1 );

    uint64_t exposed = 0;
    wait_until( "RIO burst exposed", [&]()
                {
        rocketnet_server_rio_receive_packets(rio);
        const int n = rocketnet_server_rio_process_packets(rio)->num_packets;
        CHECK(n <= rocketnet_server_receive_packets_per_update( ROCKETNET_DEFAULT_MAX_CLIENTS ));
        exposed += (uint64_t) n;
        return n == 0 && exposed > 0 && exposed == rio_counter(rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED) - received; } );
    CHECK( exposed >= (uint64_t) rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS ) );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_OVERFLOW ) == overflow );
    rio_settle( rio );
    rio_exchange( rio, &bind, client, &client_address, 64 );
    rio_finish( rio, client );
    printf( "test_rio_transport_completion_queue_full\n" );
}

// Sends a refused RIO_MSG_COMMIT_ONLY left deferred at destroy are
// committed before rocketnet_server_rio_destroy closes the socket: the
// destroy drains well inside a second and leaks no region. The close may
// abort committed sends, so delivery is printed, not checked.

static void test_rio_destroy_commits_deferred_sends()
{
    CHECK( rocketnet_server_rio_available() );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open( &bind );
    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    rio_exchange( rio, &bind, client, &client_address, 16 );

    const int round = ++fault_round & 0xFF;
    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, 1, WSAENOBUFS );
    rio_stage( rio, &client_address, round, 10 );
    rocketnet_server_rio_send_packets( rio );
    wait_until( "RIO commit refused once", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        return rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_COMMIT) == 0; } );

    const double start = rocketnet_platform_time();
    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );

    const double elapsed = rocketnet_platform_time() - start;
    CHECK( elapsed < 1.0 );
    fault_client_receive( client, round, 10 );
    rocketnet_platform_socket_destroy( client );
    printf( "test_rio_transport_destroy_commits_deferred_sends\n" );
}

// Stop sends the handed burst and also seals a finished fill phase.

static void test_rio_transport_stop_sends_handed_burst()
{
    CHECK( rocketnet_server_rio_available() );

    for ( int handed = 1; handed >= 0; --handed )
    {
        rocketnet_address_t bind;
        rocketnet_server_rio_t * rio = rio_open( &bind );
        rocketnet_address_t client_address;
        rocketnet_platform_socket_t * client = fault_client( &client_address );
        rocketnet_server_rio_test_hold_send( rio, true );

        const int round = ++fault_round & 0xFF;
        const uint64_t failed = rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
        const uint64_t sent = rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT );
        rio_stage( rio, &client_address, round, 10 );
        if ( handed )
        {
            wait_until( "RIO burst handed", [&]()
                        {
                rocketnet_server_rio_send_packets(rio);
                return rocketnet_server_rio_test_handed_phases(rio) == 1; } );
        }
        rocketnet_server_rio_stop( rio );
        rocketnet_server_rio_test_hold_send( rio, false );
        wait_until( "RIO stopped after handed sends", [&]()
                    { return rocketnet_server_rio_stopped( rio ); } );

        const int received = fault_client_receive( client, round, 10 );
        printf( "RIO stop burst: received=%d sent=%llu failed=%llu\n", received,
                (unsigned long long) ( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT ) - sent ),
                (unsigned long long) ( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED ) - failed ) );
        CHECK( received == 10 );
        CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED ) == failed );
        CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT ) - sent == 10 );
        CHECK( rocketnet_server_rio_pending( rio ) == 0 );
        rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
        rocketnet_platform_socket_destroy( client );
    }

    printf( "test_rio_transport_stop_sends_handed_burst\n" );
}

// A stop-time commit refusal must leave the request queue usable. Stop
// returns at once; the I/O worker retries the commit on each stop pass and
// closes the socket and drains only after a commit succeeds, so stopped stays
// false until then and the registered storage is never retired early.

static void test_rio_stop_retries_refused_deferred_commit()
{
    CHECK( rocketnet_server_rio_available() );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open( &bind );
    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );

    rocketnet_server_rio_test_fail_next( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT, 50, WSAENOBUFS );
    rio_stage( rio, &client_address, ++fault_round & 0xFF, 10 );
    rocketnet_server_rio_send_packets( rio );
    wait_until( "RIO deferred sends posted", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        return rocketnet_server_rio_test_faults_left(rio, ROCKETNET_RIO_TEST_FAULT_COMMIT) < 50; } );
    rocketnet_server_rio_stop( rio );
    wait_until( "RIO stopped after the commit", [&]()
                { return rocketnet_server_rio_stopped( rio ); } );
    CHECK( rocketnet_server_rio_test_faults_left( rio, ROCKETNET_RIO_TEST_FAULT_COMMIT ) == 0 );
    CHECK( rocketnet_server_rio_pending( rio ) == 0 );

    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
    rocketnet_platform_socket_destroy( client );
    printf( "test_rio_transport_stop_retries_refused_deferred_commit\n" );
}

// ---------------------------------------------------------------------------
// Windows socket rows: the bound port is the socket's alone, and a refused
// send is counted on the socket and logged with the Winsock error.

// a raw socket that asks to share the port with SO_REUSEADDR, as another
// local process would, on the address given; true when Windows lets it bind

static bool port_thief_binds( const rocketnet_address_t * owner, SOCKET * thief )
{
    *thief = socket( owner->type == ROCKETNET_ADDRESS_IPV6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( *thief != INVALID_SOCKET );

    BOOL reuse = TRUE;
    CHECK( setsockopt( *thief, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof( reuse ) ) == 0 );

    u_long non_blocking = 1;
    CHECK( ioctlsocket( *thief, (long) FIONBIO, &non_blocking ) == 0 );

    sockaddr_storage storage;
    memset( &storage, 0, sizeof( storage ) );

    int bytes;

    if ( owner->type == ROCKETNET_ADDRESS_IPV6 )
    {
        sockaddr_in6 * a = (sockaddr_in6 *) &storage;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons( owner->port );

        for ( int i = 0; i < 8; ++i )
        {
            a->sin6_addr.s6_addr[i * 2] = (uint8_t) ( owner->data.ipv6[i] >> 8 );
            a->sin6_addr.s6_addr[i * 2 + 1] = (uint8_t) owner->data.ipv6[i];
        }

        bytes = sizeof( sockaddr_in6 );
    }
    else
    {
        sockaddr_in * a = (sockaddr_in *) &storage;
        a->sin_family = AF_INET;
        a->sin_port = htons( owner->port );
        memcpy( &a->sin_addr, owner->data.ipv4, 4 );
        bytes = sizeof( sockaddr_in );
    }

    return bind( *thief, (sockaddr *) &storage, bytes ) == 0;
}

// datagrams a client sends to the owner's port that the thief receives instead

static int port_thief_steals( const rocketnet_address_t * owner, SOCKET thief )
{
    rocketnet_address_t client_address = address( owner->type == ROCKETNET_ADDRESS_IPV6 ? "[::1]:0" : "127.0.0.1:0" );
    rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address,
                                                                             ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024 );
    CHECK( client );

    uint8_t data[FAULT_BYTES];

    for ( int i = 0; i < 8; ++i )
    {
        fault_mark( data, 0x5A, i );
        rocketnet_platform_socket_send_packet( client, owner, data, FAULT_BYTES );
    }

    int stolen = 0;
    const double deadline = rocketnet_platform_time() + 1.0;
    while ( stolen < 8 && rocketnet_platform_time() < deadline )
    {
        char packet[ROCKETNET_RECEIVE_CEILING];
        if ( recv( thief, packet, sizeof( packet ), 0 ) > 0 )
        {
            ++stolen;
        }
        else
        {
            rocketnet_platform_sleep( 0.001 );
        }
    }

    rocketnet_platform_socket_destroy( client );

    return stolen;
}

// a server bound to the wildcard address owns its port: a second socket
// that binds the more specific loopback address on that port with
// SO_REUSEADDR would take every datagram sent there, and is refused.
// Portable, IOCP and RIO; IPv4 and dual-stack IPv6.

static void test_windows_port_exclusive()
{
    const char * binds[] = { "0.0.0.0:0", "[::]:0" };
    const char * targets[] = { "127.0.0.1:0", "[::1]:0" };

    for ( int b = 0; b < 2; ++b )
    {
        for ( int d = 0; d < 3; ++d )
        {
            rocketnet_address_t owner = address( binds[b] );
            rocketnet_platform_socket_t * portable = NULL;
            rocketnet_server_iocp_t * iocp = NULL;
            rocketnet_server_rio_t * rio = NULL;

            if ( d == 0 )
            {
                portable = rocketnet_platform_socket_create( NULL, &owner, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024 );
                CHECK( portable );
            }

            if ( d == 1 )
            {
                iocp = rocketnet_server_iocp_create( NULL, &owner, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
                CHECK( iocp );
            }

            if ( d == 2 )
            {
                rio = rocketnet_server_rio_create( NULL, &owner, ROCKETNET_DEFAULT_MAX_CLIENTS );
                CHECK( rio );
            }

            CHECK( owner.port != 0 );

            rocketnet_address_t target = address( targets[b] );
            target.port = owner.port;

            SOCKET thief;
            const bool bound = port_thief_binds( &target, &thief );
            const int stolen = bound ? port_thief_steals( &target, thief ) : 0;
            (void) stolen;
            closesocket( thief );
            if ( portable )
            {
                rocketnet_platform_socket_destroy( portable );
            }

            if ( iocp )
            {
                wait_stopped( iocp );
                rn_native_backend_stop_and_destroy( iocp, rocketnet_server_iocp_stop, rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
            }

            if ( rio )
            {
                rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
            }

            CHECK( !bound );
        }
    }

    printf( "test_windows_port_exclusive\n" );
}

static std::atomic<int> send_failed_logs{ 0 };

static void send_failed_log( void *, rocketnet_log_level_t, const char * message, size_t )
{
    // WSAEMSGSIZE is 10040: the Winsock error, never errno's text

    if ( strstr( message, "sendto (" ) && strstr( message, "WSA error 10040" ) )
    {
        send_failed_logs.fetch_add( 1 );
    }
}

// a sendto that answers SOCKET_ERROR (an oversize datagram: WSAEMSGSIZE) is
// counted on the socket and logged with WSAGetLastError(); a good send is not
// counted. An IPv4 socket and an IPv6 socket: both sendto arms.

// Both Windows server transports size every client-scaled array from the
// configured max_clients at create: the plan for a server grows with its
// max_clients alone (the build's ROCKETNET_MAX_CLIENTS is only the bound),
// the transport holds the send queue that max_clients sets, and the
// allocator bytes create holds stay within the plan.
static void test_windows_transports_sized_by_max_clients()
{
    const int large_clients = ROCKETNET_MAX_CLIENTS < 1000 ? ROCKETNET_MAX_CLIENTS : 1000;
    const uint64_t iocp_small = rocketnet_server_iocp_allocation_bytes( 1, 4 );
    const uint64_t iocp_large = rocketnet_server_iocp_allocation_bytes( 1, large_clients );
    const uint64_t rio_small = rocketnet_server_rio_allocation_bytes( 4 );
    const uint64_t rio_large = rocketnet_server_rio_allocation_bytes( large_clients );
    const uint64_t region_large = rocketnet_server_rio_registered_region_bytes( large_clients );
    printf( "test_windows_transports_sized_by_max_clients iocp_plan_%d=%llu rio_plan_%d=%llu rio_region_%d=%llu iocp_plan_4=%llu rio_plan_4=%llu\n",
            large_clients, (unsigned long long) iocp_large, large_clients, (unsigned long long) rio_large, large_clients,
            (unsigned long long) region_large, (unsigned long long) iocp_small, (unsigned long long) rio_small );
    CHECK( iocp_small > 0 && ( large_clients > ROCKETNET_SERVER_MIN_SIZED_CLIENTS ? iocp_small < iocp_large : iocp_small == iocp_large ) );
    CHECK( rio_small > 0 && ( large_clients > ROCKETNET_SERVER_MIN_SIZED_CLIENTS ? rio_small < rio_large : rio_small == rio_large ) && region_large > 0 );
    CHECK( rocketnet_server_iocp_allocation_bytes( 1, ROCKETNET_MAX_CLIENTS ) >= iocp_large );
    CHECK( rocketnet_server_iocp_allocation_bytes( 1, 0 ) == 0 && rocketnet_server_iocp_allocation_bytes( 1, ROCKETNET_MAX_CLIENTS + 1 ) == 0 );
    CHECK( rocketnet_server_rio_allocation_bytes( 0 ) == 0 && rocketnet_server_rio_allocation_bytes( ROCKETNET_MAX_CLIENTS + 1 ) == 0 );
    CHECK( rocketnet_server_iocp_send_queue_size( 4 ) == 1024 &&
           rocketnet_server_iocp_send_queue_size( large_clients ) == rocketnet_server_send_staging_packets( large_clients ) );
    CHECK( rocketnet_server_rio_receives( large_clients ) == 2 * rocketnet_server_send_staging_packets( large_clients ) );
    if ( large_clients == 1000 )
    {
        CHECK( rocketnet_server_iocp_send_queue_size( 1000 ) == 2048 );
        CHECK( rocketnet_server_rio_receives( 1000 ) == 4096 );
    }

    rocketnet_address_t bind = address( "127.0.0.1:0" );
    CHECK( !rocketnet_server_iocp_create( NULL, &bind, 1, 0 ) );
    CHECK( !rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_MAX_CLIENTS + 1 ) );

    for ( int max_clients : { 4, large_clients } )
    {
        const int64_t before = live_bytes.load();
        bind = address( "127.0.0.1:0" );
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, max_clients );
        CHECK( transport );
        const int64_t held = live_bytes.load() - before;
        rocketnet_server_iocp_test_queue_stats_t stats;
        rocketnet_server_iocp_test_queue_stats( transport, &stats );
        CHECK( stats.free_count == rocketnet_server_iocp_send_queue_size( max_clients ) );
        CHECK( rocketnet_server_iocp_send_room( transport ) == rocketnet_server_iocp_send_queue_size( max_clients ) );
        printf( "test_windows_transports_sized_by_max_clients iocp max_clients=%d held=%lld plan=%llu\n", max_clients, (long long) held,
                (unsigned long long) rocketnet_server_iocp_allocation_bytes( 1, max_clients ) );
        CHECK( held > 0 && (uint64_t) held <= rocketnet_server_iocp_allocation_bytes( 1, max_clients ) );
        wait_stopped( transport );
        rocketnet_server_iocp_destroy( transport );

        // destroy gives back every byte create took (VERIFY-4 W10: the unit
        // tier's RIO case counts only the fixture's free)
        CHECK( live_bytes.load() == before );
    }

    CHECK( rocketnet_server_rio_available() );
    for ( int max_clients : { 4, large_clients } )
    {
        const int64_t before = live_bytes.load();
        bind = address( "127.0.0.1:0" );
        rocketnet_server_rio_t * transport = rocketnet_server_rio_create( NULL, &bind, max_clients );
        CHECK( transport );
        const int64_t held = live_bytes.load() - before;
        CHECK( rocketnet_server_rio_send_room( transport ) == rocketnet_server_rio_send_queue_size( max_clients ) );
        // the lanes the transport made (ROCKETNET_TEST_RIO_LANES may ask for
        // more than one): each lane holds its own block and region
        const int lanes = rocketnet_server_rio_lanes( transport );
        char test_lanes[16] = "";
        const int asked = GetEnvironmentVariableA( "ROCKETNET_TEST_RIO_LANES", test_lanes, (DWORD) sizeof( test_lanes ) ) > 0 ? atoi( test_lanes ) : 0;
        CHECK( lanes == ( asked >= 2 && asked <= ROCKETNET_RIO_MAX_LANES ? asked : 1 ) );
        const uint64_t plan = rocketnet_server_rio_allocation_bytes( max_clients, lanes ) +
                              (uint64_t) lanes * rocketnet_memory_allocations( { rocketnet_server_rio_registered_region_bytes( max_clients ) } );
        printf( "test_windows_transports_sized_by_max_clients rio max_clients=%d lanes=%d held=%lld plan=%llu\n", max_clients, lanes,
                (long long) held, (unsigned long long) plan );
        CHECK( held > 0 && (uint64_t) held <= plan );
        rn_native_backend_stop_and_destroy( transport, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
        CHECK( live_bytes.load() == before );
    }
}

static void test_windows_send_failures_counted()
{
    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };
    const char * clients[] = { "127.0.0.1:0", "[::1]:0" };
    std::vector<uint8_t> oversize( 70000, 0x5A );

    for ( int c = 0; c < 2; ++c )
    {
        rocketnet_address_t bind = address( binds[c] );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024 );
        CHECK( socket );

        rocketnet_address_t client_address = address( clients[c] );
        rocketnet_platform_socket_t * client = rocketnet_platform_socket_create( NULL, &client_address, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, 256 * 1024, 256 * 1024 );
        CHECK( client );
        CHECK( rocketnet_platform_socket_send_failures( socket ) == 0 );

        uint8_t data[FAULT_BYTES];
        fault_mark( data, 0x5B, 0 );
        rocketnet_platform_socket_send_packet( socket, &client_address, data, FAULT_BYTES );
        CHECK( rocketnet_platform_socket_send_failures( socket ) == 0 );
        send_failed_logs.store( 0 );
        rocketnet_log_function( send_failed_log, NULL );
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_DEBUG );
        for ( int i = 0; i < 3; ++i )
        {
            rocketnet_platform_socket_send_packet( socket, &client_address, oversize.data(), (int) oversize.size() );
        }
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
        rocketnet_log_function( NULL, NULL );
        CHECK( rocketnet_platform_socket_send_failures( socket ) == 3 );
        CHECK( send_failed_logs.load() == 3 );
        rocketnet_platform_socket_send_packet( socket, &client_address, data, FAULT_BYTES );
        CHECK( rocketnet_platform_socket_send_failures( socket ) == 3 );
        rocketnet_platform_socket_destroy( client );
        rocketnet_platform_socket_destroy( socket );
    }

    printf( "test_windows_send_failures_counted\n" );
}

static void test_iocp_transport_socket_sets_dont_fragment()
{
    const int baseline = live_allocations.load();
    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };
    bool enabled_for_both = true;

    for ( int family = 0; family < 2; ++family )
    {
        rocketnet_address_t bind = address( binds[family] );
        rocketnet_server_iocp_t * transport = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
        CHECK( transport && bind.port != 0 );

        const SOCKET socket = (SOCKET) rocketnet_server_iocp_test_socket( transport );
        int ipv4_enabled = 0;
        int bytes = (int) sizeof( ipv4_enabled );
        CHECK( getsockopt( socket, IPPROTO_IP, IP_DONTFRAGMENT, (char *) &ipv4_enabled, &bytes ) == 0 );
        CHECK( bytes == (int) sizeof( ipv4_enabled ) );
        int ipv6_enabled = 1;
        if ( family == 1 )
        {
            ipv6_enabled = 0;
            bytes = (int) sizeof( ipv6_enabled );
            CHECK( getsockopt( socket, IPPROTO_IPV6, IPV6_DONTFRAG, (char *) &ipv6_enabled, &bytes ) == 0 );
            CHECK( bytes == (int) sizeof( ipv6_enabled ) );
        }
        printf( "test_iocp_transport_socket_sets_dont_fragment %s ipv4=%d ipv6=%d\n",
                binds[family], ipv4_enabled, ipv6_enabled );
        enabled_for_both = enabled_for_both && ipv4_enabled == 1 && ipv6_enabled == 1;
        rn_native_backend_stop_and_destroy( transport, rocketnet_server_iocp_stop,
                                            rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );
        CHECK( live_allocations.load() == baseline );
    }

    CHECK( enabled_for_both );
}

static void test_rio_transport_socket_sets_dont_fragment()
{
    const int baseline = live_allocations.load();
    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };
    bool enabled_for_both = true;

    for ( int family = 0; family < 2; ++family )
    {
        rocketnet_address_t bind = address( binds[family] );
        rocketnet_server_rio_t * transport = rocketnet_server_rio_create( NULL, &bind, ROCKETNET_DEFAULT_MAX_CLIENTS );
        CHECK( transport && bind.port != 0 );

        const SOCKET socket = (SOCKET) rocketnet_server_rio_test_socket( transport );
        int ipv4_enabled = 0;
        int bytes = (int) sizeof( ipv4_enabled );
        CHECK( getsockopt( socket, IPPROTO_IP, IP_DONTFRAGMENT, (char *) &ipv4_enabled, &bytes ) == 0 );
        CHECK( bytes == (int) sizeof( ipv4_enabled ) );
        int ipv6_enabled = 1;
        if ( family == 1 )
        {
            ipv6_enabled = 0;
            bytes = (int) sizeof( ipv6_enabled );
            CHECK( getsockopt( socket, IPPROTO_IPV6, IPV6_DONTFRAG, (char *) &ipv6_enabled, &bytes ) == 0 );
            CHECK( bytes == (int) sizeof( ipv6_enabled ) );
        }
        printf( "test_rio_transport_socket_sets_dont_fragment %s ipv4=%d ipv6=%d\n",
                binds[family], ipv4_enabled, ipv6_enabled );
        enabled_for_both = enabled_for_both && ipv4_enabled == 1 && ipv6_enabled == 1;
        rn_native_backend_stop_and_destroy( transport, rocketnet_server_rio_stop,
                                            rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
        CHECK( live_allocations.load() == baseline );
    }

    CHECK( enabled_for_both );
}

static void test_platform_socket_sets_dont_fragment()
{
    const int baseline = live_allocations.load();
    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };
    for ( int family = 0; family < 2; ++family )
    {
        int ipv4_enabled = 0;
        int ipv6_enabled = 0;
        CHECK( platform_socket_dont_fragment( binds[family], &ipv4_enabled, &ipv6_enabled ) );
        printf( "test_platform_socket_sets_dont_fragment %s ipv4=%d ipv6=%d\n", binds[family], ipv4_enabled, ipv6_enabled );
        CHECK( ipv4_enabled == 1 && ipv6_enabled == 1 );
        CHECK( live_allocations.load() == baseline );
    }
}

// every socket the Windows transports open is non-inheritable, so a child
// process spawned with handle inheritance never holds the bound port or
// stalls closesocket's cancel of the posted I/O

static bool socket_is_not_inherited( uintptr_t socket )
{
    DWORD flags = 0;
    return GetHandleInformation( (HANDLE) socket, &flags ) && ( flags & HANDLE_FLAG_INHERIT ) == 0;
}

static void test_windows_sockets_not_inherited()
{
    const int baseline = live_allocations.load();
    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };

    for ( int family = 0; family < 2; ++family )
    {
        rocketnet_address_t bind = address( binds[family] );
        rocketnet_server_iocp_t * iocp = rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS );
        CHECK( iocp );
        CHECK( socket_is_not_inherited( rocketnet_server_iocp_test_socket( iocp ) ) );
        rn_native_backend_stop_and_destroy( iocp, rocketnet_server_iocp_stop,
                                            rocketnet_server_iocp_stopped, rocketnet_server_iocp_destroy );

        bind = address( binds[family] );
        rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &bind, ROCKETNET_DEFAULT_MAX_CLIENTS );
        CHECK( rio );
        CHECK( socket_is_not_inherited( rocketnet_server_rio_test_socket( rio ) ) );
        rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop,
                                            rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );

        bind = address( binds[family] );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                 ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
        CHECK( socket );
        CHECK( socket_is_not_inherited( (uintptr_t) socket->handle ) );
        rocketnet_platform_socket_destroy( socket );

        CHECK( live_allocations.load() == baseline );
    }

    printf( "test_windows_sockets_not_inherited\n" );
}

// The platform socket is non-blocking (its one mode,
// ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING; the create sets FIONBIO). Windows
// has no getter for the mode, so a receive on the empty socket tells: a
// non-blocking socket answers WSAEWOULDBLOCK at once. A one-second receive
// timeout is set first, so a blocking socket fails WSAETIMEDOUT instead of
// hanging the case (VERIFY-4 W10: the unit tier's non-blocking check,
// tests/test_unit_platform_nits.cpp, is excluded on Windows).

static void test_platform_socket_is_non_blocking()
{
    const int baseline = live_allocations.load();
    const char * binds[] = { "127.0.0.1:0", "[::1]:0" };

    for ( int family = 0; family < 2; ++family )
    {
        rocketnet_address_t bind = address( binds[family] );
        rocketnet_platform_socket_t * socket = rocketnet_platform_socket_create( NULL, &bind, ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f,
                                                                                 ROCKETNET_SOCKET_SEND_BUFFER_SIZE, ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE );
        CHECK( socket );
        const DWORD timeout_ms = 1000;
        CHECK( setsockopt( (SOCKET) socket->handle, SOL_SOCKET, SO_RCVTIMEO, (const char *) &timeout_ms, (int) sizeof( timeout_ms ) ) == 0 );
        char byte = 0;
        const int got = recv( (SOCKET) socket->handle, &byte, 1, 0 );
        const int error = got == SOCKET_ERROR ? WSAGetLastError() : 0;
        printf( "test_platform_socket_is_non_blocking %s recv=%d error=%d\n", binds[family], got, error );
        CHECK( got == SOCKET_ERROR && error == WSAEWOULDBLOCK );
        rocketnet_platform_socket_destroy( socket );
        CHECK( live_allocations.load() == baseline );
    }
}

// A create that fails after posting receives drains them inline before it
// frees the region: closesocket cancels the posted receives and the drain
// accounts for every one. Stage 6 fails after the first receive is posted,
// stage 7 after the last. Neither leaks. The waits printed are the ones the
// drain made between passes, which is what Windows does with the cancelled
// receives: none means closesocket queued every completion before it
// returned.

static void test_rio_create_failure_drains_posted_receives()
{
    const int baseline = live_allocations.load();

    for ( int stage = 6; stage <= 7; ++stage )
    {
        rocketnet_address_t bind = address( "127.0.0.1:0" );
        const int waits_before = rocketnet_server_rio_test_create_drain_waits();
        const double start = rocketnet_platform_time();
        CHECK( !rocketnet_server_rio_create( NULL, &bind, ROCKETNET_DEFAULT_MAX_CLIENTS, stage ) );
        const double seconds = rocketnet_platform_time() - start;
        printf( "  rio create failure stage=%d: drain waits=%d, %.1f ms\n", stage,
                rocketnet_server_rio_test_create_drain_waits() - waits_before, seconds * 1000.0 );
        CHECK( live_allocations.load() == baseline );
    }

    // a lane after the first fails: the lanes already made and running stop,
    // drain and are freed, and create returns NULL with nothing left behind
    for ( int stage : { 1, 7 } )
    {
        rocketnet_address_t bind = address( "127.0.0.1:0" );
        rocketnet_server_rio_options_t options;
        options.lanes = 4;
        options.packet_filter = NULL;
        const uint64_t made = rocketnet_server_rio_test_lanes_made();
        CHECK( !rocketnet_server_rio_create( NULL, &bind, ROCKETNET_DEFAULT_MAX_CLIENTS, stage + 16 * 2, NULL, &options ) );
        CHECK( rocketnet_server_rio_test_lanes_made() - made == 2 );
        CHECK( live_allocations.load() == baseline );
    }

    printf( "test_rio_transport_create_failure_drains_posted_receives\n" );
}

// Windows refuses a thread for want of memory or commit (through the
// platform's thread start seam, which returns the Win32 error): the platform
// thread create returns NULL with OUT_OF_MEMORY for each memory and commit
// error and leaves the status untouched for any other (its callers then report
// THREAD_CREATE_FAILED), with nothing live. A transport create refused so
// returns NULL with OUT_OF_MEMORY, which ends the transport ladder, and frees
// everything it made: iocp is refused at its send worker, before any receive
// is posted; rio at its worker, after every receive is posted, so its inline
// drain runs first.

static int refused_thread_start_error = ERROR_COMMITMENT_LIMIT;

static int refused_thread_start( struct rocketnet_platform_thread_t *, void * )
{
    return refused_thread_start_error;
}

static void never_runs( rocketnet_platform_thread_t *, void * )
{
    CHECK( false );
}

static void test_windows_create_refused_for_commit_is_out_of_memory()
{
    const int baseline = live_allocations.load();
    const struct rocketnet_platform_priority_calls_t * saved = rocketnet_platform_priority_calls_current();
    struct rocketnet_platform_priority_calls_t fake = *saved;
    fake.thread_start = refused_thread_start;
    rocketnet_platform_set_priority_calls_for_testing( &fake );

    const int out_of_memory[] = { ERROR_NOT_ENOUGH_MEMORY, ERROR_OUTOFMEMORY, ERROR_COMMITMENT_LIMIT, ERROR_NO_SYSTEM_RESOURCES, WSAENOBUFS, WSA_NOT_ENOUGH_MEMORY };
    for ( int error : out_of_memory )
    {
        refused_thread_start_error = error;
        int status = ROCKETNET_STATUS_OK;
        CHECK( rocketnet_platform_thread_create( NULL, never_runs, NULL, &status ) == NULL );
        CHECK( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
        CHECK( live_allocations.load() == baseline );
    }
    const int other[] = { 1, ERROR_ACCESS_DENIED, ERROR_INVALID_PARAMETER, ERROR_MAX_THRDS_REACHED };
    for ( int error : other )
    {
        refused_thread_start_error = error;
        int status = ROCKETNET_STATUS_OK;
        CHECK( rocketnet_platform_thread_create( NULL, never_runs, NULL, &status ) == NULL );
        CHECK( status == ROCKETNET_STATUS_OK );
        CHECK( live_allocations.load() == baseline );
    }
    refused_thread_start_error = ERROR_COMMITMENT_LIMIT;

    rocketnet_address_t bind = address( "127.0.0.1:0" );
    int status = ROCKETNET_STATUS_OK;
    CHECK( !rocketnet_server_iocp_create( NULL, &bind, 1, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, 0, &status ) );
    CHECK( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
    CHECK( live_allocations.load() == baseline );

    bind = address( "127.0.0.1:0" );
    status = ROCKETNET_STATUS_OK;
    CHECK( !rocketnet_server_rio_create( NULL, &bind, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, &status ) );
    CHECK( status == ROCKETNET_STATUS_OUT_OF_MEMORY );
    CHECK( live_allocations.load() == baseline );

    rocketnet_platform_set_priority_calls_for_testing( saved );
    printf( "test_windows_create_refused_for_commit_is_out_of_memory\n" );
}

// Datagrams outside the wire's size bounds are dropped and counted once each:
// zero bytes and one byte under the minimum are too short, one byte past the
// receive ceiling (inside the slice) is too large, and one byte past the
// slice completes with WSAEMSGSIZE and is too large too. Every one also counts
// one size drop; the good datagram behind them arrives and nothing fails.

static void test_rio_receive_size_drops_counted()
{
    CHECK( rocketnet_server_rio_available() );
    static_assert( ROCKETNET_RECEIVE_CEILING + 1 < ROCKETNET_RIO_RECEIVE_SLICE_BYTES, "a too-large datagram inside the slice exists" );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open( &bind );

    SOCKET client = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( client != INVALID_SOCKET );

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons( bind.port );
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    uint64_t before[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( rio, before );

    static char data[ROCKETNET_RIO_RECEIVE_SLICE_BYTES + 1] = {};
    const int sizes[] = { 0, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES - 1, ROCKETNET_RECEIVE_CEILING + 1, ROCKETNET_RIO_RECEIVE_SLICE_BYTES + 1 };
    for ( int size : sizes )
    {
        CHECK( sendto( client, data, size, 0, (sockaddr *) &to, sizeof( to ) ) == size );
    }
    uint8_t good[FAULT_BYTES];
    const int round = ++fault_round & 0xFF;
    fault_mark( good, round, 0 );
    CHECK( sendto( client, (const char *) good, FAULT_BYTES, 0, (sockaddr *) &to, sizeof( to ) ) == FAULT_BYTES );

    int received = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets( rio );

        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_rio_process_packets( rio );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            CHECK( fault_index( batch->packet_data[i], batch->packet_bytes[i], round ) == 0 );
            ++received;
        }

        if ( received == 1 && rio_counter( rio, ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE ) - before[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE] == 4 )
        {
            break;
        }
        rocketnet_platform_sleep( 0.001 );
    }

    uint64_t after[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( rio, after );
    printf( "RIO size drops: received=%d short=%llu large=%llu size=%llu failed=%llu\n", received,
            (unsigned long long) ( after[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT] - before[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT] ),
            (unsigned long long) ( after[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE] - before[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE] ),
            (unsigned long long) ( after[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE] - before[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE] ),
            (unsigned long long) ( after[ROCKETNET_RIO_COUNTER_RECEIVE_FAILED] - before[ROCKETNET_RIO_COUNTER_RECEIVE_FAILED] ) );
    CHECK( received == 1 );
    CHECK( after[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT] - before[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT] == 2 );
    CHECK( after[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE] - before[ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE] == 2 );
    CHECK( after[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE] - before[ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE] == 4 );

    // and nothing else moved but the one datagram received
    for ( int c = 0; c < ROCKETNET_RIO_NUM_COUNTERS; ++c )
    {
        uint64_t moved = 0;
        if ( c == ROCKETNET_RIO_COUNTER_RECEIVE_TOO_SHORT || c == ROCKETNET_RIO_COUNTER_RECEIVE_TOO_LARGE )
        {
            moved = 2;
        }
        else if ( c == ROCKETNET_RIO_COUNTER_RECEIVE_DROPPED_SIZE )
        {
            moved = 4;
        }
        else if ( c == ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED )
        {
            moved = 1;
        }
        else if ( c == ROCKETNET_RIO_COUNTER_COMMITS )
        {
            continue; // receive re-posts may commit; not a size count
        }
        CHECK( after[c] - before[c] == moved );
    }

    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
    closesocket( client );
    printf( "test_rio_transport_receive_size_drops_counted\n" );
}

// A flush hands the fill phase only when the next phase is idle; otherwise it
// keeps the fill phase open and counts one wait. With the worker held, two
// flushes hand two phases and wait nothing; the third finds the next phase
// still handed and counts exactly one wait, handing nothing. Released, every
// staged send goes out.

static void test_rio_send_waits_counted()
{
    CHECK( rocketnet_server_rio_available() );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open( &bind );
    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    rocketnet_server_rio_test_hold_send( rio, true );

    const uint64_t waits = rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_WAITS );
    const uint64_t sent = rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT );
    const uint64_t failed = rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED );
    int rounds[3];

    for ( int phase = 0; phase < 2; ++phase )
    {
        rounds[phase] = ++fault_round & 0xFF;
        rio_stage( rio, &client_address, rounds[phase], 10 );
        wait_until( "RIO phase handed", [&]()
                    {
            rocketnet_server_rio_send_packets(rio);
            return rocketnet_server_rio_test_handed_phases(rio) == phase + 1; } );
        CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_WAITS ) == waits );
    }

    // a flush that loses the mutex try returns without counting, so flush
    // until the wait is counted, then stop at once

    rounds[2] = ++fault_round & 0xFF;
    rio_stage( rio, &client_address, rounds[2], 10 );
    wait_until( "RIO flush waits", [&]()
                {
        rocketnet_server_rio_send_packets(rio);
        return rio_counter(rio, ROCKETNET_RIO_COUNTER_SEND_WAITS) != waits; } );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_WAITS ) == waits + 1 );
    CHECK( rocketnet_server_rio_test_handed_phases( rio ) == 2 );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT ) == sent );

    rocketnet_server_rio_test_hold_send( rio, false );
    rio_flush( rio );
    for ( int phase = 0; phase < 3; ++phase )
    {
        CHECK( fault_client_receive( client, rounds[phase], 10 ) == 10 );
    }
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT ) - sent == 30 );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED ) == failed );

    rio_finish( rio, client );
    printf( "test_rio_transport_send_waits_counted\n" );
}

// ---- RIO lanes ----

// A loopback port nothing holds right now, for the explicit-port form.
static uint16_t rio_free_port()
{
    SOCKET s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( s != INVALID_SOCKET );
    BOOL enabled = TRUE;
    CHECK( setsockopt( s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &enabled, sizeof( enabled ) ) == 0 );
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    CHECK( bind( s, (sockaddr *) &a, sizeof( a ) ) == 0 );
    int bytes = sizeof( a );
    CHECK( getsockname( s, (sockaddr *) &a, &bytes ) == 0 );
    closesocket( s );
    return ntohs( a.sin_port );
}

static rocketnet_server_rio_t * rio_open_lanes( rocketnet_address_t * bind, int lanes, const uint8_t * packet_filter )
{
    *bind = address( "127.0.0.1:0" );
    rocketnet_server_rio_options_t options;
    options.lanes = lanes;
    options.packet_filter = packet_filter;
    rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, bind, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, NULL, &options );
    CHECK( rio && bind->port );
    CHECK( rocketnet_server_rio_lanes( rio ) == lanes );
    return rio;
}

// Four lanes on one port: every client's frames leave through the lane its
// address hashes to, arrive whole and in staged order, and the clients' own
// datagrams to the port all reach the one process view whichever lane's
// socket Windows hands them to. Every lane posts its receives.
static void test_rio_lanes_send_and_receive()
{
    CHECK( rocketnet_server_rio_available() );

    const int lanes = 4;
    const int clients = 32;
    const int n = 20;
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open_lanes( &bind, lanes, NULL );
    CHECK( rocketnet_server_rio_lanes( rio ) == lanes );

    std::vector<rocketnet_platform_socket_t *> client( (size_t) clients );
    std::vector<rocketnet_address_t> client_address( (size_t) clients );
    bool used[lanes] = {};
    for ( int c = 0; c < clients; ++c )
    {
        client[(size_t) c] = fault_client( &client_address[(size_t) c] );
        const int lane = rocketnet_server_rio_lane_of( rio, &client_address[(size_t) c] );
        CHECK( lane >= 0 && lane < lanes );
        CHECK( rocketnet_server_rio_lane_of( rio, &client_address[(size_t) c] ) == lane );
        used[lane] = true;
    }
    int lanes_used = 0;
    for ( int l = 0; l < lanes; ++l )
    {
        lanes_used += used[l] ? 1 : 0;
    }
    CHECK( lanes_used >= 2 );

    // every lane's worker answered where it runs, and on this host pinned
    int pinned = 0;
    for ( int l = 0; l < lanes; ++l )
    {
        uint64_t mask = 0;
        int group = -1;
        unsigned long error = 0;
        const int placement = rocketnet_server_rio_lane_placement( rio, l, &mask, &group, &error );
        CHECK( placement != ROCKETNET_RIO_PLACEMENT_PENDING );
        pinned += placement == ROCKETNET_RIO_PLACEMENT_PINNED && mask != 0 && group >= 0 ? 1 : 0;
    }
    CHECK( pinned == lanes );
    CHECK( rocketnet_server_rio_lane_placement( rio, lanes, NULL, NULL, NULL ) == ROCKETNET_RIO_PLACEMENT_PENDING );

    const uint64_t sent = rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT );
    const int round = ++fault_round & 0xFF;
    for ( int i = 0; i < n; ++i )
    {
        for ( int c = 0; c < clients; ++c )
        {
            uint64_t id;
            uint8_t * frame = rio_reserve( rio, &client_address[(size_t) c], &id );
            CHECK( frame );
            CHECK( id < (uint64_t) lanes * (uint64_t) ( 3 * rocketnet_server_rio_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS ) ) );
            fault_mark( frame + ROCKETNET_FRAME_WIRE_OFFSET, round, i );
            rocketnet_server_rio_finish_packet( rio, id, frame, FAULT_BYTES );
        }
    }
    rio_flush( rio );

    // each client: all n, in the order staged
    for ( int c = 0; c < clients; ++c )
    {
        int next = 0;
        const double deadline = rocketnet_platform_time() + 5.0;
        while ( next < n && rocketnet_platform_time() < deadline )
        {
            uint8_t packet[ROCKETNET_RECEIVE_CEILING];
            rocketnet_address_t from;
            const int bytes = rocketnet_platform_socket_receive_packet( client[(size_t) c], &from, packet, sizeof( packet ) );
            if ( !bytes )
            {
                rocketnet_platform_sleep( 0.001 );
                continue;
            }
            CHECK( from.port == bind.port );
            CHECK( fault_index( packet, bytes, round ) == next );
            ++next;
        }
        CHECK( next == n );
    }
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT ) - sent == (uint64_t) ( clients * n ) );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED ) == 0 );

    // inbound: every client's datagrams reach the one view
    for ( int c = 0; c < clients; ++c )
    {
        fault_client_send( client[(size_t) c], &bind, round, n );
    }
    std::vector<int> seen( (size_t) clients, 0 );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( count < clients * n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets( rio );
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_rio_process_packets( rio );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index( batch->packet_data[i], batch->packet_bytes[i], round );
            CHECK( index >= 0 && index < n );
            for ( int c = 0; c < clients; ++c )
            {
                if ( rocketnet_address_equal( &batch->from[i], &client_address[(size_t) c] ) )
                {
                    ++seen[(size_t) c];
                }
            }
            ++count;
        }
        rocketnet_platform_sleep( 0.001 );
    }
    CHECK( count == clients * n );
    for ( int c = 0; c < clients; ++c )
    {
        CHECK( seen[(size_t) c] == n );
    }
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED ) == (uint64_t) ( clients * n ) );

    // every lane's receives posted again, nothing outstanding
    const int receives = lanes * rocketnet_server_rio_receives( ROCKETNET_DEFAULT_MAX_CLIENTS );
    wait_until( "RIO lanes settle", [&]()
                {
        rocketnet_server_rio_send_packets( rio );
        rocketnet_server_rio_receive_packets( rio );
        return rocketnet_server_rio_pending( rio ) == receives && rocketnet_server_rio_test_retry_receives( rio ) == 0 &&
               rocketnet_server_rio_test_send_idle( rio ); } );

    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
    for ( int c = 0; c < clients; ++c )
    {
        rocketnet_platform_socket_destroy( client[(size_t) c] );
    }
    printf( "test_rio_transport_lanes_send_and_receive lanes_used=%d pinned=%d of %d\n", lanes_used, pinned, lanes );
}

// A port another socket already holds, with or without SO_REUSEADDR, is
// refused before any lane binds: the exclusive probe. One lane is refused as
// before, by its own exclusive bind.
static void test_rio_lanes_refuse_a_held_port()
{
    CHECK( rocketnet_server_rio_available() );

    for ( int reuse = 0; reuse < 2; ++reuse )
    {
        SOCKET holder = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
        CHECK( holder != INVALID_SOCKET );
        BOOL enabled = TRUE;
        if ( reuse )
        {
            CHECK( setsockopt( holder, SOL_SOCKET, SO_REUSEADDR, (const char *) &enabled, sizeof( enabled ) ) == 0 );
        }
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        CHECK( bind( holder, (sockaddr *) &a, sizeof( a ) ) == 0 );
        int bytes = sizeof( a );
        CHECK( getsockname( holder, (sockaddr *) &a, &bytes ) == 0 );

        for ( int lanes : { 1, 4 } )
        {
            rocketnet_address_t at = address( "127.0.0.1:0" );
            at.port = ntohs( a.sin_port );
            rocketnet_server_rio_options_t options;
            options.lanes = lanes;
            options.packet_filter = NULL;
            // the create fails naming the refused bind, and no lane is made
            int status = ROCKETNET_STATUS_OK;
#if ROCKETNET_DEV
            const uint64_t made = rocketnet_server_rio_test_lanes_made();
#endif // #if ROCKETNET_DEV
            CHECK( !rocketnet_server_rio_create( NULL, &at, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, &status, &options ) );
            CHECK( status == ROCKETNET_STATUS_TRANSPORT_BIND_REFUSED );
#if ROCKETNET_DEV
            CHECK( rocketnet_server_rio_test_lanes_made() == made );
#endif // #if ROCKETNET_DEV
        }
        closesocket( holder );
    }

    // lane counts outside 0 .. ROCKETNET_RIO_MAX_LANES are refused
    for ( int lanes : { -1, ROCKETNET_RIO_MAX_LANES + 1 } )
    {
        rocketnet_address_t at = address( "127.0.0.1:0" );
        rocketnet_server_rio_options_t options;
        options.lanes = lanes;
        options.packet_filter = NULL;
        int status = ROCKETNET_STATUS_OK;
#if ROCKETNET_DEV
        const uint64_t made = rocketnet_server_rio_test_lanes_made();
#endif // #if ROCKETNET_DEV
        CHECK( !rocketnet_server_rio_create( NULL, &at, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, &status, &options ) );
#if ROCKETNET_DEV
        CHECK( rocketnet_server_rio_test_lanes_made() == made );
#endif // #if ROCKETNET_DEV
    }
    printf( "test_rio_transport_lanes_refuse_a_held_port\n" );
}

// The lanes' receives of port_thief_steals' 8 datagrams (marked 0x5A, 0..7):
// how many distinct ones the transport hands up within a second.
static int rio_lanes_take( rocketnet_server_rio_t * rio, int n )
{
    std::vector<bool> seen( (size_t) n, false );
    int count = 0;
    const double deadline = rocketnet_platform_time() + 1.0;
    while ( count < n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets( rio );
        rocketnet_server_socket_process_packets_t * batch = rocketnet_server_rio_process_packets( rio );
        for ( int i = 0; i < batch->num_packets; ++i )
        {
            const int index = fault_index( batch->packet_data[i], batch->packet_bytes[i], 0x5A );
            if ( index >= 0 && index < n && !seen[(size_t) index] )
            {
                seen[(size_t) index] = true;
                ++count;
            }
        }
        rocketnet_platform_sleep( 0.001 );
    }
    return count;
}

// A later socket on the lanes' port, bound plainly, with SO_REUSEADDR, or with
// SIO_CPU_AFFINITY to processor 0 (another process doing what the lanes do);
// true when Windows lets it bind
static bool rio_later_binder( const rocketnet_address_t * at, int mode, SOCKET * s )
{
    *s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( *s != INVALID_SOCKET );
    BOOL enabled = TRUE;
    if ( mode == 1 )
    {
        CHECK( setsockopt( *s, SOL_SOCKET, SO_REUSEADDR, (const char *) &enabled, sizeof( enabled ) ) == 0 );
    }
    else if ( mode == 2 )
    {
        USHORT processor = 0;
        DWORD returned = 0;
        CHECK( WSAIoctl( *s, 0x98000015u, &processor, sizeof( processor ), NULL, 0, &returned, NULL, NULL ) == 0 );
    }
    u_long non_blocking = 1;
    CHECK( ioctlsocket( *s, (long) FIONBIO, &non_blocking ) == 0 );
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons( at->port );
    memcpy( &a.sin_addr, at->data.ipv4, 4 );
    return bind( *s, (sockaddr *) &a, sizeof( a ) ) == 0;
}

// The lanes share the port by SIO_CPU_AFFINITY. A later plain or
// SO_REUSEADDR socket on the lanes' own address and port is refused; a later
// plain socket on the wildcard at that port binds but is less specific than
// the lanes and receives nothing; a later socket that also sets
// SIO_CPU_AFFINITY joins the port (the documented trade: Windows has no
// exclusive form of a port shared by processor). This pins all of it: a
// change in Windows shows here first. (A binder before the lanes is refused by
// the probe: test_rio_lanes_refuse_a_held_port.)
static void test_rio_lanes_later_binder_receives_nothing()
{
    CHECK( rocketnet_server_rio_available() );

    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open_lanes( &bind, 4, NULL );
    rocketnet_address_t target = address( "127.0.0.1:0" );
    target.port = bind.port;

    SOCKET plain;
    const bool plain_bound = rio_later_binder( &target, 0, &plain );
    closesocket( plain );
    SOCKET reuse;
    const bool reuse_bound = rio_later_binder( &target, 1, &reuse );
    closesocket( reuse );

    rocketnet_address_t wildcard = address( "0.0.0.0:0" );
    wildcard.port = bind.port;
    SOCKET wild_thief;
    const bool wild_bound = rio_later_binder( &wildcard, 0, &wild_thief );
    const int wild_stolen = wild_bound ? port_thief_steals( &target, wild_thief ) : 0;
    const int wild_kept = rio_lanes_take( rio, 8 );
    closesocket( wild_thief );

    SOCKET joiner;
    const bool joined = rio_later_binder( &target, 2, &joiner );
    closesocket( joiner );

    rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
    printf( "test_rio_transport_lanes_later_binder_receives_nothing plain_bound=%d reuse_bound=%d wildcard_bound=%d wildcard_stolen=%d wildcard_kept=%d affinity_joined=%d\n",
            plain_bound ? 1 : 0, reuse_bound ? 1 : 0, wild_bound ? 1 : 0, wild_stolen, wild_kept, joined ? 1 : 0 );
    CHECK( !plain_bound );
    CHECK( !reuse_bound );
    // the wildcard bind must succeed, or the specificity rule was not tested
    CHECK( wild_bound );
    CHECK( wild_stolen == 0 && wild_kept == 8 );
    CHECK( joined );
}

// More than one lane refuses the wildcard address (either family) and any
// IPv4-mapped address with ROCKETNET_STATUS_INVALID_ADDRESS, and takes a specific address on port 0
// (lane 0 is given the port, the others join it) or on an explicit port (the
// probe first); one lane takes the wildcard, as before.
static void test_rio_lanes_bind_forms()
{
    CHECK( rocketnet_server_rio_available() );

    const uint16_t port = rio_free_port();
    const char * wildcards[] = { "0.0.0.0:0", "[::]:0", "[::ffff:0.0.0.0]:0", "[::ffff:127.0.0.1]:0" };
    for ( const char * text : wildcards )
    {
        for ( const uint16_t at_port : { (uint16_t) 0, port } )
        {
            rocketnet_address_t at = address( text );
            at.port = at_port;
            rocketnet_server_rio_options_t options;
            options.lanes = 2;
            options.packet_filter = NULL;
            int status = ROCKETNET_STATUS_OK;
            CHECK( !rocketnet_server_rio_create( NULL, &at, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, &status, &options ) );
            CHECK( status == ROCKETNET_STATUS_INVALID_ADDRESS );
        }
    }
    for ( const uint16_t at_port : { (uint16_t) 0, port } )
    {
        rocketnet_address_t at = address( "127.0.0.1:0" );
        at.port = at_port;
        rocketnet_server_rio_options_t options;
        options.lanes = 3;
        options.packet_filter = NULL;
        int status = ROCKETNET_STATUS_OK;
        rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &at, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, &status, &options );
        CHECK( rio && rocketnet_server_rio_lanes( rio ) == 3 );
        CHECK( at.port != 0 && ( at_port == 0 || at.port == at_port ) );
        rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
    }
    {
        rocketnet_address_t at = address( "0.0.0.0:0" );
        rocketnet_server_rio_options_t options;
        options.lanes = 1;
        options.packet_filter = NULL;
        int status = ROCKETNET_STATUS_OK;
        rocketnet_server_rio_t * rio = rocketnet_server_rio_create( NULL, &at, ROCKETNET_DEFAULT_MAX_CLIENTS, 0, &status, &options );
        CHECK( rio && at.port != 0 );
        rn_native_backend_stop_and_destroy( rio, rocketnet_server_rio_stop, rocketnet_server_rio_stopped, rocketnet_server_rio_destroy );
    }
    printf( "test_rio_transport_lanes_bind_forms\n" );
}

// Lanes on port 0 rest on Windows never handing a port-0 bind a port another
// socket holds (no probe guards that form). This checks it on the Windows
// that runs the test: ports held by plain, SO_REUSEADDR and
// SO_EXCLUSIVEADDRUSE sockets on loopback and SO_REUSEADDR sockets on the
// wildcard, then SIO_CPU_AFFINITY port-0 binds as lane 0 makes them; none may
// land on a held port or on each other's.
static SOCKET rio_held_socket( int kind, ULONG ip )
{
    SOCKET s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    CHECK( s != INVALID_SOCKET );
    BOOL enabled = TRUE;
    if ( kind == 1 )
    {
        CHECK( setsockopt( s, SOL_SOCKET, SO_REUSEADDR, (const char *) &enabled, sizeof( enabled ) ) == 0 );
    }
    else if ( kind == 2 )
    {
        CHECK( setsockopt( s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &enabled, sizeof( enabled ) ) == 0 );
    }
    else if ( kind == 3 )
    {
        USHORT processor = 0;
        DWORD returned = 0;
        CHECK( WSAIoctl( s, 0x98000015u, &processor, sizeof( processor ), NULL, 0, &returned, NULL, NULL ) == 0 );
    }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl( ip );
    CHECK( bind( s, (sockaddr *) &a, sizeof( a ) ) == 0 );
    return s;
}

static uint16_t rio_socket_port( SOCKET s )
{
    sockaddr_in a = {};
    int bytes = sizeof( a );
    CHECK( getsockname( s, (sockaddr *) &a, &bytes ) == 0 );
    return ntohs( a.sin_port );
}

static void test_rio_lanes_port_zero_never_held()
{
    const int per_kind = 100;
    const int binds = 400;
    std::vector<SOCKET> sockets;
    std::vector<uint8_t> held( 65536, 0 );
    const struct
    {
        int kind;
        ULONG ip;
    } holders[] = { { 0, INADDR_LOOPBACK }, { 1, INADDR_LOOPBACK }, { 2, INADDR_LOOPBACK }, { 1, INADDR_ANY } };
    for ( const auto & h : holders )
    {
        for ( int i = 0; i < per_kind; ++i )
        {
            SOCKET s = rio_held_socket( h.kind, h.ip );
            held[rio_socket_port( s )] = 1;
            sockets.push_back( s );
        }
    }
    int collisions = 0;
    for ( int i = 0; i < binds; ++i )
    {
        SOCKET s = rio_held_socket( 3, INADDR_LOOPBACK );
        const uint16_t port = rio_socket_port( s );
        if ( held[port] )
        {
            ++collisions;
        }
        held[port] = 1;
        sockets.push_back( s );
    }
    for ( SOCKET s : sockets )
    {
        closesocket( s );
    }
    printf( "test_rio_transport_lanes_port_zero_never_held binds=%d collisions=%d\n", binds, collisions );
    CHECK( collisions == 0 );
}

// With a packet filter every lane worker runs the prefilter before banking:
// datagrams that fail it never reach the view, are not PACKETS_RECEIVED, and
// are counted by reason in receive_drop_totals.
static void test_rio_lanes_prefilter_counts_drops()
{
    CHECK( rocketnet_server_rio_available() );

    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x5a, 0x17, 0xc3, 0x09 };
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open_lanes( &bind, 2, filter );

    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );
    const int n = 50;
    const int round = ++fault_round & 0xFF;
    fault_client_send( client, &bind, round, n );

    uint64_t totals[ROCKETNET_RECEIVE_NUM_DROPS];
    uint64_t dropped = 0;
    int viewed = 0;
    const double deadline = rocketnet_platform_time() + 5.0;
    while ( dropped < (uint64_t) n && rocketnet_platform_time() < deadline )
    {
        rocketnet_server_rio_receive_packets( rio );
        viewed += rocketnet_server_rio_process_packets( rio )->num_packets;
        rocketnet_server_rio_receive_drop_totals( rio, totals );
        dropped = 0;
        for ( int i = 0; i < ROCKETNET_RECEIVE_NUM_DROPS; ++i )
        {
            dropped += totals[i];
        }
        rocketnet_platform_sleep( 0.001 );
    }
    CHECK( totals[ROCKETNET_RECEIVE_ACCEPT] == 0 );
    CHECK( totals[ROCKETNET_RECEIVE_DROP_BANK_FULL] == 0 );
    CHECK( dropped == (uint64_t) n );
    CHECK( viewed == 0 );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_RECEIVED ) == 0 );

    rio_finish( rio, client );
    printf( "test_rio_transport_lanes_prefilter_counts_drops\n" );
}

// finish refuses an id past the last lane's last slot, the largest id, an id
// that names another slot of the frame's lane, and one that names the same
// slot on another lane, as SEND_FAILED; the
// reserved slot is skipped, nothing is sent and the lanes go idle.
static void test_rio_lanes_finish_refuses_foreign_ids()
{
    CHECK( rocketnet_server_rio_available() );

    const int lanes = 3;
    rocketnet_address_t bind;
    rocketnet_server_rio_t * rio = rio_open_lanes( &bind, lanes, NULL );
    rocketnet_address_t client_address;
    rocketnet_platform_socket_t * client = fault_client( &client_address );

    const uint64_t slots = 3ull * (uint64_t) rocketnet_server_rio_send_queue_size( ROCKETNET_DEFAULT_MAX_CLIENTS );
    const uint64_t foreign[] = { (uint64_t) lanes * slots, 0xFFFFFFFFFFFFFFFFull };
    const uint64_t sent = rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT );
    for ( uint64_t bad : foreign )
    {
        uint64_t id;
        uint8_t * frame = rio_reserve( rio, &client_address, &id );
        CHECK( frame && id < (uint64_t) lanes * slots );
        fault_mark( frame + ROCKETNET_FRAME_WIRE_OFFSET, 1, 0 );
        rocketnet_server_rio_finish_packet( rio, bad, frame, FAULT_BYTES );
        rocketnet_server_rio_abort_packet( rio, bad, frame );
    }
    // an id in range that names another slot of the frame's own lane
    {
        uint64_t id;
        uint8_t * frame = rio_reserve( rio, &client_address, &id );
        CHECK( frame && id < (uint64_t) lanes * slots );
        fault_mark( frame + ROCKETNET_FRAME_WIRE_OFFSET, 1, 0 );
        const uint64_t other = ( id % slots ) + 1 < slots ? id + 1 : id - 1;
        rocketnet_server_rio_finish_packet( rio, other, frame, FAULT_BYTES );
        rocketnet_server_rio_abort_packet( rio, other, frame );
    }
    // an id that names the same slot on another lane
    {
        uint64_t id;
        uint8_t * frame = rio_reserve( rio, &client_address, &id );
        CHECK( frame && id < (uint64_t) lanes * slots );
        fault_mark( frame + ROCKETNET_FRAME_WIRE_OFFSET, 1, 0 );
        const uint64_t other = ( id + slots ) % ( (uint64_t) lanes * slots );
        rocketnet_server_rio_finish_packet( rio, other, frame, FAULT_BYTES );
        rocketnet_server_rio_abort_packet( rio, other, frame );
    }
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_SEND_FAILED ) == 4 );
    rio_flush( rio );
    CHECK( rio_counter( rio, ROCKETNET_RIO_COUNTER_PACKETS_SENT ) == sent );

    rio_finish( rio, client );
    printf( "test_rio_transport_lanes_finish_refuses_foreign_ids\n" );
}

struct fault_case_t
{
    const char * name;
    void ( *run )();
};

static const fault_case_t fault_cases[] = {
    { "test_rio_transport_stop_sends_handed_burst", test_rio_transport_stop_sends_handed_burst },
    { "test_iocp_transport_receive_boundaries", test_receive_boundaries_both_paths },
    { "test_iocp_transport_completion_batches_counted", test_completion_batches_both_paths },
    { "test_rio_transport_receive_size_drops_counted", test_rio_receive_size_drops_counted },
    { "test_rio_transport_send_waits_counted", test_rio_send_waits_counted },
    { "test_iocp_transport_socket_sets_dont_fragment", test_iocp_transport_socket_sets_dont_fragment },
    { "test_rio_transport_socket_sets_dont_fragment", test_rio_transport_socket_sets_dont_fragment },
    { "test_platform_socket_sets_dont_fragment", test_platform_socket_sets_dont_fragment },
    { "test_iocp_transport_finish_refuses_unissued_packet_id", test_iocp_finish_refuses_unissued_packet_id },
    { "test_iocp_transport_finish_refuses_bad_packet_bytes", test_iocp_finish_refuses_bad_packet_bytes },
    { "test_iocp_transport_stop_sends_published_burst", test_iocp_stop_sends_published_burst },
    { "test_iocp_transport_send_completion_fails", test_iocp_send_completion_fails },
    { "test_iocp_transport_send_post_fails", test_iocp_send_post_fails },
    { "test_iocp_transport_receive_completion_fails", test_iocp_receive_completion_fails },
    { "test_windows_oversize_receive_at_post", test_windows_oversize_receive_at_post },
    { "test_iocp_transport_receive_oversize_queued_at_post", test_receive_oversize_queued_at_post },
    { "test_iocp_transport_send_queue_full_recovers", test_iocp_send_queue_full_recovers },
    { "test_iocp_transport_destroy_never_hangs", test_iocp_destroy_never_hangs },
    { "test_rio_transport_completion_queue_full", test_rio_completion_queue_full },
    { "test_rio_transport_request_queue_full", test_rio_request_queue_full },
    { "test_rio_transport_send_post_fails", test_rio_send_post_fails },
    { "test_rio_transport_destroy_commits_deferred_sends", test_rio_destroy_commits_deferred_sends },
    { "test_rio_transport_stop_retries_refused_deferred_commit", test_rio_stop_retries_refused_deferred_commit },
    { "test_rio_transport_create_failure_drains_posted_receives", test_rio_create_failure_drains_posted_receives },
    { "test_windows_create_refused_for_commit_is_out_of_memory", test_windows_create_refused_for_commit_is_out_of_memory },
    { "test_windows_port_exclusive", test_windows_port_exclusive },
    { "test_windows_send_failures_counted", test_windows_send_failures_counted },
    { "test_windows_sockets_not_inherited", test_windows_sockets_not_inherited },
    { "test_platform_socket_is_non_blocking", test_platform_socket_is_non_blocking },
    { "test_windows_transports_sized_by_max_clients", test_windows_transports_sized_by_max_clients },
    { "test_rio_transport_lanes_send_and_receive", test_rio_lanes_send_and_receive },
    { "test_rio_transport_lanes_refuse_a_held_port", test_rio_lanes_refuse_a_held_port },
    { "test_rio_transport_lanes_later_binder_receives_nothing", test_rio_lanes_later_binder_receives_nothing },
    { "test_rio_transport_lanes_prefilter_counts_drops", test_rio_lanes_prefilter_counts_drops },
    { "test_rio_transport_lanes_finish_refuses_foreign_ids", test_rio_lanes_finish_refuses_foreign_ids },
    { "test_rio_transport_lanes_bind_forms", test_rio_lanes_bind_forms },
    { "test_rio_transport_lanes_port_zero_never_held", test_rio_lanes_port_zero_never_held },
};

// With no argument every case runs; with names, only those cases run.

int main( int argc, char ** argv )
{
    iocp_default_assert = rn_assert_function;
    rocketnet_assert_function( iocp_print_assert );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    CHECK( rocketnet_init( NULL, allocate, release ) == ROCKETNET_STATUS_OK );

    if ( argc < 2 )
    {
        test_create_cleanup();
        test_completion_owns_frames();
        test_uso_run_owns_frames();
        test_datagrams_and_producers();
        test_receive_publication_never_waits();
        test_receive_ready_banks_fifo();
        for ( const fault_case_t & c : fault_cases )
        {
            c.run();
        }
    }

    for ( int i = 1; i < argc; ++i )
    {
        bool found = false;
        for ( const fault_case_t & c : fault_cases )
        {
            if ( strcmp( argv[i], c.name ) == 0 )
            {
                c.run();
                found = true;
            }
        }
        CHECK( found );
    }

    rocketnet_term();
    CHECK( live_allocations.load() == 0 );

    return 0;
}

#endif // ROCKETNET_DEV
