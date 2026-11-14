/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The out-of-the-box client: connects to a server with a development
    token minted locally from the published development keys, then
    exchanges payloads. Run ./bin/server, then ./bin/client. Pass an
    address to connect elsewhere: ./bin/client 192.0.2.10:40000

    --matrix runs the live-stress message matrix (docs/LIVE-XDP.md):
    every mode, every channel at once, the size schedule, both directions,
    a cycle of reconnects with completed clean disconnects, and
    result lines of section 7 written to --results. Each end judges what the
    other actually sent (tests/matrix_receive.h); the client
    paces its sends by rocketnet_client_can_send_message and exits 1 when
    any check failed. --self-test-result writes a sample result file without
    any socket, for the merge to read.
*/

#include "tests/native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_internal.h"
#include "sdk/rocketnet_connect_token.h"
#include "sdk/rocketnet_counters.h"
#include "tests/live_verify.h"
#include "tests/matrix_receive.h"
#include "tests/portable_crt.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#if defined( _WIN32 )
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif // #if defined( _WIN32 )

// Internal transport tools mint the binary development token directly.
static int dev_token_binary( const char * address, uint64_t client_id, uint64_t timestamp, uint8_t * token, size_t token_bytes )
{
    return rocketnet_write_test_connect_token( address, client_id, timestamp, token, token_bytes ) == ROCKETNET_STATUS_OK;
}

static volatile int quit;

void interrupt_handler( int signal )
{
    (void) signal;
    quit = 1;
}

typedef struct opts_t
{
    const char * address;
    const char * label;
    const char * results_path;
    const char * status_path;
    double duration;
    int cycles;
    double stagger;
    uint64_t seed;
    int send_kbps;
    int update_hz;
    int packets_per_update;
    double counters_every;
    int matrix;
    unsigned mode_mask;
    int channels;
} opts_t;

static void usage( const char * prog )
{
    printf( "usage: %s [address] [flags]\n", prog );
    printf( "flags:\n" );
    printf( "  --matrix                 run the message matrix stress mode (docs/LIVE-XDP.md)\n" );
    printf( "  --address A:P            the server address to mint tokens for\n" );
    printf( "  --label L                bench label written on every result line\n" );
    printf( "  --duration S             stress window length in seconds (default 35)\n" );
    printf( "  --cycles N               reconnects before the final window (default 8)\n" );
    printf( "  --stagger S              seconds to wait before the first connect (default 0)\n" );
    printf( "  --seed X                 the run seed (default random)\n" );
    printf( "  --modes M                comma-separated modes to drive (default 0,1,2,3,4,5,6,7)\n" );
    printf( "  --channels C             channel count to drive, 1 to 32 (default 32)\n" );
    printf( "  --send-kbps K            client send budget in kbps\n" );
    printf( "  --max-packets-per-update N  the client config's packets_per_update (default 64)\n" );
    printf( "  --update-hz H            main loop update rate in Hz\n" );
    printf( "  --counters-every S       print counters every S seconds (default 60)\n" );
    printf( "  --results PATH           where result lines go\n" );
    printf( "  --status PATH            one line per state change\n" );
    printf( "  --self-test-result FILE  write a sample result file without any socket and exit\n" );
    printf( "  --print-dev-tokens N A:P [EXPIRE_S]  print N development tokens for A:P, base64, and exit\n" );
    printf( "environment: ROCKETNET_DEV_CLIENT_LATENCY_MS, _JITTER_MS, _PACKET_LOSS_PERCENT and _DUPLICATE_PERCENT\n" );
    printf( "  set the development link simulator on the client's sends\n" );
    printf( "  --help                   this message\n" );
}

static uint64_t parse_u64( const char * s, const char * flag )
{
    char * end = NULL;

    errno = 0;

    uint64_t v = strtoull( s, &end, 10 );

    // digits only: strtoull takes a leading space, sign or minus (and wraps
    // "-1" to the largest value), which a count never is
    if ( s[0] < '0' || s[0] > '9' || !end || *end || errno )
    {
        fprintf( stderr, "client: %s wants a nonnegative integer\n", flag );
        exit( 2 );
    }

    return v;
}

static double parse_f64( const char * s, const char * flag )
{
    char * end = NULL;
    double v = strtod( s, &end );
    if ( !s[0] || !end || *end )
    {
        fprintf( stderr, "client: %s wants a number\n", flag );
        exit( 2 );
    }

    return v;
}

static void default_opts( opts_t * o, int argc, char ** argv )
{
    memset( o, 0, sizeof( *o ) );
    o->address = argc > 1 ? argv[1] : "127.0.0.1:40000";
    o->label = "client";
    o->duration = 35.0;
    o->cycles = 8;
    o->stagger = 0.0;
    o->seed = (uint64_t) time( NULL ) ^ ( (uint64_t) getpid() << 32 );
    o->send_kbps = 1000;
    o->update_hz = 1000;
    o->packets_per_update = 64;
    o->counters_every = 60.0;
    o->mode_mask = 0xFFu;
    o->channels = ROCKETNET_LIVE_NUM_CHANNELS;
}

// section 2: --modes takes the modes to drive, default all eight
static unsigned parse_modes( const char * s )
{
    unsigned mask = 0;
    const char * p = s;
    while ( *p )
    {
        char * end = NULL;

        errno = 0;

        unsigned long v = strtoul( p, &end, 10 );
        if ( end == p || errno || v > 7 )
        {
            fprintf( stderr, "client: --modes wants a comma-separated list of modes 0 to 7\n" );
            exit( 2 );
        }

        mask |= 1u << v;
        p = end;
        if ( *p == ',' )
        {
            p++;
        }
        else if ( *p )
        {
            fprintf( stderr, "client: --modes wants a comma-separated list of modes 0 to 7\n" );
            exit( 2 );
        }
    }

    if ( !mask )
    {
        fprintf( stderr, "client: --modes names no mode\n" );
        exit( 2 );
    }

    return mask;
}

static void parse_args( opts_t * o, int argc, char ** argv, int * self_test, const char ** self_test_file )
{
    *self_test = 0;
    *self_test_file = NULL;

    int i = 1;

    if ( argc > 1 && argv[1][0] != '-' )
    {
        i = 2;
    }

    for ( ; i < argc; i++ )
    {
        const char * a = argv[i];
        if ( strcmp( a, "--help" ) == 0 )
        {
            usage( argv[0] );
            exit( 0 );
        }
        else if ( strcmp( a, "--matrix" ) == 0 )
        {
            o->matrix = 1;
        }
        else if ( strcmp( a, "--address" ) == 0 && i + 1 < argc )
        {
            o->address = argv[++i];
        }
        else if ( strcmp( a, "--label" ) == 0 && i + 1 < argc )
        {
            o->label = argv[++i];
        }
        else if ( strcmp( a, "--duration" ) == 0 && i + 1 < argc )
        {
            o->duration = parse_f64( argv[++i], a );
        }
        else if ( strcmp( a, "--cycles" ) == 0 && i + 1 < argc )
        {
            // the windows are cycles + 1, counted in an int
            const uint64_t cycles = parse_u64( argv[++i], a );
            if ( cycles >= (uint64_t) INT_MAX )
            {
                fprintf( stderr, "client: --cycles wants less than %d\n", INT_MAX );
                exit( 2 );
            }
            o->cycles = (int) cycles;
        }
        else if ( strcmp( a, "--stagger" ) == 0 && i + 1 < argc )
        {
            o->stagger = parse_f64( argv[++i], a );
        }
        else if ( strcmp( a, "--seed" ) == 0 && i + 1 < argc )
        {
            o->seed = parse_u64( argv[++i], a );
        }
        else if ( strcmp( a, "--modes" ) == 0 && i + 1 < argc )
        {
            o->mode_mask = parse_modes( argv[++i] );
        }
        else if ( strcmp( a, "--channels" ) == 0 && i + 1 < argc )
        {
            o->channels = (int) parse_u64( argv[++i], a );
            if ( o->channels < 1 || o->channels > ROCKETNET_LIVE_NUM_CHANNELS )
            {
                fprintf( stderr, "client: --channels wants 1 to %d\n", (int) ROCKETNET_LIVE_NUM_CHANNELS );
                exit( 2 );
            }
        }
        else if ( strcmp( a, "--send-kbps" ) == 0 && i + 1 < argc )
        {
            o->send_kbps = (int) parse_u64( argv[++i], a );
        }
        else if ( strcmp( a, "--max-packets-per-update" ) == 0 && i + 1 < argc )
        {
            o->packets_per_update = (int) parse_u64( argv[++i], a );
        }
        else if ( strcmp( a, "--update-hz" ) == 0 && i + 1 < argc )
        {
            o->update_hz = (int) parse_u64( argv[++i], a );
        }
        else if ( strcmp( a, "--counters-every" ) == 0 && i + 1 < argc )
        {
            o->counters_every = parse_f64( argv[++i], a );
        }
        else if ( strcmp( a, "--results" ) == 0 && i + 1 < argc )
        {
            o->results_path = argv[++i];
        }
        else if ( strcmp( a, "--status" ) == 0 && i + 1 < argc )
        {
            o->status_path = argv[++i];
        }
        else if ( strcmp( a, "--self-test-result" ) == 0 && i + 1 < argc )
        {
            *self_test = 1;
            *self_test_file = argv[++i];
        }
        else
        {
            fprintf( stderr, "client: unknown flag %s\n", a );
            usage( argv[0] );
            exit( 2 );
        }
    }
}

