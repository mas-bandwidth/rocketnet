/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The out-of-the-box server: runs with the published development keys, so
    a first server needs no backend and no configuration -- run it, point a
    client at it, connected. Echoes every payload back to its sender. Pass
    the public address clients dial: ./bin/server 192.0.2.10:40000

    The live XDP fleet run (docs/LIVE-XDP.md): `--matrix` runs the message
    matrix instead of the payload echo. The server sends its own
    sequence-derived stream to each client and verifies what it receives
    against what that client sent, using tests/live_verify.h and
    tests/matrix_receive.h (sections 3 to 5). It runs at the
    max_clients the run sets, dumps counters and resident memory each minute,
    stops on SIGINT, after --duration seconds, or when a watched stdin (a
    pipe, socket or terminal: its controlling ssh) closes, and refuses to
    start when the XDP transport is pinned and the interface reports the
    in-tree i40e on a kernel older than 7.0.0-43 (section 9). The matrix exits 1 when any check failed.
*/

#include "tests/native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_counters.h"
#include "tests/live_verify.h"
#include "tests/matrix_receive.h"
#include "tests/portable_crt.h"

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#if !defined( _WIN32 )
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#endif // #if !defined( _WIN32 )

#define ROCKETNET_SERVER_LABEL_MAX           64
#define ROCKETNET_SERVER_TINY                 ROCKETNET_LIVE_HEADER_BYTES
#define ROCKETNET_SERVER_OVER_MAX_DELTA      1

static volatile int quit;

// section 2: the run seed offsets the length rule (section 4), and both ends
// get the same seed, so every schedule computation in this program reads it
// from here rather than a literal.
static struct rocketnet_live_config_t g_live = { .seed = 0, .max_message_bytes = 262144 };

void interrupt_handler( int signal )
{
    (void) signal;
    quit = 1;
}

static void usage( void )
{
    printf(
        "usage: server [options] [address]\n"
        "  address                 the public address clients dial (default 127.0.0.1:40000)\n"
        "  --matrix                run the message matrix instead of the payload echo\n"
        "  --address A:P           the public address it binds and that tokens name\n"
        "  --label L               the bench label written on every result line\n"
        "  --seed X                the run seed, printed at start\n"
        "  --send-kbps K           the per-client send budget\n"
        "  --max-clients N         max_clients, set explicitly (the run uses 1000)\n"
        "  --max-packets-per-update N  the message config's packets_per_update\n"
        "  --update-hz H           update rate of the main loop\n"
        "  --num-xdp-queues N      the transport config's num_xdp_queues (0 is the transport default; xdp thread k pins to cpu 1 + k)\n"
        "  --duration S            stop after S seconds (default 0: run until SIGINT or a watched stdin closes)\n"
        "  --simulator L,J,P,D     the development link simulator on the server's sends: latency ms, jitter ms, loss %%, duplicate %%\n"
        "  --counters-every S      print counters, RSS and transport stats every S seconds\n"
        "  --results PATH          where the result lines go\n"
        "  --status PATH           one line per state change, for the orchestrator\n"
        "  --transport T           pin the transport (e.g. xdp); the in-tree i40e before kernel 7.0.0-43 refuses\n"
        "  --self-test-result FILE write a sample result file without any socket and exit\n"
        "  --i40e-kernel-check R   print whether section 9 refuses the in-tree i40e on kernel release R (exit 1) and exit\n"
        "  --help                  this message\n"
        "environment:\n"
        "  ROCKETNET_SERVER_TRANSPORT   the transport candidate list\n"
        "  ROCKETNET_SERVER_PRINT_COUNTERS=1  every server counter at stop\n" );
}

static void set_status( const char * path, const char * line )
{
    if ( !path )
    {
        return;
    }

    FILE * f = rocketnet_test_fopen( path, "w" );
    if ( f )
    {
        fprintf( f, "%s\n", line );
        fclose( f );
    }
}

// the result lines (docs/LIVE-XDP.md section 7): tab-separated lines of the
// six kinds cell, bench, counters, rss, event and fail, appended to --results.
static FILE * g_results = NULL;

#if defined( __GNUC__ ) || defined( __clang__ )
__attribute__( ( format( printf, 2, 3 ) ) )
#endif // #if defined( __GNUC__ ) || defined( __clang__ )
static void
results_line( const char * kind, const char * fmt, ... )
{
    if ( !g_results )
    {
        return;
    }

    va_list ap;
    va_start( ap, fmt );
    fprintf( g_results, "%s\t", kind );
    vfprintf( g_results, fmt, ap );
    fprintf( g_results, "\n" );
    va_end( ap );
    fflush( g_results );
}

#if defined( __GNUC__ ) || defined( __clang__ )
__attribute__( ( format( printf, 1, 2 ) ) )
#endif // #if defined( __GNUC__ ) || defined( __clang__ )
static void
results_event( const char * fmt, ... )
{
    if ( !g_results )
    {
        return;
    }

    va_list ap;
    va_start( ap, fmt );
    fprintf( g_results, "event\t" );
    vfprintf( g_results, fmt, ap );
    fprintf( g_results, "\n" );
    va_end( ap );
    fflush( g_results );
}

// The live XDP fleet run's helpers below (resident memory, the stdin watch,
// the NIC transport check) read Linux and POSIX facilities. On Windows the
// out-of-the-box server builds without them: resident memory reads 0 (as on
// macOS, which has no /proc), stdin is not watched, and no interface is found.
static uint64_t resident_bytes( void )
{
#if defined( _WIN32 )
    return 0;
#else
    FILE * f = fopen( "/proc/self/statm", "r" );
    if ( !f )
    {
        return 0;
    }

    long total = 0, resident = 0;
    if ( fscanf( f, "%ld %ld", &total, &resident ) != 2 )
    {
        resident = 0;
    }

    fclose( f );

    const long page = sysconf( _SC_PAGESIZE );
    return (uint64_t) resident * (uint64_t) ( page > 0 ? page : 4096 );
#endif // #if defined( _WIN32 )
}

// the card: the server exits when its controlling ssh or stdin closes. Only
// a pipe, a socket or a terminal is watched: a service manager hands the
// server /dev/null or a file, which reads end of file at once and would stop
// it the moment it starts; such a server stops on SIGINT or --duration.
static int stdin_watched( void )
{
#if defined( _WIN32 )
    return 0;
#else
    struct stat st;
    if ( fstat( 0, &st ) != 0 )
    {
        return 0;
    }
    return S_ISFIFO( st.st_mode ) || S_ISSOCK( st.st_mode ) || isatty( 0 );
#endif // #if defined( _WIN32 )
}

static void check_stdin_closed( int watched )
{
    if ( !watched )
    {
        return;
    }

#if !defined( _WIN32 )
    fd_set rfds;
    FD_ZERO( &rfds );
    FD_SET( 0, &rfds );

    struct timeval tv = { 0, 0 };
    if ( select( 1, &rfds, NULL, NULL, &tv ) > 0 )
    {
        char buffer[64];
        const ssize_t n = read( 0, buffer, sizeof buffer );
        if ( n == 0 )
        {
            quit = 1;
        }
    }
#endif // #if !defined( _WIN32 )
}

static void dump_counters( const struct rocketnet_counters_t * counters )
{
    const int n = rocketnet_num_counters();
    for ( int i = 0; i < n; i++ )
    {
        printf( "rocketnet counter %s %llu\n", rocketnet_counter_name( i ), (unsigned long long) rocketnet_counter_value( counters, i ) );
        results_line( "counters", "%s\t%llu", rocketnet_counter_name( i ), (unsigned long long) rocketnet_counter_value( counters, i ) );
    }

    printf( "rocketnet rss %llu\n", (unsigned long long) resident_bytes() );
    results_line( "rss", "%llu", (unsigned long long) resident_bytes() );
    fflush( stdout );
}

