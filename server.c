/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The out-of-the-box server: runs with the published development keys, so
    a first server needs no backend and no configuration -- run it, point a
    client at it, connected. Echoes every payload back to its sender. Pass
    the public address clients dial: ./bin/server 192.0.2.10:40000

    The live XDP fleet run (docs/LIVE-XDP.md): `--matrix` runs the message
    matrix instead of the payload echo. The server sends its own
    sequence-derived stream to each client and verifies what it receives,
    using tests/live_verify.h (sections 3 to 5). It runs at the max_clients
    the run sets, dumps counters and resident memory each minute, exits when
    its controlling ssh or stdin closes, and refuses to start when the XDP
    transport is pinned and the interface reports the in-tree transport (section 9).
*/

#include "tests/native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_counters.h"
#include "tests/live_verify.h"
#include "tests/portable_crt.h"

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/select.h>
#include <sys/stat.h>
#endif

#define ROCKETNET_SERVER_LABEL_MAX           64
#define ROCKETNET_SERVER_DIRECTIONS           2
#define ROCKETNET_SERVER_TINY                 ROCKETNET_LIVE_HEADER_BYTES
#define ROCKETNET_SERVER_MAX_SEQUENCES       256
#define ROCKETNET_SERVER_OVER_MAX_DELTA      1
#define ROCKETNET_SERVER_HELLO_NIBBLE        0xD0u
#define ROCKETNET_SERVER_PROGRESS_NIBBLE     0xC0u
#define ROCKETNET_SERVER_PROGRESS_CHANNEL    31
#define ROCKETNET_SERVER_PROGRESS_SECONDS    0.05
#define ROCKETNET_SERVER_SEND_BUFFER_HALF    400000u
#define ROCKETNET_SERVER_SEND_QUEUE_HALF     512u
#define ROCKETNET_SERVER_WINDOW_HALF         512u

static volatile int quit;

// section 2: the run seed offsets the length rule (section 4), and both ends
// get the same seed, so every schedule computation in this program reads it
// from here rather than a literal.
static struct rocketnet_live_config_t g_live = { .seed = 0, .max_message_bytes = 262144 };

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
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
        "  --counters-every S      print counters, RSS and transport stats every S seconds\n"
        "  --results PATH          where the result lines go\n"
        "  --status PATH           one line per state change, for the orchestrator\n"
        "  --transport T           pin the transport (e.g. xdp); the in-tree transport refuses\n"
        "  --self-test-result FILE write a sample result file without any socket and exit\n"
        "  --help                  this message\n"
        "environment:\n"
        "  ROCKETNET_SERVER_TRANSPORT   the transport candidate list\n"
        "  ROCKETNET_SERVER_PRINT_COUNTERS=1  every server counter at stop\n" );
}