// ---- results ----

static void r_cell( FILE * f, const char * label, int window, int dir, int mode, int channel,
                    int bucket, uint64_t sent, uint64_t received, uint64_t verified, uint64_t failed,
                    uint64_t lost, uint64_t superseded, uint64_t missing, uint64_t ooo,
                    uint64_t dup, uint64_t over, uint64_t paced )
{
    fprintf( f, "cell\t%s\t%d\t%d\t%d\t%d\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
             label, window, dir, mode, channel, rocketnet_live_bucket_name( bucket ),
             (unsigned long long) sent, (unsigned long long) received, (unsigned long long) verified,
             (unsigned long long) failed, (unsigned long long) lost, (unsigned long long) superseded,
             (unsigned long long) missing, (unsigned long long) ooo, (unsigned long long) dup,
             (unsigned long long) over, (unsigned long long) paced );
}

static void r_connect( FILE * f, const char * label, int cycle, int ok, double ms )
{
    fprintf( f, "connect\t%s\t%d\t%d\t%.1f\n", label, cycle, ok, ms );
}

static void r_cycle( FILE * f, const char * label, int cycle, const char * kind )
{
    fprintf( f, "cycle\t%s\t%d\t%s\n", label, cycle, kind );
}

// section 7, "Failures are recorded with their reason": the denial code or
// client state, the side, the counters at that moment and a log excerpt.
static void r_fail( FILE * f, const char * label, int cycle, int dir, int mode, int channel,
                    int64_t sequence, const char * reason, int bytes,
                    int denial, const char * side, const char * counters, const char * excerpt )
{
    fprintf( f, "fail\t%s\t%d\t%d\t%d\t%d\t%lld\t%s\t%d\t%d\t%s\t%s\t%s\n", label, cycle, dir, mode, channel,
             (long long) sequence, reason, bytes, denial, side, counters ? counters : "-", excerpt ? excerpt : "-" );
}

static void r_counters( FILE * f, const char * label, const char * name, uint64_t value, double seconds )
{
    fprintf( f, "counters\t%s\t%s\t%llu\t%.1f\n", label, name, (unsigned long long) value, seconds );
}

static void r_stat( FILE * f, const char * label, int cycle, double seconds, double rtt, double jitter, double loss )
{
    fprintf( f, "stat\t%s\t%d\t%.1f\t%.3f\t%.3f\t%.3f\n", label, cycle, seconds, rtt, jitter, loss );
}

static void r_event( FILE * f, const char * label, int cycle, const char * kind, const char * detail )
{
    fprintf( f, "event\t%s\t%d\t%s\t%s\n", label, cycle, kind, detail );
}

static void r_bench( FILE * f, const char * label, int connects, int ok, double avg_ms,
                     double rp50, double rp99, double rmax, double rtt50, double rtt99, double loss,
                     int cycles, int clean, int silent, int denials, int fulls )
{
    fprintf( f, "bench\t%s\tconnects\t%d\tok\t%d\tavg-ms\t%.1f\treconnect-p50\t%.1f\treconnect-p99\t%.1f\treconnect-max\t%.1f\t"
                "rtt-p50\t%.3f\trtt-p99\t%.3f\tloss\t%.3f\tcycles\t%d\tclean\t%d\tsilent\t%d\tdenials\t%d\tfulls\t%d\n",
             label, connects, ok, avg_ms, rp50, rp99, rmax, rtt50, rtt99, loss, cycles, clean, silent, denials, fulls );
}

static void r_rss( FILE * f, const char * label, double seconds, long kb )
{
    fprintf( f, "rss\t%s\t%.1f\t%ld\n", label, seconds, kb );
}

static long resident_kb( void )
{
    FILE * f = rocketnet_test_fopen( "/proc/self/statm", "r" );
    if ( !f )
    {
        return 0;
    }

    char line[256];
    const int have_line = fgets( line, sizeof( line ), f ) != NULL;

    fclose( f );
    if ( !have_line )
    {
        return 0;
    }
    // statm is "total resident ...": skip the total, read the resident pages
    char * after_total = NULL;
    char * after_resident = NULL;
    strtol( line, &after_total, 10 );

    const long resident = strtol( after_total, &after_resident, 10 );

    if ( after_total == line || after_resident == after_total )
    {
        return 0;
    }

    return resident; /* pages; the page size is 4 KiB on Linux, kb = pages * 4 */
}

// ---- self test ----

static void write_self_test( const char * path, const char * label )
{
    FILE * f = rocketnet_test_fopen( path, "w" );
    if ( !f )
    {
        fprintf( stderr, "client: cannot write %s\n", path );
        exit( 1 );
    }

    // section 7: one row per bench x direction x mode x channel x size bucket

    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int dir = 0; dir < 2; dir++ )
        {
            for ( int channel = 0; channel < ( mode == 1 || mode == 3 || mode == 4 || mode == 7 ? 31 : 32 ); channel++ )
            {
                for ( int bucket = 0; bucket < ROCKETNET_LIVE_NUM_BUCKETS; bucket++ )
                {
                    const int over = ( bucket == ROCKETNET_LIVE_BUCKET_OVER );

                    r_cell( f, label, 0, dir, mode, channel, bucket,
                            1, over ? 0 : 1, over ? 0 : 1, 0, 0, 0, 0, 0, 0, (uint64_t) over, 0 );
                }
            }
        }
    }

    for ( int c = 0; c <= 8; c++ )
    {
        r_connect( f, label, c, 1, 1.5 + c * 0.1 );
        r_cycle( f, label, c, ( c % 2 ) ? "silent" : "clean" );
        r_stat( f, label, c, (double) c, 20.0 + c, 1.0, 0.0 );
        r_rss( f, label, (double) c, 40000 );
        r_counters( f, label, "MESSAGE_RESENT", 0, (double) c );
        r_counters( f, label, "MESSAGE_UNRELIABLE_OVER_BUDGET", 10, (double) c );
        r_counters( f, label, "XDP_KERNEL_REDIRECTED", 1000, (double) c );
    }

    r_bench( f, label, 9, 9, 1.8, 1.6, 2.3, 2.3, 24.0, 28.0, 0.0, 9, 5, 4, 0, 0 );
    fprintf( f, "verdict\tPASS\n" );
    fclose( f );
}

