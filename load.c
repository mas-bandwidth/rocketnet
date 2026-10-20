/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Load: single-process many-client load harness. Stands up one message
    server and N message clients (N up to ROCKETNET_MAX_CLIENTS = 1000) in
    one process on loopback, connects every client within 10 seconds, drives
    reliable, reliable-ordered, unreliable, and sequenced traffic across all
    8 reliability and channel combinations for a configured duration, and
    verifies five pass criteria:
      1. connect_within_10s
      2. no_unexpected_disconnect
      3. reliable_delivery
      4. unreliable_integrity
      5. counters_agree
*/

#include "rocketnet.h"
#include "rocketnet_counters.h"
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
    /* One proc_pidinfo call lists the open descriptors; a per-fd probe over a
       65536-wide table every sample would itself load the run being measured. */
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

static int cmp_u64( const void * a, const void * b )
{
    const uint64_t ua = *(const uint64_t *) a;
    const uint64_t ub = *(const uint64_t *) b;
    return ( ua > ub ) - ( ua < ub );
}

static int cmp_double( const void * a, const void * b )
{
    const double da = *(const double *) a;
    const double db = *(const double *) b;
    return ( da > db ) - ( da < db );
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

    if ( rocketnet_init( NULL ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "load: rocketnet_init failed\n" );
        return 1;
    }
    /* Start is sampled after rocketnet_init: init opens the process-wide random
       source once and keeps it for the life of the process, so the baseline
       holds it and start, peak and end measure what servers and clients add. */
    const long fds_start = count_open_fds();
    const long threads_start = count_threads();
    long fds_peak = fds_start;
    long threads_peak = threads_start;
    rocketnet_set_log_level( ROCKETNET_LOG_LEVEL_NONE );

    struct rocketnet_server_transport_config_t server_tcfg;
    rocketnet_server_transport_dev_config( &server_tcfg );
    server_tcfg.max_clients = num_clients;
    server_tcfg.max_payload_bytes = ROCKETNET_MAX_PAYLOAD_BYTES;
    server_tcfg.num_reassembly_slots = 2;
    server_tcfg.handshake_deadline_seconds = 10;

    struct rocketnet_client_transport_config_t client_tcfg;
    rocketnet_client_transport_default_config( &client_tcfg );
    client_tcfg.max_payload_bytes = ROCKETNET_MAX_PAYLOAD_BYTES;
    client_tcfg.num_reassembly_slots = 1;

    struct rocketnet_message_config_t msg_cfg;
    rocketnet_message_default_config( &msg_cfg );
    msg_cfg.num_channels = LOAD_NUM_COMBOS;
    msg_cfg.window_messages = 64;
    msg_cfg.receive_ring_bytes = 16384;
    msg_cfg.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    msg_cfg.send_buffer_bytes = 32768;
    msg_cfg.send_queue_messages = 64;
    msg_cfg.packets_in_flight = 32;
    msg_cfg.max_packets_per_update = 4;
    msg_cfg.unreliable_deliveries = 64;
    msg_cfg.sequenced_holds = 16;
    msg_cfg.receipt_ring = 64;
    msg_cfg.reliable_timeout_ms = 3000;

    errno = 0;
    struct rocketnet_message_server_t * server = rocketnet_message_server_create(
        NULL, "127.0.0.1", &server_tcfg, &msg_cfg, NULL, 0, 0 );
    if ( !server )
    {
        const int saved_errno = errno;
        fprintf( stderr, "load: failed to create message server on 127.0.0.1: %s\n",
                 saved_errno ? strerror( saved_errno ) : "no errno from create" );
        rocketnet_term();
        return 1;
    }

    struct rocketnet_server_transport_t * server_transport = rocketnet_message_server_transport( server );
    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d",
              rocketnet_server_transport_port( server_transport ) );

    struct rocketnet_message_client_t ** clients =
        (struct rocketnet_message_client_t **) calloc( (size_t) num_clients, sizeof( *clients ) );
    int * client_ever_connected = (int *) calloc( (size_t) num_clients, sizeof( int ) );
    double * client_connect_seconds = (double *) malloc( (size_t) num_clients * sizeof( double ) );
    uint32_t * client_next_combo = (uint32_t *) calloc( (size_t) num_clients, sizeof( uint32_t ) );
    struct load_stream_state_t * c2s_streams = (struct load_stream_state_t *) calloc(
        (size_t) num_clients * LOAD_NUM_COMBOS, sizeof( struct load_stream_state_t ) );
    struct load_stream_state_t * s2c_streams = (struct load_stream_state_t *) calloc(
        (size_t) num_clients, sizeof( struct load_stream_state_t ) );

    size_t update_cap = 262144;
    size_t update_count = 0;
    uint64_t * update_us = (uint64_t *) malloc( update_cap * sizeof( uint64_t ) );

    if ( !clients || !client_ever_connected || !client_connect_seconds || !client_next_combo ||
         !c2s_streams || !s2c_streams || !update_us )
    {
        fprintf( stderr, "load: allocation failed\n" );
        return 1;
    }
    for ( int i = 0; i < num_clients; i++ )
        client_connect_seconds[i] = -1.0;

    const uint64_t token_now = (uint64_t) time( NULL );

    for ( int i = 0; i < num_clients; i++ )
    {
        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        if ( rocketnet_write_dev_connect_token( server_address, (uint64_t) ( i + 1 ), token_now, connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) != ROCKETNET_STATUS_OK )
        {
            fprintf( stderr, "load: failed to create dev connect token for client %d\n", i );
            return 1;
        }
        clients[i] = rocketnet_message_client_create(
            NULL, connect_token, &client_tcfg, &msg_cfg, NULL, 0, 0 );
        if ( !clients[i] )
        {
            fprintf( stderr, "load: failed to create client %d\n", i );
            return 1;
        }
        client_next_combo[i] = (uint32_t) ( i % LOAD_NUM_COMBOS );
    }
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
            rocketnet_message_client_update( clients[i], now );
            rocketnet_message_client_send_packets( clients[i] );
            const int st = rocketnet_message_client_state( clients[i] );
            if ( st == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                if ( client_connect_seconds[i] < 0.0 )
                    client_connect_seconds[i] = now - start_time;
                client_ever_connected[i] = 1;
                connected_count++;
            }
            else if ( st < 0 || ( client_ever_connected[i] && st != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ) )
            {
                unexpected_disconnects++;
            }

            if ( ( ( i + 1 ) & 63 ) == 0 || i + 1 == num_clients )
            {
                const double t0 = rocketnet_time();
                rocketnet_message_server_update( server, t0 );
                const double t1 = rocketnet_time();
                if ( update_count < update_cap )
                    update_us[update_count++] = (uint64_t) ( ( t1 - t0 ) * 1e6 );
                rocketnet_message_server_send_packets( server );
            }
        }

        if ( connected_count == num_clients &&
             rocketnet_server_transport_num_connected_clients( server_transport ) == num_clients )
        {
            all_connected = 1;
            connect_elapsed = rocketnet_time() - start_time;
            break;
        }

        if ( rocketnet_time() - start_time > LOAD_CONNECT_DEADLINE_S )
        {
            connect_elapsed = rocketnet_time() - start_time;
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

    const double traffic_start = rocketnet_time();
    const double drop_at = duration_seconds * 0.25 < 1.0 ? duration_seconds * 0.25 : 1.0;
    int drop_injected = 0;
    int send_cursor = 0;
    int server_send_cursor = 0;
    double last_send_batch_time = traffic_start;

    /* Step 2: Drive traffic for <seconds>, then 1.0s drain for in-flight delivery */
    const double drain_seconds = 3.0;
    while ( !quit )
    {
        const double now = rocketnet_time();
        const double elapsed = now - traffic_start;
        const int in_traffic_window = ( elapsed < duration_seconds );
        if ( elapsed >= duration_seconds + drain_seconds )
            break;

        if ( drop_one && !drop_injected && elapsed >= drop_at )
            drop_injected = 1;

        /* Pace sends every 20ms so all 8 reliability/channel combinations are exercised across all clients */
        if ( in_traffic_window && ( now - last_send_batch_time >= 0.020 ) )
        {
            last_send_batch_time = now;
            const int batch = num_clients < 32 ? num_clients : 32;
            for ( int b = 0; b < batch; b++ )
            {
                const int idx = send_cursor;
                send_cursor = ( send_cursor + 1 ) % num_clients;
                if ( drop_injected && idx == 0 )
                    continue;
                if ( rocketnet_message_client_state( clients[idx] ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                    continue;

                const uint32_t combo = client_next_combo[idx] % LOAD_NUM_COMBOS;
                client_next_combo[idx]++;
                const int channel = (int) combo;
                const int reliability = (int) combo;

                struct load_stream_state_t * st = &c2s_streams[(size_t) idx * LOAD_NUM_COMBOS + combo];
                if ( st->sent_count + 1 < LOAD_MAX_SEQ_TRACKED )
                {
                    const uint32_t next_seq = st->sent_count + 1;
                    uint8_t payload[LOAD_PAYLOAD_BYTES];
                    build_message( payload, LOAD_MAGIC_CLIENT_TO_SV, (uint32_t) idx, combo, next_seq );
                    if ( rocketnet_message_client_send( clients[idx], channel, reliability, payload, LOAD_PAYLOAD_BYTES ) != 0 )
                    {
                        st->sent_count = next_seq;
                        c2s_sent_total++;
                    }
                }
            }

            /* Also send server -> client reliable-ordered messages on channel 3 (including to client 0 under drop-one) */
            const int sv_batch = num_clients < 16 ? num_clients : 16;
            for ( int b = 0; b < sv_batch; b++ )
            {
                const int target_idx = ( drop_injected && b == 0 ) ? 0 : server_send_cursor;
                if ( !( drop_injected && b == 0 ) )
                    server_send_cursor = ( server_send_cursor + 1 ) % num_clients;

                const int sv_slot = rocketnet_client_transport_client_index( rocketnet_message_client_transport( clients[target_idx] ) );
                struct load_stream_state_t * sv_st = &s2c_streams[target_idx];
                if ( sv_slot >= 0 && sv_st->sent_count + 1 < LOAD_MAX_SEQ_TRACKED &&
                     rocketnet_server_transport_is_client_connected( server_transport, sv_slot ) )
                {
                    const uint32_t next_seq = sv_st->sent_count + 1;
                    uint8_t payload[LOAD_PAYLOAD_BYTES];
                    build_message( payload, LOAD_MAGIC_SV_TO_CLIENT, (uint32_t) target_idx, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, next_seq );
                    if ( rocketnet_message_server_send( server, sv_slot, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, payload, LOAD_PAYLOAD_BYTES ) != 0 )
                    {
                        sv_st->sent_count = next_seq;
                    }
                }
            }
        }

        /* Update a slice of up to 200 clients per tick, then run one server update */
        const int slice = num_clients < 100 ? num_clients : 100;
        static int client_update_cursor = 0;
        for ( int k = 0; k < slice; k++ )
        {
            const int i = client_update_cursor;
            client_update_cursor = ( client_update_cursor + 1 ) % num_clients;
            if ( !( drop_injected && i == 0 ) )
            {
                rocketnet_message_client_update( clients[i], rocketnet_time() );

                const uint8_t * data = NULL;
                int bytes = 0;
                while ( rocketnet_message_client_receive( clients[i], ROCKETNET_RELIABILITY_RELIABLE_ORDERED, &data, &bytes ) )
                {
                    uint32_t msg_client = 0, msg_combo = 0, msg_seq = 0;
                    if ( !verify_message( data, bytes, LOAD_MAGIC_SV_TO_CLIENT, &msg_client, &msg_combo, &msg_seq ) ||
                         msg_client != (uint32_t) i || msg_combo != ROCKETNET_RELIABILITY_RELIABLE_ORDERED )
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
                    rocketnet_message_client_release( clients[i], data );
                }

                struct rocketnet_message_receipt_t receipts[16];
                (void) rocketnet_message_client_receipts( clients[i], receipts, 16 );

                rocketnet_message_client_send_packets( clients[i] );
            }

            const int st = rocketnet_message_client_state( clients[i] );
            if ( client_ever_connected[i] && st != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
                unexpected_disconnects++;

            if ( k + 1 == slice )
            {
                const double t0 = rocketnet_time();
                rocketnet_message_server_update( server, t0 );
                const double t1 = rocketnet_time();
                if ( update_count < update_cap )
                    update_us[update_count++] = (uint64_t) ( ( t1 - t0 ) * 1e6 );

                for ( int s = 0; s < num_clients; s++ )
                {
                    for ( int ch = 0; ch < LOAD_NUM_COMBOS; ch++ )
                    {
                        const uint8_t * sdata = NULL;
                        int sbytes = 0;
                        while ( rocketnet_message_server_receive( server, s, ch, &sdata, &sbytes ) )
                        {
                            c2s_received_total++;
                            uint32_t msg_client = 0, msg_combo = 0, msg_seq = 0;
                            if ( !verify_message( sdata, sbytes, LOAD_MAGIC_CLIENT_TO_SV, &msg_client, &msg_combo, &msg_seq ) ||
                                 msg_client >= (uint32_t) num_clients || msg_combo >= LOAD_NUM_COMBOS )
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

                                if ( ch != expected_rx_ch )
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
                            rocketnet_message_server_release( server, s, sdata );
                        }
                    }
                }
                rocketnet_message_server_send_packets( server );
            }
        }

        if ( now >= next_resource_sample )
        {
            sample_resources( &fds_peak, &threads_peak );
            next_resource_sample = now + 0.25;
        }
        rocketnet_sleep( 0.001 );
    }

    /* Final verification of all 5 criteria */
    if ( rocketnet_server_transport_num_connected_clients( server_transport ) != num_clients )
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
            reliable_errors++;
    }

    uint64_t client_counted_drops = 0;
    for ( int i = 0; i < num_clients; i++ )
    {
        const struct rocketnet_counters_t * cc = rocketnet_message_client_counters( clients[i] );
        if ( cc )
        {
            client_counted_drops += rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
            client_counted_drops += rocketnet_counter_value( cc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
        }
    }

    uint64_t server_counted_drops = 0;
    const struct rocketnet_counters_t * sc = rocketnet_message_server_counters( server );
    if ( sc )
    {
        server_counted_drops += rocketnet_counter_value( sc, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );
        server_counted_drops += rocketnet_counter_value( sc, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        server_counted_drops += rocketnet_counter_value( sc, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
        server_counted_drops += rocketnet_counter_value( sc, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
    }

    const struct rocketnet_counters_t * stc = rocketnet_server_transport_counters( server_transport );
    if ( stc )
    {
        if ( rocketnet_counter_value( stc, ROCKETNET_COUNTER_CLIENT_DISCONNECTED ) > 0 ||
             rocketnet_counter_value( stc, ROCKETNET_COUNTER_SERVER_DISCONNECTED_CLIENT ) > 0 ||
             rocketnet_counter_value( stc, ROCKETNET_COUNTER_CLIENT_TIMED_OUT ) > 0 )
        {
            unexpected_disconnects++;
        }
    }

    const uint64_t total_counted_drops = client_counted_drops + server_counted_drops;
    const int crit1_connect_within_10s = ( all_connected && connect_elapsed <= LOAD_CONNECT_DEADLINE_S ) ? 1 : 0;
    const int crit2_no_unexpected_disconnect = ( unexpected_disconnects == 0 ) ? 1 : 0;
    const int crit3_reliable_delivery = ( reliable_errors == 0 && c2s_sent_total > 0 ) ? 1 : 0;
    const int crit4_unreliable_integrity = ( unreliable_errors == 0 ) ? 1 : 0;
    const int crit5_counters_agree = ( c2s_sent_total == c2s_received_total + total_counted_drops ) ? 1 : 0;

    uint64_t p50_us = 0;
    uint64_t p99_us = 0;
    if ( update_count > 0 )
    {
        qsort( update_us, update_count, sizeof( uint64_t ), cmp_u64 );
        p50_us = update_us[( update_count * 50 ) / 100];
        p99_us = update_us[( update_count * 99 ) / 100];
    }
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

    char server_driver[32];
    char client_driver[32];
    {
        const char * sd = rocketnet_server_transport_driver( server_transport );
        const char * cd = clients[0] ? rocketnet_client_transport_driver( rocketnet_message_client_transport( clients[0] ) ) : NULL;
        snprintf( server_driver, sizeof( server_driver ), "%s", sd ? sd : "none" );
        snprintf( client_driver, sizeof( client_driver ), "%s", cd ? cd : "none" );
    }

    for ( int i = 0; i < num_clients; i++ )
    {
        if ( clients[i] )
            rocketnet_message_client_destroy( clients[i] );
    }
    rocketnet_message_server_destroy( server );
    free( clients );
    free( client_ever_connected );
    free( client_connect_seconds );
    free( client_next_combo );
    free( c2s_streams );
    free( s2c_streams );
    free( update_us );
    rocketnet_term();

    const long fds_end = count_open_fds();
    const long threads_end = count_threads();

    if ( pass )
    {
        printf( "load: connected=%d connect_s=%.3f connect_p50_s=%.3f connect_max_s=%.3f "
                "connect_within_10s=PASS no_unexpected_disconnect=PASS "
                "reliable_delivery=PASS unreliable_integrity=PASS counters_agree=PASS "
                "sent=%" PRIu64 " received=%" PRIu64 " drops=%" PRIu64 " "
                "server_update_p50_us=%" PRIu64 " server_update_p99_us=%" PRIu64 " rss_mb=%.2f "
                "server_driver=%s client_driver=%s "
                "fd_limit_soft=%s fd_limit_hard=%s fds_start=%ld fds_peak=%ld fds_end=%ld "
                "threads_start=%ld threads_peak=%ld threads_end=%ld verdict=PASS\n",
                connected_count, connect_elapsed, connect_p50_s, connect_max_s,
                c2s_sent_total, c2s_received_total, total_counted_drops,
                p50_us, p99_us, rss_mb,
                server_driver, client_driver,
                fd_limit_soft_text, fd_limit_hard_text,
                fds_start, fds_peak, fds_end,
                threads_start, threads_peak, threads_end );
    }
    else
    {
        printf( "load: connected=%d connect_s=%.3f connect_p50_s=%.3f connect_max_s=%.3f "
                "connect_within_10s=%s no_unexpected_disconnect=%s "
                "reliable_delivery=%s unreliable_integrity=%s counters_agree=%s "
                "sent=%" PRIu64 " received=%" PRIu64 " drops=%" PRIu64 " "
                "server_update_p50_us=%" PRIu64 " server_update_p99_us=%" PRIu64 " rss_mb=%.2f "
                "server_driver=%s client_driver=%s "
                "fd_limit_soft=%s fd_limit_hard=%s fds_start=%ld fds_peak=%ld fds_end=%ld "
                "threads_start=%ld threads_peak=%ld threads_end=%ld "
                "failed_criterion=%s verdict=FAIL\n",
                connected_count, connect_elapsed, connect_p50_s, connect_max_s,
                crit1_connect_within_10s ? "PASS" : "FAIL",
                crit2_no_unexpected_disconnect ? "PASS" : "FAIL",
                crit3_reliable_delivery ? "PASS" : "FAIL",
                crit4_unreliable_integrity ? "PASS" : "FAIL",
                crit5_counters_agree ? "PASS" : "FAIL",
                c2s_sent_total, c2s_received_total, total_counted_drops,
                p50_us, p99_us, rss_mb,
                server_driver, client_driver,
                fd_limit_soft_text, fd_limit_hard_text,
                fds_start, fds_peak, fds_end,
                threads_start, threads_peak, threads_end,
                failed_criterion );
    }

    return pass ? 0 : 1;
}