// the echo loop's main-call durations since the last counters dump, for a bench
// run that floods the server (docs/XDP-PACKET-BLASTER.md): p50, p99 and max
#define ROCKETNET_SERVER_TIMES_MAX 65536

struct update_times_t
{
    int count;
    double max;
    double samples[ROCKETNET_SERVER_TIMES_MAX];
};

static struct update_times_t g_update_times;
static struct update_times_t g_send_times;

static void update_times_add( struct update_times_t * t, double seconds )
{
    if ( t->count < ROCKETNET_SERVER_TIMES_MAX )
    {
        t->samples[t->count++] = seconds;
    }
    if ( seconds > t->max )
    {
        t->max = seconds;
    }
}

static int update_times_compare( const void * a, const void * b )
{
    const double x = *(const double *) a;
    const double y = *(const double *) b;
    return ( x > y ) - ( x < y );
}

static void update_times_print( const char * name, struct update_times_t * t )
{
    if ( t->count > 0 )
    {
        qsort( t->samples, (size_t) t->count, sizeof( double ), update_times_compare );
        printf( "rocketnet time %s n %d p50_us %.1f p99_us %.1f max_us %.1f\n", name, t->count,
                t->samples[t->count / 2] * 1e6, t->samples[( t->count * 99 ) / 100] * 1e6, t->max * 1e6 );
        fflush( stdout );
    }
    t->count = 0;
    t->max = 0.0;
}

// the interface that carries an IPv4 address, via `ip -o -4 addr show`
static int interface_for_address( const char * address, char * out, size_t out_size )
{
#if defined( _WIN32 )
    (void) address;
    (void) out;
    (void) out_size;
    return 0;
#else
    // the address arrives as A:P; `ip` prints A/len, so compare the host part only

    char host[64] = { 0 };
    snprintf( host, sizeof host, "%s", address );
    char * colon = strrchr( host, ':' );
    if ( colon )
    {
        *colon = 0;
    }

    char cmd[256];
    snprintf( cmd, sizeof cmd, "ip -o -4 addr show 2>/dev/null | awk -v a=%s '{ split( $4, p, \"/\" ); if ( p[1] == a ) { print $2; exit } }'", host );

    FILE * f = popen( cmd, "r" );
    if ( !f )
    {
        return 0;
    }

    if ( fgets( out, (int) out_size, f ) )
    {
        size_t len = strlen( out );
        while ( len && ( out[len - 1] == '\n' || out[len - 1] == '\r' ) )
        {
            out[--len] = 0;
        }

        pclose( f );
        return len > 0;
    }

    pclose( f );
    return 0;
#endif // #if defined( _WIN32 )
}

// Section 9: the in-tree i40e reports the running kernel's version (the same
// string as `uname -r`); the out-of-tree build reports its own release number.
// Only i40e is refused: other in-tree drivers (ixgbe) pass.
static int in_tree_i40e( const char * nic )
{
#if defined( _WIN32 )
    (void) nic;
    return 0;
#else
    char cmd[256];
    snprintf( cmd, sizeof cmd, "ethtool -i %s 2>/dev/null | awk '/^driver:/{d=$2} /^version:/{v=$2} END{print d, v}'", nic );

    FILE * f = popen( cmd, "r" );
    if ( !f )
    {
        return 1;
    }

    char transport[64] = { 0 }, version[64] = { 0 };
    if ( fscanf( f, "%63s %63s", transport, version ) != 2 )
    {
        pclose( f );
        return 1;
    }

    pclose( f );

    FILE * u = popen( "uname -r", "r" );
    if ( !u )
    {
        return 1;
    }

    char kernel[64] = { 0 };
    if ( fgets( kernel, sizeof kernel, u ) )
    {
        size_t len = strlen( kernel );
        while ( len && ( kernel[len - 1] == '\n' || kernel[len - 1] == '\r' ) )
        {
            kernel[--len] = 0;
        }
    }

    pclose( u );
    return strcmp( transport, "i40e" ) == 0 && strcmp( version, kernel ) == 0;
#endif // #if defined( _WIN32 )
}

// Section 9: 1 when kernel release R, read as Ubuntu's A.B.C-N, is older than
// 7.0.0-43, the kernel whose in-tree i40e passed the 60-minute zero-copy soak
// (docs/LINUX.md). A release that does not read so counts as older, so the
// refusal stays. scripts/live-xdp-fleet-remote (i40e_kernel_refused) is the
// same rule; its i40e-guard-selftest holds both to one case list.
static int kernel_release_before_i40e_soak( const char * release )
{
    int v[4] = { 0, 0, 0, 0 };
    if ( sscanf( release, "%d.%d.%d-%d", &v[0], &v[1], &v[2], &v[3] ) < 3 )
    {
        return 1;
    }

    const int soak[4] = { 7, 0, 0, 43 };
    for ( int i = 0; i < 4; i++ )
    {
        if ( v[i] != soak[i] )
        {
            return v[i] < soak[i];
        }
    }

    return 0;
}

// the running kernel (`uname -r`) under kernel_release_before_i40e_soak
static int kernel_before_i40e_soak( void )
{
#if defined( _WIN32 )
    return 0;
#else
    FILE * u = popen( "uname -r", "r" );
    if ( !u )
    {
        return 1;
    }

    char kernel[64] = { 0 };
    const int got_line = fgets( kernel, sizeof kernel, u ) != NULL;
    pclose( u );

    return !got_line || kernel_release_before_i40e_soak( kernel );
#endif // #if defined( _WIN32 )
}

// ---- the matrix server (docs/LIVE-XDP.md sections 3 to 7) ----
//
// One window per connection, every slot in parallel. A slot opens when its
// client connects and waits for the HELLO, which names the cycle every
// message of the connection carries (and the seed and max_message_bytes the
// length rule reads, which must match this server's). The server answers
// the HELLO, then runs its own bounded stream of every (mode, channel) and
// judges what the client sends against tests/matrix_receive.h.
// The client's sent report ends the server's sends: it answers with its own
// sent report and keeps reporting progress until the client disconnects.
// At the disconnect every owed sequence the client sent must have arrived.

// the server's send side of one (mode, channel) stream
struct server_tx_stream_t
{
    uint32_t total;
    uint32_t next;
    uint32_t confirmed;                                     // the client's progress: every owed sequence below it arrived
    uint32_t bucket_sent[ROCKETNET_LIVE_NUM_BUCKETS];
    uint32_t bucket_paced[ROCKETNET_LIVE_NUM_BUCKETS];      // held by the ledger, or refused by the library for want of room: never an error
};

// the server's receive side of one (mode, channel, size bucket) cell
struct server_rx_cell_t
{
    uint32_t received;
    uint32_t verified;
    uint32_t failed;
    uint32_t missing;
    uint32_t ooo;
    uint32_t dup;
};

// the per-connection state, allocated while the slot is connected
struct server_slot_t
{
    struct matrix_rx_t rx;
    struct server_rx_cell_t cells[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_LIVE_NUM_CHANNELS][ROCKETNET_LIVE_NUM_BUCKETS];
    struct server_tx_stream_t tx[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_LIVE_NUM_CHANNELS];
};

