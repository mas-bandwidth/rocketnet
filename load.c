/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Load: single-process many-client load harness driving the public API.
    Stands up one public server (rocketnet_server_t) and N public clients
    (rocketnet_client_t, N up to ROCKETNET_MAX_CLIENTS = 1000) in one process
    on loopback, connects every client within 10 seconds, drives the selected
    profile (10 Hz updates, one message per client per update in each
    direction: the client-to-server message rotates through all eight
    reliability modes via client_next_combo, and the server-to-client
    message is reliable-ordered) for a configured duration, and verifies
    five pass criteria:
      1. connect_within_10s
      2. no_unexpected_disconnect
      3. reliable_delivery
      4. unreliable_integrity
      5. counters_agree

    Configuration:
    LOAD_PROFILE=default (the default) keeps the SDK's default message and
    transport sizing. LOAD_PROFILE=light keeps the same offered traffic and
    pass criteria but sizes both endpoints for the harness's 32-byte messages,
    eight reliability cases, and one transport fragment slot. The server's
    max_clients and 10-second handshake deadline are set in both profiles.

    Linux. ROCKETNET_SERVER_TRANSPORT pins the server transport and
    ROCKETNET_CLIENT_TRANSPORT=portable the clients; the summary line's
    server_transport_io= names the transport granted, and fds_end and threads_end
    equal fds_start and threads_start on a clean run. The server cells:
      portable   ROCKETNET_SERVER_TRANSPORT=portable
      gso        ROCKETNET_SERVER_TRANSPORT=gso
      gso plain  ROCKETNET_SERVER_TRANSPORT=gso ROCKETNET_TEST_GSO_PLAIN=1: the
                 development library load links skips the UDP_SEGMENT and
                 UDP_GRO probes (sdk/rocketnet_server_gso_linux.cpp:627-629)
      io_uring   ROCKETNET_SERVER_TRANSPORT=io_uring, where
                 /proc/sys/kernel/io_uring_disabled reads 0
      xdp        ROCKETNET_SERVER_TRANSPORT=xdp ROCKETNET_SERVER_NUM_XDP_QUEUES=1
                 ROCKETNET_SERVER_NUM_OS_CPUS=1, run as root inside a virtme-ng
                 guest booted with irqaffinity=0 isolcpus=1 nohz_full=1, on a
                 veth pair whose far end sits in its own namespace:
                 LOAD_SERVER_ADDRESS=<near address>:<port> binds the server
                 (load.c; xdp discovers no port, sdk/rocketnet_drivers.inc:7)
                 and LOAD_CLIENT_NETNS=/var/run/netns/<name> opens every client
                 socket in the far namespace.
    load raises RLIMIT_NOFILE to 65536 or the hard limit and refuses by name
    when the result is below 2 fds per client plus 64, so `ulimit -n 512`
    fails a 1000-client run before it starts.
    .github/workflows/nightly-soak.yml runs `load 1000 60` once per cell.
*/

/* the title's protocol id: the server config's and every token's */
#define LOAD_PROTOCOL_ID 0x1122334455667788ULL

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE     /* setns, for LOAD_CLIENT_NETNS */
#endif

#include "examples/shutdown.h"
#include "rocketnet.h"
#include "test/portable_crt.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <fcntl.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libproc.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif

#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

#define LOAD_NUM_COMBOS         8
#define LOAD_PAYLOAD_BYTES      32
#define LOAD_MAGIC_CLIENT_TO_SV 0x524E4C44u
#define LOAD_MAGIC_SV_TO_CLIENT 0x524E5356u
#define LOAD_CONNECT_DEADLINE_S 10.0
#define LOAD_MAX_SEQ_TRACKED    4096u
#define LOAD_TIMING_BUCKET_NS   1000ULL
#define LOAD_TIMING_MAX_BUCKET_US 100000ULL
#define LOAD_TIMING_BUCKET_COUNT ( LOAD_TIMING_MAX_BUCKET_US + 1 )

enum load_timing_call_t
{
    LOAD_TIMING_CLIENT_UPDATE,
    LOAD_TIMING_CLIENT_SEND_PACKETS,
    LOAD_TIMING_CLIENT_STATE,
    LOAD_TIMING_CLIENT_INDEX,
    LOAD_TIMING_CLIENT_RECEIVE_MESSAGES,
    LOAD_TIMING_CLIENT_RECEIPTS,
    LOAD_TIMING_CLIENT_START_MESSAGE,
    LOAD_TIMING_CLIENT_FINISH_MESSAGE,
    LOAD_TIMING_SERVER_UPDATE,
    LOAD_TIMING_SERVER_SEND_PACKETS,
    LOAD_TIMING_SERVER_CLIENT_CONNECTED,
    LOAD_TIMING_SERVER_NUM_CONNECTED_CLIENTS,
    LOAD_TIMING_SERVER_RECEIVE_MESSAGES,
    LOAD_TIMING_SERVER_START_MESSAGE,
    LOAD_TIMING_SERVER_FINISH_MESSAGE,
    LOAD_TIMING_SERVER_DISCONNECT_REASON,
    LOAD_TIMING_CLOCK_PAIR,
    LOAD_TIMING_CALL_COUNT
};

struct load_timing_t
{
    uint64_t * buckets;
    int enabled;
    uint64_t count[LOAD_TIMING_CALL_COUNT];
    uint64_t max_ns[LOAD_TIMING_CALL_COUNT];
    uint64_t overbudget_count[LOAD_TIMING_CALL_COUNT];
};

static const char * load_timing_names[LOAD_TIMING_CALL_COUNT] =
{
    "client_update",
    "client_send_packets",
    "client_state",
    "client_index",
    "client_receive_messages",
    "client_receipts",
    "client_start_message",
    "client_finish_message",
    "server_update",
    "server_send_packets",
    "server_client_connected",
    "server_num_connected_clients",
    "server_receive_messages",
    "server_start_message",
    "server_finish_message",
    "server_disconnect_reason",
    "clock_pair"
};

static uint64_t load_clock_ns( void )
{
#if defined(_WIN32)
    LARGE_INTEGER counter;
    static LARGE_INTEGER frequency;
    if ( frequency.QuadPart == 0 && !QueryPerformanceFrequency( &frequency ) )
    {
        fprintf( stderr, "load: QueryPerformanceFrequency failed; timing incomplete\n" );
        exit( 2 );
    }
    if ( !QueryPerformanceCounter( &counter ) )
    {
        fprintf( stderr, "load: QueryPerformanceCounter failed; timing incomplete\n" );
        exit( 2 );
    }
    const uint64_t ticks = (uint64_t) counter.QuadPart;
    const uint64_t hz = (uint64_t) frequency.QuadPart;
    return ticks / hz * 1000000000ULL + ticks % hz * 1000000000ULL / hz;
#else
    struct timespec value;
    if ( clock_gettime( CLOCK_MONOTONIC, &value ) != 0 )
    {
        fprintf( stderr, "load: clock_gettime failed; timing incomplete\n" );
        exit( 2 );
    }
    return (uint64_t) value.tv_sec * 1000000000ULL + (uint64_t) value.tv_nsec;
#endif
}

static void load_timing_record_elapsed( struct load_timing_t * timing, enum load_timing_call_t call, uint64_t elapsed_ns )
{
    const uint64_t bucket = elapsed_ns / LOAD_TIMING_BUCKET_NS;
    const uint64_t bounded_bucket = bucket <= LOAD_TIMING_MAX_BUCKET_US ? bucket : LOAD_TIMING_MAX_BUCKET_US;
    timing->buckets[(size_t) call * (size_t) LOAD_TIMING_BUCKET_COUNT + (size_t) bounded_bucket]++;
    timing->count[call]++;
    if ( elapsed_ns > timing->max_ns[call] )
        timing->max_ns[call] = elapsed_ns;
    if ( elapsed_ns >= 100000ULL )
        timing->overbudget_count[call]++;
}

static void load_timing_record( struct load_timing_t * timing, enum load_timing_call_t call, uint64_t start_ns )
{
    const uint64_t end_ns = load_clock_ns();
    load_timing_record_elapsed( timing, call, end_ns >= start_ns ? end_ns - start_ns : 0 );
}

