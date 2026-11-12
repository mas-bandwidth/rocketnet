/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// One client's per-frame cost on the game thread, against a real server over
// real sockets, at the space game's client profile. A bench tool, never a
// unit test: it uses real time and real sockets. See client_frame_time.md.

#if defined( _WIN32 )
#define _CRT_SECURE_NO_WARNINGS     // fopen, getenv
#define NOMINMAX
#endif // #if defined( _WIN32 )

#include "rocketnet.h"
#include "client_frame_time.h"
#include "../examples/shutdown.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <vector>

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <intrin.h>
typedef SOCKET cft_socket_t;
typedef int socklen_t;
typedef int cft_length_t;
#define CFT_BAD_SOCKET INVALID_SOCKET
#else // #if defined( _WIN32 )
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int cft_socket_t;
typedef size_t cft_length_t;
#define CFT_BAD_SOCKET ( -1 )
#endif // #if defined( _WIN32 )

#if defined( __linux__ )
#include <errno.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#endif // #if defined( __linux__ )

#if defined( __APPLE__ )
#include <mach/mach.h>
#endif // #if defined( __APPLE__ )

#define CFT_PROTOCOL_ID 0x43465431464d4531ULL
#define CFT_FILTER { 0x43, 0x46, 0x54, 0x31 }

static const char * const cft_call_names[CFT_NUM_CALLS] = { "none", "update", "receive", "write", "send" };

static void cft_fail( const char * what )
{
    fprintf( stderr, "client_frame_time: FAIL: %s\n", what );
    exit( 1 );
}

static const char * cft_arg( int argc, char ** argv, const char * name, const char * fallback )
{
    for ( int i = 2; i + 1 < argc; i++ )
    {
        if ( strcmp( argv[i], name ) == 0 )
        {
            return argv[i + 1];
        }
    }
    return fallback;
}

static double cft_seconds()
{
    return (double) cft_now_ns() / 1e9;
}

static void cft_sleep_ns( uint64_t ns )
{
    std::this_thread::sleep_for( std::chrono::nanoseconds( ns ) );
}

// ---------------------------------------------------------------
//    allocations: the library's own allocator hooks (rocketnet_init)
// ---------------------------------------------------------------

static uint64_t cft_allocs_in_call[CFT_NUM_CALLS];      // game thread, inside a call window
static std::atomic<uint64_t> cft_allocs_elsewhere( 0 );  // any thread outside a window, once steady
static std::atomic<bool> cft_steady( false );

static void * cft_malloc( void * context, size_t bytes )
{
    (void) context;
    const int call = cft_get_call();
    if ( call != CFT_CALL_NONE )
    {
        cft_allocs_in_call[call]++;
    }
    else if ( cft_steady.load( std::memory_order_relaxed ) )
    {
        cft_allocs_elsewhere.fetch_add( 1, std::memory_order_relaxed );
    }
    return malloc( bytes );
}

static void cft_free( void * context, void * p )
{
    (void) context;
    free( p );
}

// ---------------------------------------------------------------
//    system calls
// ---------------------------------------------------------------

// Linux: a per-thread counter on the raw_syscalls:sys_enter tracepoint
// (needs root or perf_event_paranoid <= -1), exact for the game thread.
// macOS: the task's unix plus mach system call counts, over the window: every
// thread of the process, so an upper bound on the game thread's.
// Windows: none; the hooked entry points (wake, os_other) are reported instead.

#if defined( __linux__ )
static int cft_syscall_fd = -1;
#endif // #if defined( __linux__ )

static const char * cft_syscall_source = "none";

static void cft_syscalls_open()
{
#if defined( __linux__ )
    const char * paths[] = { "/sys/kernel/tracing/events/raw_syscalls/sys_enter/id", "/sys/kernel/debug/tracing/events/raw_syscalls/sys_enter/id" };
    for ( const char * path : paths )
    {
        FILE * file = fopen( path, "r" );
        if ( !file )
        {
            continue;
        }
        unsigned long long id = 0;
        const int read = fscanf( file, "%llu", &id );
        fclose( file );
        if ( read != 1 )
        {
            continue;
        }
        struct perf_event_attr attr;
        memset( &attr, 0, sizeof( attr ) );
        attr.type = PERF_TYPE_TRACEPOINT;
        attr.size = sizeof( attr );
        attr.config = id;
        const long fd = syscall( SYS_perf_event_open, &attr, 0, -1, -1, 0 );
        if ( fd >= 0 )
        {
            cft_syscall_fd = (int) fd;
            cft_syscall_source = "perf-tracepoint-game-thread";
            return;
        }
        fprintf( stderr, "client_frame_time: perf_event_open on %s (id %llu) refused: %s\n", path, id, strerror( errno ) );
    }
#elif defined( __APPLE__ ) // #if defined( __linux__ )
    cft_syscall_source = "task-events-whole-process";
#endif // #if defined( __linux__ )
}