enum
{
    SERVER_SLOT_IDLE = 0,                   // no connection
    SERVER_SLOT_AWAITING_HELLO,             // connected, no HELLO yet
    SERVER_SLOT_ACTIVE,                     // HELLO answered: sending and judging
    SERVER_SLOT_DRAINING,                   // the client's sent report arrived: no new sends
};

struct server_client_t
{
    int state;
    uint32_t cycle;
    struct server_slot_t * slot;
    uint64_t failures;
    uint64_t verified;
    uint64_t refusals;                      // over-maximum starts refused, as they must be
    uint64_t refused;                       // matrix sends held by the ledger or refused for want of room
    uint64_t outstanding;                   // the pacing ledger: reliable bytes sent and not yet confirmed
    uint32_t kbps;                          // this slot's send budget
    char label[ROCKETNET_SERVER_LABEL_MAX];
    int sent_report_due;
    double last_progress;                   // when this slot last sent its progress report
    uint64_t progress_sent;
    uint64_t windows;
};

static const char * g_label = "server";
static uint32_t g_send_kbps = 500;
static uint64_t g_failures = 0;

static uint64_t server_counter( const struct rocketnet_counters_t * counters, int index )
{
    return counters ? counters->values[index] : 0;
}

// section 3: fail <label> <cycle> <direction> <mode> <channel> <sequence> <reason> <bytes>
static void matrix_fail( struct server_client_t * c, int direction, int mode, int channel, int64_t sequence, const char * reason, int bytes )
{
    if ( c->failures < 64 )
    {
        printf( "fail %s %u %d %d %d %lld %s %d\n", c->label, c->cycle, direction, mode, channel, (long long) sequence, reason, bytes );
    }

    results_line( "fail", "%s\t%u\t%d\t%d\t%d\t%lld\t%s\t%d", c->label, c->cycle, direction, mode, channel, (long long) sequence, reason, bytes );
    c->failures++;
    g_failures++;
}

// a receive-side failure reported by tests/matrix_receive.h:
// the client-to-server direction
static void matrix_rx_failed( void * context, int mode, int channel, int64_t sequence, const char * reason, int bytes )
{
    matrix_fail( (struct server_client_t *) context, 0, mode, channel, sequence, reason, bytes );
}

// section 5: every 50 ms the server sends its progress report, driven by the
// update time. The report's one sending call is behind a pointer so the
// self-test can observe emission without a socket. It is
// UNRELIABLE_SEQUENCED on channel 31, which carries no matrix stream of the
// modes that keep a channel, so it neither supersedes nor is superseded by one.
typedef int ( *server_progress_send_fn )( struct rocketnet_message_server_t * server, int client_index, int channel,
                                          int reliability, const uint8_t * data, int bytes );

static int server_progress_send( struct rocketnet_message_server_t * server, int client_index, int channel,
                                 int reliability, const uint8_t * data, int bytes )
{
    return (int) rocketnet_message_server_send( server, client_index, channel, reliability, data, bytes );
}

static server_progress_send_fn g_progress_send = server_progress_send;

static void matrix_send_progress( struct rocketnet_message_server_t * server, int client_index, struct server_client_t * c, double now )
{
    if ( c->last_progress > 0.0 && now - c->last_progress < MATRIX_PROGRESS_SECONDS )
    {
        return;
    }
    c->last_progress = now;

    uint8_t report[MATRIX_REPORT_BYTES];
    const int n = matrix_rx_progress( &c->slot->rx, report, (int) sizeof report );
    if ( n <= 1 )
    {
        return;
    }
    if ( g_progress_send( server, client_index, MATRIX_CONTROL_CHANNEL, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, report, n ) )
    {
        c->progress_sent++;
    }
}

// section 4, the over-maximum start, once per window per mode on channels 0
// and 31: codes 0 and 1 one past their single-record maxima start as their
// reliable twins (the promotion rule, aborted so nothing is sent); every mode
// one past max_message_bytes is refused, counted once by its named counter.
static void matrix_over_maximum( struct rocketnet_message_server_t * server, struct server_client_t * c, int client_index )
{
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        const int channels[2] = { 0, ROCKETNET_LIVE_NUM_CHANNELS - 1 };

        for ( int i = 0; i < 2; i++ )
        {
            const int channel = channels[i];

            if ( mode <= 1 )
            {
                const uint32_t promoted = rocketnet_live_mode_max( &g_live, mode ) + 1;
                const uint64_t failed_before = server_counter( rocketnet_message_server_counters( server ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
                uint8_t * q = rocketnet_message_server_start( server, client_index, channel, mode, (int) promoted );
                const uint64_t failed_delta = server_counter( rocketnet_message_server_counters( server ), ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) - failed_before;
                if ( q )
                {
                    rocketnet_message_server_abort( server, client_index );
                }
                if ( !q || failed_delta != 0 )
                {
                    matrix_fail( c, 1, mode, channel, -1, "promotion-refused", (int) promoted );
                }
            }

            const uint32_t over = g_live.max_message_bytes + 1;
            const int unreliable = mode <= 1 || mode == 5;
            const int counter = unreliable ? ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED : ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED;
            const int other = unreliable ? ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED : ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED;
            const uint64_t before = server_counter( rocketnet_message_server_counters( server ), counter );
            const uint64_t before_other = server_counter( rocketnet_message_server_counters( server ), other );
            uint8_t * p = rocketnet_message_server_start( server, client_index, channel, mode, (int) over );
            const uint64_t delta = server_counter( rocketnet_message_server_counters( server ), counter ) - before;
            const uint64_t delta_other = server_counter( rocketnet_message_server_counters( server ), other ) - before_other;
            printf( "refused %d %d %u %s %llu\n", mode, channel, over, rocketnet_counter_name( counter ), (unsigned long long) delta );
            if ( p )
            {
                rocketnet_message_server_abort( server, client_index );
            }
            if ( p != NULL || delta != ROCKETNET_SERVER_OVER_MAX_DELTA || delta_other != 0 )
            {
                matrix_fail( c, 1, mode, channel, -1, p ? "start-succeeded" : "wrong-counter", (int) over );
            }

            c->refusals++;
        }
    }
}

static void matrix_open_slot( struct server_client_t * c )
{
    memset( c, 0, sizeof( *c ) );
    snprintf( c->label, sizeof c->label, "%s", g_label );
    c->slot = (struct server_slot_t *) calloc( 1, sizeof( struct server_slot_t ) );
    if ( !c->slot )
    {
        fprintf( stderr, "server: out of memory for a slot\n" );
        exit( 1 );
    }
    matrix_rx_reset( &c->slot->rx );
    c->state = SERVER_SLOT_AWAITING_HELLO;
}

// section 6: the first message of every connection is a HELLO, nibble 0xD,
// carrying the path's cap in kbps, the cycle, the seed, max_message_bytes and
// the bench label. The answer echoes the cycle; the client sends its matrix
// only after it, so every message the server judges carries a known cycle.
static void matrix_hello( struct rocketnet_message_server_t * server, struct server_client_t * c, int client_index, const uint8_t * data, int bytes )
{
    if ( c->state != SERVER_SLOT_AWAITING_HELLO )
    {
        matrix_fail( c, 0, -1, MATRIX_CONTROL_CHANNEL, -1, "second-hello", bytes );
        return;
    }

    if ( bytes < MATRIX_HELLO_BYTES )
    {
        matrix_fail( c, 0, -1, MATRIX_CONTROL_CHANNEL, -1, "bad-hello", bytes );
        return;
    }

    const uint32_t cap = matrix_get_u32( data + 1 );
    const uint32_t cycle = matrix_get_u32( data + 5 );
    const uint32_t seed = matrix_get_u32( data + 9 );
    const uint32_t max_message_bytes = matrix_get_u32( data + 13 );

    if ( bytes > MATRIX_HELLO_BYTES )
    {
        int l = bytes - MATRIX_HELLO_BYTES;
        if ( l > ROCKETNET_SERVER_LABEL_MAX - 1 )
        {
            l = ROCKETNET_SERVER_LABEL_MAX - 1;
        }
        memcpy( c->label, data + MATRIX_HELLO_BYTES, (size_t) l );
        c->label[l] = 0;
    }

    c->cycle = cycle;
    printf( "hello %s %u cycle %u\n", c->label, cap, cycle );
    results_event( "hello %s %u %u", c->label, cap, cycle );

    // both ends compute every length from the seed and max_message_bytes:
    // a mismatch would fail every message, so it is the one failure named
    if ( seed != g_live.seed || max_message_bytes != g_live.max_message_bytes )
    {
        printf( "server: client %s seed %u max_message_bytes %u, server seed %u max_message_bytes %u\n", c->label, seed,
                max_message_bytes, g_live.seed, g_live.max_message_bytes );
        matrix_fail( c, 0, -1, MATRIX_CONTROL_CHANNEL, -1, "config-mismatch", bytes );
        return;
    }

    c->kbps = g_send_kbps;
    if ( cap > 0 )
    {
        rocketnet_message_server_set_send_kbps( server, client_index, (int32_t) cap );
        c->kbps = cap;
    }

    uint8_t answer[MATRIX_HELLO_ANSWER_BYTES];
    answer[0] = MATRIX_HELLO_NIBBLE;
    matrix_put_u32( answer + 1, cycle );
    if ( !rocketnet_message_server_send( server, client_index, MATRIX_CONTROL_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, answer, (int) sizeof answer ) )
    {
        matrix_fail( c, 1, -1, MATRIX_CONTROL_CHANNEL, -1, "hello-answer-refused", (int) sizeof answer );
        return;
    }

    matrix_over_maximum( server, c, client_index );

    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        const uint32_t total = rocketnet_live_sequences_per_stream( &g_live, mode );
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            c->slot->tx[mode][channel].total = total;
        }
    }

    c->state = SERVER_SLOT_ACTIVE;
}