static int load_timing_init( struct load_timing_t * timing )
{
    memset( timing, 0, sizeof( *timing ) );
    const size_t bucket_count = (size_t) LOAD_TIMING_BUCKET_COUNT;
    if ( bucket_count > SIZE_MAX / (size_t) LOAD_TIMING_CALL_COUNT / sizeof( uint64_t ) )
        return 0;
    timing->buckets = (uint64_t *) calloc( bucket_count * (size_t) LOAD_TIMING_CALL_COUNT, sizeof( uint64_t ) );
    return timing->buckets != NULL;
}

static uint64_t load_timing_quantile_upper_us( const struct load_timing_t * timing, enum load_timing_call_t call, unsigned percentile )
{
    const uint64_t count = timing->count[call];
    if ( count == 0 )
        return 0;
    const uint64_t target = count * percentile / 100;
    uint64_t seen = 0;
    const uint64_t offset = (uint64_t) call * LOAD_TIMING_BUCKET_COUNT;
    for ( uint64_t bucket = 0; bucket <= LOAD_TIMING_MAX_BUCKET_US; bucket++ )
    {
        seen += timing->buckets[(size_t) ( offset + bucket )];
        if ( seen > target )
        {
            if ( bucket == LOAD_TIMING_MAX_BUCKET_US )
                return ( timing->max_ns[call] + 999ULL ) / 1000ULL;
            return bucket + 1;
        }
    }
    return ( timing->max_ns[call] + 999ULL ) / 1000ULL;
}

static void load_timing_print( const struct load_timing_t * timing )
{
    for ( int call = 0; call < LOAD_TIMING_CLOCK_PAIR; call++ )
    {
        printf( " frame_%s_count=%" PRIu64 " frame_%s_p50_upper_us=%" PRIu64
                " frame_%s_p99_upper_us=%" PRIu64 " frame_%s_max_us=%.3f frame_%s_overbudget_count=%" PRIu64,
                load_timing_names[call], timing->count[call],
                load_timing_names[call], load_timing_quantile_upper_us( timing, (enum load_timing_call_t) call, 50 ),
                load_timing_names[call], load_timing_quantile_upper_us( timing, (enum load_timing_call_t) call, 99 ),
                load_timing_names[call], (double) timing->max_ns[call] / 1000.0,
                load_timing_names[call], timing->overbudget_count[call] );
    }
    printf( " clock_pair_count=%" PRIu64 " clock_pair_p50_upper_us=%" PRIu64
            " clock_pair_p99_upper_us=%" PRIu64 " clock_pair_max_us=%.3f clock_pair_overbudget_count=%" PRIu64,
            timing->count[LOAD_TIMING_CLOCK_PAIR],
            load_timing_quantile_upper_us( timing, LOAD_TIMING_CLOCK_PAIR, 50 ),
            load_timing_quantile_upper_us( timing, LOAD_TIMING_CLOCK_PAIR, 99 ),
            (double) timing->max_ns[LOAD_TIMING_CLOCK_PAIR] / 1000.0,
            timing->overbudget_count[LOAD_TIMING_CLOCK_PAIR] );
}

#define LOAD_TIMED_VOID( timing, call, expression ) do { \
    if ( (timing)->enabled ) { \
        const uint64_t load_timing_start_ns = load_clock_ns(); \
        expression; \
        load_timing_record( (timing), (call), load_timing_start_ns ); \
    } else { expression; } \
} while ( 0 )

#define LOAD_TIMED_VALUE( timing, call, output, expression ) do { \
    if ( (timing)->enabled ) { \
        const uint64_t load_timing_start_ns = load_clock_ns(); \
        (output) = (expression); \
        load_timing_record( (timing), (call), load_timing_start_ns ); \
    } else { (output) = (expression); } \
} while ( 0 )

static int load_timing_selftest( void )
{
    struct load_timing_t timing;
    memset( &timing, 0, sizeof( timing ) );
    timing.buckets = (uint64_t *) calloc( (size_t) LOAD_TIMING_BUCKET_COUNT, sizeof( uint64_t ) );
    if ( !timing.buckets )
        return 0;

    int calls = 0;
    LOAD_TIMED_VOID( &timing, LOAD_TIMING_CLIENT_UPDATE, calls++ );
    int ok = calls == 1 && timing.count[LOAD_TIMING_CLIENT_UPDATE] == 0 &&
             load_timing_quantile_upper_us( &timing, LOAD_TIMING_CLIENT_UPDATE, 50 ) == 0;

    load_timing_record_elapsed( &timing, LOAD_TIMING_CLIENT_UPDATE, 99999ULL );
    load_timing_record_elapsed( &timing, LOAD_TIMING_CLIENT_UPDATE, 100000ULL );
    load_timing_record_elapsed( &timing, LOAD_TIMING_CLIENT_UPDATE, 100000001ULL );
    ok = ok && timing.count[LOAD_TIMING_CLIENT_UPDATE] == 3 &&
         timing.overbudget_count[LOAD_TIMING_CLIENT_UPDATE] == 2 &&
         timing.max_ns[LOAD_TIMING_CLIENT_UPDATE] == 100000001ULL &&
         load_timing_quantile_upper_us( &timing, LOAD_TIMING_CLIENT_UPDATE, 50 ) == 101 &&
         load_timing_quantile_upper_us( &timing, LOAD_TIMING_CLIENT_UPDATE, 99 ) == 100001;

    timing.enabled = 0;
    LOAD_TIMED_VOID( &timing, LOAD_TIMING_CLIENT_UPDATE, calls++ );
    ok = ok && calls == 2 && timing.count[LOAD_TIMING_CLIENT_UPDATE] == 3;
    free( timing.buckets );
    return ok;
}

static enum rocketnet_client_state_t timed_client_state( struct load_timing_t * timing, struct rocketnet_client_t * client )
{
    enum rocketnet_client_state_t result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_CLIENT_STATE, result, rocketnet_client_state( client ) );
    return result;
}

static int timed_client_index( struct load_timing_t * timing, struct rocketnet_client_t * client )
{
    int result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_CLIENT_INDEX, result, rocketnet_client_index( client ) );
    return result;
}

static int timed_server_num_connected_clients( struct load_timing_t * timing, struct rocketnet_server_t * server )
{
    int result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_SERVER_NUM_CONNECTED_CLIENTS, result, rocketnet_server_num_connected_clients( server ) );
    return result;
}

static int timed_server_client_connected( struct load_timing_t * timing, struct rocketnet_server_t * server, int client_index )
{
    int result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_SERVER_CLIENT_CONNECTED, result, rocketnet_server_client_connected( server, client_index ) );
    return result;
}

static const struct rocketnet_message_batch_t * timed_client_receive_messages( struct load_timing_t * timing, struct rocketnet_client_t * client )
{
    const struct rocketnet_message_batch_t * result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_CLIENT_RECEIVE_MESSAGES, result, rocketnet_client_receive_messages( client ) );
    return result;
}

static const struct rocketnet_message_batch_t * timed_server_receive_messages( struct load_timing_t * timing, struct rocketnet_server_t * server )
{
    const struct rocketnet_message_batch_t * result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_SERVER_RECEIVE_MESSAGES, result, rocketnet_server_receive_messages( server ) );
    return result;
}

static int timed_client_receipts( struct load_timing_t * timing, struct rocketnet_client_t * client, struct rocketnet_receipt_t * receipts, int capacity )
{
    int result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_CLIENT_RECEIPTS, result, rocketnet_client_receipts( client, receipts, capacity ) );
    return result;
}

static uint8_t * timed_client_start_message( struct load_timing_t * timing, struct rocketnet_client_t * client, int channel, enum rocketnet_reliability_t reliability, int bytes )
{
    uint8_t * result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_CLIENT_START_MESSAGE, result, rocketnet_client_start_message( client, channel, reliability, bytes ) );
    return result;
}

static int timed_client_finish_message( struct load_timing_t * timing, struct rocketnet_client_t * client, int bytes )
{
    int result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_CLIENT_FINISH_MESSAGE, result, rocketnet_client_finish_message( client, bytes ) );
    return result;
}

static uint8_t * timed_server_start_message( struct load_timing_t * timing, struct rocketnet_server_t * server, uint64_t client_id, int channel, enum rocketnet_reliability_t reliability, int bytes )
{
    uint8_t * result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_SERVER_START_MESSAGE, result, rocketnet_server_start_message( server, client_id, channel, reliability, bytes ) );
    return result;
}