// ---- matrix mode ----
//
// One window per connection. The client sends the HELLO (its cap, the cycle,
// the seed and max_message_bytes, and its label) and sends its matrix only
// after the server's answer, so every message either side judges carries a
// cycle both agree on. Each pass sends one message per stream, paced by
// rocketnet_client_can_send_message: a stream the library has no room for
// waits for the next pass, counted as paced, never an error. At the end of
// the window the client sends its sent report and drains: it waits until the
// server's progress reports confirm every owed sequence the client sent, and
// until every owed sequence of the server's own sent report has arrived. What
// is still missing then is lost. tests/matrix_receive.h holds
// the receive rule both ends share.

typedef struct cell_counts_t
{
    uint64_t sent, received, verified, failed, lost, superseded, missing, ooo, dup, over, paced;
} cell_counts_t;

typedef struct stream_t
{
    uint32_t sequence;          // the next sequence to send, and the count sent
    uint64_t sent_bytes;
    uint32_t confirmed;         // the server's progress: every owed sequence below it arrived
    uint64_t confirmed_bytes;   // the bytes the server reports received
} stream_t;

typedef struct matrix_state_t
{
    opts_t * o;
    FILE * res;
    FILE * status;
    rocketnet_live_config_t cfg;
    uint32_t cycle;
    uint64_t fails;
    stream_t * streams;         // [mode][channel], the client's sends
    struct matrix_rx_t * rx;    // the server's sends, as received
    cell_counts_t * cells;      // [dir][mode][channel][bucket]
    int hello_answered;
    uint64_t outstanding;       // the pacing ledger: reliable bytes sent and not yet confirmed
    double last_progress;       // when the client last sent its progress report
    double * rtt;
    double * loss;
    int rtt_count;
    int loss_count;
    int connect_count;
    int connect_ok;
    double connect_ms_sum;
    double * reconnect_ms;
    int reconnect_count;
    int denials;
    int fulls;
    int clean_count;
    int silent_count;
    const char * last_excerpt;
    double run_start;
} matrix_state_t;

#define RN_STREAM( st, mode, channel ) ( &(st)->streams[(mode) * ROCKETNET_LIVE_NUM_CHANNELS + (channel)] )
#define RN_CELL( st, dir, mode, channel, bucket ) ( &(st)->cells[(((dir) * ROCKETNET_LIVE_NUM_MODES + (mode)) * ROCKETNET_LIVE_NUM_CHANNELS + (channel)) * ROCKETNET_LIVE_NUM_BUCKETS + (bucket)] )

static int stream_channels( int mode, int channel )
{
    /* section 5: channel-less modes 0, 2, 5, 6 name all 32 channels; the
       modes 1, 3, 4, 7 run on channels 0 to 30 (31 carries the progress
       reports and the HELLO). */
    return channel < matrix_channels( mode );
}

// a start for a control message on channel 31 (HELLO, sent report): the
// library's refusal returns 0 and the caller tries again on the next pass
static int send_control( struct rocketnet_client_t * client, const uint8_t * data, int bytes )
{
    if ( !rocketnet_client_can_send_message( client, MATRIX_CONTROL_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes ) )
    {
        return 0;
    }

    uint8_t * out = rocketnet_client_start_message( client, MATRIX_CONTROL_CHANNEL, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, bytes );
    if ( !out )
    {
        return 0;
    }
    memcpy( out, data, (size_t) bytes );
    rocketnet_client_finish_message( client, bytes );
    return 1;
}

// one matrix message: its length, 0 when it was not sent because the library
// has no room for it now (a refusal: the stream waits), or -1 when the start
// failed although rocketnet_client_can_send_message said it would be given room
static int send_message( struct rocketnet_client_t * client, int mode, int channel, uint32_t cycle,
                         uint32_t sequence, uint8_t * buf, int cap, matrix_state_t * st )
{
    const uint32_t length = rocketnet_live_length( &st->cfg, mode, channel, sequence );
    if ( length > (uint32_t) cap )
    {
        return -1;
    }

    if ( !rocketnet_client_can_send_message( client, channel, (enum rocketnet_reliability_t) mode, (int) length ) )
    {
        return 0;
    }

    const int len = rocketnet_live_generate( &st->cfg, 0, mode, channel, cycle, sequence, buf, cap );
    uint8_t * out = rocketnet_client_start_message( client, channel, (enum rocketnet_reliability_t) mode, len );
    if ( !out )
    {
        return -1;
    }
    memcpy( out, buf, (size_t) len );
    rocketnet_client_finish_message( client, len );
    return len;
}

static void receive_fail( matrix_state_t * st, int mode, int channel, int64_t sequence, const char * reason, int bytes, const char * excerpt )
{
    r_fail( st->res, st->o->label, (int) st->cycle, 1, mode, channel, sequence, reason, bytes, 0, "receiver", "-", excerpt );
    st->fails++;
}

// a receive-side failure reported by tests/matrix_receive.h
static void receive_failed( void * context, int mode, int channel, int64_t sequence, const char * reason, int bytes )
{
    receive_fail( (matrix_state_t *) context, mode, channel, sequence, reason, bytes, reason );
}

// the server's progress report: per stream, the highest sequence below
// which every owed one arrived, and the bytes received
static void receive_progress( matrix_state_t * st, const uint8_t * data, int bytes )
{
    if ( ( bytes - 1 ) % MATRIX_PROGRESS_RECORD != 0 )
    {
        receive_fail( st, -1, MATRIX_CONTROL_CHANNEL, -1, "bad-progress", bytes, "progress length" );
        return;
    }

    for ( int off = 1; off + MATRIX_PROGRESS_RECORD <= bytes; off += MATRIX_PROGRESS_RECORD )
    {
        const int mode = data[off];
        const int channel = data[off + 1];
        if ( mode >= ROCKETNET_LIVE_NUM_MODES || channel >= matrix_channels( mode ) )
        {
            receive_fail( st, mode, channel, -1, "bad-progress", bytes, "progress record" );
            continue;
        }

        stream_t * s = RN_STREAM( st, mode, channel );
        const uint32_t confirmed = matrix_get_u32( data + off + 2 ) + 1u;
        if ( confirmed > s->sequence )
        {
            receive_fail( st, mode, channel, (int64_t) confirmed - 1, "progress-past-sent", bytes, "the server confirms a sequence never sent" );
            continue;
        }
        if ( confirmed > s->confirmed )
        {
            const uint64_t released = matrix_ledger_bytes( &st->cfg, mode, channel, s->confirmed, confirmed );
            st->outstanding = st->outstanding > released ? st->outstanding - released : 0;
            s->confirmed = confirmed;
        }
        s->confirmed_bytes = matrix_get_u32( data + off + 6 );
    }
}