static void set_status( const char * path, const char * line )
{
    if ( !path )
        return;
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
__attribute__(( format( printf, 2, 3 ) ))
#endif
static void results_line( const char * kind, const char * fmt, ... )
{
    if ( !g_results )
        return;
    va_list ap;
    va_start( ap, fmt );
    fprintf( g_results, "%s\t", kind );
    vfprintf( g_results, fmt, ap );
    fprintf( g_results, "\n" );
    va_end( ap );
    fflush( g_results );
}

#if defined( __GNUC__ ) || defined( __clang__ )
__attribute__(( format( printf, 1, 2 ) ))
#endif
static void results_event( const char * fmt, ... )
{
    if ( !g_results )
        return;
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
#if defined(_WIN32)
    return 0;
#else
    FILE * f = fopen( "/proc/self/statm", "r" );
    if ( !f )
        return 0;
    long total = 0, resident = 0;
    if ( fscanf( f, "%ld %ld", &total, &resident ) != 2 )
        resident = 0;
    fclose( f );
    const long page = sysconf( _SC_PAGESIZE );
    return (uint64_t) resident * (uint64_t) ( page > 0 ? page : 4096 );
#endif
}

// the card: the server exits when its controlling ssh or stdin closes
static void check_stdin_closed( void )
{
#if !defined(_WIN32)
    fd_set rfds;
    FD_ZERO( &rfds );
    FD_SET( 0, &rfds );
    struct timeval tv = { 0, 0 };
    if ( select( 1, &rfds, NULL, NULL, &tv ) > 0 )
    {
        char buffer[64];
        const ssize_t n = read( 0, buffer, sizeof buffer );
        if ( n == 0 )
            quit = 1;
    }
#endif
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

// the interface that carries an IPv4 address, via `ip -o -4 addr show`
static int interface_for_address( const char * address, char * out, size_t out_size )
{
#if defined(_WIN32)
    (void) address; (void) out; (void) out_size;
    return 0;
#else
    char cmd[256];
    snprintf( cmd, sizeof cmd, "ip -o -4 addr show 2>/dev/null | awk -v a=%s '$4 ~ a\"$\" {print $2; exit}'", address );
    FILE * f = popen( cmd, "r" );
    if ( !f )
        return 0;
    if ( fgets( out, (int) out_size, f ) )
    {
        size_t len = strlen( out );
        while ( len && ( out[len-1] == '\n' || out[len-1] == '\r' ) )
            out[--len] = 0;
        pclose( f );
        return len > 0;
    }
    pclose( f );
    return 0;
#endif
}

// Section 9: the in-tree i40e reports the running kernel's version (the same
// string as `uname -r`); the out-of-tree build reports its own release number.
static int in_tree_transport( const char * nic )
{
#if defined(_WIN32)
    (void) nic;
    return 0;
#else
    char cmd[256];
    snprintf( cmd, sizeof cmd, "ethtool -i %s 2>/dev/null | awk '/^transport:/{d=$2} /^version:/{v=$2} END{print d, v}'", nic );
    FILE * f = popen( cmd, "r" );
    if ( !f )
        return 1;
    char transport[64] = {0}, version[64] = {0};
    if ( fscanf( f, "%63s %63s", transport, version ) != 2 )
    {
        pclose( f );
        return 1;
    }
    pclose( f );
    FILE * u = popen( "uname -r", "r" );
    if ( !u )
        return 1;
    char kernel[64] = {0};
    if ( fgets( kernel, sizeof kernel, u ) )
    {
        size_t len = strlen( kernel );
        while ( len && ( kernel[len-1] == '\n' || kernel[len-1] == '\r' ) )
            kernel[--len] = 0;
    }
    pclose( u );
    return strcmp( version, kernel ) == 0;
#endif
}

// ---- the matrix server (docs/LIVE-XDP.md sections 3 to 7) ----

struct server_stream_t
{
    int active;
    uint32_t total;
    uint32_t next;
    uint32_t sent;
    uint32_t received;
    uint32_t expected;              // ordered modes: the next sequence due
    int64_t last;                   // sequenced modes
    uint64_t seen[ROCKETNET_SERVER_MAX_SEQUENCES / 64];
    uint32_t bucket_sent[ROCKETNET_LIVE_NUM_BUCKETS];
    uint32_t bucket_received[ROCKETNET_LIVE_NUM_BUCKETS];
    uint32_t confirmed;             // contiguous sequences the client reported received (highest + 1)
    uint32_t contiguous;            // the sequence after the highest contiguous one received (section 5)
    uint64_t bytes_received;        // the bytes of the messages received for this stream
};

struct server_client_t
{
    struct server_stream_t streams[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_LIVE_NUM_CHANNELS];
    uint32_t tiny_sent[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_SERVER_TINY];
    uint32_t tiny_received[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_SERVER_TINY];
    uint32_t cycle;
    uint64_t failures;
    uint64_t verified;
    uint64_t refusals;
    int window_done;
    char label[ROCKETNET_SERVER_LABEL_MAX];
    int have_label;
    int hello_seen;
    uint32_t out_bytes;
    uint32_t out_messages;
    uint32_t out_numbers;
    uint64_t paced;
    double last_progress;           // when this slot last sent its 0xC report (section 5)
    uint64_t progress_sent;
};

static int matrix_channels( int mode )
{
    return ( mode == 1 || mode == 3 || mode == 4 || mode == 7 ) ? 31 : 32;
}

static int matrix_ordered( int mode )
{
    return mode == 3 || mode == 7;
}

static int matrix_sequenced( int mode )
{
    return mode == 1 || mode == 4;
}

// section 6: the pacing ledger holds the reliable paths (modes 2, 3, 4, 6,
// 7, and 5 above its single-record edge, where it travels as a reliable block);
// unreliable sends are budget-paced and never counted against the negotiated
static int server_reliable_path( int mode, uint32_t length )
{
    if ( mode == 5 )
        return length > ROCKETNET_LIVE_UNRELIABLE_LIMIT;
    return mode == 2 || mode == 3 || mode == 4 || mode == 6 || mode == 7;
}

static uint64_t server_counter( const struct rocketnet_counters_t * counters, int index )
{
    return counters ? counters->values[index] : 0;
}

static void matrix_fail( struct server_client_t * c, int mode, int channel, int64_t sequence, const char * reason, int bytes )
{
    if ( c->failures < 64 )
        printf( "fail %s %u 1 %d %d %lld %s %d\n", c->label, c->cycle, mode, channel, (long long) sequence, reason, bytes );
    results_line( "fail", "%s\t%u\t1\t%d\t%d\t%lld\t%s\t%d", c->label, c->cycle, mode, channel, (long long) sequence, reason, bytes );
    c->failures++;
}

// the pacing ledger (docs/LIVE-XDP.md section 6): outstanding bytes, messages
// and reliable numbers, admitted only while each stays under its half margin.
// Outstanding is sent minus what the client's progress reports have confirmed
// (section 5: the client reports, per stream, the highest contiguous sequence
// it received); the ledger shrinks as those reports arrive. Tiny messages
// carry no sequence and occupy negligible memory, so they are never admitted
// or counted here.
static void ledger_push( struct server_client_t * c, uint32_t length, uint32_t num )
{
    c->out_bytes += length;
    c->out_messages++;
    c->out_numbers += num;
}

// a progress report confirms every sequence up to hi_seq of one stream, so the
// ledger releases exactly the bytes, messages and numbers those sends added
static void ledger_progress( struct server_client_t * c, int mode, int channel, uint32_t hi_seq )
{
    struct server_stream_t * s = &c->streams[mode][channel];
    uint32_t new_confirm = hi_seq + 1u;
    if ( new_confirm > s->next || new_confirm < s->confirmed )
        new_confirm = s->confirmed;
    if ( new_confirm <= s->confirmed )
        return;
    const struct rocketnet_live_config_t live = g_live;
    uint64_t dbytes = 0, dnums = 0;
    uint32_t dmsg = 0;
    for ( uint32_t q = s->confirmed; q < new_confirm; q++ )
    {
        const uint32_t L = rocketnet_live_length( &live, mode, channel, q );
        if ( L < ROCKETNET_SERVER_TINY || !server_reliable_path( mode, L ) )
            continue;
        const uint32_t F = ROCKETNET_LIVE_BLOCK_STRIDE;
        dbytes += L;
        dnums += ( L + F - 1 ) / F;
        dmsg++;
    }
    if ( c->out_messages >= dmsg ) c->out_messages -= dmsg; else c->out_messages = 0;
    if ( c->out_bytes >= dbytes ) c->out_bytes -= (uint32_t) dbytes; else c->out_bytes = 0;
    if ( c->out_numbers >= dnums ) c->out_numbers -= (uint32_t) dnums; else c->out_numbers = 0;
    s->confirmed = new_confirm;
}

// a progress report (docs/LIVE-XDP.md section 5): tag nibble 0xC on channel
// 31, followed by 10-byte stream records of mode (u8), channel (u8), the
// highest contiguous sequence (u32 little endian) and the bytes received (u32
// little endian). Each record releases that stream's confirmed sends from the
// pacing ledger, which is how reliable modes 2, 3 and 4 unblock.
static void matrix_progress( struct server_client_t * c, const uint8_t * data, int bytes )
{
    int off = 1;
    while ( off + 10 <= bytes )
    {
        const int mode = data[off];
        const int channel = data[off + 1];
        const uint32_t seq = (uint32_t) data[off + 2] | ( (uint32_t) data[off + 3] << 8 ) |
                             ( (uint32_t) data[off + 4] << 16 ) | ( (uint32_t) data[off + 5] << 24 );
        off += 10;
        if ( mode < 0 || mode >= ROCKETNET_LIVE_NUM_MODES )
            continue;
        if ( channel < 0 || channel >= matrix_channels( mode ) )
            continue;
        ledger_progress( c, mode, channel, seq );
    }
}

// section 5: mark one received sequence and advance the stream's highest
// contiguous sequence, which is what the progress report carries.
static void matrix_stream_mark( struct server_stream_t * s, uint32_t q )
{
    if ( q >= ROCKETNET_SERVER_MAX_SEQUENCES )
        return;
    s->seen[q / 64] |= 1ull << ( q % 64 );
    while ( s->contiguous < s->total && s->contiguous < ROCKETNET_SERVER_MAX_SEQUENCES &&
            ( s->seen[s->contiguous / 64] & ( 1ull << ( s->contiguous % 64 ) ) ) )
        s->contiguous++;
}

// section 3: a message under 7 bytes carries no sequence, so the receiver
// matches it to the next tiny slot of the stream that the seed'd schedule
// (section 4) places there and that has not arrived yet.
static void matrix_mark_tiny( struct server_stream_t * s, int mode, int channel )
{
    for ( uint32_t q = 0; q < s->next && q < ROCKETNET_SERVER_MAX_SEQUENCES; q++ )
    {
        if ( s->seen[q / 64] & ( 1ull << ( q % 64 ) ) )
            continue;
        if ( rocketnet_live_length( &g_live, mode, channel, q ) < ROCKETNET_SERVER_TINY )
        {
            matrix_stream_mark( s, q );
            return;
        }
    }
}

// section 5: the report is one tag byte 0xC0 then, per stream with a
// contiguous arrival, a 10-byte record: mode (u8), channel (u8), the highest
// contiguous sequence received (u32 little endian), and the bytes received
// (u32 little endian).
static int matrix_build_progress( const struct server_client_t * c, uint8_t * buffer, int capacity )
{
    int off = 0;
    if ( capacity < 1 )
        return 0;
    buffer[off++] = ROCKETNET_SERVER_PROGRESS_NIBBLE;
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            const struct server_stream_t * s = &c->streams[mode][channel];
            if ( s->contiguous == 0 )
                continue;                       // nothing contiguous received yet
            if ( off + 10 > capacity )
                return off;
            const uint32_t hi = s->contiguous - 1u;
            const uint32_t bytes = s->bytes_received > 0xffffffffu ? 0xffffffffu : (uint32_t) s->bytes_received;
            buffer[off++] = (uint8_t) mode;
            buffer[off++] = (uint8_t) channel;
            buffer[off++] = (uint8_t) ( hi );
            buffer[off++] = (uint8_t) ( hi >> 8 );
            buffer[off++] = (uint8_t) ( hi >> 16 );
            buffer[off++] = (uint8_t) ( hi >> 24 );
            buffer[off++] = (uint8_t) ( bytes );
            buffer[off++] = (uint8_t) ( bytes >> 8 );
            buffer[off++] = (uint8_t) ( bytes >> 16 );
            buffer[off++] = (uint8_t) ( bytes >> 24 );
        }
    }
    return off;
}