// the client's progress report: per stream, the sequence below which every
// owed one arrived; it releases the server's ledger
static void matrix_progress( struct server_client_t * c, const uint8_t * data, int bytes )
{
    if ( ( bytes - 1 ) % MATRIX_PROGRESS_RECORD != 0 )
    {
        matrix_fail( c, 0, -1, MATRIX_CONTROL_CHANNEL, -1, "bad-progress", bytes );
        return;
    }

    for ( int off = 1; off + MATRIX_PROGRESS_RECORD <= bytes; off += MATRIX_PROGRESS_RECORD )
    {
        const int mode = data[off];
        const int channel = data[off + 1];
        if ( mode >= ROCKETNET_LIVE_NUM_MODES || channel >= matrix_channels( mode ) )
        {
            matrix_fail( c, 0, mode, channel, -1, "bad-progress", bytes );
            continue;
        }

        struct server_tx_stream_t * tx = &c->slot->tx[mode][channel];
        const uint32_t confirmed = matrix_get_u32( data + off + 2 ) + 1u;
        if ( confirmed > tx->next )
        {
            matrix_fail( c, 1, mode, channel, (int64_t) confirmed - 1, "progress-past-sent", bytes );
            continue;
        }
        if ( confirmed > tx->confirmed )
        {
            const uint64_t released = matrix_ledger_bytes( &g_live, mode, channel, tx->confirmed, confirmed );
            c->outstanding = c->outstanding > released ? c->outstanding - released : 0;
            tx->confirmed = confirmed;
        }
    }
}

static void matrix_arrival( struct rocketnet_message_server_t * server, struct server_client_t * c,
                            int client_index, int receive_channel, const uint8_t * data, int bytes )
{
    if ( c->state == SERVER_SLOT_IDLE )
    {
        matrix_fail( c, 0, -1, receive_channel, -1, "no-connection", bytes );
        return;
    }

    if ( bytes < 1 )
    {
        matrix_fail( c, 0, -1, receive_channel, -1, "bad-header", bytes );
        return;
    }

    const uint32_t nibble = data[0] & MATRIX_NIBBLE_MASK;
    if ( nibble == MATRIX_HELLO_NIBBLE || nibble == MATRIX_SENT_NIBBLE || nibble == MATRIX_PROGRESS_NIBBLE )
    {
        if ( receive_channel != MATRIX_CONTROL_CHANNEL )
        {
            matrix_fail( c, 0, -1, receive_channel, -1, "receive-channel", bytes );
            return;
        }

        if ( nibble == MATRIX_HELLO_NIBBLE )
        {
            matrix_hello( server, c, client_index, data, bytes );
            return;
        }

        if ( nibble == MATRIX_PROGRESS_NIBBLE )
        {
            if ( c->state != SERVER_SLOT_ACTIVE && c->state != SERVER_SLOT_DRAINING )
            {
                matrix_fail( c, 0, -1, receive_channel, -1, "before-hello", bytes );
                return;
            }
            matrix_progress( c, data, bytes );
            return;
        }

        if ( c->state != SERVER_SLOT_ACTIVE )
        {
            matrix_fail( c, 0, -1, receive_channel, -1, "sent-report-out-of-turn", bytes );
            return;
        }

        // the client stopped sending: judge against its counts, stop the
        // server's own sends and answer with the server's counts
        matrix_rx_sent_report( &g_live, &c->slot->rx, data, bytes, matrix_rx_failed, c );
        c->state = SERVER_SLOT_DRAINING;
        c->sent_report_due = 1;
        return;
    }

    if ( c->state != SERVER_SLOT_ACTIVE && c->state != SERVER_SLOT_DRAINING )
    {
        matrix_fail( c, 0, -1, receive_channel, -1, "before-hello", bytes );
        return;
    }

    if ( nibble != ROCKETNET_LIVE_TAG_BASE || ( ( data[0] >> 3 ) & 1 ) != 0 )
    {
        matrix_fail( c, 0, -1, receive_channel, -1, "bad-header", bytes );
        return;
    }

    const int mode = data[0] & 7;
    const int channel = bytes >= 2 ? data[1] : receive_channel;
    const int want_channel = matrix_carries_channel( mode ) ? channel : 0;
    if ( receive_channel != want_channel || channel >= matrix_channels( mode ) )
    {
        matrix_fail( c, 0, mode, channel, -1, "receive-channel", bytes );
        return;
    }

    struct server_rx_cell_t * cell = &c->slot->cells[mode][channel][rocketnet_live_bucket( &g_live, mode, (uint32_t) bytes )];
    cell->received++;

    struct matrix_rx_stream_t * s = &c->slot->rx.streams[mode][channel];
    const int64_t expected = matrix_ordered( mode ) ? (int64_t) s->expected : -1;
    int offset = -1;
    const int result = rocketnet_live_check( &g_live, data, bytes, 0, mode, channel, c->cycle, expected, &offset );
    if ( result != ROCKETNET_LIVE_OK )
    {
        cell->failed++;
        matrix_fail( c, 0, mode, channel, bytes >= ROCKETNET_SERVER_TINY ? (int64_t) rocketnet_live_sequence( data, bytes ) : expected,
                     rocketnet_live_result_name( result ), bytes );
        return;
    }

    const char * reason = NULL;
    int64_t sequence = -1;
    if ( matrix_counted_tiny( mode, (uint32_t) bytes ) )
    {
        reason = matrix_rx_tiny( &c->slot->rx, mode, bytes );
    }
    else
    {
        reason = matrix_rx_accept( &g_live, s, mode, channel, data, bytes, &sequence );
    }

    if ( reason )
    {
        cell->failed++;
        if ( strcmp( reason, "out-of-order" ) == 0 )
        {
            cell->ooo++;
        }
        else if ( strncmp( reason, "duplicate", 9 ) == 0 )
        {
            cell->dup++;
        }
        matrix_fail( c, 0, mode, channel, sequence, reason, bytes );
        return;
    }

    cell->verified++;
    c->verified++;
}