static void receive_check( matrix_state_t * st, struct rocketnet_client_t * client )
{
    /* the one receive queue, in batches: each entry names the channel it was
       delivered on; modes 0, 2, 5, 6 arrive on channel 0. The next batch call
       or update releases a batch */
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
    {
        for ( int m = 0; m < batch->num_messages; m++ )
        {
            const uint8_t * data = batch->data[m];
            const int bytes = batch->bytes[m];
            const int receive_channel = batch->channel[m];
            if ( bytes < 1 )
            {
                receive_fail( st, -1, receive_channel, -1, "short", bytes, "empty message" );
                continue;
            }

            // the control messages on channel 31

            const uint32_t nibble = data[0] & MATRIX_NIBBLE_MASK;
            if ( nibble == MATRIX_HELLO_NIBBLE || nibble == MATRIX_PROGRESS_NIBBLE || nibble == MATRIX_SENT_NIBBLE )
            {
                if ( receive_channel != MATRIX_CONTROL_CHANNEL )
                {
                    receive_fail( st, -1, receive_channel, -1, "receive-channel", bytes, "control message off channel 31" );
                }
                else if ( nibble == MATRIX_HELLO_NIBBLE )
                {
                    if ( bytes != MATRIX_HELLO_ANSWER_BYTES || matrix_get_u32( data + 1 ) != st->cycle || st->hello_answered )
                    {
                        receive_fail( st, -1, receive_channel, -1, "bad-hello-answer", bytes, "hello answer" );
                    }
                    else
                    {
                        st->hello_answered = 1;
                    }
                }
                else if ( nibble == MATRIX_PROGRESS_NIBBLE )
                {
                    receive_progress( st, data, bytes );
                }
                else
                {
                    matrix_rx_sent_report( &st->cfg, st->rx, data, bytes, receive_failed, st );
                }
                continue;
            }

            if ( nibble != ROCKETNET_LIVE_TAG_BASE || ( ( data[0] >> 3 ) & 1 ) != 1 )
            {
                receive_fail( st, -1, receive_channel, -1, "bad-header", bytes, "tag" );
                continue;
            }

            // section 5: codes 1, 3, 4 and 7 arrive on the channel sent, the
            // others on channel 0; the result row names the sender's channel

            const int mode = data[0] & 7;
            const int channel = bytes >= 2 ? data[1] : receive_channel;
            const int want_channel = matrix_carries_channel( mode ) ? channel : 0;
            if ( receive_channel != want_channel || channel >= matrix_channels( mode ) )
            {
                receive_fail( st, mode, channel, -1, "receive-channel", bytes, "receive channel" );
                continue;
            }

            const int bucket = rocketnet_live_bucket( &st->cfg, mode, (uint32_t) bytes );
            cell_counts_t * cell = RN_CELL( st, 1, mode, channel, bucket );
            cell->received++;

            struct matrix_rx_stream_t * s = &st->rx->streams[mode][channel];
            const int64_t expected = matrix_ordered( mode ) ? (int64_t) s->expected : -1;
            int bad = -1;
            const int r = rocketnet_live_check( &st->cfg, data, bytes, 1, mode, channel, st->cycle, expected, &bad );
            if ( r != ROCKETNET_LIVE_OK )
            {
                char excerpt[128];
                snprintf( excerpt, sizeof( excerpt ), "check=%s offset=%d", rocketnet_live_result_name( r ), bad );
                cell->failed++;
                receive_fail( st, mode, channel, bytes >= ROCKETNET_LIVE_HEADER_BYTES ? (int64_t) rocketnet_live_sequence( data, bytes ) : expected,
                              rocketnet_live_result_name( r ), bytes, excerpt );
                continue;
            }

            const int64_t last = s->last;
            const char * reason = NULL;
            int64_t sequence = -1;
            if ( matrix_counted_tiny( mode, (uint32_t) bytes ) )
            {
                reason = matrix_rx_tiny( st->rx, mode, bytes );
            }
            else
            {
                reason = matrix_rx_accept( &st->cfg, s, mode, channel, data, bytes, &sequence );
                if ( !reason )
                {
                    reason = matrix_rx_interleave( st->rx, mode, channel, (uint32_t) sequence );
                }
            }

            if ( reason )
            {
                char excerpt[96];
                snprintf( excerpt, sizeof( excerpt ), "%s sequence=%lld", reason, (long long) sequence );
                cell->failed++;
                if ( strcmp( reason, "out-of-order" ) == 0 )
                {
                    cell->ooo++;
                }
                else if ( strncmp( reason, "duplicate", 9 ) == 0 )
                {
                    cell->dup++;
                }
                receive_fail( st, mode, channel, sequence, reason, bytes, excerpt );
                continue;
            }

            cell->verified++;

            // the sequenced modes skip by design: code 1 loses, code 4 supersedes
            if ( matrix_sequenced( mode ) && last >= 0 && sequence > last + 1 )
            {
                if ( mode == 1 )
                {
                    cell->lost += (uint64_t) ( sequence - last - 1 );
                }
                else
                {
                    cell->superseded += (uint64_t) ( sequence - last - 1 );
                }
            }
        }
    }
}

static double quantile( const double * values, int count, double q )
{
    if ( count <= 0 )
    {
        return 0.0;
    }

    double * copy = (double *) malloc( (size_t) count * sizeof( double ) );
    if ( !copy )
    {
        return 0.0;
    }
    memcpy( copy, values, (size_t) count * sizeof( double ) );
    for ( int i = 1; i < count; i++ )
    {
        double v = copy[i];
        int j = i - 1;
        while ( j >= 0 && copy[j] > v )
        {
            copy[j + 1] = copy[j];
            j--;
        }

        copy[j + 1] = v;
    }

    int index = (int) ( q * ( count - 1 ) + 0.5 );
    if ( index < 0 )
    {
        index = 0;
    }

    if ( index >= count )
    {
        index = count - 1;
    }

    double result = copy[index];

    free( copy );

    return result;
}

static long resident_kb( void );

// section 7: every counter by name, message and transport counters summed.
// The public client is not a message client: its connection is resolved
// through rocketnet_client_message_client, NULL while disconnected or after a
// refused connect, when there are no connection counters to read and only the
// resident size is written.
static void sample_counters( matrix_state_t * st, struct rocketnet_client_t * client, double seconds )
{
    if ( !client )
    {
        return;
    }

    struct rocketnet_message_client_t * message_client = rocketnet_client_message_client( client );
    if ( !message_client )
    {
        r_rss( st->res, st->o->label, seconds, resident_kb() * 4 );
        return;
    }

    const struct rocketnet_counters_t * message = rocketnet_message_client_counters( message_client );
    struct rocketnet_client_transport_t * transport = rocketnet_message_client_transport( message_client );
    const struct rocketnet_counters_t * transport_counters = rocketnet_client_transport_counters( transport );
    const int n = rocketnet_num_counters();
    for ( int i = 0; i < n; i++ )
    {
        const uint64_t value = rocketnet_counter_value( message, i ) + rocketnet_counter_value( transport_counters, i );
        r_counters( st->res, st->o->label, rocketnet_counter_name( i ), value, seconds );
    }

    char report[256];
    const int mc = rocketnet_counters_conserved( message, report, (int) sizeof( report ) );
    const int tc = rocketnet_counters_conserved( transport_counters, report, (int) sizeof( report ) );
    fprintf( st->res, "conserved\t%s\t%.1f\t%d\t%d\n", st->o->label, seconds, mc, tc );
    r_rss( st->res, st->o->label, seconds, resident_kb() * 4 );

    if ( !mc || !tc )
    {
        char excerpt[160];
        snprintf( excerpt, sizeof( excerpt ), "conservation message=%d transport=%d", mc, tc );
        r_fail( st->res, st->o->label, (int) st->cycle, 0, 0, 0, 0, "counters-not-conserved", 0, 0, "client", "-", excerpt );
        st->fails++;
    }
}

static int last_stream_channel( int mode )
{
    return matrix_channels( mode ) - 1;
}