// section 5: the report's one sending call, behind a pointer so the self-test
// can observe emission without a socket (the card's test rule). The running
// program keeps the library call.
typedef int (*server_progress_send_fn)( struct rocketnet_message_server_t * server, int client_index, int channel,
                                        int reliability, const uint8_t * data, int bytes );

static int server_progress_send( struct rocketnet_message_server_t * server, int client_index, int channel,
                                 int reliability, const uint8_t * data, int bytes )
{
    return (int) rocketnet_message_server_send( server, client_index, channel, reliability, data, bytes );
}

static server_progress_send_fn g_progress_send = server_progress_send;

// section 5: every 50 ms the server sends its own report, on the same cadence
// as the client, driven by the update time. It is UNRELIABLE_SEQUENCED on
// channel 31, which carries no matrix stream, so it neither supersedes nor is
// superseded by one.
static void matrix_send_progress( struct rocketnet_message_server_t * server, int client_index, struct server_client_t * c, double now )
{
    if ( c->last_progress > 0.0 && now - c->last_progress < ROCKETNET_SERVER_PROGRESS_SECONDS )
        return;
    c->last_progress = now;
    uint8_t report[1 + ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * 10];
    const int n = matrix_build_progress( c, report, (int) sizeof report );
    if ( n <= 1 )
        return;
    if ( g_progress_send( server, client_index, ROCKETNET_SERVER_PROGRESS_CHANNEL,
                          ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, report, n ) )
        c->progress_sent++;
}