static int timed_server_finish_message( struct load_timing_t * timing, struct rocketnet_server_t * server, int bytes )
{
    int result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_SERVER_FINISH_MESSAGE, result, rocketnet_server_finish_message( server, bytes ) );
    return result;
}

static enum rocketnet_disconnect_reason_t timed_server_disconnect_reason( struct load_timing_t * timing, struct rocketnet_server_t * server, int client_index )
{
    enum rocketnet_disconnect_reason_t result;
    LOAD_TIMED_VALUE( timing, LOAD_TIMING_SERVER_DISCONNECT_REASON, result, rocketnet_server_disconnect_reason( server, client_index ) );
    return result;
}

static volatile sig_atomic_t quit = 0;

static void interrupt_handler( int sig )
{
    (void) sig;
    quit = 1;
}

#define LOAD_FD_MARGIN 64

static long count_open_fds( void )
{
#if defined(_WIN32)
    DWORD handles = 0;
    if ( GetProcessHandleCount( GetCurrentProcess(), &handles ) )
        return (long) handles;
    return -1;
#elif defined(__APPLE__)
    const int bytes = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, NULL, 0 );
    if ( bytes <= 0 )
        return -1;
    struct proc_fdinfo * fds = (struct proc_fdinfo *) malloc( (size_t) bytes );
    if ( !fds )
        return -1;
    const int used = proc_pidinfo( getpid(), PROC_PIDLISTFDS, 0, fds, bytes );
    free( fds );
    if ( used < 0 )
        return -1;
    return (long) ( used / (int) PROC_PIDLISTFD_SIZE );
#else
    long cap = 65536;
    struct rlimit rl;
    if ( getrlimit( RLIMIT_NOFILE, &rl ) == 0 && rl.rlim_cur != RLIM_INFINITY &&
         rl.rlim_cur < (rlim_t) cap )
        cap = (long) rl.rlim_cur;
    long count = 0;
    for ( long fd = 0; fd < cap; fd++ )
    {
        errno = 0;
        if ( fcntl( (int) fd, F_GETFD ) != -1 || errno != EBADF )
            count++;
    }
    return count;
#endif
}

static long count_threads( void )
{
#if defined(__APPLE__)
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    if ( task_threads( mach_task_self(), &threads, &count ) != KERN_SUCCESS )
        return -1;
    for ( mach_msg_type_number_t i = 0; i < count; i++ )
        (void) mach_port_deallocate( mach_task_self(), threads[i] );
    (void) vm_deallocate( mach_task_self(), (vm_address_t) threads,
                          (vm_size_t) ( count * sizeof( thread_act_t ) ) );
    return (long) count;
#elif defined(__linux__)
    FILE * f = fopen( "/proc/self/status", "r" );
    if ( f )
    {
        char line[256];
        long threads = -1;
        while ( fgets( line, sizeof( line ), f ) )
        {
            if ( strncmp( line, "Threads:", 8 ) == 0 )
            {
                threads = strtol( line + 8, NULL, 10 );
                break;
            }
        }
        fclose( f );
        return threads;
    }
    return -1;
#elif defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( snapshot == INVALID_HANDLE_VALUE )
        return -1;
    THREADENTRY32 entry;
    entry.dwSize = sizeof( entry );
    const DWORD self_pid = GetCurrentProcessId();
    long count = 0;
    if ( Thread32First( snapshot, &entry ) )
    {
        do
        {
            if ( entry.th32OwnerProcessID == self_pid )
                count++;
        } while ( Thread32Next( snapshot, &entry ) );
    }
    CloseHandle( snapshot );
    return count;
#else
    return -1;
#endif
}

static void sample_resources( long * peak_fds, long * peak_threads )
{
    const long fds = count_open_fds();
    const long threads = count_threads();
    if ( fds > *peak_fds )
        *peak_fds = fds;
    if ( threads > *peak_threads )
        *peak_threads = threads;
}

#if !defined(_WIN32)
struct load_fd_limit_t
{
    rlim_t soft;
    rlim_t hard;
};

static void format_rlim( rlim_t value, char * out, size_t out_size )
{
    if ( value == RLIM_INFINITY )
        snprintf( out, out_size, "infinity" );
    else
        snprintf( out, out_size, "%llu", (unsigned long long) value );
}

static int ensure_fd_limit( int num_clients, struct load_fd_limit_t * limit )
{
    const rlim_t required = (rlim_t) num_clients * 2 + (rlim_t) LOAD_FD_MARGIN;
    struct rlimit rl;
    if ( getrlimit( RLIMIT_NOFILE, &rl ) != 0 )
    {
        fprintf( stderr, "load: getrlimit(RLIMIT_NOFILE) failed: %s\n", strerror( errno ) );
        return 0;
    }

    rlim_t target = 65536;
    if ( target < required )
        target = required;
    if ( rl.rlim_max != RLIM_INFINITY && rl.rlim_max < target )
        target = rl.rlim_max;
    if ( rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur < target )
    {
        rl.rlim_cur = target;
        (void) setrlimit( RLIMIT_NOFILE, &rl );
    }

    if ( getrlimit( RLIMIT_NOFILE, &rl ) != 0 )
    {
        fprintf( stderr, "load: getrlimit(RLIMIT_NOFILE) failed after raise: %s\n", strerror( errno ) );
        return 0;
    }
    limit->soft = rl.rlim_cur;
    limit->hard = rl.rlim_max;

    if ( rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= required )
        return 1;

    char soft_text[32];
    char hard_text[32];
    format_rlim( rl.rlim_cur, soft_text, sizeof( soft_text ) );
    format_rlim( rl.rlim_max, hard_text, sizeof( hard_text ) );
    fprintf( stderr,
             "load: RLIMIT_NOFILE soft=%s hard=%s cannot cover clients=%d (2 fds per client) "
             "plus margin=%d (need soft >= %llu)\n",
             soft_text, hard_text, num_clients, LOAD_FD_MARGIN, (unsigned long long) required );
    fprintf( stderr, "load: raise it with: ulimit -n %llu\n", (unsigned long long) required );
    fprintf( stderr,
             "load: if the hard limit blocks that, raise the hard limit in a new shell "
             "(macOS: sudo launchctl limit maxfiles 65536 65536)\n" );
    return 0;
}
#endif

static uint64_t query_rss_bytes( void )
{
#if defined(__linux__)
    FILE * f = fopen( "/proc/self/statm", "r" );
    if ( f )
    {
        unsigned long size_pages = 0;
        unsigned long rss_pages = 0;
        if ( fscanf( f, "%lu %lu", &size_pages, &rss_pages ) == 2 )
        {
            fclose( f );
            long page_size = sysconf( _SC_PAGESIZE );
            if ( page_size > 0 )
                return (uint64_t) rss_pages * (uint64_t) page_size;
        }
        else
        {
            fclose( f );
        }
    }
#endif
#if !defined(_WIN32)
    struct rusage ru;
    if ( getrusage( RUSAGE_SELF, &ru ) == 0 )
    {
#if defined(__APPLE__)
        return (uint64_t) ru.ru_maxrss;
#else
        return (uint64_t) ru.ru_maxrss * 1024ULL;
#endif
    }
    return 0;
#else
    PROCESS_MEMORY_COUNTERS pmc;
    if ( GetProcessMemoryInfo( GetCurrentProcess(), &pmc, sizeof( pmc ) ) )
        return (uint64_t) pmc.WorkingSetSize;
    return 0;
#endif
}

static void write_u32_le( uint8_t * dst, uint32_t v )
{
    dst[0] = (uint8_t) ( v & 0xFFu );
    dst[1] = (uint8_t) ( ( v >> 8 ) & 0xFFu );
    dst[2] = (uint8_t) ( ( v >> 16 ) & 0xFFu );
    dst[3] = (uint8_t) ( ( v >> 24 ) & 0xFFu );
}

static uint32_t read_u32_le( const uint8_t * src )
{
    return (uint32_t) src[0] |
           ( (uint32_t) src[1] << 8 ) |
           ( (uint32_t) src[2] << 16 ) |
           ( (uint32_t) src[3] << 24 );
}

static uint8_t pattern_byte( uint32_t magic, uint32_t client_idx, uint32_t combo_idx, uint32_t seq, int offset )
{
    uint32_t h = magic ^ ( client_idx * 2654435761u ) ^ ( combo_idx * 2246822519u ) ^ ( seq * 3266489917u ) ^ (uint32_t) ( offset * 374761393u );
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    return (uint8_t) ( h & 0xFFu );
}