// the server's one receive queue across every client: each message names its
// sender's slot, and the channel it was delivered on
static void matrix_receive( struct rocketnet_message_server_t * server, struct server_client_t * clients )
{
    const uint8_t * data = NULL;
    int bytes = 0;
    int client_index = 0;
    int channel = 0;
    while ( rocketnet_message_server_receive( server, &client_index, &channel, &data, &bytes ) )
    {
        matrix_arrival( server, &clients[client_index], client_index, channel, data, bytes );
        rocketnet_message_server_release( server, data );
    }
}

// One pass of the server's own streams: each sends its next sequences until
// the ledger holds a reliable send or the library refuses a start for want of
// room; either is counted as paced and tried again on the next pass.
static void matrix_send_pass( struct rocketnet_message_server_t * server, struct server_client_t * c, int client_index )
{
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            struct server_tx_stream_t * tx = &c->slot->tx[mode][channel];
            while ( tx->next < tx->total )
            {
                const uint32_t length = rocketnet_live_length( &g_live, mode, channel, tx->next );
                const int bucket = rocketnet_live_bucket( &g_live, mode, length );
                const int reliable = matrix_reliable_path( mode, length );
                if ( reliable && !matrix_ledger_admits( c->outstanding, length, c->kbps ) )
                {
                    tx->bucket_paced[bucket]++;
                    c->refused++;
                    break;
                }

                uint8_t * p = rocketnet_message_server_start( server, client_index, channel, mode, (int) length );
                if ( !p )
                {
                    tx->bucket_paced[bucket]++;
                    c->refused++;
                    break;
                }

                const int written = rocketnet_live_generate( &g_live, 1, mode, channel, c->cycle, tx->next, p, (int) length );
                if ( written != (int) length )
                {
                    rocketnet_message_server_abort( server, client_index );
                    matrix_fail( c, 1, mode, channel, tx->next, "generate-failed", (int) length );
                    tx->next = tx->total;
                    break;
                }

                if ( rocketnet_message_server_finish( server, client_index, written ) == 0 )
                {
                    matrix_fail( c, 1, mode, channel, tx->next, "finish-failed", written );
                    tx->next = tx->total;
                    break;
                }

                tx->bucket_sent[bucket]++;
                tx->next++;
                if ( reliable )
                {
                    c->outstanding += length;
                }
            }
        }
    }
}

// the server's sent report: tag 0xE0, then per stream its count
static void matrix_send_sent_report( struct rocketnet_message_server_t * server, struct server_client_t * c, int client_index )
{
    uint8_t report[1 + ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * MATRIX_SENT_RECORD];
    int off = 0;
    report[off++] = MATRIX_SENT_NIBBLE;
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            report[off++] = (uint8_t) mode;
            report[off++] = (uint8_t) channel;
            matrix_put_u32( report + off, c->slot->tx[mode][channel].next );
            off += 4;
        }
    }

    if ( rocketnet_message_server_send( server, client_index, MATRIX_CONTROL_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, report, off ) )
    {
        c->sent_report_due = 0;
    }
}

static void matrix_write_cells( struct server_client_t * c )
{
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            const struct server_tx_stream_t * tx = &c->slot->tx[mode][channel];

            for ( int bucket = 0; bucket < ROCKETNET_LIVE_NUM_BUCKETS; bucket++ )
            {
                // the server to client direction: what this server sent; the client's rows carry what arrived
                if ( tx->bucket_sent[bucket] || tx->bucket_paced[bucket] )
                {
                    results_line( "cell", "%s\t%u\t1\t%d\t%d\t%s\t%u\t0\t0\t0\t0\t0\t0\t0\t0\t0\t%u",
                                  c->label, c->cycle, mode, channel, rocketnet_live_bucket_name( bucket ),
                                  tx->bucket_sent[bucket], tx->bucket_paced[bucket] );
                }

                // the client to server direction: what arrived; the client's rows carry what it sent
                const struct server_rx_cell_t * r = &c->slot->cells[mode][channel][bucket];
                if ( r->received || r->failed || r->missing )
                {
                    results_line( "cell", "%s\t%u\t0\t%d\t%d\t%s\t0\t%u\t%u\t%u\t0\t0\t%u\t%u\t%u\t0\t0",
                                  c->label, c->cycle, mode, channel, rocketnet_live_bucket_name( bucket ),
                                  r->received, r->verified, r->failed, r->missing, r->ooo, r->dup );
                }
            }
        }
    }
}

// The connection ended: every owed sequence below the client's counts must
// have arrived, and every owed tiny message of each length.
static void matrix_close_slot( struct server_client_t * c )
{
    if ( c->state == SERVER_SLOT_AWAITING_HELLO )
    {
        matrix_fail( c, 0, -1, MATRIX_CONTROL_CHANNEL, -1, "no-hello", 0 );
    }
    else if ( c->state == SERVER_SLOT_ACTIVE )
    {
        matrix_fail( c, 0, -1, MATRIX_CONTROL_CHANNEL, -1, "no-sent-report", 0 );
    }
    else if ( c->state == SERVER_SLOT_DRAINING )
    {
        struct matrix_rx_t * rx = &c->slot->rx;
        for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
        {
            for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
            {
                uint32_t by_bucket[ROCKETNET_LIVE_NUM_BUCKETS] = { 0 };
                const uint32_t missing = matrix_rx_missing( &g_live, &rx->streams[mode][channel], mode, channel, by_bucket );
                if ( missing )
                {
                    matrix_fail( c, 0, mode, channel, rx->streams[mode][channel].owed, "lost", (int) missing );
                }
                for ( int bucket = 0; bucket < ROCKETNET_LIVE_NUM_BUCKETS; bucket++ )
                {
                    c->slot->cells[mode][channel][bucket].missing += by_bucket[bucket];
                }
            }

            for ( int length = 1; length < ROCKETNET_LIVE_HEADER_BYTES; length++ )
            {
                if ( matrix_owed( mode, (uint32_t) length ) && rx->tiny_received[mode][length] < rx->tiny_sent[mode][length] )
                {
                    matrix_fail( c, 0, mode, -1, -1, "missing-tiny", length );
                }
            }
        }
    }

    if ( c->state != SERVER_SLOT_AWAITING_HELLO )
    {
        matrix_write_cells( c );
    }

    printf( "window %s %u verified %llu refused %llu failures %llu progress %llu\n", c->label, c->cycle,
            (unsigned long long) c->verified, (unsigned long long) c->refused, (unsigned long long) c->failures,
            (unsigned long long) c->progress_sent );
    results_event( "window %s %u verified %llu refused %llu failures %llu", c->label, c->cycle, (unsigned long long) c->verified,
                   (unsigned long long) c->refused, (unsigned long long) c->failures );
    fflush( stdout );

    free( c->slot );
    c->slot = NULL;
    c->state = SERVER_SLOT_IDLE;
}