// section 4, "The over-maximum send": once per window, per mode, channel 0
// and the last stream channel. Codes 0 and 1 one past their single-record
// maxima start as their reliable twins (the promotion rule; aborted, so
// nothing is sent). Every mode one past max_message_bytes is refused, and
// its named counter rises by exactly one while the other does not move.
static void over_maximum( matrix_state_t * st, struct rocketnet_client_t * client, int window )
{
    struct rocketnet_message_client_t * message_client = rocketnet_client_message_client( client );
    if ( !message_client )
    {
        r_fail( st->res, st->o->label, window, 0, 0, 0, 0, "over-max-no-connection", 0, 0, "client", "-", "no connection to count against" );
        st->fails++;
        return;
    }

    const struct rocketnet_counters_t * counters = rocketnet_message_client_counters( message_client );

    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        if ( !( st->o->mode_mask & ( 1u << mode ) ) )
        {
            continue;
        }

        const int channels[2] = { 0, last_stream_channel( mode ) };

        for ( int k = 0; k < 2; k++ )
        {
            const int channel = channels[k];

            if ( mode <= 1 )
            {
                const int promoted = (int) rocketnet_live_mode_max( &st->cfg, mode ) + 1;
                const uint64_t failed_before = rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
                uint8_t * q = rocketnet_client_start_message( client, channel, (enum rocketnet_reliability_t) mode, promoted );
                const uint64_t failed_delta = rocketnet_counter_value( counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) - failed_before;
                if ( q )
                {
                    rocketnet_client_abort_message( client );
                }
                if ( !q || failed_delta != 0 )
                {
                    char excerpt[96];
                    snprintf( excerpt, sizeof( excerpt ), "promotion start=%d failed_delta=%llu", q != NULL, (unsigned long long) failed_delta );
                    r_fail( st->res, st->o->label, window, 0, mode, channel, 0, "promotion-refused", promoted, 0, "client", "-", excerpt );
                    st->fails++;
                }
            }

            const int over = (int) st->cfg.max_message_bytes + 1;
            const int unreliable = mode <= 1 || mode == 5;
            const int counter = unreliable ? ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED : ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED;
            const int other = unreliable ? ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED : ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED;
            const uint64_t before = rocketnet_counter_value( counters, counter );
            const uint64_t before_other = rocketnet_counter_value( counters, other );
            uint8_t * out = rocketnet_client_start_message( client, channel, (enum rocketnet_reliability_t) mode, over );
            if ( out )
            {
                rocketnet_client_abort_message( client );

                char excerpt[96];
                snprintf( excerpt, sizeof( excerpt ), "start accepted %d bytes", over );
                r_fail( st->res, st->o->label, window, 0, mode, channel, 0, "over-max-not-refused", over, 0, "client", "-", excerpt );
                st->fails++;
                continue;
            }

            const uint64_t delta = rocketnet_counter_value( counters, counter ) - before;
            const uint64_t delta_other = rocketnet_counter_value( counters, other ) - before_other;
            cell_counts_t * cell = RN_CELL( st, 0, mode, channel, ROCKETNET_LIVE_BUCKET_OVER );
            cell->sent++;
            cell->over++;

            if ( delta != 1 || delta_other != 0 )
            {
                char excerpt[128];
                snprintf( excerpt, sizeof( excerpt ), "counter=%s delta=%llu other=%llu", rocketnet_counter_name( counter ),
                          (unsigned long long) delta, (unsigned long long) delta_other );
                r_fail( st->res, st->o->label, window, 0, mode, channel, 0, "over-counter-delta", over,
                        0, "client", "-", excerpt );
                st->fails++;
            }
        }
    }
}

// the HELLO: tag 0xD0, the cap in kbps, the cycle, the seed and
// max_message_bytes (u32 little endian each), then the bench label
static int send_hello( matrix_state_t * st, struct rocketnet_client_t * client )
{
    uint8_t hello[MATRIX_HELLO_BYTES + 64];
    int n = MATRIX_HELLO_BYTES;
    hello[0] = MATRIX_HELLO_NIBBLE;
    matrix_put_u32( hello + 1, (uint32_t) st->o->send_kbps );
    matrix_put_u32( hello + 5, st->cycle );
    matrix_put_u32( hello + 9, st->cfg.seed );
    matrix_put_u32( hello + 13, st->cfg.max_message_bytes );
    for ( const char * p = st->o->label; *p && n < (int) sizeof( hello ); p++ )
    {
        hello[n++] = (uint8_t) *p;
    }
    return send_control( client, hello, n );
}

// the sent report: tag 0xE0, then per stream driven its count
static int send_sent_report( matrix_state_t * st, struct rocketnet_client_t * client )
{
    uint8_t report[1 + ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * MATRIX_SENT_RECORD];
    int off = 0;
    report[off++] = MATRIX_SENT_NIBBLE;
    for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
    {
        for ( int c = 0; c < matrix_channels( m ); c++ )
        {
            report[off++] = (uint8_t) m;
            report[off++] = (uint8_t) c;
            matrix_put_u32( report + off, RN_STREAM( st, m, c )->sequence );
            off += 4;
        }
    }
    return send_control( client, report, off );
}

// section 5: every 50 ms the client reports, per server stream, the
// sequence below which every owed one arrived; it releases the server's ledger
static void send_progress( matrix_state_t * st, struct rocketnet_client_t * client )
{
    const double now = rocketnet_time();
    if ( st->last_progress > 0.0 && now - st->last_progress < MATRIX_PROGRESS_SECONDS )
    {
        return;
    }

    uint8_t report[MATRIX_REPORT_BYTES];
    const int n = matrix_rx_progress( st->rx, report, (int) sizeof( report ) );
    if ( n <= 1 )
    {
        return;
    }

    const enum rocketnet_reliability_t reliability = ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED;
    if ( !rocketnet_client_can_send_message( client, MATRIX_CONTROL_CHANNEL, reliability, n ) )
    {
        return;         // no room now: the next pass tries again
    }

    uint8_t * out = rocketnet_client_start_message( client, MATRIX_CONTROL_CHANNEL, reliability, n );
    if ( out )
    {
        memcpy( out, report, (size_t) n );
        rocketnet_client_finish_message( client, n );
        st->last_progress = now;
    }
}

// the drain is done when the server confirmed every owed sequence the client
// sent, and every owed sequence of the server's sent report arrived
static int drain_done( matrix_state_t * st )
{
    for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
    {
        for ( int c = 0; c < matrix_channels( m ); c++ )
        {
            const stream_t * s = RN_STREAM( st, m, c );
            if ( s->confirmed < s->sequence )
            {
                return 0;
            }
        }
    }
    return matrix_rx_complete( st->rx );
}

// bytes still to cross at the start of the drain, both ways, for its bound:
// the client's sends less what the server reports received, and once the
// server's sent report is in, its sends less what arrived here
static uint64_t drain_outstanding( matrix_state_t * st )
{
    uint64_t outstanding = 0;
    for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
    {
        for ( int c = 0; c < matrix_channels( m ); c++ )
        {
            const stream_t * s = RN_STREAM( st, m, c );
            if ( s->sent_bytes > s->confirmed_bytes )
            {
                outstanding += s->sent_bytes - s->confirmed_bytes;
            }

            const struct matrix_rx_stream_t * r = &st->rx->streams[m][c];
            if ( r->have_sent )
            {
                uint64_t server_bytes = 0;
                for ( uint32_t q = 0; q < r->sent; q++ )
                {
                    server_bytes += rocketnet_live_length( &st->cfg, m, c, q );
                }
                if ( server_bytes > r->bytes )
                {
                    outstanding += server_bytes - r->bytes;
                }
            }
        }
    }
    return outstanding;
}

// section 5: the drain's checks. Every owed sequence of the server's sent
// report arrived, or it is lost; every owed sequence the client sent was
// confirmed by the server's progress, or it is unconfirmed.
static void drain_judge( matrix_state_t * st, int window )
{
    if ( !st->rx->have_sent_report )
    {
        receive_fail( st, -1, MATRIX_CONTROL_CHANNEL, -1, "no-sent-report", 0, "the server's sent report never arrived" );
    }
    else
    {
        for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
        {
            for ( int c = 0; c < matrix_channels( m ); c++ )
            {
                uint32_t by_bucket[ROCKETNET_LIVE_NUM_BUCKETS] = { 0 };
                const struct matrix_rx_stream_t * r = &st->rx->streams[m][c];
                const uint32_t missing = matrix_rx_missing( &st->cfg, r, m, c, by_bucket );
                if ( missing )
                {
                    char excerpt[96];
                    snprintf( excerpt, sizeof( excerpt ), "missing=%u first=%u sent=%u", missing, r->owed, r->sent );
                    receive_fail( st, m, c, (int64_t) r->owed, "lost", (int) missing, excerpt );
                }
                for ( int b = 0; b < ROCKETNET_LIVE_NUM_BUCKETS; b++ )
                {
                    RN_CELL( st, 1, m, c, b )->missing += by_bucket[b];
                }
            }

            for ( int length = 1; length < ROCKETNET_LIVE_HEADER_BYTES; length++ )
            {
                if ( matrix_owed( m, (uint32_t) length ) && st->rx->tiny_received[m][length] < st->rx->tiny_sent[m][length] )
                {
                    receive_fail( st, m, -1, -1, "missing-tiny", length, "tiny count short" );
                }
            }
        }
    }

    for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
    {
        for ( int c = 0; c < matrix_channels( m ); c++ )
        {
            const stream_t * s = RN_STREAM( st, m, c );
            if ( s->confirmed < s->sequence )
            {
                char excerpt[96];
                snprintf( excerpt, sizeof( excerpt ), "confirmed=%u sent=%u", s->confirmed, s->sequence );
                r_fail( st->res, st->o->label, window, 0, m, c, (int64_t) s->confirmed, "unconfirmed", 0, 0, "client", "-", excerpt );
                st->fails++;
            }
        }
    }
}