static int64_t cft_syscalls_now()
{
#if defined( __linux__ )
    if ( cft_syscall_fd < 0 )
    {
        return -1;
    }
    uint64_t value = 0;
    if ( read( cft_syscall_fd, &value, sizeof( value ) ) != (ssize_t) sizeof( value ) )
    {
        return -1;
    }
    return (int64_t) value;
#elif defined( __APPLE__ ) // #if defined( __linux__ )
    task_events_info_data_t info;
    mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;
    if ( task_info( mach_task_self(), TASK_EVENTS_INFO, reinterpret_cast<task_info_t>( &info ), &count ) != KERN_SUCCESS )
    {
        return -1;
    }
    return (int64_t) (uint32_t) info.syscalls_unix + (int64_t) (uint32_t) info.syscalls_mach;
#else // #if defined( __linux__ )
    return -1;
#endif // #if defined( __linux__ )
}

// ---------------------------------------------------------------
//    the crypto implementation the bundled sodium selects here
//    (sodium/sodium.c _crypto_stream_chacha20_pick_best_implementation,
//    _crypto_onetimeauth_poly1305_pick_best_implementation; sodium/sodium.h)
// ---------------------------------------------------------------

static const char * cft_crypto_choice()
{
#if defined( _MSC_VER ) && !defined( __clang__ ) && ( defined( _M_X64 ) || defined( _M_AMD64 ) )
    int info[4];
    __cpuid( info, 1 );
    const bool ssse3 = ( info[2] & ( 1 << 9 ) ) != 0;
#if defined( __AVX2__ )
    __cpuidex( info, 7, 0 );
    if ( ( info[1] & ( 1 << 5 ) ) != 0 )
    {
        return "chacha20=dolbeau-avx2 poly1305=donna32";
    }
#endif // #if defined( __AVX2__ )
    return ssse3 ? "chacha20=dolbeau-ssse3 poly1305=donna32" : "chacha20=ref poly1305=donna32";
#elif ( defined( __GNUC__ ) || defined( __clang__ ) ) && ( defined( __x86_64__ ) || defined( __amd64__ ) ) // #if defined( _MSC_VER ) ...
    if ( __builtin_cpu_supports( "avx2" ) )
    {
        return "chacha20=dolbeau-avx2 poly1305=sse2";
    }
    return __builtin_cpu_supports( "ssse3" ) ? "chacha20=dolbeau-ssse3 poly1305=sse2" : "chacha20=ref poly1305=sse2";
#elif defined( __SIZEOF_INT128__ ) // #if defined( _MSC_VER ) ...
    return "chacha20=ref poly1305=donna64";
#else // #if defined( _MSC_VER ) ...
    return "chacha20=ref poly1305=donna32";
#endif // #if defined( _MSC_VER ) ...
}

// ---------------------------------------------------------------
//    the impairment relay (macOS and Windows; Linux uses netem on the veth)
// ---------------------------------------------------------------

// It binds the address the server advertises, 127.0.0.1:port, with
// SO_REUSEADDR beside the server's wildcard bind of the same port, so the
// client's datagrams reach it (the most specific bind). It forwards them to
// the server at forward_ip:port from a socket of its own, and the server's
// replies back to the client, each direction with loss, delay, jitter and
// reorder.

struct cft_relay_packet_t
{
    uint64_t due_ns;
    bool to_server;
    int bytes;
    uint8_t data[1500];
};

struct cft_relay_t
{
    cft_socket_t client_side;
    cft_socket_t server_side;
    sockaddr_in server_address;
    sockaddr_in client_address;
    bool have_client;
    double loss;
    double reorder;
    uint64_t delay_ns;
    uint64_t jitter_ns;
    std::atomic<bool> quit;
    std::thread thread;
};

static void cft_close_socket( cft_socket_t s )
{
#if defined( _WIN32 )
    closesocket( s );
#else // #if defined( _WIN32 )
    close( s );
#endif // #if defined( _WIN32 )
}