static void build_message( uint8_t * buf, uint32_t magic, uint32_t client_idx, uint32_t combo_idx, uint32_t seq )
{
    write_u32_le( buf + 0, magic );
    write_u32_le( buf + 4, client_idx );
    write_u32_le( buf + 8, combo_idx );
    write_u32_le( buf + 12, seq );
    for ( int i = 16; i < LOAD_PAYLOAD_BYTES; i++ )
        buf[i] = pattern_byte( magic, client_idx, combo_idx, seq, i );
}

static int verify_message( const uint8_t * buf, int bytes, uint32_t expected_magic,
                           uint32_t * out_client, uint32_t * out_combo, uint32_t * out_seq )
{
    if ( !buf || bytes != LOAD_PAYLOAD_BYTES )
        return 0;
    const uint32_t magic = read_u32_le( buf + 0 );
    if ( magic != expected_magic )
        return 0;
    const uint32_t client_idx = read_u32_le( buf + 4 );
    const uint32_t combo_idx = read_u32_le( buf + 8 );
    const uint32_t seq = read_u32_le( buf + 12 );
    for ( int i = 16; i < LOAD_PAYLOAD_BYTES; i++ )
    {
        if ( buf[i] != pattern_byte( magic, client_idx, combo_idx, seq, i ) )
            return 0;
    }
    *out_client = client_idx;
    *out_combo = combo_idx;
    *out_seq = seq;
    return 1;
}

static int combo_is_reliable_every( uint32_t combo )
{
    return combo == ROCKETNET_RELIABILITY_RELIABLE ||
           combo == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           combo == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ||
           combo == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static int combo_is_ordered( uint32_t combo )
{
    return combo == ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
           combo == ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT;
}

static int combo_is_unreliable_family( uint32_t combo )
{
    return combo == ROCKETNET_RELIABILITY_UNRELIABLE ||
           combo == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
           combo == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT;
}

struct load_stream_state_t
{
    uint32_t sent_count;
    uint32_t recv_count;
    uint32_t max_seq_seen;
    uint8_t seen_bitmap[LOAD_MAX_SEQ_TRACKED / 8];
};

static int stream_seen_test_and_set( struct load_stream_state_t * st, uint32_t seq )
{
    if ( seq == 0 || seq >= LOAD_MAX_SEQ_TRACKED )
        return -1;
    const uint32_t byte_idx = seq >> 3;
    const uint8_t mask = (uint8_t) ( 1u << ( seq & 7u ) );
    if ( st->seen_bitmap[byte_idx] & mask )
        return 1;
    st->seen_bitmap[byte_idx] |= mask;
    return 0;
}

static int cmp_double( const void * a, const void * b )
{
    const double da = *(const double *) a;
    const double db = *(const double *) b;
    return ( da > db ) - ( da < db );
}

/* The stats structs' counters, as name/value tables for the diagnostics:
   one stats call fills the struct, and the table walks its fields. */
#define LOAD_SERVER_COUNTER_FIELDS( X ) \
    X( packets_sent ) X( packets_received ) X( bytes_sent ) X( bytes_received ) \
    X( connections_accepted ) X( connections_denied_full ) X( handshakes_dropped_over_budget ) \
    X( payloads_received ) X( messages_resent ) X( messages_duplicated ) \
    X( receipts_acked ) X( receipts_lost ) X( messages_dropped_over_budget ) X( messages_dropped_not_connected )

#define LOAD_CLIENT_COUNTER_FIELDS( X ) \
    X( packets_sent ) X( packets_received ) X( bytes_sent ) X( bytes_received ) \
    X( payloads_received ) X( messages_resent ) X( messages_duplicated ) \
    X( receipts_acked ) X( receipts_lost ) X( messages_dropped_over_budget ) \
    X( messages_dropped_not_connected )

#define LOAD_COUNT_ONE( name ) + 1
#define LOAD_NAME_ONE( name ) #name,
#define LOAD_VALUE_ONE( name ) values[k++] = stats.name;

enum { LOAD_SERVER_COUNTERS = 0 LOAD_SERVER_COUNTER_FIELDS( LOAD_COUNT_ONE ) };
enum { LOAD_CLIENT_COUNTERS = 0 LOAD_CLIENT_COUNTER_FIELDS( LOAD_COUNT_ONE ) };

static const char * server_counter_names[LOAD_SERVER_COUNTERS] = { LOAD_SERVER_COUNTER_FIELDS( LOAD_NAME_ONE ) };
static const char * client_counter_names[LOAD_CLIENT_COUNTERS] = { LOAD_CLIENT_COUNTER_FIELDS( LOAD_NAME_ONE ) };

static void server_counter_values( const struct rocketnet_server_t * server, uint64_t values[LOAD_SERVER_COUNTERS] )
{
    struct rocketnet_server_stats_t stats;
    rocketnet_server_stats( server, &stats );
    int k = 0;
    LOAD_SERVER_COUNTER_FIELDS( LOAD_VALUE_ONE )
}

static void client_counter_values( const struct rocketnet_client_t * client, uint64_t values[LOAD_CLIENT_COUNTERS] )
{
    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( client, &stats );
    int k = 0;
    LOAD_CLIENT_COUNTER_FIELDS( LOAD_VALUE_ONE )
}

int main( int argc, char ** argv )
{
    if ( argc < 3 || argc > 4 )
    {
        fprintf( stderr, "usage: load <clients> <seconds> [drop-one]\n" );
        return 2;
    }

    char * end_clients = NULL;
    errno = 0;
    long long parsed_clients = strtoll( argv[1], &end_clients, 10 );
    if ( argv[1][0] == 0 || !end_clients || *end_clients != 0 || errno != 0 )
    {
        fprintf( stderr, "load: invalid client count %s (max ROCKETNET_MAX_CLIENTS = %d)\n",
                 argv[1], ROCKETNET_MAX_CLIENTS );
        return 2;
    }
    if ( parsed_clients < 1 || parsed_clients > ROCKETNET_MAX_CLIENTS )
    {
        fprintf( stderr, "load: client count %lld out of range 1..%d (ROCKETNET_MAX_CLIENTS = %d)\n",
                 parsed_clients, ROCKETNET_MAX_CLIENTS, ROCKETNET_MAX_CLIENTS );
        printf( "load: refused clients=%lld exceeds ROCKETNET_MAX_CLIENTS=%d\n",
                parsed_clients, ROCKETNET_MAX_CLIENTS );
        return 2;
    }

    char * end_seconds = NULL;
    errno = 0;
    long long parsed_seconds = strtoll( argv[2], &end_seconds, 10 );
    if ( argv[2][0] == 0 || !end_seconds || *end_seconds != 0 || errno != 0 || parsed_seconds < 1 )
    {
        fprintf( stderr, "load: seconds must be a positive integer\n" );
        return 2;
    }

    int drop_one = 0;
    if ( argc == 4 )
    {
        if ( strcmp( argv[3], "drop-one" ) != 0 )
        {
            fprintf( stderr, "load: unknown option %s (expected drop-one)\n", argv[3] );
            return 2;
        }
        drop_one = 1;
    }

    const int num_clients = (int) parsed_clients;
    const double duration_seconds = (double) parsed_seconds;

    char profile[256] = { 0 };
    const int profile_found = rocketnet_test_getenv( "LOAD_PROFILE", profile, sizeof( profile ) );
    const char * profile_name = profile_found && profile[0] != 0 ? profile : "default";
    if ( strcmp( profile_name, "light" ) != 0 && strcmp( profile_name, "default" ) != 0 )
    {
        fprintf( stderr, "load: LOAD_PROFILE=%s is not light or default\n", profile_name );
        return 2;
    }
    const int light_profile = strcmp( profile_name, "light" ) == 0;

#if !defined(_WIN32)
    struct load_fd_limit_t fd_limit;
    if ( !ensure_fd_limit( num_clients, &fd_limit ) )
        return 1;
    char fd_limit_soft_text[32];
    char fd_limit_hard_text[32];
    format_rlim( fd_limit.soft, fd_limit_soft_text, sizeof( fd_limit_soft_text ) );
    format_rlim( fd_limit.hard, fd_limit_hard_text, sizeof( fd_limit_hard_text ) );
#else
    const char * fd_limit_soft_text = "n/a";
    const char * fd_limit_hard_text = "n/a";
#endif
    signal( SIGINT, interrupt_handler );

    struct load_timing_t timing;
    if ( !load_timing_init( &timing ) )
    {
        fprintf( stderr, "load: timing histogram allocation failed\n" );
        return 1;
    }
    if ( !load_timing_selftest() )
    {
        fprintf( stderr, "load: timing histogram self-test failed\n" );
        free( timing.buckets );
        return 1;
    }
    for ( int i = 0; i < 10000; i++ )
    {
        const uint64_t start_ns = load_clock_ns();
        load_timing_record( &timing, LOAD_TIMING_CLOCK_PAIR, start_ns );
    }

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "load: rocketnet_init failed\n" );
        return 1;
    }
    const long fds_start = count_open_fds();
    const long threads_start = count_threads();
    long fds_peak = fds_start;
    long threads_peak = threads_start;
    char log_level[256];
    if ( !rocketnet_test_getenv( "ROCKETNET_LOG_LEVEL", log_level, sizeof( log_level ) ) )
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    /* Public server and client configurations. The default profile preserves
       SDK defaults; light narrows memory geometry to the workload below. */
    struct rocketnet_server_config_t server_config;
    /* the published development backend key, set by hand (a real server
       brings its backend's key); the server makes its own keypair at create,
       and every client's token is sealed to rocketnet_server_public_key */
    static const uint8_t dev_backend_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = LOAD_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, dev_backend_key, sizeof( server_config.backend_public_key ) );
    server_config.max_clients = num_clients;
    server_config.handshake_deadline_seconds = 10;

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );

    if ( light_profile )
    {
        /* At 1000 clients each endpoint emits one 32-byte message per
           100 ms; the eight reliability cases rotate across updates, so
           payload is 2.56 kbps per direction and 64 kbps is 25x that rate.
           The 100-client slice runs every 10 ms: at 1..100 clients each
           client updates at 100 Hz (25.6 kbps payload; 2.5x budget headroom).
           Intermediate populations follow the number of slices per round. */
        server_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        server_config.max_fragmented_messages = 1;
        server_config.per_client_send_kbps = 64;
        server_config.num_channels = 8;
        server_config.reliable_messages_in_flight = 64;
        server_config.receive_message_buffer_bytes = 4096;
        server_config.max_message_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1;
        server_config.send_message_buffer_bytes = 12288;
        server_config.send_queue_messages = 16;
        server_config.packets_in_flight = 16;

        client_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
        client_config.max_fragmented_messages = 1;
        client_config.send_kbps = 64;
        client_config.num_channels = 8;
        client_config.reliable_messages_in_flight = 64;
        client_config.receive_message_buffer_bytes = 4096;
        client_config.max_message_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1;
        client_config.send_message_buffer_bytes = 12288;
        client_config.send_queue_messages = 16;
        client_config.packets_in_flight = 16;
    }

    const size_t client_public_required_bytes = rocketnet_client_required_memory( &client_config );
    const size_t server_public_required_bytes = rocketnet_server_required_memory( &server_config );

    char server_bind_buf[64];
    char server_bind_env[sizeof( server_bind_buf ) - 2];   // room to append ":0"
    const char * server_bind = server_bind_env;
    if ( !rocketnet_test_getenv( "LOAD_SERVER_ADDRESS", server_bind_env, sizeof( server_bind_env ) ) || server_bind[0] == 0 )
        server_bind = "127.0.0.1:0";
    else if ( !strchr( server_bind, ':' ) )
    {
        snprintf( server_bind_buf, sizeof( server_bind_buf ), "%s:0", server_bind );
        server_bind = server_bind_buf;
    }
    char server_host[64];
    snprintf( server_host, sizeof( server_host ), "%s", server_bind );
    server_host[strcspn( server_host, ":" )] = 0;

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, server_bind, NULL, &server_config, &create_status );
    if ( !server )
    {
        fprintf( stderr, "load: failed to create server: %s\n", rocketnet_status_name( create_status ) );
        rocketnet_term();
        return 1;
    }
    if ( rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "load: failed to start server\n" );
        rn_cli_server_stop_and_destroy( server );
        rocketnet_term();
        return 1;
    }

    char server_address[96];
    snprintf( server_address, sizeof( server_address ), "%s:%d", server_host,
              (int) rocketnet_server_port( server ) );
    uint8_t server_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_key );

    struct rocketnet_client_t ** clients =
        (struct rocketnet_client_t **) calloc( (size_t) num_clients, sizeof( *clients ) );
    int * client_ever_connected = (int *) calloc( (size_t) num_clients, sizeof( int ) );
    int * client_slot = (int *) calloc( (size_t) num_clients, sizeof( int ) );
    double * client_connect_seconds = (double *) malloc( (size_t) num_clients * sizeof( double ) );
    uint32_t * client_next_combo = (uint32_t *) calloc( (size_t) num_clients, sizeof( uint32_t ) );
    struct load_stream_state_t * c2s_streams = (struct load_stream_state_t *) calloc(
        (size_t) num_clients * LOAD_NUM_COMBOS, sizeof( struct load_stream_state_t ) );
    struct load_stream_state_t * s2c_streams = (struct load_stream_state_t *) calloc(
        (size_t) num_clients, sizeof( struct load_stream_state_t ) );

    double * client_last_update = (double *) calloc( (size_t) num_clients, sizeof( double ) );
    double * client_max_update_gap = (double *) calloc( (size_t) num_clients, sizeof( double ) );

    if ( !clients || !client_ever_connected || !client_connect_seconds || !client_next_combo ||
         !c2s_streams || !s2c_streams || !client_last_update || !client_max_update_gap )
    {
        fprintf( stderr, "load: allocation failed\n" );
        return 1;
    }
    for ( int i = 0; i < num_clients; i++ )
        client_connect_seconds[i] = -1.0;

    const uint64_t token_now = (uint64_t) time( NULL );