static uint64_t run_matrix( opts_t * o )
{
    FILE * res = stdout;
    FILE * status = NULL;

    if ( o->results_path )
    {
        res = rocketnet_test_fopen( o->results_path, "w" );
        if ( !res )
        {
            perror( "client: results" );
            exit( 1 );
        }
    }

    if ( o->status_path )
    {
        status = rocketnet_test_fopen( o->status_path, "w" );
        if ( !status )
        {
            perror( "client: status" );
            exit( 1 );
        }
    }

    struct rocketnet_client_config_t defaults;
    rocketnet_client_default_config( &defaults );

    matrix_state_t st;
    memset( &st, 0, sizeof( st ) );
    st.o = o;
    st.res = res;
    st.status = status;
    st.cfg.seed = (uint32_t) o->seed;
    st.cfg.max_message_bytes = (uint32_t) defaults.max_message_bytes;
    st.streams = (stream_t *) calloc( ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS, sizeof( stream_t ) );
    st.rx = (struct matrix_rx_t *) calloc( 1, sizeof( struct matrix_rx_t ) );
    st.cells = (cell_counts_t *) calloc( 2 * ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * ROCKETNET_LIVE_NUM_BUCKETS, sizeof( cell_counts_t ) );
    st.rtt = (double *) calloc( 200000, sizeof( double ) );
    st.loss = (double *) calloc( 200000, sizeof( double ) );
    st.reconnect_ms = (double *) calloc( 4096, sizeof( double ) );
    uint8_t * buf = (uint8_t *) malloc( (size_t) st.cfg.max_message_bytes );
    if ( !st.streams || !st.rx || !st.cells || !st.rtt || !st.loss || !st.reconnect_ms || !buf )
    {
        fprintf( stderr, "client: out of memory\n" );
        exit( 1 );
    }
    const int cap = (int) st.cfg.max_message_bytes;

    st.run_start = rocketnet_time();

    int timeout_seconds = ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS;
    double wait = 2.0 * timeout_seconds;

    printf( "client: matrix mode, timeout %d s, wait %.1f s, seed %u, label %s, modes 0x%x, channels %d, max_message_bytes %u\n",
            timeout_seconds, wait, st.cfg.seed, o->label, o->mode_mask, o->channels, st.cfg.max_message_bytes );

    int window = 0;
    double step = 1.0 / ( o->update_hz > 0 ? o->update_hz : 1000 );

    long long last_clock = 0;

    if ( o->stagger > 0 )
    {
        rocketnet_sleep( o->stagger );
    }

    for ( window = 0; window <= o->cycles && !quit; window++ )
    {
        int is_final = ( window == o->cycles );

        st.cycle = (uint32_t) window;

        // section 11: each client re-reads its clock at every mint

        const long long now = (long long) time( NULL );
        if ( last_clock && now < last_clock )
        {
            char excerpt[96];
            snprintf( excerpt, sizeof( excerpt ), "clock went back %lld s", last_clock - now );
            r_fail( res, o->label, window, 0, 0, 0, 0, "clock-went-backwards", 0, 0, "client", "-", excerpt );
            st.fails++;
        }

        last_clock = now;
        r_event( res, o->label, window, "clock", "mint" );

        // bin/server is an internal message server on the transport dev config,
        // so its server key is the fixed internal development server key
        // (rocketnet_dev_server_public_key, sdk/rocketnet_connect_token.h): the
        // client needs nothing from the server process to mint its token.

        char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        const int address_is_ipv6 = ( o->address[0] == '[' );
        if ( rocketnet_create_dev_connect_token( address_is_ipv6 ? NULL : o->address, address_is_ipv6 ? o->address : NULL,
                                                 rocketnet_dev_server_public_key,
                                                 ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS, ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS,
                                                 rocketnet_random_uint64(), ROCKETNET_DEV_PROTOCOL_ID, (uint64_t) now, NULL, connect_token ) != ROCKETNET_STATUS_OK )
        {
            r_fail( res, o->label, window, 0, 0, 0, 0, "token-mint-failed", 0, 0, "client", "-", "mint" );
            st.fails++;
            break;
        }

        struct rocketnet_client_config_t cc;
        rocketnet_client_default_config( &cc );
        cc.send_kbps = o->send_kbps;
        cc.packets_per_update = o->packets_per_update;     // the budget, section 2

        enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &cc, &create_status );
        if ( !client )
        {
            printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
        }

        if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
        {
            rn_cli_client_stop_and_destroy( client, rocketnet_time() );
            client = NULL;
        }

        if ( !client )
        {
            r_fail( res, o->label, window, 0, 0, 0, 0, "client-create-failed", 0, 0, "client", "-", "create" );
            st.fails++;
            break;
        }

        // section 7: counters and RSS at start

        sample_counters( &st, client, rocketnet_time() - st.run_start );

        if ( status )
        {
            fprintf( status, "client\tconnecting\n" );
        }

        double t0 = rocketnet_time();
        double connect_ms = 0.0;
        int connected = 0;
        int state_at_fail = 0;
        double connect_timeout = 5.0;

        while ( !quit )
        {
            rocketnet_client_update( client, rocketnet_time() );
            rocketnet_client_send_packets( client );

            int state = rocketnet_client_state( client );
            if ( state >= ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                connected = 1;
                connect_ms = ( rocketnet_time() - t0 ) * 1000.0;
                break;
            }

            if ( state < 0 )
            {
                state_at_fail = state;
                break;
            }

            if ( rocketnet_time() - t0 > connect_timeout )
            {
                break;
            }
            rocketnet_sleep( step );
        }

        r_connect( res, o->label, window, connected, connect_ms );
        st.connect_count++;

        if ( connected )
        {
            st.connect_ok++;
            st.connect_ms_sum += connect_ms;
            if ( window > 0 && st.reconnect_count < 4096 )
            {
                st.reconnect_ms[st.reconnect_count++] = connect_ms;
            }
        }
        else
        {
            char counters[160];
            snprintf( counters, sizeof( counters ), "state=%s denied_state=%d", rocketnet_client_state_name( state_at_fail ), state_at_fail );

            const int denial = state_at_fail < 0 ? state_at_fail : 0;
            r_fail( res, o->label, window, 0, 0, 0, 0, "connect-failed", 0, denial, "client", counters,
                    rocketnet_client_state_name( state_at_fail ) );
            r_event( res, o->label, window, state_at_fail < 0 ? "denial" : "connect-timeout",
                     rocketnet_client_state_name( state_at_fail ) );

            if ( state_at_fail < 0 )
            {
                st.denials++;
            }

            st.fails++;
            rn_cli_client_stop_and_destroy( client, rocketnet_time() );
            break;
        }

        if ( status )
        {
            fprintf( status, "client\tconnected\n" );
        }

        // the stress window
        memset( st.streams, 0, ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * sizeof( stream_t ) );
        memset( st.cells, 0, 2 * ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * ROCKETNET_LIVE_NUM_BUCKETS * sizeof( cell_counts_t ) );
        matrix_rx_reset( st.rx );
        for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
        {
            st.rx->totals[m] = rocketnet_live_sequences_per_stream( &st.cfg, m );      // the server's streams end there
        }
        st.hello_answered = 0;
        st.outstanding = 0;
        st.last_progress = 0.0;

        int aborted = 0;

        // section 6: the HELLO first, and the matrix only after its answer

        int hello_sent = 0;
        const double h0 = rocketnet_time();
        while ( !quit && !st.hello_answered && rocketnet_time() - h0 < connect_timeout )
        {
            rocketnet_client_update( client, rocketnet_time() );
            if ( !hello_sent )
            {
                hello_sent = send_hello( &st, client );
            }
            rocketnet_client_send_packets( client );
            receive_check( &st, client );
            if ( rocketnet_client_state( client ) < 0 )
            {
                break;
            }
            rocketnet_sleep( step );
        }

        if ( !quit && !st.hello_answered )
        {
            r_fail( res, o->label, window, 0, -1, MATRIX_CONTROL_CHANNEL, -1, hello_sent ? "no-hello-answer" : "hello-not-sent", 0, 0, "client", "-",
                    "the server did not answer the HELLO" );
            st.fails++;
            aborted = 1;
        }

        // section 4: the over-maximum send, once per window per mode, while
        // the send store is empty so the promotion start has its room

        if ( !aborted && !quit )
        {
            over_maximum( &st, client, window );
        }

        double w0 = rocketnet_time();
        double last_counters = w0;
        double last_stat = w0;

        while ( !aborted && !quit && rocketnet_time() - w0 < o->duration )
        {
            rocketnet_client_update( client, rocketnet_time() );

            const int state = rocketnet_client_state( client );
            if ( state < 0 )
            {
                char excerpt[128];

                // a negative state names how the connection ended

                snprintf( excerpt, sizeof( excerpt ), "state=%s", rocketnet_client_state_name( state ) );
                r_fail( res, o->label, window, 0, 0, 0, 0, "unexpected-disconnect", 0, state, "client",
                        "-", excerpt );
                r_event( res, o->label, window, "unexpected-disconnect", rocketnet_client_state_name( state ) );
                st.fails++;
                aborted = 1;
                break;
            }

            // receive first: the batch the update made is read before the
            // sends that follow, and the progress reports confirm early
            receive_check( &st, client );

            // send: one message per stream per pass, round robin, each only
            // when the library has room for it
            for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES && !quit; m++ )
            {
                if ( !( o->mode_mask & ( 1u << m ) ) )
                {
                    continue;
                }

                for ( int c = 0; c < o->channels; c++ )
                {
                    if ( !stream_channels( m, c ) )
                    {
                        continue;
                    }

                    stream_t * s = RN_STREAM( &st, m, c );

                    // modes 3 and 7 share the channel's order: interleave them
                    // (tests/matrix_receive.h) so the server can check it; the
                    // client's streams have no total
                    const int partner = matrix_interleave_partner( m );
                    if ( partner >= 0 &&
                         !matrix_interleave_admits( m, s->sequence, RN_STREAM( &st, partner, c )->sequence, 0, (int) ( ( o->mode_mask >> partner ) & 1u ) ) )
                    {
                        continue;
                    }

                    const uint32_t length = rocketnet_live_length( &st.cfg, m, c, s->sequence );
                    const int bucket = rocketnet_live_bucket( &st.cfg, m, length );
                    const int reliable = matrix_reliable_path( m, length );
                    const int r = !reliable || matrix_ledger_admits( st.outstanding, length, (uint32_t) o->send_kbps )
                                      ? send_message( client, m, c, st.cycle, s->sequence, buf, cap, &st )
                                      : 0;
                    if ( r > 0 )
                    {
                        s->sequence++;
                        s->sent_bytes += (uint64_t) r;
                        if ( reliable )
                        {
                            st.outstanding += (uint64_t) r;
                        }
                        RN_CELL( &st, 0, m, c, bucket )->sent++;
                    }
                    else if ( r == 0 )
                    {
                        RN_CELL( &st, 0, m, c, bucket )->paced++;
                    }
                    else
                    {
                        char excerpt[96];
                        snprintf( excerpt, sizeof( excerpt ), "start refused after can_send mode=%d channel=%d", m, c );
                        r_fail( res, o->label, window, 0, m, c, s->sequence, "send-start-failed", 0, 0,
                                "client", "-", excerpt );
                        st.fails++;
                    }
                }
            }

            send_progress( &st, client );
            rocketnet_client_send_packets( client );

            if ( rocketnet_time() - last_stat >= 1.0 )
            {
                last_stat = rocketnet_time();

                if ( st.rtt_count < 200000 )
                {
                    st.rtt[st.rtt_count] = rn_client_stats_of( client ).rtt_ms;
                    st.loss[st.loss_count] = rn_client_stats_of( client ).packet_loss_percent;
                    r_stat( res, o->label, window, rocketnet_time() - w0,
                            st.rtt[st.rtt_count],
                            rn_client_stats_of( client ).jitter_ms,
                            st.loss[st.loss_count] );
                    st.rtt_count++;
                    st.loss_count++;
                }
            }

            if ( rocketnet_time() - last_counters >= o->counters_every )
            {
                last_counters = rocketnet_time();
                sample_counters( &st, client, rocketnet_time() - st.run_start );
            }

            rocketnet_sleep( step );
        }

        if ( !aborted )
        {
            // the drain: stop sending, report the counts sent, then wait for
            // the server's progress and its own sent report, bounded by the
            // reliable timeout plus twice the time the budget needs for the
            // bytes still outstanding
            const double kbps = o->send_kbps > 0 ? (double) o->send_kbps : 1.0;
            int report_sent = 0;
            double d0 = rocketnet_time();
            double drain_bound = MATRIX_DRAIN_SECONDS + 2.0 * (double) drain_outstanding( &st ) * 8.0 / ( kbps * 1000.0 );
            int server_report_seen = 0;
            while ( !quit && rocketnet_time() - d0 < drain_bound )
            {
                rocketnet_client_update( client, rocketnet_time() );
                if ( !report_sent )
                {
                    report_sent = send_sent_report( &st, client );
                }
                send_progress( &st, client );
                rocketnet_client_send_packets( client );
                receive_check( &st, client );
                if ( rocketnet_client_state( client ) < 0 )
                {
                    char excerpt[128];
                    snprintf( excerpt, sizeof( excerpt ), "state=%s during drain", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
                    r_fail( res, o->label, window, 0, 0, 0, 0, "unexpected-disconnect", 0,
                            rocketnet_client_state( client ), "client", "-", excerpt );
                    r_event( res, o->label, window, "unexpected-disconnect", "drain" );
                    st.fails++;
                    aborted = 1;
                    break;
                }

                if ( st.rx->have_sent_report && !server_report_seen )
                {
                    // the server's counts are known now: its outstanding bytes join the bound
                    server_report_seen = 1;
                    drain_bound = ( rocketnet_time() - d0 ) + MATRIX_DRAIN_SECONDS + 2.0 * (double) drain_outstanding( &st ) * 8.0 / ( kbps * 1000.0 );
                }

                if ( report_sent && drain_done( &st ) )
                {
                    break;
                }

                rocketnet_sleep( step );
            }

            if ( !aborted && !quit )
            {
                if ( !report_sent )
                {
                    r_fail( res, o->label, window, 0, -1, MATRIX_CONTROL_CHANNEL, -1, "sent-report-not-sent", 0, 0, "client", "-",
                            "no room for the sent report" );
                    st.fails++;
                }
                drain_judge( &st, window );
            }
        }

        // section 7: the per-window table, one row per (dir, mode, channel, bucket)

        for ( int dir = 0; dir < 2; dir++ )
        {
            for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
            {
                if ( !( o->mode_mask & ( 1u << m ) ) && dir == 0 )
                {
                    continue;
                }
                for ( int c = 0; c < ROCKETNET_LIVE_NUM_CHANNELS; c++ )
                {
                    for ( int b = 0; b < ROCKETNET_LIVE_NUM_BUCKETS; b++ )
                    {
                        cell_counts_t * cell = RN_CELL( &st, dir, m, c, b );
                        if ( cell->sent || cell->received || cell->verified || cell->failed || cell->lost ||
                             cell->superseded || cell->missing || cell->ooo || cell->dup || cell->over || cell->paced )
                        {
                            r_cell( res, o->label, window, dir, m, c, b,
                                    cell->sent, cell->received, cell->verified, cell->failed, cell->lost,
                                    cell->superseded, cell->missing, cell->ooo, cell->dup, cell->over, cell->paced );
                        }
                    }
                }
            }
        }

        // section 7: counters and RSS at exit of the window

        sample_counters( &st, client, rocketnet_time() - st.run_start );

        /* Every in-process cycle uses orderly request/completion shutdown.
           Silent client loss needs a separate process-kill experiment. */
        const int ended = !aborted ? rocketnet_client_state( client ) : ROCKETNET_CLIENT_STATE_CONNECTED;
        if ( !aborted && ended < 0 )
        {
            char excerpt[96];
            snprintf( excerpt, sizeof( excerpt ), "state=%s before clean close", rocketnet_client_state_name( (enum rocketnet_client_state_t) ended ) );
            r_fail( res, o->label, window, 0, 0, 0, 0, "unexpected-disconnect-reason", 0, ended, "client", "-", excerpt );
            r_event( res, o->label, window, "unexpected-disconnect", "reason" );
            st.fails++;
        }

        rocketnet_client_disconnect( client );
        rn_cli_client_wait_disconnected( client, rocketnet_time() );
        r_cycle( res, o->label, window, "clean" );
        st.clean_count++;
        rn_cli_client_stop_and_destroy( client, rocketnet_time() );

        printf( "client: window %d done, failures so far %llu\n", window, (unsigned long long) st.fails );
        fflush( stdout );

        if ( !is_final )
        {
            rocketnet_sleep( wait );
        }
    }

    // every requested window ran to its checks: an interrupt before the first
    // window, or inside one (where the checks are skipped), leaves checks
    // undone, which fails and never prints PASS
    if ( quit || window != o->cycles + 1 )
    {
        r_event( res, o->label, window, "interrupted", "checks-not-completed" );
        printf( "client: interrupted after %d of %d windows: the checks did not complete\n", window, o->cycles + 1 );
        st.fails++;
    }

    // section 7, per bench: connect and reconnect times, RTT quantiles, loss

    double reconnect_max = 0.0;

    for ( int i = 0; i < st.reconnect_count; i++ )
    {
        if ( st.reconnect_ms[i] > reconnect_max )
        {
            reconnect_max = st.reconnect_ms[i];
        }
    }

    r_bench( res, o->label, st.connect_count, st.connect_ok,
             st.connect_count ? st.connect_ms_sum / st.connect_count : 0.0,
             quantile( st.reconnect_ms, st.reconnect_count, 0.50 ),
             quantile( st.reconnect_ms, st.reconnect_count, 0.99 ),
             reconnect_max,
             quantile( st.rtt, st.rtt_count, 0.50 ),
             quantile( st.rtt, st.rtt_count, 0.99 ),
             quantile( st.loss, st.loss_count, 0.50 ),
             st.clean_count + st.silent_count, st.clean_count, st.silent_count, st.denials, st.fulls );

    // counters at process exit
    r_counters( res, o->label, "RUN_FAILURES", st.fails, rocketnet_time() - st.run_start );
    fprintf( res, "verdict\t%s\n", st.fails ? "FAIL" : "PASS" );
    printf( "client: verdict %s failures %llu\n", st.fails ? "FAIL" : "PASS", (unsigned long long) st.fails );

    const uint64_t fails = st.fails;

    free( st.streams );
    free( st.rx );
    free( st.cells );
    free( st.rtt );
    free( st.loss );
    free( st.reconnect_ms );
    free( buf );

    if ( status )
    {
        fprintf( status, "client\tdone\n" );
    }

    if ( res != stdout )
    {
        fclose( res );
    }

    if ( status )
    {
        fclose( status );
    }

    return fails;
}