static void matrix_write_cells( struct server_client_t * c )
{
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            struct server_stream_t * s = &c->streams[mode][channel];
            for ( int bucket = 0; bucket < ROCKETNET_LIVE_NUM_BUCKETS; bucket++ )
            {
                const uint32_t sent = s->bucket_sent[bucket];
                const uint32_t recv = s->bucket_received[bucket];
                if ( sent == 0 && recv == 0 )
                    continue;
                results_line( "cell", "%s\t%u\t1\t%d\t%d\t%s\t%u\t%u\t0\t0\t%u\t0\t0\t0\t0\t0\t0",
                              c->label, c->cycle, mode, channel, rocketnet_live_bucket_name( bucket ),
                              sent, recv, sent > recv ? sent - recv : 0u );
            }
        }
    }
}

static void client_streams_reset( struct rocketnet_live_config_t * live, struct server_client_t * c )
{
    memset( c, 0, sizeof( *c ) );
    c->cycle = 1;
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        const uint32_t total = rocketnet_live_sequences_per_stream( live, mode );
        for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
        {
            struct server_stream_t * s = &c->streams[mode][channel];
            s->active = 1;
            s->total = total;
            s->last = -1;
        }
    }
}

// add an over-maximum refusal, once per window per mode, on channels 0 and the last
static void matrix_over_maximum( struct rocketnet_message_server_t * server, struct server_client_t * c,
                                 int client_index, const struct rocketnet_live_config_t * live )
{
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        const int channels[2] = { 0, ROCKETNET_LIVE_NUM_CHANNELS - 1 };
        for ( int i = 0; i < 2; i++ )
        {
            const int channel = channels[i];
            const uint32_t over = rocketnet_live_mode_max( live, mode ) + 1;
            const int counter = mode <= 1 ? ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED : ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED;
            const int other = mode <= 1 ? ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED : ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED;
            struct rocketnet_message_server_t * s = server;
            const uint64_t before = server_counter( rocketnet_message_server_counters( s ), counter );
            const uint64_t before_other = server_counter( rocketnet_message_server_counters( s ), other );
            uint8_t * p = rocketnet_message_server_start( s, client_index, channel, mode, (int) over );
            const uint64_t delta = server_counter( rocketnet_message_server_counters( s ), counter ) - before;
            const uint64_t delta_other = server_counter( rocketnet_message_server_counters( s ), other ) - before_other;
            printf( "refused %d %d %u %s %llu\n", mode, channel, over, rocketnet_counter_name( counter ), (unsigned long long) delta );
            if ( p != NULL || delta != ROCKETNET_SERVER_OVER_MAX_DELTA || delta_other != 0 )
            {
                matrix_fail( c, mode, channel, -1, p ? "start-succeeded" : "wrong-counter", (int) over );
                if ( p )
                    rocketnet_message_server_abort( s, client_index );
            }
            c->refusals++;
        }
    }
}