static cft_socket_t cft_udp_bind( const char * ip, uint16_t port, bool reuse )
{
    cft_socket_t s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( s == CFT_BAD_SOCKET )
    {
        return s;
    }
    if ( reuse )
    {
        const int one = 1;
        setsockopt( s, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, (socklen_t) sizeof( one ) );
    }
    sockaddr_in address;
    memset( &address, 0, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_port = htons( port );
    inet_pton( AF_INET, ip, &address.sin_addr );
    if ( bind( s, (const sockaddr *) &address, (socklen_t) sizeof( address ) ) != 0 )
    {
        cft_close_socket( s );
        return CFT_BAD_SOCKET;
    }
    return s;
}

static void cft_relay_run( cft_relay_t * relay )
{
    std::mt19937_64 random( 1234 );
    std::uniform_real_distribution<double> unit( 0.0, 1.0 );
    std::vector<cft_relay_packet_t> queue;
    queue.reserve( 4096 );

    while ( !relay->quit.load( std::memory_order_relaxed ) )
    {
        fd_set readable;
        FD_ZERO( &readable );
        FD_SET( relay->client_side, &readable );
        FD_SET( relay->server_side, &readable );
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 250;
        const int highest = (int) ( relay->client_side > relay->server_side ? relay->client_side : relay->server_side );
        const int ready = select( highest + 1, &readable, NULL, NULL, &timeout );
        const uint64_t now = cft_now_ns();

        for ( int side = 0; ready > 0 && side < 2; side++ )
        {
            const cft_socket_t s = side == 0 ? relay->client_side : relay->server_side;
            if ( !FD_ISSET( s, &readable ) )
            {
                continue;
            }
            cft_relay_packet_t packet;
            sockaddr_in from;
            socklen_t from_bytes = sizeof( from );
            const int bytes = (int) recvfrom( s, (char *) packet.data, (cft_length_t) sizeof( packet.data ), 0, (sockaddr *) &from, &from_bytes );
            if ( bytes <= 0 )
            {
                continue;
            }
            if ( side == 0 )
            {
                relay->client_address = from;
                relay->have_client = true;
            }
            if ( unit( random ) < relay->loss )
            {
                continue;
            }
            uint64_t delay = relay->delay_ns + (uint64_t) ( unit( random ) * (double) relay->jitter_ns );
            if ( unit( random ) < relay->reorder )
            {
                delay += relay->jitter_ns + 5000000ull;
            }
            packet.due_ns = now + delay;
            packet.to_server = side == 0;
            packet.bytes = bytes;
            queue.push_back( packet );
        }

        for ( size_t i = 0; i < queue.size(); )
        {
            if ( queue[i].due_ns > now )
            {
                i++;
                continue;
            }
            const cft_relay_packet_t & packet = queue[i];
            if ( packet.to_server )
            {
                sendto( relay->server_side, (const char *) packet.data, (cft_length_t) packet.bytes, 0, (const sockaddr *) &relay->server_address, (socklen_t) sizeof( relay->server_address ) );
            }
            else if ( relay->have_client )
            {
                sendto( relay->client_side, (const char *) packet.data, (cft_length_t) packet.bytes, 0, (const sockaddr *) &relay->client_address, (socklen_t) sizeof( relay->client_address ) );
            }
            queue[i] = queue.back();
            queue.pop_back();
        }
    }
}

// ---------------------------------------------------------------
//    server
// ---------------------------------------------------------------

static int cft_server( int argc, char ** argv )
{
    const char * bind_address = cft_arg( argc, argv, "--bind", "127.0.0.1:40000" );
    const char * token_file = cft_arg( argc, argv, "--token-file", "client_frame_time.token" );
    const char * transport = cft_arg( argc, argv, "--transport", NULL );
    const int packets = atoi( cft_arg( argc, argv, "--packets", "15" ) );
    const int packet_bytes = atoi( cft_arg( argc, argv, "--packet-bytes", "1200" ) );
    const double hz = atof( cft_arg( argc, argv, "--hz", "60" ) );
    const double max_seconds = atof( cft_arg( argc, argv, "--seconds", "900" ) );
    const char * relay_forward = cft_arg( argc, argv, "--relay-forward-ip", NULL );

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        cft_fail( "init" );
    }

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    if ( rocketnet_generate_backend_keypair( backend_public, backend_secret ) != ROCKETNET_STATUS_OK )
    {
        cft_fail( "backend keypair" );
    }

    struct rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    const uint8_t filter[] = CFT_FILTER;
    config.protocol_id = CFT_PROTOCOL_ID;
    memcpy( config.packet_filter, filter, sizeof( filter ) );
    memcpy( config.backend_public_key, backend_public, sizeof( backend_public ) );
    config.max_clients = 1;
    config.transport = transport;
    config.per_client_send_kbps = 100000;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, bind_address, NULL, &config, &status );
    if ( server && status == ROCKETNET_STATUS_OK )
    {
        status = rocketnet_server_start( server );
    }
    if ( !server || status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "server: %s\n", rocketnet_status_name( status ) );
        cft_fail( "server create or start" );
    }

    char served[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    if ( !rocketnet_server_ipv4_address( server, served, sizeof( served ) ) )
    {
        cft_fail( "server address" );
    }

    cft_relay_t * relay = NULL;
    if ( relay_forward )
    {
        relay = new cft_relay_t;
        const uint16_t port = rocketnet_server_port( server );
        relay->client_side = cft_udp_bind( "127.0.0.1", port, true );
        relay->server_side = cft_udp_bind( relay_forward, 0, false );
        if ( relay->client_side == CFT_BAD_SOCKET || relay->server_side == CFT_BAD_SOCKET )
        {
            cft_fail( "relay bind" );
        }
        memset( &relay->server_address, 0, sizeof( relay->server_address ) );
        relay->server_address.sin_family = AF_INET;
        relay->server_address.sin_port = htons( port );
        inet_pton( AF_INET, relay_forward, &relay->server_address.sin_addr );
        relay->have_client = false;
        relay->loss = atof( cft_arg( argc, argv, "--loss", "0.02" ) );
        relay->reorder = atof( cft_arg( argc, argv, "--reorder", "0.05" ) );
        relay->delay_ns = (uint64_t) ( atof( cft_arg( argc, argv, "--delay-ms", "20" ) ) * 1e6 );
        relay->jitter_ns = (uint64_t) ( atof( cft_arg( argc, argv, "--jitter-ms", "10" ) ) * 1e6 );
        relay->quit.store( false );
        relay->thread = std::thread( cft_relay_run, relay );
    }

    struct rocketnet_connect_token_config_t token_config;
    rocketnet_connect_token_default_config( &token_config );
    memcpy( token_config.backend_secret_key, backend_secret, sizeof( backend_secret ) );
    token_config.server_address_ipv4 = served;
    rocketnet_server_public_key( server, token_config.server_public_key );
    token_config.client_id = 1;
    token_config.protocol_id = CFT_PROTOCOL_ID;
    token_config.expire_seconds = 3600;
    token_config.timeout_seconds = 10;
    token_config.unix_timestamp = (uint64_t) time( NULL );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( rocketnet_create_connect_token( &token_config, token ) != ROCKETNET_STATUS_OK )
    {
        cft_fail( "connect token" );
    }

    char temporary[1024];
    snprintf( temporary, sizeof( temporary ), "%s.partial", token_file );
    FILE * file = fopen( temporary, "w" );
    if ( !file )
    {
        cft_fail( "token file" );
    }
    fprintf( file, "%s\n", token );
    fclose( file );
    remove( token_file );
    if ( rename( temporary, token_file ) != 0 )
    {
        cft_fail( "token file rename" );
    }

    printf( "server transport=%s address=%s relay=%s packets=%d packet_bytes=%d hz=%.1f\n", rocketnet_server_transport_name( server ), served,
            relay ? "on" : "off", packets, packet_bytes, hz );
    fflush( stdout );

    std::vector<uint8_t> body( (size_t) packet_bytes, 0x5a );
    const double start = rocketnet_time();
    double next = start;
    bool seen_client = false;
    double gone_since = 0.0;
    uint64_t tick = 0;

    while ( rocketnet_time() - start < max_seconds )
    {
        const double now = rocketnet_time();
        rocketnet_server_update( server, now );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
        }

        if ( rocketnet_server_client_connected( server, 0 ) )
        {
            seen_client = true;
            gone_since = 0.0;
            for ( int i = 0; i < packets; i++ )
            {
                uint8_t * p = rocketnet_server_start_message( server, 0, 0, ROCKETNET_RELIABILITY_UNRELIABLE, packet_bytes );
                if ( !p )
                {
                    break;
                }
                memcpy( p, body.data(), body.size() );
                p[0] = (uint8_t) i;
                rocketnet_server_finish_message( server, packet_bytes );
            }
            uint8_t * p = rocketnet_server_start_message( server, 0, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 64 );
            if ( p )
            {
                memset( p, (int) ( tick & 0xff ), 64 );
                rocketnet_server_finish_message( server, 64 );
            }
        }
        else if ( seen_client )
        {
            if ( gone_since == 0.0 )
            {
                gone_since = now;
            }
            else if ( now - gone_since > 1.0 )
            {
                break;
            }
        }

        rocketnet_server_send_packets( server );
        tick++;

        next += 1.0 / hz;
        const double wait = next - rocketnet_time();
        if ( wait > 0.0 )
        {
            rocketnet_sleep( wait );
        }
        else if ( wait < -0.5 )
        {
            next = rocketnet_time();
        }
    }

    if ( relay )
    {
        relay->quit.store( true );
        relay->thread.join();
        cft_close_socket( relay->client_side );
        cft_close_socket( relay->server_side );
        delete relay;
    }

    rn_cli_server_stop_and_destroy( server );
    rocketnet_term();
    printf( "server done ticks=%llu\n", (unsigned long long) tick );
    return 0;
}