struct progress_record_t
{
    int calls;
    int client_index;
    int channel;
    int reliability;
    int bytes;
    uint8_t data[MATRIX_REPORT_BYTES];
};

static struct progress_record_t g_progress_record;

// section 5: records one report instead of sending it, so the self-test sees
// the channel, the reliability and the payload the server emits.
static int progress_record( struct rocketnet_message_server_t * server, int client_index, int channel,
                            int reliability, const uint8_t * data, int bytes )
{
    (void) server;
    g_progress_record.calls++;
    g_progress_record.client_index = client_index;
    g_progress_record.channel = channel;
    g_progress_record.reliability = reliability;
    g_progress_record.bytes = bytes;
    if ( bytes > 0 && bytes <= (int) sizeof g_progress_record.data )
    {
        memcpy( g_progress_record.data, data, (size_t) bytes );
    }

    return 1;
}

static void self_test_fail( void * context, int mode, int channel, int64_t sequence, const char * reason, int bytes )
{
    (void) mode;
    (void) channel;
    (void) sequence;
    (void) bytes;
    printf( "server self-test rx %s\n", reason );
    ( *(int *) context )++;
}

// The receive rule of tests/matrix_receive.h without a socket:
// a duplicate, a sequence past the sender's count, an out-of-order arrival,
// a sequenced step back and an owed sequence that never came are each named.
static int self_test_receive( void )
{
    int failures = 0;
    struct rocketnet_live_config_t live = g_live;
    struct matrix_rx_t * rx = (struct matrix_rx_t *) calloc( 1, sizeof( struct matrix_rx_t ) );
    if ( !rx )
    {
        return 1;
    }
    matrix_rx_reset( rx );

    uint8_t message[ROCKETNET_LIVE_DEFAULT_PAYLOAD_BYTES * 2];
    int64_t sequence = -1;
    const char * reasons[6] = { NULL, NULL, NULL, NULL, NULL, NULL };

    // mode 2 (owed, any order) on channel 1: sequences 1, 2, 1 again
    {
        const int n1 = rocketnet_live_generate( &live, 0, 2, 1, 0, 1, message, (int) sizeof message );
        reasons[0] = matrix_rx_accept( &live, &rx->streams[2][1], 2, 1, message, n1, &sequence );
        const int n2 = rocketnet_live_generate( &live, 0, 2, 1, 0, 2, message, (int) sizeof message );
        reasons[1] = matrix_rx_accept( &live, &rx->streams[2][1], 2, 1, message, n2, &sequence );
        const int n3 = rocketnet_live_generate( &live, 0, 2, 1, 0, 1, message, (int) sizeof message );
        reasons[2] = matrix_rx_accept( &live, &rx->streams[2][1], 2, 1, message, n3, &sequence );
    }

    // mode 3 (ordered) on channel 4: sequence 1 before 0
    {
        const int n = rocketnet_live_generate( &live, 0, 3, 4, 0, 1, message, (int) sizeof message );
        reasons[3] = matrix_rx_accept( &live, &rx->streams[3][4], 3, 4, message, n, &sequence );
    }

    // mode 1 (sequenced) on channel 2: 9 then 8
    {
        const int n1 = rocketnet_live_generate( &live, 0, 1, 2, 0, 9, message, (int) sizeof message );
        (void) matrix_rx_accept( &live, &rx->streams[1][2], 1, 2, message, n1, &sequence );
        const int n2 = rocketnet_live_generate( &live, 0, 1, 2, 0, 8, message, (int) sizeof message );
        reasons[4] = matrix_rx_accept( &live, &rx->streams[1][2], 1, 2, message, n2, &sequence );
    }

    // the sender reports 2 sent on (2, 1): sequence 2 was never sent, and 0 is owed and lost
    int report_failures = 0;
    uint8_t report[1 + 2 * MATRIX_SENT_RECORD];
    report[0] = MATRIX_SENT_NIBBLE;
    report[1] = 2;
    report[2] = 1;
    matrix_put_u32( report + 3, 2 );
    report[7] = 1;
    report[8] = 2;
    matrix_put_u32( report + 9, 10 );
    matrix_rx_sent_report( &live, rx, report, (int) sizeof report, self_test_fail, &report_failures );

    uint32_t by_bucket[ROCKETNET_LIVE_NUM_BUCKETS] = { 0 };
    const uint32_t lost = matrix_rx_missing( &live, &rx->streams[2][1], 2, 1, by_bucket );

    const int n4 = rocketnet_live_generate( &live, 0, 2, 1, 0, 5, message, (int) sizeof message );
    reasons[5] = matrix_rx_accept( &live, &rx->streams[2][1], 2, 1, message, n4, &sequence );

    printf( "server self-test rx %s %s %s %s %s %s report-failures %d lost %u complete %d\n", reasons[0] ? reasons[0] : "ok",
            reasons[1] ? reasons[1] : "ok", reasons[2] ? reasons[2] : "ok", reasons[3] ? reasons[3] : "ok",
            reasons[4] ? reasons[4] : "ok", reasons[5] ? reasons[5] : "ok", report_failures, lost, matrix_rx_complete( rx ) );

    if ( reasons[0] || reasons[1] || !reasons[2] || strcmp( reasons[2], "duplicate" ) != 0 || !reasons[3] ||
         strcmp( reasons[3], "out-of-order" ) != 0 || !reasons[4] || strcmp( reasons[4], "not-increasing" ) != 0 || !reasons[5] ||
         strcmp( reasons[5], "never-sent" ) != 0 || report_failures != 1 || lost != 1 || matrix_rx_complete( rx ) )
    {
        failures++;
    }

    free( rx );
    return failures;
}