#if defined(__linux__)
    int self_netns = -1;
    int client_netns = -1;
    char client_netns_path[256];
    if ( rocketnet_test_getenv( "LOAD_CLIENT_NETNS", client_netns_path, sizeof( client_netns_path ) ) && client_netns_path[0] != 0 )
    {
        self_netns = open( "/proc/self/ns/net", O_RDONLY );
        client_netns = open( client_netns_path, O_RDONLY );
        if ( self_netns < 0 || client_netns < 0 )
        {
            fprintf( stderr, "load: cannot open the namespaces /proc/self/ns/net and %s: %s\n", client_netns_path, strerror( errno ) );
            return 1;
        }
    }
#endif

    for ( int i = 0; i < num_clients; i++ )
    {
        char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
#if defined(__linux__)
        if ( client_netns >= 0 && setns( client_netns, CLONE_NEWNET ) != 0 )
        {
            fprintf( stderr, "load: setns into %s failed: %s\n", client_netns_path, strerror( errno ) );
            return 1;
        }
#endif
        if ( rocketnet_create_dev_connect_token( server_address, NULL, server_key, 30, 10, (uint64_t) ( i + 1 ), LOAD_PROTOCOL_ID, token_now, NULL, connect_token ) != ROCKETNET_STATUS_OK )
        {
            fprintf( stderr, "load: failed to create dev connect token for client %d\n", i );
            return 1;
        }
        create_status = ROCKETNET_STATUS_OK;
        clients[i] = rocketnet_client_create( NULL, &client_config, &create_status );
        if ( !clients[i] )
        {
            fprintf( stderr, "load: failed to create client %d: %s\n", i, rocketnet_status_name( create_status ) );
            return 1;
        }
        if ( rocketnet_client_connect( clients[i], connect_token ) != ROCKETNET_STATUS_OK )
        {
            fprintf( stderr, "load: failed to connect client %d\n", i );
            return 1;
        }
#if defined(__linux__)
        if ( client_netns >= 0 && setns( self_netns, CLONE_NEWNET ) != 0 )
        {
            fprintf( stderr, "load: setns back to the server namespace failed: %s\n", strerror( errno ) );
            return 1;
        }
#endif
        client_next_combo[i] = (uint32_t) ( i % LOAD_NUM_COMBOS );
    }
#if defined(__linux__)
    if ( client_netns >= 0 )
    {
        close( client_netns );
        close( self_netns );
    }