// ---------------------------------------------------------------
//    client
// ---------------------------------------------------------------

struct cft_window_t
{
    double us;
    int64_t syscalls;
    uint64_t allocs;
    uint64_t trylock;
    uint64_t trylock_busy;
    uint64_t lock;
    uint64_t lock_ns;
    uint64_t wake;
    uint64_t os_other;
};

struct cft_frame_t
{
    cft_window_t call[CFT_NUM_CALLS];
    double total_us;
    int messages;
    int hitch_offset;      // frames since the last hitch, -1 before the first
    bool connected;
};

static int64_t cft_syscall_baseline = 0;

template <typename F> static void cft_window( int call, cft_window_t * w, F f )
{
    const cft_counts_t before = cft_counts;
    const uint64_t allocs_before = cft_allocs_in_call[call];
    const int64_t syscalls_before = cft_syscalls_now();

    cft_set_call( call );
    const uint64_t start = cft_now_ns();
    f();
    const uint64_t end = cft_now_ns();
    cft_set_call( CFT_CALL_NONE );

    const int64_t syscalls_after = cft_syscalls_now();

    w->us = (double) ( end - start ) / 1000.0;
    w->syscalls = syscalls_before < 0 || syscalls_after < 0 ? -1 : syscalls_after - syscalls_before - cft_syscall_baseline;
    w->allocs = cft_allocs_in_call[call] - allocs_before;
    w->trylock = cft_counts.trylock - before.trylock;
    w->trylock_busy = cft_counts.trylock_busy - before.trylock_busy;
    w->lock = cft_counts.lock - before.lock;
    w->lock_ns = cft_counts.lock_ns - before.lock_ns;
    w->wake = cft_counts.wake - before.wake;
    w->os_other = cft_counts.os_other - before.os_other;
}