// the AF_XDP packet blaster's token file (docs/XDP-PACKET-BLASTER.md): N distinct
// development tokens for bin/server at the address, one canonical base64 line each
static int print_dev_tokens( int argc, char ** argv )
{
    const uint64_t count = parse_u64( argv[2], "--print-dev-tokens" );
    const uint64_t expire = argc == 5 ? parse_u64( argv[4], "--print-dev-tokens expire" ) : (uint64_t) ROCKETNET_DEV_CONNECT_TOKEN_EXPIRE_SECONDS;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR ); // stdout carries only the token lines
    if ( count == 0 || count > 1000 || expire == 0 || expire > 3600 || rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "client: --print-dev-tokens wants 1..1000 tokens and 1..3600 expire seconds\n" );
        return 2;
    }

    const uint64_t now = (uint64_t) time( NULL );
    for ( uint64_t i = 0; i < count; i++ )
    {
        char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
        if ( rocketnet_create_dev_connect_token( argv[3], NULL, rocketnet_dev_server_public_key, (int) expire,
                                                 ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS, rocketnet_random_uint64(),
                                                 ROCKETNET_DEV_PROTOCOL_ID, now, NULL, connect_token ) != ROCKETNET_STATUS_OK )
        {
            fprintf( stderr, "client: token mint failed\n" );
            rocketnet_term();
            return 1;
        }
        printf( "%s\n", connect_token );
    }

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "client: term_status=%s: the library still holds allocations at term\n", rocketnet_status_name( term_status ) );
        return 1;
    }
    return 0;
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    if ( ( argc == 4 || argc == 5 ) && strcmp( argv[1], "--print-dev-tokens" ) == 0 )
    {
        return print_dev_tokens( argc, argv );
    }

    opts_t o;
    default_opts( &o, argc, argv );

    int self_test;
    const char * self_test_file;
    parse_args( &o, argc, argv, &self_test, &self_test_file );

    if ( self_test )
    {
        write_self_test( self_test_file, o.label );
        return 0;
    }

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    char connect_address_buffer[256];
    const char * connect_address = connect_address_buffer;
    if ( !rocketnet_test_getenv( "ROCKETNET_CLIENT_CONNECT_ADDRESS", connect_address_buffer, sizeof( connect_address_buffer ) ) )
    {
        connect_address = o.address;
    }

    if ( o.matrix )
    {
        const uint64_t fails = run_matrix( &o );
        rocketnet_term();
        return fails ? 1 : 0;
    }

    // the development connect: mint a token locally against the published
    // development keys. a production client is handed its token by a backend.

    // the binary token of the internal client transport, sealed to the fixed
    // internal development server key that bin/server's transport dev config carries

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !dev_token_binary( connect_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token, sizeof( connect_token ) ) )
    {
        printf( "error: failed to write dev connect token\n" );
        rocketnet_term();
        return 1;
    }

    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_time() );
    if ( !client )
    {
        rocketnet_term();
        return 1;
    }

    int last_state = 0;
    uint64_t payloads_sent = 0;
    uint64_t payloads_received = 0;

    uint8_t * received = (uint8_t *) malloc( (size_t) rocketnet_client_transport_max_payload_bytes( client ) );
    if ( !received )
    {
        rn_native_client_transport_stop_and_destroy( client );
        rocketnet_term();
        return 1;
    }

    while ( !quit )
    {
        rocketnet_client_transport_update( client, rocketnet_time() );

        const int state = rocketnet_client_transport_state( client );
        if ( state != last_state )
        {
            printf( "client state: %s\n", rocketnet_client_transport_state_name( state ) );
            last_state = state;
            if ( state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            {
                break;
            }
        }

        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            rocketnet_client_transport_send_packet( client, payload, sizeof( payload ) );
            payloads_sent++;

            int received_bytes;
            while ( ( received_bytes = rocketnet_client_transport_receive_packet( client, received, rocketnet_client_transport_max_payload_bytes( client ) ) ) > 0 )
            {
                payloads_received++;
                if ( ( payloads_received % 100 ) == 0 )
                {
                    printf( "client sent %llu and received %llu payloads\n",
                            (unsigned long long) payloads_sent, (unsigned long long) payloads_received );
                }
            }
        }

        rocketnet_sleep( 0.01 );
    }

    printf( "\nclient disconnecting\n" );
    rocketnet_client_transport_disconnect( client );
    rocketnet_sleep( 0.1 );
    free( received );

    rn_native_client_transport_stop_and_destroy( client );

    // term refuses while the library still holds an allocation: a run that
    // leaked fails (TA022)
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "client: term_status=%s: the library still holds allocations at term\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    return 0;
}