// The server-side checks the card asks for without a socket: a nonzero seed
// moves the length schedule (sections 2 and 4), the 0xC report of section 5
// encodes a stream's highest owed-contiguous sequence and bytes received, the
// same report is emitted on channel 31 at the 50 ms cadence (section 5), and
// the receive rule names each fault. Writes a sample result file and returns
// 0 when all hold.
static int run_self_test( const char * path, const char * label, uint32_t seed )
{
    int failures = 0;

    struct rocketnet_live_config_t zero = g_live;
    zero.seed = 0;

    struct rocketnet_live_config_t seeded = g_live;
    seeded.seed = seed ? seed : 1u;

    int changed = 0;

    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            for ( uint32_t sequence = 1; sequence < 32; sequence++ )
            {
                if ( rocketnet_live_length( &zero, mode, channel, sequence ) !=
                     rocketnet_live_length( &seeded, mode, channel, sequence ) )
                {
                    changed++;
                }
            }
        }
    }

    printf( "server self-test seed-lengths %d\n", changed );
    if ( changed == 0 )
    {
        failures++;
    }

    struct server_client_t c;
    matrix_open_slot( &c );
    c.slot->rx.streams[0][3].owed = 2;
    c.slot->rx.streams[0][3].bytes = 40;
    c.slot->rx.streams[2][0].owed = 5;
    c.slot->rx.streams[2][0].bytes = 100;

    uint8_t report[MATRIX_REPORT_BYTES];
    const int n = matrix_rx_progress( &c.slot->rx, report, (int) sizeof report );
    const int records = n > 1 ? ( n - 1 ) / MATRIX_PROGRESS_RECORD : 0;
    printf( "server self-test report tag %u records %d bytes %d\n", (unsigned) report[0], records, n );
    if ( report[0] != MATRIX_PROGRESS_NIBBLE || records != 2 )
    {
        failures++;
    }
    else
    {
        const uint32_t hi0 = matrix_get_u32( report + 3 );
        const uint32_t bytes0 = matrix_get_u32( report + 7 );
        if ( report[1] != 0 || report[2] != 3 || hi0 != 1u || bytes0 != 40u )
        {
            failures++;
        }
    }

    // section 5: the server emits the report itself, on channel 31, as
    // UNRELIABLE_SEQUENCED, and no more often than every 50 ms. A clock that
    // starts at 1000.0 keeps the cadence an argument, not the wall clock.

    struct server_client_t silent;
    matrix_open_slot( &silent );

    struct server_client_t emitted;
    matrix_open_slot( &emitted );
    emitted.slot->rx.streams[0][3].owed = 2;
    emitted.slot->rx.streams[0][3].bytes = 40;
    memset( &g_progress_record, 0, sizeof g_progress_record );
    g_progress_send = progress_record;
    matrix_send_progress( NULL, 5, &silent, 1000.0 );

    const int silent_calls = g_progress_record.calls;

    matrix_send_progress( NULL, 4, &emitted, 1000.0 );
    matrix_send_progress( NULL, 4, &emitted, 1000.02 );
    matrix_send_progress( NULL, 4, &emitted, 1000.06 );
    g_progress_send = server_progress_send;

    const int calls = g_progress_record.calls;

    printf( "server self-test emission calls %d channel %d reliability %d bytes %d sent %llu\n",
            calls, g_progress_record.channel, g_progress_record.reliability, g_progress_record.bytes,
            (unsigned long long) emitted.progress_sent );
    if ( silent_calls != 0 || calls != 2 ||
         g_progress_record.client_index != 4 ||
         g_progress_record.channel != MATRIX_CONTROL_CHANNEL ||
         g_progress_record.reliability != ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
         g_progress_record.bytes != 11 ||
         g_progress_record.data[0] != MATRIX_PROGRESS_NIBBLE ||
         emitted.progress_sent != 2 )
    {
        failures++;
    }

    free( c.slot );
    free( silent.slot );
    free( emitted.slot );

    failures += self_test_receive();

    if ( path )
    {
        g_results = rocketnet_test_fopen( path, "w" );
        if ( g_results )
        {
            results_line( "benchmarks", "%s\t%u\t500\t1000\tself-test", label, seed );
            results_line( "cell", "%s\t1\t1\t2\t0\tsmall\t5\t5\t0\t0\t0\t0\t0\t0\t0\t0\t0", label );
            results_event( "self-test %s", failures ? "FAIL" : "PASS" );
            fclose( g_results );
            g_results = NULL;
        }
    }

    printf( "server self-test %s\n", failures ? "FAIL" : "PASS" );
    return failures ? 1 : 0;
}