// nearest rank on a sorted copy
static double cft_percentile( std::vector<double> values, double p )
{
    if ( values.empty() )
    {
        return 0.0;
    }
    std::sort( values.begin(), values.end() );
    const double rank = p / 100.0 * (double) values.size();
    size_t index = (size_t) rank;
    if ( (double) index < rank )
    {
        index++;
    }
    if ( index > 0 )
    {
        index--;
    }
    return values[std::min( index, values.size() - 1 )];
}

static void cft_report( const char * label, const std::vector<double> & values, const std::vector<cft_frame_t> & frames, int call )
{
    double syscall_sum = 0.0;
    int64_t syscall_max = 0;
    uint64_t allocs = 0, alloc_frames = 0, trylock = 0, busy = 0, lock = 0, lock_ns = 0, wake = 0, other = 0;
    bool syscalls_known = true;
    for ( const cft_frame_t & frame : frames )
    {
        if ( call < 0 )
        {
            continue;
        }
        const cft_window_t & w = frame.call[call];
        if ( w.syscalls < 0 )
        {
            syscalls_known = false;
        }
        syscall_sum += (double) w.syscalls;
        syscall_max = std::max( syscall_max, w.syscalls );
        allocs += w.allocs;
        alloc_frames += w.allocs > 0 ? 1u : 0u;
        trylock += w.trylock;
        busy += w.trylock_busy;
        lock += w.lock;
        lock_ns += w.lock_ns;
        wake += w.wake;
        other += w.os_other;
    }
    const double n = frames.empty() ? 1.0 : (double) frames.size();
    printf( "cft call=%s frames=%zu p50_us=%.2f p99_us=%.2f p999_us=%.2f max_us=%.2f", label, values.size(), cft_percentile( values, 50.0 ),
            cft_percentile( values, 99.0 ), cft_percentile( values, 99.9 ), values.empty() ? 0.0 : *std::max_element( values.begin(), values.end() ) );
    if ( call >= 0 )
    {
        if ( syscalls_known )
        {
            printf( " syscalls_mean=%.3f syscalls_max=%lld", syscall_sum / n, (long long) syscall_max );
        }
        else
        {
            printf( " syscalls_mean=na syscalls_max=na" );
        }
        printf( " wake_mean=%.3f os_other=%llu allocs=%llu alloc_frames=%llu trylock_mean=%.3f trylock_busy=%llu lock_waits=%llu lock_wait_us=%.3f",
                (double) wake / n, (unsigned long long) other, (unsigned long long) allocs, (unsigned long long) alloc_frames, (double) trylock / n,
                (unsigned long long) busy, (unsigned long long) lock, (double) lock_ns / 1000.0 );
    }
    printf( "\n" );
}