#endif
    sample_resources( &fds_peak, &threads_peak );

    int connected_count = 0;
    int all_connected = 0;
    double connect_elapsed = 0.0;
    int unexpected_disconnects = 0;
    int reliable_errors = 0;
    int unreliable_errors = 0;

    uint64_t c2s_sent_total = 0;
    uint64_t c2s_received_total = 0;
    double next_resource_sample = rocketnet_time();

    /* Step 1: Connect all clients within 10s */
    const double start_time = rocketnet_time();
    while ( !quit )
    {
        const double now = rocketnet_time();
        connected_count = 0;
        for ( int i = 0; i < num_clients; i++ )
        {
            LOAD_TIMED_VOID( &timing, LOAD_TIMING_CLIENT_UPDATE, rocketnet_client_update( clients[i], now ) );
            LOAD_TIMED_VOID( &timing, LOAD_TIMING_CLIENT_SEND_PACKETS, rocketnet_client_send_packets( clients[i] ) );
            const enum rocketnet_client_state_t st = timed_client_state( &timing, clients[i] );
            if ( st == ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                /* the time this client is seen connected, read here rather
                   than the sweep's now, so every client keeps its own time */
                if ( client_connect_seconds[i] < 0.0 )
                    client_connect_seconds[i] = rocketnet_time() - start_time;
                client_ever_connected[i] = 1;
                client_slot[i] = timed_client_index( &timing, clients[i] );
                connected_count++;
            }
            else if ( (int) st < 0 || ( client_ever_connected[i] && st != ROCKETNET_CLIENT_STATE_CONNECTED ) )
            {
                unexpected_disconnects++;
            }

            if ( ( ( i + 1 ) & 63 ) == 0 || i + 1 == num_clients )
            {
                const double t0 = rocketnet_time();
                LOAD_TIMED_VOID( &timing, LOAD_TIMING_SERVER_UPDATE, rocketnet_server_update( server, t0 ) );
                const double t1 = rocketnet_time();
                LOAD_TIMED_VOID( &timing, LOAD_TIMING_SERVER_SEND_PACKETS, rocketnet_server_send_packets( server ) );
            }
        }

        if ( connected_count == num_clients &&
             timed_server_num_connected_clients( &timing, server ) == num_clients )
        {
            all_connected = 1;
            connect_elapsed = rocketnet_time() - start_time;
            break;
        }

        if ( rocketnet_time() - start_time > LOAD_CONNECT_DEADLINE_S )
        {
            connect_elapsed = rocketnet_time() - start_time;
            int named = 0;
            for ( int i = 0; i < num_clients && named < 8; i++ )
            {
                const enum rocketnet_client_state_t st = timed_client_state( &timing, clients[i] );
                if ( st != ROCKETNET_CLIENT_STATE_CONNECTED )
                {
                    fprintf( stderr, "load: client %d not connected at the deadline: state=%d %s\n",
                             i, (int) st, rocketnet_client_state_name( st ) );
                    uint64_t values[LOAD_CLIENT_COUNTERS];
                    client_counter_values( clients[i], values );
                    fprintf( stderr, "load: client %d transport=%s packets_sent=%llu packets_received=%llu\n", i,
                             rocketnet_client_transport_name( clients[i] ),
                             (unsigned long long) values[0], (unsigned long long) values[1] );
                    for ( int k = 0; k < LOAD_CLIENT_COUNTERS; k++ )
                    {
                        if ( values[k] )
                            fprintf( stderr, "load: client %d counter %s=%llu\n", i, client_counter_names[k], (unsigned long long) values[k] );
                    }
                    named++;
                }
            }
            fprintf( stderr, "load: server reports %d of %d clients connected at the deadline\n",
                     timed_server_num_connected_clients( &timing, server ), num_clients );
            uint64_t server_values[LOAD_SERVER_COUNTERS];
            server_counter_values( server, server_values );
            for ( int k = 0; k < LOAD_SERVER_COUNTERS; k++ )
            {
                if ( server_values[k] )
                    fprintf( stderr, "load: server counter %s=%llu\n", server_counter_names[k], (unsigned long long) server_values[k] );
            }
            break;
        }
        const double sample_now = rocketnet_time();
        if ( sample_now >= next_resource_sample )
        {
            sample_resources( &fds_peak, &threads_peak );
            next_resource_sample = sample_now + 0.25;
        }
        rocketnet_sleep( 0.001 );
    }

    /* Step 2: Drive traffic for <seconds>, then 3.0s drain for in-flight delivery */
    const double traffic_start = rocketnet_time();
    timing.enabled = 1;
    const double drain_seconds = 3.0;
    const double drop_at = duration_seconds * 0.25 < 1.0 ? duration_seconds * 0.25 : 1.0;
    int drop_injected = 0;
    double max_server_update_s = 0.0;
    double max_server_update_at = 0.0;

    double next_slice_time = traffic_start;
    static int client_update_cursor = 0;

    while ( !quit )
    {
        const double now = rocketnet_time();
        const double elapsed = now - traffic_start;
        if ( elapsed >= duration_seconds + drain_seconds )
            break;

        const int in_traffic_window = ( elapsed < duration_seconds );

        if ( drop_one && !drop_injected && ( elapsed >= drop_at ) )
        {
            rn_cli_client_stop_and_destroy( clients[0] );
            clients[0] = NULL;
            drop_injected = 1;
        }

        /* Visit a slice of up to 100 clients every 10 ms. Per-client rate is
           100 Hz for <=100 clients and 10 Hz at 1000 clients. */
        const int slice = num_clients < 100 ? num_clients : 100;
        for ( int k = 0; k < slice; k++ )
        {
            const int i = client_update_cursor;
            client_update_cursor = ( client_update_cursor + 1 ) % num_clients;
            if ( !( drop_injected && i == 0 ) )
            {
                const double update_time = rocketnet_time();
                if ( client_last_update[i] > 0.0 && update_time - client_last_update[i] > client_max_update_gap[i] )
                    client_max_update_gap[i] = update_time - client_last_update[i];
                client_last_update[i] = update_time;
                LOAD_TIMED_VOID( &timing, LOAD_TIMING_CLIENT_UPDATE, rocketnet_client_update( clients[i], update_time ) );

                const struct rocketnet_message_batch_t * batch = NULL;
                while ( ( batch = timed_client_receive_messages( &timing, clients[i] ) ) != NULL )
                {
                    for ( int j = 0; j < batch->num_messages; j++ )
                    {
                        uint32_t msg_client = 0, msg_combo = 0, msg_seq = 0;
                        if ( batch->client_index[j] != 0 ||
                             !verify_message( batch->data[j], batch->bytes[j], LOAD_MAGIC_SV_TO_CLIENT, &msg_client, &msg_combo, &msg_seq ) ||
                             msg_client != (uint32_t) i || msg_combo != ROCKETNET_RELIABILITY_RELIABLE_ORDERED ||
                             batch->channel[j] != ROCKETNET_RELIABILITY_RELIABLE_ORDERED )
                        {
                            reliable_errors++;
                        }
                        else
                        {
                            struct load_stream_state_t * sv_st = &s2c_streams[i];
                            if ( msg_seq != sv_st->recv_count + 1 || stream_seen_test_and_set( sv_st, msg_seq ) != 0 )
                                reliable_errors++;
                            sv_st->recv_count++;
                            sv_st->max_seq_seen = msg_seq;
                        }
                    }
                }

                struct rocketnet_receipt_t receipts[16];
                (void) timed_client_receipts( &timing, clients[i], receipts, 16 );

                /* On each update, send one message rotating through all eight
                   reliability modes, and server queues one reliable-ordered to client */
                if ( in_traffic_window &&
                     timed_client_state( &timing, clients[i] ) == ROCKETNET_CLIENT_STATE_CONNECTED )
                {
                    const uint32_t combo = client_next_combo[i] % LOAD_NUM_COMBOS;
                    client_next_combo[i]++;
                    struct load_stream_state_t * cst = &c2s_streams[(size_t) i * LOAD_NUM_COMBOS + combo];
                    uint8_t payload[LOAD_PAYLOAD_BYTES];
                    if ( cst->sent_count + 1 < LOAD_MAX_SEQ_TRACKED )
                    {
                        const uint32_t next_seq = cst->sent_count + 1;
                        build_message( payload, LOAD_MAGIC_CLIENT_TO_SV, (uint32_t) i, combo, next_seq );
                        uint8_t * out = timed_client_start_message( &timing, clients[i], (int) combo, (enum rocketnet_reliability_t) combo, LOAD_PAYLOAD_BYTES );
                        if ( out )
                        {
                            memcpy( out, payload, LOAD_PAYLOAD_BYTES );
                            if ( timed_client_finish_message( &timing, clients[i], LOAD_PAYLOAD_BYTES ) != 0 )
                            {
                                cst->sent_count = next_seq;
                                c2s_sent_total++;
                            }
                        }
                    }

                    const int sv_slot = timed_client_index( &timing, clients[i] );   /* -1 until CONNECTED */
                    struct load_stream_state_t * sv_st = &s2c_streams[i];
                    if ( sv_st->sent_count + 1 < LOAD_MAX_SEQ_TRACKED && sv_slot >= 0 &&
                         timed_server_client_connected( &timing, server, sv_slot ) )
                    {
                        const uint32_t next_seq = sv_st->sent_count + 1;
                        build_message( payload, LOAD_MAGIC_SV_TO_CLIENT, (uint32_t) i, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, next_seq );
                        uint8_t * out = timed_server_start_message( &timing, server, (uint64_t) sv_slot, (int) ROCKETNET_RELIABILITY_RELIABLE_ORDERED, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, LOAD_PAYLOAD_BYTES );
                        if ( out )
                        {
                            memcpy( out, payload, LOAD_PAYLOAD_BYTES );
                            if ( timed_server_finish_message( &timing, server, LOAD_PAYLOAD_BYTES ) != 0 )
                            {
                                sv_st->sent_count = next_seq;
                            }
                        }
                    }
                }

                LOAD_TIMED_VOID( &timing, LOAD_TIMING_CLIENT_SEND_PACKETS, rocketnet_client_send_packets( clients[i] ) );

                const enum rocketnet_client_state_t st = timed_client_state( &timing, clients[i] );
                if ( client_ever_connected[i] && st != ROCKETNET_CLIENT_STATE_CONNECTED )
                {
                    const int sv_slot = client_slot[i];
                    const enum rocketnet_disconnect_reason_t sv_reason = timed_server_disconnect_reason( &timing, server, sv_slot );
                    if ( unexpected_disconnects < 8 )
                    {
                        fprintf( stderr, "load: client %d (slot %d) left CONNECTED at t=%.3f: state=%d %s server_reason=%s\n",
                                 i, sv_slot, rocketnet_time() - traffic_start, (int) st, rocketnet_client_state_name( st ),
                                 rocketnet_disconnect_reason_name( sv_reason ) );
                        fprintf( stderr, "load: client %d last updated %.3f s ago, longest update gap %.3f s; longest server update %.3f s at t=%.3f\n",
                                 i, rocketnet_time() - client_last_update[i], client_max_update_gap[i], max_server_update_s, max_server_update_at );
                    }
                    unexpected_disconnects++;
                    client_ever_connected[i] = 0;
                }
            }

            if ( k + 1 == slice )
            {
                const double t0 = rocketnet_time();
                LOAD_TIMED_VOID( &timing, LOAD_TIMING_SERVER_UPDATE, rocketnet_server_update( server, t0 ) );
                const double t1 = rocketnet_time();
                if ( t1 - t0 > max_server_update_s )
                {
                    max_server_update_s = t1 - t0;
                    max_server_update_at = t0 - traffic_start;
                }

                const struct rocketnet_message_batch_t * batch = NULL;
                while ( ( batch = timed_server_receive_messages( &timing, server ) ) != NULL )
                {
                    for ( int j = 0; j < batch->num_messages; j++ )
                    {
                        c2s_received_total++;
                        uint32_t msg_client = 0, msg_combo = 0, msg_seq = 0;
                        if ( !verify_message( batch->data[j], batch->bytes[j], LOAD_MAGIC_CLIENT_TO_SV, &msg_client, &msg_combo, &msg_seq ) ||
                             msg_client >= (uint32_t) num_clients || msg_combo >= LOAD_NUM_COMBOS ||
                             batch->client_index[j] != client_slot[msg_client] )
                        {
                            reliable_errors++;
                            unreliable_errors++;
                        }
                        else
                        {
                            const int expected_rx_ch = ( msg_combo == ROCKETNET_RELIABILITY_UNRELIABLE ||
                                                         msg_combo == ROCKETNET_RELIABILITY_RELIABLE ||
                                                         msg_combo == ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT ||
                                                         msg_combo == ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT ) ? 0 : (int) msg_combo;
                            struct load_stream_state_t * cst = &c2s_streams[(size_t) msg_client * LOAD_NUM_COMBOS + msg_combo];
                            const int dup = stream_seen_test_and_set( cst, msg_seq );

                            if ( batch->channel[j] != expected_rx_ch )
                            {
                                if ( combo_is_unreliable_family( msg_combo ) )
                                    unreliable_errors++;
                                else
                                    reliable_errors++;
                            }
                            else if ( combo_is_unreliable_family( msg_combo ) )
                            {
                                if ( dup != 0 )
                                    unreliable_errors++;
                                if ( msg_combo == ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED && msg_seq <= cst->max_seq_seen )
                                    unreliable_errors++;
                                if ( msg_seq > cst->max_seq_seen )
                                    cst->max_seq_seen = msg_seq;
                                cst->recv_count++;
                            }
                            else if ( msg_combo == ROCKETNET_RELIABILITY_RELIABLE_SEQUENCED )
                            {
                                if ( dup != 0 || msg_seq <= cst->max_seq_seen )
                                    reliable_errors++;
                                else
                                    cst->max_seq_seen = msg_seq;
                                cst->recv_count++;
                            }
                            else if ( combo_is_reliable_every( msg_combo ) )
                            {
                                if ( dup != 0 )
                                    reliable_errors++;
                                if ( combo_is_ordered( msg_combo ) && msg_seq != cst->recv_count + 1 )
                                    reliable_errors++;
                                if ( msg_seq > cst->max_seq_seen )
                                    cst->max_seq_seen = msg_seq;
                                cst->recv_count++;
                            }
                        }
                    }
                }
                LOAD_TIMED_VOID( &timing, LOAD_TIMING_SERVER_SEND_PACKETS, rocketnet_server_send_packets( server ) );
            }
        }

        if ( now >= next_resource_sample )
        {
            sample_resources( &fds_peak, &threads_peak );
            next_resource_sample = now + 0.25;
        }

        next_slice_time += 0.010;
        const double left = next_slice_time - rocketnet_time();
        if ( left > 0.0 )
            rocketnet_sleep( left );
    }

    timing.enabled = 0;

    /* Final verification of all 5 criteria */
    if ( rocketnet_server_num_connected_clients( server ) != num_clients )
        unexpected_disconnects++;

    for ( int i = 0; i < num_clients; i++ )
    {
        for ( uint32_t combo = 0; combo < LOAD_NUM_COMBOS; combo++ )
        {
            const struct load_stream_state_t * st = &c2s_streams[(size_t) i * LOAD_NUM_COMBOS + combo];
            if ( combo_is_reliable_every( combo ) )
            {
                if ( st->recv_count != st->sent_count || st->max_seq_seen != st->sent_count )
                    reliable_errors++;
            }
        }
        const struct load_stream_state_t * sv_st = &s2c_streams[i];
        if ( sv_st->recv_count != sv_st->sent_count || sv_st->max_seq_seen != sv_st->sent_count )
        {
            reliable_errors++;
        }
    }

    uint64_t client_counted_drops = 0;
    for ( int i = 0; i < num_clients; i++ )
    {
        if ( clients[i] )
        {
            struct rocketnet_client_stats_t stats;
            rocketnet_client_stats( clients[i], &stats );
            client_counted_drops += stats.messages_dropped_over_budget;
            client_counted_drops += stats.messages_dropped_not_connected;
        }
    }

    uint64_t server_counted_drops = 0;
    {
        struct rocketnet_server_stats_t stats;
        rocketnet_server_stats( server, &stats );
        server_counted_drops += stats.messages_dropped_over_budget;
        server_counted_drops += stats.messages_dropped_not_connected;
    }

    const uint64_t total_counted_drops = client_counted_drops + server_counted_drops;
    const int crit1_connect_within_10s = ( all_connected && connect_elapsed <= LOAD_CONNECT_DEADLINE_S ) ? 1 : 0;
    const int crit2_no_unexpected_disconnect = ( unexpected_disconnects == 0 ) ? 1 : 0;
    const int crit3_reliable_delivery = ( reliable_errors == 0 && c2s_sent_total > 0 ) ? 1 : 0;
    const int crit4_unreliable_integrity = ( unreliable_errors == 0 ) ? 1 : 0;
    const int crit5_counters_agree = ( c2s_sent_total == c2s_received_total + total_counted_drops ) ? 1 : 0;

    if ( !crit5_counters_agree )
    {
        static uint64_t client_counter_sum[LOAD_CLIENT_COUNTERS];
        memset( client_counter_sum, 0, sizeof( client_counter_sum ) );
        for ( int i = 0; i < num_clients; i++ )
        {
            if ( clients[i] )
            {
                uint64_t values[LOAD_CLIENT_COUNTERS];
                client_counter_values( clients[i], values );
                for ( int k = 0; k < LOAD_CLIENT_COUNTERS; k++ )
                    client_counter_sum[k] += values[k];
            }
        }
        uint64_t server_values[LOAD_SERVER_COUNTERS];
        server_counter_values( server, server_values );
        for ( int k = 0; k < LOAD_SERVER_COUNTERS; k++ )
        {
            if ( server_values[k] )
                fprintf( stderr, "load: counters_agree server %s=%llu\n", server_counter_names[k], (unsigned long long) server_values[k] );
        }
        for ( int k = 0; k < LOAD_CLIENT_COUNTERS; k++ )
        {
            if ( client_counter_sum[k] )
                fprintf( stderr, "load: counters_agree client sum %s=%llu\n", client_counter_names[k], (unsigned long long) client_counter_sum[k] );
        }
        for ( uint32_t combo = 0; combo < LOAD_NUM_COMBOS; combo++ )
        {
            uint64_t combo_sent = 0;
            uint64_t combo_received = 0;
            for ( int i = 0; i < num_clients; i++ )
            {
                combo_sent += c2s_streams[(size_t) i * LOAD_NUM_COMBOS + combo].sent_count;
                combo_received += c2s_streams[(size_t) i * LOAD_NUM_COMBOS + combo].recv_count;
            }
            if ( combo_sent || combo_received )
                fprintf( stderr, "load: counters_agree combo=%u sent=%llu received=%llu\n", combo, (unsigned long long) combo_sent, (unsigned long long) combo_received );
        }
    }

    const uint64_t p50_us = load_timing_quantile_upper_us( &timing, LOAD_TIMING_SERVER_UPDATE, 50 );
    const uint64_t p99_us = load_timing_quantile_upper_us( &timing, LOAD_TIMING_SERVER_UPDATE, 99 );
    int connected_samples = 0;
    for ( int i = 0; i < num_clients; i++ )
    {
        if ( client_connect_seconds[i] >= 0.0 )
            connected_samples++;
    }
    double connect_p50_s = 0.0;
    double connect_max_s = 0.0;
    if ( connected_samples > 0 )
    {
        qsort( client_connect_seconds, (size_t) num_clients, sizeof( double ), cmp_double );
        const int first = num_clients - connected_samples;
        const int median = ( connected_samples * 50 ) / 100;
        connect_p50_s = client_connect_seconds[first + ( median < connected_samples ? median : connected_samples - 1 )];
        connect_max_s = client_connect_seconds[num_clients - 1];
    }

    const uint64_t rss_bytes = query_rss_bytes();
    const double rss_mb = (double) rss_bytes / ( 1024.0 * 1024.0 );

    const char * failed_criterion = "none";
    if ( !crit1_connect_within_10s )
        failed_criterion = "connect_within_10s";
    else if ( !crit3_reliable_delivery )
        failed_criterion = "reliable_delivery";
    else if ( !crit2_no_unexpected_disconnect )
        failed_criterion = "no_unexpected_disconnect";
    else if ( !crit4_unreliable_integrity )
        failed_criterion = "unreliable_integrity";
    else if ( !crit5_counters_agree )
        failed_criterion = "counters_agree";

    const int pass = crit1_connect_within_10s &&
                     crit2_no_unexpected_disconnect &&
                     crit3_reliable_delivery &&
                     crit4_unreliable_integrity &&
                     crit5_counters_agree &&
                     !quit;

    char server_transport_io[32];
    char client_transport_io[32];
    {
        const char * sd = rocketnet_server_transport_name( server );
        const char * cd = clients[0] ? rocketnet_client_transport_name( clients[0] ) : NULL;
        snprintf( server_transport_io, sizeof( server_transport_io ), "%s", sd ? sd : "none" );
        snprintf( client_transport_io, sizeof( client_transport_io ), "%s", cd ? cd : "none" );
    }

    for ( int i = 0; i < num_clients; i++ )
    {
        if ( clients[i] )
            rn_cli_client_stop_and_destroy( clients[i] );
    }
    rn_cli_server_stop_and_destroy( server );
    free( clients );
    free( client_ever_connected );
    free( client_slot );
    free( client_connect_seconds );
    free( client_next_combo );
    free( c2s_streams );
    free( s2c_streams );
    free( client_last_update );
    free( client_max_update_gap );
    rocketnet_term();

    const long fds_end = count_open_fds();
    const long threads_end = count_threads();

    if ( pass )
    {
        printf( "load: connected=%d connect_s=%.3f connect_p50_s=%.3f connect_max_s=%.3f "
                "profile=%s client_public_required_bytes=%zu server_public_required_bytes=%zu "
                "connect_within_10s=PASS no_unexpected_disconnect=PASS "
                "reliable_delivery=PASS unreliable_integrity=PASS counters_agree=PASS "
                "sent=%" PRIu64 " received=%" PRIu64 " drops=%" PRIu64 " "
                "server_update_p50_us=%" PRIu64 " server_update_p99_us=%" PRIu64 " rss_mb=%.2f "
                "server_transport_io=%s client_transport_io=%s "
                "fd_limit_soft=%s fd_limit_hard=%s fds_start=%ld fds_peak=%ld fds_end=%ld "
                "threads_start=%ld threads_peak=%ld threads_end=%ld verdict=PASS",
                connected_count, connect_elapsed, connect_p50_s, connect_max_s,
                profile_name, client_public_required_bytes, server_public_required_bytes,
                c2s_sent_total, c2s_received_total, total_counted_drops,
                p50_us, p99_us, rss_mb,
                server_transport_io, client_transport_io,
                fd_limit_soft_text, fd_limit_hard_text,
                fds_start, fds_peak, fds_end,
                threads_start, threads_peak, threads_end );
        load_timing_print( &timing );
        putchar( '\n' );
    }
    else
    {
        printf( "load: connected=%d connect_s=%.3f connect_p50_s=%.3f connect_max_s=%.3f "
                "profile=%s client_public_required_bytes=%zu server_public_required_bytes=%zu "
                "connect_within_10s=%s no_unexpected_disconnect=%s "
                "reliable_delivery=%s unreliable_integrity=%s counters_agree=%s "
                "sent=%" PRIu64 " received=%" PRIu64 " drops=%" PRIu64 " "
                "server_update_p50_us=%" PRIu64 " server_update_p99_us=%" PRIu64 " rss_mb=%.2f "
                "server_transport_io=%s client_transport_io=%s "
                "fd_limit_soft=%s fd_limit_hard=%s fds_start=%ld fds_peak=%ld fds_end=%ld "
                "threads_start=%ld threads_peak=%ld threads_end=%ld "
                "failed_criterion=%s verdict=FAIL",
                connected_count, connect_elapsed, connect_p50_s, connect_max_s,
                profile_name, client_public_required_bytes, server_public_required_bytes,
                crit1_connect_within_10s ? "PASS" : "FAIL",
                crit2_no_unexpected_disconnect ? "PASS" : "FAIL",
                crit3_reliable_delivery ? "PASS" : "FAIL",
                crit4_unreliable_integrity ? "PASS" : "FAIL",
                crit5_counters_agree ? "PASS" : "FAIL",
                c2s_sent_total, c2s_received_total, total_counted_drops,
                p50_us, p99_us, rss_mb,
                server_transport_io, client_transport_io,
                fd_limit_soft_text, fd_limit_hard_text,
                fds_start, fds_peak, fds_end,
                threads_start, threads_peak, threads_end,
                failed_criterion );
        load_timing_print( &timing );
        putchar( '\n' );
    }

    free( timing.buckets );
    return pass ? 0 : 1;
}