static void matrix_arrival( struct rocketnet_message_server_t * server, struct server_client_t * c,
                            int client_index, int receive_channel, const uint8_t * data, int bytes )
{
    if ( bytes >= 1 )
    {
        const uint8_t nib = data[0] & ROCKETNET_LIVE_TAG_MASK;
        if ( nib == ROCKETNET_SERVER_HELLO_NIBBLE )
        {
            // section 6: the first message of every connection is a HELLO,
            // ribble 0xD, carrying the path's cap in kbps and the bench label.
            uint32_t cap = 0;
            if ( bytes >= 5 )
                cap = (uint32_t) data[1] | ( (uint32_t) data[2] << 8 ) | ( (uint32_t) data[3] << 16 ) | ( (uint32_t) data[4] << 24 );
            if ( !c->hello_seen )
            {
                c->hello_seen = 1;
                if ( cap > 0 )
                    rocketnet_message_server_set_send_kbps( server, client_index, (int) cap );
                if ( bytes > 5 )
                {
                    int l = bytes - 5;
                    if ( l > ROCKETNET_SERVER_LABEL_MAX - 1 )
                        l = ROCKETNET_SERVER_LABEL_MAX - 1;
                    memcpy( c->label, data + 5, (size_t) l );
                    c->label[l] = 0;
                    c->have_label = 1;
                }
                printf( "hello %s %u\n", c->label, cap );
                results_event( "hello %s %u", c->label, cap );
            }
            return;
        }
        if ( nib == ROCKETNET_SERVER_PROGRESS_NIBBLE )
        {
            matrix_progress( c, data, bytes );
            return;     // a progress report on channel 31, not a matrix message
        }
    }
    if ( bytes < 1 || ( data[0] & ROCKETNET_LIVE_TAG_MASK ) != ROCKETNET_LIVE_TAG_BASE || ( ( data[0] >> 3 ) & 1 ) != 0 )
    {
        matrix_fail( c, -1, receive_channel, -1, "bad-header", bytes );
        return;
    }
    const int mode = data[0] & 7;
    const int channel = bytes >= 2 ? data[1] : receive_channel;
    const int want_channel = ( mode == 1 || mode == 3 || mode == 4 || mode == 7 ) ? channel : 0;
    if ( receive_channel != want_channel || channel >= matrix_channels( mode ) )
    {
        matrix_fail( c, mode, channel, -1, "receive-channel", bytes );
        return;
    }
    struct server_stream_t * s = &c->streams[mode][channel];
    const int64_t expected = matrix_ordered( mode ) ? (int64_t) s->expected : -1;
    int offset = -1;
    const int result = rocketnet_live_check( &g_live, data, bytes, 0, mode, channel, c->cycle, expected, &offset );
    if ( result != ROCKETNET_LIVE_OK )
    {
        matrix_fail( c, mode, channel, bytes >= ROCKETNET_SERVER_TINY ? (int64_t) rocketnet_live_sequence( data, bytes ) : expected,
                     rocketnet_live_result_name( result ), bytes );
        return;
    }
    if ( bytes < ROCKETNET_SERVER_TINY && !matrix_ordered( mode ) )
    {
        c->tiny_received[mode][bytes]++;
        if ( c->tiny_received[mode][bytes] > c->tiny_sent[mode][bytes] )
            matrix_fail( c, mode, channel, -1, "duplicate-tiny", bytes );
        s->received++;
        s->bytes_received += (uint32_t) bytes;
        s->bucket_received[rocketnet_live_bucket( &g_live, mode, (uint32_t) bytes )]++;
        matrix_mark_tiny( s, mode, channel );
        c->verified++;
        return;
    }
    const uint32_t q = bytes >= ROCKETNET_SERVER_TINY ? rocketnet_live_sequence( data, bytes ) : s->expected;
    if ( q >= s->next || q >= ROCKETNET_SERVER_MAX_SEQUENCES )
    {
        matrix_fail( c, mode, channel, q, "never-sent", bytes );
        return;
    }
    if ( s->seen[q / 64] & ( 1ull << ( q % 64 ) ) )
    {
        matrix_fail( c, mode, channel, q, "duplicate", bytes );
        return;
    }
    if ( matrix_ordered( mode ) && q != s->expected )
    {
        matrix_fail( c, mode, channel, q, "out-of-order", bytes );
        return;
    }
    if ( matrix_sequenced( mode ) && s->last >= 0 && (int64_t) q <= s->last )
    {
        matrix_fail( c, mode, channel, q, "not-increasing", bytes );
        return;
    }
    matrix_stream_mark( s, q );
    s->last = q;
    s->received++;
    s->bytes_received += (uint32_t) bytes;
    s->bucket_received[rocketnet_live_bucket( &g_live, mode, bytes >= ROCKETNET_SERVER_TINY ? rocketnet_live_length( &g_live, mode, channel, q ) : (uint32_t) bytes )]++;
    if ( matrix_ordered( mode ) )
        s->expected++;
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

static uint32_t matrix_send_one( struct rocketnet_message_server_t * server, int client_index, int mode, int channel,
                                 uint32_t cycle, uint32_t sequence, uint32_t length )
{
    uint8_t * p = rocketnet_message_server_start( server, client_index, channel, mode, (int) length );
    if ( !p )
        return 0;
    const int written = rocketnet_live_generate( &g_live, 1, mode, channel, cycle, sequence, p, (int) length );
    if ( written != (int) length )
    {
        rocketnet_message_server_abort( server, client_index );
        return 0;
    }
    return rocketnet_message_server_finish( server, client_index, written );
}

static void matrix_client_window( struct rocketnet_message_server_t * server, int client_index, struct server_client_t * clients )
{
    struct server_client_t * c = &clients[client_index];
    client_streams_reset( &g_live, c );
    matrix_over_maximum( server, c, client_index, &g_live );

    int all_done = 0;
    while ( !all_done && !quit )
    {
        rocketnet_message_server_update( server, rocketnet_time() );
        matrix_receive( server, clients );
        matrix_send_progress( server, client_index, c, rocketnet_time() );
        all_done = 1;
        for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES && all_done; mode++ )
        {
            for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
            {
                struct server_stream_t * s = &c->streams[mode][channel];
                while ( s->next < s->total && s->next < ROCKETNET_SERVER_MAX_SEQUENCES )
                {
                    const uint32_t length = rocketnet_live_length( &g_live, mode, channel, s->next );
                    const uint32_t F = ROCKETNET_LIVE_BLOCK_STRIDE;
                    const uint32_t rnum = ( mode >= 2 && mode <= 7 ) ? ( length + F - 1 ) / F : 0u;
                    const int tiny = length < ROCKETNET_SERVER_TINY;
                    const int reliable = server_reliable_path( mode, length );
                    if ( !tiny && reliable && ( c->out_bytes + length > ROCKETNET_SERVER_SEND_BUFFER_HALF ||
                                                c->out_messages + 1 > ROCKETNET_SERVER_SEND_QUEUE_HALF ||
                                                c->out_numbers + rnum > ROCKETNET_SERVER_WINDOW_HALF ) )
                    {
                        c->paced++;
                        break;
                    }
                    const uint32_t serial = matrix_send_one( server, client_index, mode, channel, c->cycle, s->next, length );
                    if ( serial == 0 )
                        break;
                    if ( !tiny && reliable )
                        ledger_push( c, length, rnum );
                    s->next++;
                    s->sent++;
                    s->bucket_sent[rocketnet_live_bucket( &g_live, mode, length )]++;
                    if ( tiny )
                        c->tiny_sent[mode][length]++;
                }
                if ( s->next < s->total )
                    all_done = 0;
            }
        }
        rocketnet_message_server_send_packets( server );
        rocketnet_platform_sleep( 0.0005 );
    }
    matrix_write_cells( c );
}