int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    int matrix = 0;
    int self_test = 0;
    const char * self_test_path = NULL;
    const char * address = NULL;
    const char * label = "server";
    const char * transport = NULL;
    const char * results_path = NULL;
    const char * status_path = NULL;
    int max_clients = 1000;
    int packets_per_update = 64;
    double counters_every = 60.0;
    double update_hz = 1000.0;
    double duration = 0.0;
    int num_xdp_queues = 0;
    int simulator[4] = { 0, 0, 0, 0 };
    int update_hz_given = 0;
    uint32_t seed = 0;
    uint32_t send_kbps = 500;

    for ( int i = 1; i < argc; i++ )
    {
        const char * a = argv[i];
        if ( strcmp( a, "--help" ) == 0 || strcmp( a, "-h" ) == 0 )
        {
            usage();
            return 0;
        }
        else if ( strcmp( a, "--matrix" ) == 0 )
        {
            matrix = 1;
        }
        else if ( strcmp( a, "--address" ) == 0 && i + 1 < argc )
        {
            address = argv[++i];
        }
        else if ( strcmp( a, "--label" ) == 0 && i + 1 < argc )
        {
            label = argv[++i];
        }
        else if ( strcmp( a, "--seed" ) == 0 && i + 1 < argc )
        {
            seed = (uint32_t) strtoul( argv[++i], NULL, 10 );
        }
        else if ( strcmp( a, "--send-kbps" ) == 0 && i + 1 < argc )
        {
            send_kbps = (uint32_t) strtoul( argv[++i], NULL, 10 );
        }
        else if ( strcmp( a, "--max-clients" ) == 0 && i + 1 < argc )
        {
            max_clients = (int) strtol( argv[++i], NULL, 10 );
        }
        else if ( strcmp( a, "--max-packets-per-update" ) == 0 && i + 1 < argc )
        {
            packets_per_update = (int) strtol( argv[++i], NULL, 10 );
        }
        else if ( strcmp( a, "--update-hz" ) == 0 && i + 1 < argc )
        {
            update_hz = strtod( argv[++i], NULL );
            if ( !( update_hz >= 1.0 && update_hz <= 100000.0 ) )
            {
                fprintf( stderr, "server: --update-hz must be 1 to 100000\n" );
                return 1;
            }
            update_hz_given = 1;
        }
        else if ( strcmp( a, "--num-xdp-queues" ) == 0 && i + 1 < argc )
        {
            num_xdp_queues = (int) strtol( argv[++i], NULL, 10 );
        }
        else if ( strcmp( a, "--simulator" ) == 0 && i + 1 < argc )
        {
            const char * p = argv[++i];
            for ( int k = 0; k < 4; k++ )
            {
                char * end = NULL;
                simulator[k] = (int) strtol( p, &end, 10 );
                if ( end == p || *end != ( k < 3 ? ',' : '\0' ) )
                {
                    fprintf( stderr, "server: --simulator wants latency,jitter,loss,duplicate\n" );
                    return 1;
                }
                p = end + 1;
            }
        }
        else if ( strcmp( a, "--duration" ) == 0 && i + 1 < argc )
        {
            duration = strtod( argv[++i], NULL );
        }
        else if ( strcmp( a, "--counters-every" ) == 0 && i + 1 < argc )
        {
            counters_every = strtod( argv[++i], NULL );
        }
        else if ( strcmp( a, "--results" ) == 0 && i + 1 < argc )
        {
            results_path = argv[++i];
        }
        else if ( strcmp( a, "--status" ) == 0 && i + 1 < argc )
        {
            status_path = argv[++i];
        }
        else if ( strcmp( a, "--transport" ) == 0 && i + 1 < argc )
        {
            transport = argv[++i];
        }
        else if ( strcmp( a, "--self-test-result" ) == 0 && i + 1 < argc )
        {
            self_test = 1;
            self_test_path = argv[++i];
        }
        else if ( strcmp( a, "--i40e-kernel-check" ) == 0 && i + 1 < argc )
        {
            // section 9's kernel rule on a given release: exit 1 refused, 0 allowed
            // (scripts/live-xdp-fleet-remote i40e-guard-selftest)
            const int refused = kernel_release_before_i40e_soak( argv[++i] );
            printf( "%s\n", refused ? "refused" : "allowed" );
            return refused;
        }
        else if ( a[0] != '-' && !address )
        {
            address = a;
        }
        else
        {
            fprintf( stderr, "server: unknown argument: %s\n", a );
            usage();
            return 1;
        }
    }

    // the echo loop keeps its historical 60 Hz unless --update-hz is given
    const double echo_sleep = update_hz_given ? 1.0 / update_hz : 1.0 / 60.0;

    if ( !address )
    {
        address = "127.0.0.1:40000";
    }

    g_live.seed = seed;
    g_label = label;
    g_send_kbps = send_kbps;

    if ( self_test )
    {
        return run_self_test( self_test_path, label, seed );
    }

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;
    config.num_xdp_queues = num_xdp_queues;
    config.simulated_latency_ms = simulator[0];
    config.simulated_jitter_ms = simulator[1];
    config.simulated_loss_percent = simulator[2];
    config.simulated_duplicate_percent = simulator[3];

    char env_transport[sizeof config.transport];

    if ( transport )
    {
        snprintf( config.transport, sizeof config.transport, "%s", transport );
    }
    else if ( rocketnet_test_getenv( "ROCKETNET_SERVER_TRANSPORT", env_transport, sizeof env_transport ) )
    {
        snprintf( config.transport, sizeof config.transport, "%s", env_transport );
    }

    // section 9: when xdp is pinned, refuse the in-tree i40e on a kernel older
    // than 7.0.0-43, the one its zero-copy soak passed on (docs/LINUX.md). The
    // in-tree i40e of 7.0.0-31 panicked twice in i40e_construct_skb_zc

    if ( strcmp( config.transport, "xdp" ) == 0 )
    {
        char nic[64] = { 0 };
        if ( interface_for_address( address, nic, sizeof nic ) && in_tree_i40e( nic ) && kernel_before_i40e_soak() )
        {
            fprintf( stderr, "server: refusing: interface %s reports the in-tree i40e on a kernel older than 7.0.0-43\n", nic );
            rocketnet_term();
            return 1;
        }
    }

    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    message_config.packets_per_update = packets_per_update;
    g_live.max_message_bytes = (uint32_t) message_config.max_message_bytes;

    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, address, &config, &message_config, NULL, 0, send_kbps );
    if ( !server )
    {
        rocketnet_term();
        return 1;
    }

    struct rocketnet_server_transport_t * transport_obj = rocketnet_message_server_transport( server );
    printf( "server transport %s\n", rocketnet_server_transport_io_name( transport_obj ) );
    printf( "server max_clients %d\n", config.max_clients );

    if ( results_path )
    {
        g_results = rocketnet_test_fopen( results_path, "w" );
        if ( g_results )
        {
            results_line( "benchmarks", "%s\t%u\t%u\t%d\t%s", label, seed, send_kbps, max_clients, rocketnet_server_transport_io_name( transport_obj ) );
            results_event( "listening" );
        }
    }

    set_status( status_path, "listening" );

    const int watched = stdin_watched();
    const double started = rocketnet_time();

    if ( !matrix )
    {
        // the payload echo, exactly as before

        int last_num_connected = 0;
        double last_counters = 0.0;

        while ( !quit )
        {
            const double update_start = rocketnet_time();
            rocketnet_server_transport_update( transport_obj, update_start );
            update_times_add( &g_update_times, rocketnet_time() - update_start );

            const int num_connected = rocketnet_server_transport_num_connected_clients( transport_obj );
            if ( num_connected != last_num_connected )
            {
                printf( "server has %d client%s connected\n", num_connected, num_connected == 1 ? "" : "s" );
                last_num_connected = num_connected;
            }

            const int num_payloads = rocketnet_server_transport_num_payloads( transport_obj );
            for ( int i = 0; i < num_payloads; i++ )
            {
                int client_index, payload_bytes;
                uint8_t * payload = rocketnet_server_transport_payload( transport_obj, i, &client_index, &payload_bytes );
                rocketnet_server_transport_send_packet( transport_obj, client_index, payload, payload_bytes );
                // the message server defers payload acks to whoever consumes the payload; the echo consumes it here
                rocketnet_server_transport_accept_payload( transport_obj, i );
            }

            const double send_start = rocketnet_time();
            rocketnet_server_transport_send_packets( transport_obj );
            update_times_add( &g_send_times, rocketnet_time() - send_start );

            const double now = rocketnet_time();
            if ( counters_every > 0.0 && now - last_counters >= counters_every )
            {
                dump_counters( rocketnet_server_transport_counters( transport_obj ) );
                update_times_print( "update", &g_update_times );
                update_times_print( "send_packets", &g_send_times );
                last_counters = now;
            }

            check_stdin_closed( watched );
            if ( duration > 0.0 && rocketnet_time() - started >= duration )
            {
                quit = 1;
            }
            rocketnet_sleep( echo_sleep );
        }
    }
    else
    {
        // the matrix mode: one window per connected client, in parallel

        struct server_client_t * clients = calloc( (size_t) max_clients, sizeof( *clients ) );
        if ( !clients )
        {
            rn_native_message_server_stop_and_destroy( server );
            rocketnet_term();
            return 1;
        }

        for ( int i = 0; i < max_clients; i++ )
        {
            snprintf( clients[i].label, sizeof clients[i].label, "%s", label );
        }

        const double step = update_hz > 0.0 ? 1.0 / update_hz : 0.0005;
        double last_counters = 0.0;

        while ( !quit )
        {
            const double now = rocketnet_time();
            rocketnet_message_server_update( server, now );

            // a slot opens when its client connects, before that client's
            // first message is read
            for ( int i = 0; i < max_clients; i++ )
            {
                if ( clients[i].state == SERVER_SLOT_IDLE && rocketnet_message_server_client_connected( server, i ) )
                {
                    const uint64_t windows = clients[i].windows;
                    matrix_open_slot( &clients[i] );
                    clients[i].windows = windows;
                }
            }

            matrix_receive( server, clients );

            for ( int i = 0; i < max_clients; i++ )
            {
                struct server_client_t * c = &clients[i];
                if ( c->state == SERVER_SLOT_IDLE )
                {
                    continue;
                }

                if ( !rocketnet_message_server_client_connected( server, i ) )
                {
                    matrix_close_slot( c );
                    c->windows++;
                    continue;
                }

                if ( c->state == SERVER_SLOT_ACTIVE )
                {
                    matrix_send_pass( server, c, i );
                }

                if ( c->sent_report_due )
                {
                    matrix_send_sent_report( server, c, i );
                }

                if ( c->state == SERVER_SLOT_ACTIVE || c->state == SERVER_SLOT_DRAINING )
                {
                    matrix_send_progress( server, i, c, rocketnet_time() );
                }
            }

            rocketnet_message_server_send_packets( server );

            if ( counters_every > 0.0 && now - last_counters >= counters_every )
            {
                dump_counters( rocketnet_message_server_counters( server ) );
                last_counters = now;
            }

            check_stdin_closed( watched );
            if ( duration > 0.0 && rocketnet_time() - started >= duration )
            {
                quit = 1;
            }

            rocketnet_sleep( step );
        }

        // a window still open at the stop was cut: its checks run, and fail
        // where the client had not finished
        for ( int i = 0; i < max_clients; i++ )
        {
            if ( clients[i].state != SERVER_SLOT_IDLE )
            {
                matrix_close_slot( &clients[i] );
                clients[i].windows++;
            }
        }

        uint64_t windows = 0;
        for ( int i = 0; i < max_clients; i++ )
        {
            windows += clients[i].windows;
        }

        printf( "server matrix windows %llu failures %llu verdict %s\n", (unsigned long long) windows, (unsigned long long) g_failures,
                g_failures ? "FAIL" : "PASS" );
        results_line( "verdict", "%s", g_failures ? "FAIL" : "PASS" );

        free( clients );
    }

    printf( "\nserver stopping\n" );
    results_event( "stopping" );
    set_status( status_path, "stopping" );
    rn_native_message_server_stop_and_destroy( server );

    rocketnet_term();

    results_event( "stopped" );
    set_status( status_path, "stopped" );

    if ( g_results )
    {
        fclose( g_results );
        g_results = NULL;
    }

    return g_failures ? 1 : 0;
}