static int cft_client( int argc, char ** argv )
{
    const char * token_file = cft_arg( argc, argv, "--token-file", "client_frame_time.token" );
    const char * transport = cft_arg( argc, argv, "--transport", NULL );
    const char * condition = cft_arg( argc, argv, "--condition", "clean" );
    const int frames_wanted = atoi( cft_arg( argc, argv, "--frames", "3600" ) );
    const int warmup = atoi( cft_arg( argc, argv, "--warmup", "600" ) );
    const double hz = atof( cft_arg( argc, argv, "--hz", "60" ) );
    const int hitch_every = atoi( cft_arg( argc, argv, "--hitch-every", "0" ) );
    const double hitch_ms = atof( cft_arg( argc, argv, "--hitch-ms", "100" ) );
    const double second_hitch_us = atof( cft_arg( argc, argv, "--second-hitch-us", "1000" ) );
    const char * plant_call_name = cft_arg( argc, argv, "--plant-call", "update" );
    const char * plant_kind_name = cft_arg( argc, argv, "--plant", "none" );
    const double plant_us = atof( cft_arg( argc, argv, "--plant-us", "500" ) );
    const double assert_max_us = atof( cft_arg( argc, argv, "--assert-max-us", "0" ) );
    const bool assert_zero_alloc = strcmp( cft_arg( argc, argv, "--assert-zero-alloc", "0" ), "1" ) == 0;
    const char * csv = cft_arg( argc, argv, "--csv", NULL );

    cft_hooks_init();
    cft_syscalls_open();

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    if ( rocketnet_init( NULL, cft_malloc, cft_free ) != ROCKETNET_STATUS_OK )
    {
        cft_fail( "init" );
    }

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES + 2];
    memset( token, 0, sizeof( token ) );
    const double token_deadline = cft_seconds() + 60.0;
    while ( true )
    {
        FILE * file = fopen( token_file, "r" );
        if ( file )
        {
            const bool got = fgets( token, (int) sizeof( token ), file ) != NULL;
            fclose( file );
            if ( got )
            {
                break;
            }
        }
        if ( cft_seconds() > token_deadline )
        {
            cft_fail( "no token file" );
        }
        cft_sleep_ns( 50000000ull );
    }
    token[strcspn( token, "\r\n" )] = '\0';

    struct rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );
    const uint8_t filter[] = CFT_FILTER;
    memcpy( config.packet_filter, filter, sizeof( filter ) );
    config.transport = transport;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &config, &status );
    if ( client && status == ROCKETNET_STATUS_OK )
    {
        status = rocketnet_client_connect( client, token );
    }
    if ( !client || status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "client: %s\n", rocketnet_status_name( status ) );
        cft_fail( "client create or connect" );
    }

    const double connect_deadline = cft_seconds() + 15.0;
    while ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED )
    {
        rocketnet_client_update( client, cft_seconds() );
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
        }
        rocketnet_client_send_packets( client );
        if ( cft_seconds() > connect_deadline || rocketnet_client_state( client ) < ROCKETNET_CLIENT_STATE_DISCONNECTED )
        {
            fprintf( stderr, "client state %s\n", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
            cft_fail( "connect" );
        }
        cft_sleep_ns( 1000000ull );
    }

    // the system call window's own cost: the smallest count an empty window reads

    int64_t baseline = INT64_MAX;
    for ( int i = 0; i < 200; i++ )
    {
        cft_window_t empty;
        cft_window( CFT_CALL_UPDATE, &empty, [] {} );
        baseline = std::min( baseline, empty.syscalls );
    }
    cft_syscall_baseline = baseline > 0 ? baseline : 0;

    int plant_call = CFT_CALL_NONE;
    for ( int i = 1; i < CFT_NUM_CALLS; i++ )
    {
        if ( strcmp( plant_call_name, cft_call_names[i] ) == 0 )
        {
            plant_call = i;
        }
    }
    int plant_kind = CFT_PLANT_NONE;
    plant_kind = strcmp( plant_kind_name, "busy" ) == 0 ? CFT_PLANT_BUSY : plant_kind;
    plant_kind = strcmp( plant_kind_name, "alloc" ) == 0 ? CFT_PLANT_ALLOC : plant_kind;
    plant_kind = strcmp( plant_kind_name, "syscall" ) == 0 ? CFT_PLANT_SYSCALL : plant_kind;

    std::vector<cft_frame_t> frames;
    frames.reserve( (size_t) frames_wanted );
    volatile uint32_t touched = 0;
    const uint8_t input[48] = { 1 };
    const uint64_t frame_ns = (uint64_t) ( 1e9 / hz );
    uint64_t next = cft_now_ns();
    int hitch_offset = -1;
    uint64_t not_connected = 0;

    for ( int frame = -warmup; frame < frames_wanted; frame++ )
    {
        if ( frame == 0 )
        {
            cft_steady.store( true );
            cft_set_plant( plant_call, plant_kind, plant_us, cft_malloc, cft_free );
        }

        if ( hitch_offset >= 0 )
        {
            hitch_offset++;
        }
        if ( frame >= 0 && hitch_every > 0 && frame % hitch_every == hitch_every / 2 )
        {
            cft_sleep_ns( (uint64_t) ( hitch_ms * 1e6 ) );      // the game thread stalls; the I/O thread does not
            hitch_offset = 0;
        }

        cft_frame_t f;
        memset( &f, 0, sizeof( f ) );
        int messages = 0;

        cft_window( CFT_CALL_UPDATE, &f.call[CFT_CALL_UPDATE], [&] { rocketnet_client_update( client, cft_seconds() ); } );
        cft_window( CFT_CALL_RECEIVE, &f.call[CFT_CALL_RECEIVE], [&] {
            const struct rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
            {
                for ( int m = 0; m < batch->num_messages; m++ )
                {
                    touched = touched + batch->data[m][0];
                }
                messages += batch->num_messages;
            }
        } );
        cft_window( CFT_CALL_WRITE, &f.call[CFT_CALL_WRITE], [&] {
            uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE, (int) sizeof( input ) );
            if ( p )
            {
                memcpy( p, input, sizeof( input ) );
                rocketnet_client_finish_message( client, (int) sizeof( input ) );
            }
            if ( ( frame & 7 ) == 0 )
            {
                p = rocketnet_client_start_message( client, 1, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 32 );
                if ( p )
                {
                    memset( p, frame & 0xff, 32 );
                    rocketnet_client_finish_message( client, 32 );
                }
            }
        } );
        cft_window( CFT_CALL_SEND, &f.call[CFT_CALL_SEND], [&] { rocketnet_client_send_packets( client ); } );

        f.connected = rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED;
        if ( frame >= 0 )
        {
            f.messages = messages;
            f.hitch_offset = hitch_offset;
            f.total_us = f.call[CFT_CALL_UPDATE].us + f.call[CFT_CALL_RECEIVE].us + f.call[CFT_CALL_WRITE].us + f.call[CFT_CALL_SEND].us;
            not_connected += f.connected ? 0u : 1u;
            frames.push_back( f );
        }

        // fixed 60 Hz frames; a late frame starts at once, so a hitch is followed by back-to-back catch-up frames

        next += frame_ns;
        const uint64_t now = cft_now_ns();
        if ( now < next )
        {
            cft_sleep_ns( next - now );
        }
        else if ( now - next > 500000000ull )
        {
            next = now;
        }
    }

    cft_set_plant( CFT_CALL_NONE, CFT_PLANT_NONE, 0.0, NULL, NULL );
    cft_steady.store( false );
    const uint64_t allocs_elsewhere = cft_allocs_elsewhere.load();

    printf( "cft transport=%s condition=%s frames=%d hz=%.1f crypto=\"%s\" syscalls=%s syscall_baseline=%lld hooks=%s plant=%s plant_call=%s plant_us=%.1f\n",
            rocketnet_client_transport_name( client ), condition, frames_wanted, hz, cft_crypto_choice(), cft_syscall_source, (long long) cft_syscall_baseline,
            cft_hooks_describe(), plant_kind_name, plant_call_name, plant_us );

    std::vector<double> values;
    for ( int call = 1; call < CFT_NUM_CALLS; call++ )
    {
        values.clear();
        for ( const cft_frame_t & f : frames )
        {
            values.push_back( f.call[call].us );
        }
        cft_report( cft_call_names[call], values, frames, call );
    }
    values.clear();
    uint64_t allocs_total = 0;
    double messages_total = 0.0;
    for ( const cft_frame_t & f : frames )
    {
        values.push_back( f.total_us );
        messages_total += f.messages;
        for ( int call = 1; call < CFT_NUM_CALLS; call++ )
        {
            allocs_total += f.call[call].allocs;
        }
    }
    cft_report( "frame", values, frames, -1 );
    const double worst = values.empty() ? 0.0 : *std::max_element( values.begin(), values.end() );
    printf( "cft messages_per_frame=%.2f not_connected_frames=%llu allocs_in_calls=%llu allocs_elsewhere=%llu final_state=%s\n",
            messages_total / (double) ( frames.empty() ? 1 : frames.size() ), (unsigned long long) not_connected, (unsigned long long) allocs_total,
            (unsigned long long) allocs_elsewhere, rocketnet_client_state_name( rocketnet_client_state( client ) ) );

    if ( hitch_every > 0 )
    {
        // per offset from the hitch: the frame's call time and messages, median and worst over hitches

        for ( int offset = 0; offset <= 8; offset++ )
        {
            std::vector<double> at;
            std::vector<double> messages_at;
            for ( const cft_frame_t & f : frames )
            {
                if ( f.hitch_offset == offset )
                {
                    at.push_back( f.total_us );
                    messages_at.push_back( (double) f.messages );
                }
            }
            printf( "cft hitch offset=%d hitches=%zu total_p50_us=%.2f total_max_us=%.2f update_max_us=%.2f messages_p50=%.0f messages_max=%.0f\n", offset,
                    at.size(), cft_percentile( at, 50.0 ), at.empty() ? 0.0 : *std::max_element( at.begin(), at.end() ),
                    [&] {
                        double m = 0.0;
                        for ( const cft_frame_t & f : frames )
                        {
                            m = f.hitch_offset == offset ? std::max( m, f.call[CFT_CALL_UPDATE].us ) : m;
                        }
                        return m;
                    }(),
                    cft_percentile( messages_at, 50.0 ), messages_at.empty() ? 0.0 : *std::max_element( messages_at.begin(), messages_at.end() ) );
        }
        uint64_t second = 0;
        double worst_after = 0.0;
        for ( const cft_frame_t & f : frames )
        {
            if ( f.hitch_offset > 8 )
            {
                second += f.total_us > second_hitch_us ? 1u : 0u;
                worst_after = std::max( worst_after, f.total_us );
            }
        }
        printf( "cft hitch second_hitch_frames=%llu threshold_us=%.0f worst_after_recovery_us=%.2f\n", (unsigned long long) second, second_hitch_us, worst_after );
    }

    if ( csv )
    {
        FILE * file = fopen( csv, "w" );
        if ( file )
        {
            fprintf( file, "frame,hitch_offset,messages,update_us,receive_us,write_us,send_us,update_sys,send_sys,update_wake,send_wake,allocs\n" );
            for ( size_t i = 0; i < frames.size(); i++ )
            {
                const cft_frame_t & f = frames[i];
                fprintf( file, "%zu,%d,%d,%.2f,%.2f,%.2f,%.2f,%lld,%lld,%llu,%llu,%llu\n", i, f.hitch_offset, f.messages, f.call[1].us, f.call[2].us, f.call[3].us,
                         f.call[4].us, (long long) f.call[1].syscalls, (long long) f.call[4].syscalls, (unsigned long long) f.call[1].wake,
                         (unsigned long long) f.call[4].wake,
                         (unsigned long long) ( f.call[1].allocs + f.call[2].allocs + f.call[3].allocs + f.call[4].allocs ) );
            }
            fclose( file );
        }
    }

    (void) touched;
    rn_cli_client_stop_and_destroy( client, cft_seconds() );
    rocketnet_term();

    int result = 0;
    if ( assert_zero_alloc && allocs_total > 0 )
    {
        printf( "cft ASSERT FAILED: %llu allocations inside client calls in steady state\n", (unsigned long long) allocs_total );
        result = 2;
    }
    if ( assert_max_us > 0.0 && worst > assert_max_us )
    {
        printf( "cft ASSERT FAILED: worst client frame %.2f us is over %.2f us\n", worst, assert_max_us );
        result = result ? result : 3;
    }
    if ( not_connected > 0 )
    {
        printf( "cft ASSERT FAILED: %llu measured frames were not connected\n", (unsigned long long) not_connected );
        result = result ? result : 4;
    }
    return result;
}

int main( int argc, char ** argv )
{
#if defined( _WIN32 )
    WSADATA wsa;
    WSAStartup( MAKEWORD( 2, 2 ), &wsa );
#endif // #if defined( _WIN32 )

    if ( argc >= 2 && strcmp( argv[1], "server" ) == 0 )
    {
        return cft_server( argc, argv );
    }
    if ( argc >= 2 && strcmp( argv[1], "client" ) == 0 )
    {
        return cft_client( argc, argv );
    }
    fprintf( stderr, "usage: client_frame_time server|client [--option value ...] (benchmarks/client_frame_time.md)\n" );
    return 1;
}