struct progress_record_t
{
    int calls;
    int client_index;
    int channel;
    int reliability;
    int bytes;
    uint8_t data[1 + ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * 10];
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
        memcpy( g_progress_record.data, data, (size_t) bytes );
    return 1;
}

// The server-side checks the card asks for without a socket: a nonzero seed
// moves the length schedule (sections 2 and 4), the 0xC report of section 5
// encodes a stream's highest contiguous sequence and bytes received, and the
// same report is emitted on channel 31 at the 50 ms cadence (section 5).
// Writes a sample result file and returns 0 when all hold.
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
                    changed++;
            }
        }
    }
    printf( "server self-test seed-lengths %d\n", changed );
    if ( changed == 0 )
        failures++;

    struct server_client_t c;
    memset( &c, 0, sizeof c );
    c.streams[0][3].contiguous = 2;
    c.streams[0][3].bytes_received = 40;
    c.streams[2][0].contiguous = 5;
    c.streams[2][0].bytes_received = 100;
    uint8_t report[1 + ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * 10];
    const int n = matrix_build_progress( &c, report, (int) sizeof report );
    const int records = n > 1 ? ( n - 1 ) / 10 : 0;
    printf( "server self-test report tag %u records %d bytes %d\n", (unsigned) report[0], records, n );
    if ( report[0] != ROCKETNET_SERVER_PROGRESS_NIBBLE || records != 2 )
        failures++;
    else
    {
        const uint32_t hi0 = (uint32_t) report[3] | ( (uint32_t) report[4] << 8 ) | ( (uint32_t) report[5] << 16 ) | ( (uint32_t) report[6] << 24 );
        const uint32_t bytes0 = (uint32_t) report[7] | ( (uint32_t) report[8] << 8 ) | ( (uint32_t) report[9] << 16 ) | ( (uint32_t) report[10] << 24 );
        if ( report[1] != 0 || report[2] != 3 || hi0 != 1u || bytes0 != 40u )
            failures++;
    }

    // section 5: the server emits the report itself, on channel 31, as
    // UNRELIABLE_SEQUENCED, and no more often than every 50 ms. A clock that
    // starts at 1000.0 keeps the cadence an argument, not the wall clock.
    struct server_client_t silent;
    memset( &silent, 0, sizeof silent );
    struct server_client_t emitted;
    memset( &emitted, 0, sizeof emitted );
    emitted.streams[0][3].contiguous = 2;
    emitted.streams[0][3].bytes_received = 40;
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
         g_progress_record.channel != ROCKETNET_SERVER_PROGRESS_CHANNEL ||
         g_progress_record.reliability != ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED ||
         g_progress_record.bytes != 11 ||
         g_progress_record.data[0] != ROCKETNET_SERVER_PROGRESS_NIBBLE ||
         emitted.progress_sent != 2 )
        failures++;

    if ( path )
    {
        g_results = rocketnet_test_fopen( path, "w" );
        if ( g_results )
        {
            results_line( "bench", "%s\t%u\t500\t1000\tself-test", label, seed );
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
        else if ( strcmp( a, "--matrix" ) == 0 ) matrix = 1;
        else if ( strcmp( a, "--address" ) == 0 && i + 1 < argc ) address = argv[++i];
        else if ( strcmp( a, "--label" ) == 0 && i + 1 < argc ) label = argv[++i];
        else if ( strcmp( a, "--seed" ) == 0 && i + 1 < argc ) seed = (uint32_t) strtoul( argv[++i], NULL, 10 );
        else if ( strcmp( a, "--send-kbps" ) == 0 && i + 1 < argc ) send_kbps = (uint32_t) strtoul( argv[++i], NULL, 10 );
        else if ( strcmp( a, "--max-clients" ) == 0 && i + 1 < argc ) max_clients = (int) strtol( argv[++i], NULL, 10 );
        else if ( strcmp( a, "--max-packets-per-update" ) == 0 && i + 1 < argc ) packets_per_update = (int) strtol( argv[++i], NULL, 10 );
        else if ( strcmp( a, "--update-hz" ) == 0 && i + 1 < argc ) update_hz = strtod( argv[++i], NULL );
        else if ( strcmp( a, "--counters-every" ) == 0 && i + 1 < argc ) counters_every = strtod( argv[++i], NULL );
        else if ( strcmp( a, "--results" ) == 0 && i + 1 < argc ) results_path = argv[++i];
        else if ( strcmp( a, "--status" ) == 0 && i + 1 < argc ) status_path = argv[++i];
        else if ( strcmp( a, "--transport" ) == 0 && i + 1 < argc ) transport = argv[++i];
        else if ( strcmp( a, "--self-test-result" ) == 0 && i + 1 < argc ) { self_test = 1; self_test_path = argv[++i]; }
        else if ( a[0] != '-' && !address ) address = a;
        else
        {
            fprintf( stderr, "server: unknown argument: %s\n", a );
            usage();
            return 1;
        }
    }
    (void) update_hz;

    if ( !address )
        address = "127.0.0.1:40000";

    g_live.seed = seed;

    if ( self_test )
        return run_self_test( self_test_path, label, seed );

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;

    char env_transport[sizeof config.transport];
    if ( transport )
        snprintf( config.transport, sizeof config.transport, "%s", transport );
    else if ( rocketnet_test_getenv( "ROCKETNET_SERVER_TRANSPORT", env_transport, sizeof env_transport ) )
        snprintf( config.transport, sizeof config.transport, "%s", env_transport );

    // section 9: when xdp is pinned, refuse the in-tree transport
    if ( strcmp( config.transport, "xdp" ) == 0 )
    {
        char nic[64] = {0};
        if ( interface_for_address( address, nic, sizeof nic ) && in_tree_transport( nic ) )
        {
            fprintf( stderr, "server: refusing: interface %s reports the in-tree transport\n", nic );
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
            results_line( "bench", "%s\t%u\t%u\t%d\t%s", label, seed, send_kbps, max_clients, rocketnet_server_transport_io_name( transport_obj ) );
            results_event( "listening" );
        }
    }
    set_status( status_path, "listening" );

    if ( !matrix )
    {
        // the payload echo, exactly as before
        struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, 60.0 );
        if ( !frame_sync )
        {
            rn_native_message_server_stop_and_destroy( server );
            rocketnet_term();
            return 1;
        }
        int last_num_connected = 0;
        double last_counters = 0.0;
        while ( !quit )
        {
            rocketnet_server_transport_update( transport_obj, rocketnet_time() );
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
            }
            rocketnet_server_transport_send_packets( transport_obj );
            const double now = rocketnet_time();
            if ( counters_every > 0.0 && now - last_counters >= counters_every )
            {
                dump_counters( rocketnet_server_transport_counters( transport_obj ) );
                last_counters = now;
            }
            check_stdin_closed();
            rocketnet_frame_sync_wait( frame_sync );
        }
        rocketnet_frame_sync_destroy( frame_sync );
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
            snprintf( clients[i].label, sizeof clients[i].label, "%s", label );
        double last_counters = 0.0;
        while ( !quit )
        {
            rocketnet_message_server_update( server, rocketnet_time() );
            rocketnet_message_server_send_packets( server );
            matrix_receive( server, clients );
            for ( int i = 0; i < max_clients; i++ )
            {
                if ( !rocketnet_message_server_client_connected( server, i ) )
                    continue;
                if ( !clients[i].window_done )
                {
                    matrix_client_window( server, i, clients );
                    clients[i].window_done = 1;
                }
                matrix_send_progress( server, i, &clients[i], rocketnet_time() );
            }
            rocketnet_message_server_send_packets( server );
            const double now = rocketnet_time();
            if ( counters_every > 0.0 && now - last_counters >= counters_every )
            {
                dump_counters( rocketnet_message_server_counters( server ) );
                last_counters = now;
            }
            check_stdin_closed();
            rocketnet_platform_sleep( 0.0005 );
        }
        for ( int i = 0; i < max_clients; i++ )
        {
            if ( clients[i].progress_sent )
                printf( "server progress %s %llu\n", clients[i].label, (unsigned long long) clients[i].progress_sent );
        }
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
    return 0;
}
