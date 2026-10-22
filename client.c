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
    result lines of section 7 written to --results. --self-test-result
    writes a sample result file without any socket, for the merge to read.
*/

#include "tests/native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "sdk/rocketnet_connect_token.h"
#include "sdk/rocketnet_counters.h"
#include "tests/live_verify.h"
#include "tests/portable_crt.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

// Internal transport tools mint the binary development token directly.
static int dev_token_binary( const char * address, uint64_t client_id, uint64_t timestamp, uint8_t * token, size_t token_bytes )
{
    return rocketnet_write_test_connect_token( address, client_id, timestamp, token, token_bytes ) == ROCKETNET_STATUS_OK;
}

static volatile int quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
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
    printf( "  --update-hz H            main loop update rate in Hz\n" );
    printf( "  --counters-every S       print counters every S seconds (default 60)\n" );
    printf( "  --results PATH           where result lines go\n" );
    printf( "  --status PATH            one line per state change\n" );
    printf( "  --self-test-result FILE  write a sample result file without any socket and exit\n" );
    printf( "  --help                   this message\n" );
}

static uint64_t parse_u64( const char * s, const char * flag )
{
    char * end = NULL;
    errno = 0;
    uint64_t v = strtoull( s, &end, 10 );
    if ( !s[0] || !end || *end || errno )
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
    memset( o, 0, sizeof(*o) );
    o->address = argc > 1 ? argv[1] : "127.0.0.1:40000";
    o->label = "client";
    o->duration = 35.0;
    o->cycles = 8;
    o->stagger = 0.0;
    o->seed = (uint64_t) time( NULL ) ^ ( (uint64_t) getpid() << 32 );
    o->send_kbps = 1000;
    o->update_hz = 1000;
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
        if ( *p == ',' ) p++;
        else if ( *p ) { fprintf( stderr, "client: --modes wants a comma-separated list of modes 0 to 7\n" ); exit( 2 ); }
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
        i = 2;
    for ( ; i < argc; i++ )
    {
        const char * a = argv[i];
        if ( strcmp( a, "--help" ) == 0 )
        {
            usage( argv[0] );
            exit( 0 );
        }
        else if ( strcmp( a, "--matrix" ) == 0 )
            o->matrix = 1;
        else if ( strcmp( a, "--address" ) == 0 && i + 1 < argc )
            o->address = argv[++i];
        else if ( strcmp( a, "--label" ) == 0 && i + 1 < argc )
            o->label = argv[++i];
        else if ( strcmp( a, "--duration" ) == 0 && i + 1 < argc )
            o->duration = parse_f64( argv[++i], a );
        else if ( strcmp( a, "--cycles" ) == 0 && i + 1 < argc )
            o->cycles = (int) parse_u64( argv[++i], a );
        else if ( strcmp( a, "--stagger" ) == 0 && i + 1 < argc )
            o->stagger = parse_f64( argv[++i], a );
        else if ( strcmp( a, "--seed" ) == 0 && i + 1 < argc )
            o->seed = parse_u64( argv[++i], a );
        else if ( strcmp( a, "--modes" ) == 0 && i + 1 < argc )
            o->mode_mask = parse_modes( argv[++i] );
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
            o->send_kbps = (int) parse_u64( argv[++i], a );
        else if ( strcmp( a, "--update-hz" ) == 0 && i + 1 < argc )
            o->update_hz = (int) parse_u64( argv[++i], a );
        else if ( strcmp( a, "--counters-every" ) == 0 && i + 1 < argc )
            o->counters_every = parse_f64( argv[++i], a );
        else if ( strcmp( a, "--results" ) == 0 && i + 1 < argc )
            o->results_path = argv[++i];
        else if ( strcmp( a, "--status" ) == 0 && i + 1 < argc )
            o->status_path = argv[++i];
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
                     uint32_t sequence, const char * reason, int bytes,
                     int denial, const char * side, const char * counters, const char * excerpt )
{
    fprintf( f, "fail\t%s\t%d\t%d\t%d\t%d\t%u\t%s\t%d\t%d\t%s\t%s\t%s\n", label, cycle, dir, mode, channel,
             (unsigned) sequence, reason, bytes, denial, side, counters ? counters : "-", excerpt ? excerpt : "-" );
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
    if ( !f ) return 0;
    char line[256];
    const int have_line = fgets( line, sizeof( line ), f ) != NULL;
    fclose( f );
    if ( !have_line ) return 0;
    /* statm is "total resident ...": skip the total, read the resident pages */
    char * after_total = NULL;
    char * after_resident = NULL;
    strtol( line, &after_total, 10 );
    const long resident = strtol( after_total, &after_resident, 10 );
    if ( after_total == line || after_resident == after_total ) return 0;
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

typedef struct cell_counts_t
{
    uint64_t sent, received, verified, failed, lost, superseded, missing, ooo, dup, over, paced;
} cell_counts_t;

typedef struct stream_t
{
    uint32_t sequence;
    uint64_t sent;
    int channel;
    int mode;
    uint64_t seen[1024];        // 65,536-bit seen window per stream (section 5)
    uint32_t seen_high;
    int seen_valid;
    int64_t expected;           // ordered modes' next sequence
    int64_t last;               // sequenced modes' last sequence
    int has_last;
} stream_t;

typedef struct matrix_state_t
{
    opts_t * o;
    FILE * res;
    FILE * status;
    rocketnet_live_config_t cfg;
    uint32_t cycle;
    uint64_t fails;
    stream_t * streams;         // [mode][channel]
    cell_counts_t * cells;      // [dir][mode][channel][bucket]
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
    if ( mode == 0 || mode == 2 || mode == 5 || mode == 6 )
        return 1;
    return channel < 31;
}

static int send_message( struct rocketnet_client_t * client, int mode, int channel, uint32_t cycle,
                         uint32_t sequence, uint8_t * buf, int cap, matrix_state_t * st )
{
    int len = rocketnet_live_generate( &st->cfg, 0, mode, channel, cycle, sequence, buf, cap );
    if ( len <= 0 ) return 0;
    uint8_t * out = rocketnet_client_start_message( client, channel, (enum rocketnet_reliability_t) mode, len );
    if ( !out ) return -1;
    memcpy( out, buf, (size_t) len );
    rocketnet_client_finish_message( client, len );
    return len;
}

// section 5: a 65,536-bit seen-window per stream for the unordered modes.
static int seen_test_and_set( stream_t * s, uint32_t sequence )
{
    if ( !s->seen_valid )
    {
        s->seen_valid = 1;
        s->seen_high = sequence;
        s->seen[( sequence >> 6 ) & 1023] |= 1ull << ( sequence & 63 );
        return 0;
    }
    if ( sequence > s->seen_high )
    {
        const uint32_t gap = sequence - s->seen_high;
        if ( gap >= 65536u )
            memset( s->seen, 0, sizeof(s->seen) );
        else
            for ( uint32_t q = s->seen_high + 1; q <= sequence; q++ )
                s->seen[( q >> 6 ) & 1023] &= ~( 1ull << ( q & 63 ) );
        s->seen_high = sequence;
        s->seen[( sequence >> 6 ) & 1023] |= 1ull << ( sequence & 63 );
        return 0;
    }
    if ( (uint64_t) sequence + 65536u <= s->seen_high )
        return 0;
    uint64_t * word = &s->seen[( sequence >> 6 ) & 1023];
    const uint64_t bit = 1ull << ( sequence & 63 );
    if ( *word & bit )
        return 1;
    *word |= bit;
    return 0;
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
            const int channel = batch->channel[m];
            if ( bytes < 1 ) continue;
            int tag = data[0];
            int mode = tag & 7;
            int recv_channel = ( mode == 0 || mode == 2 || mode == 5 || mode == 6 ) ? 0 : channel;
            // section 5: the result row names the channel the sender passed
            int sender_channel = bytes >= 2 ? data[1] : recv_channel;
            if ( sender_channel < 0 || sender_channel >= ROCKETNET_LIVE_NUM_CHANNELS )
                sender_channel = recv_channel;
            const int bucket = rocketnet_live_bucket( &st->cfg, mode, (uint32_t) bytes );
            cell_counts_t * cell = RN_CELL( st, 1, mode, sender_channel, bucket );
            cell->received++;
            int bad;
            int r = rocketnet_live_check( &st->cfg, data, bytes, 1, mode, recv_channel, st->cycle, -1, &bad );
            if ( r != ROCKETNET_LIVE_OK )
            {
                const uint32_t seq = rocketnet_live_sequence( data, bytes );
                char excerpt[128];
                snprintf( excerpt, sizeof(excerpt), "check=%s offset=%d", rocketnet_live_result_name( r ), bad );
                r_fail( st->res, st->o->label, (int) st->cycle, 1, mode, sender_channel, seq,
                         rocketnet_live_result_name( r ), bytes, 0, "receiver", "-", excerpt );
                cell->failed++;
                st->fails++;
            }
            else
            {
                cell->verified++;
                if ( bytes >= ROCKETNET_LIVE_HEADER_BYTES )
                {
                    stream_t * s = RN_STREAM( st, mode, sender_channel );
                    const uint32_t seq = rocketnet_live_sequence( data, bytes );
                    const int reliable = ( mode == 2 || mode == 3 || mode == 4 || mode == 6 || mode == 7 );
                    if ( reliable && seen_test_and_set( s, seq ) )
                    {
                        cell->dup++;
                        char excerpt[96];
                        snprintf( excerpt, sizeof(excerpt), "duplicate sequence=%u", (unsigned) seq );
                        r_fail( st->res, st->o->label, (int) st->cycle, 1, mode, sender_channel, seq,
                                 "duplicate", bytes, 0, "receiver", "-", excerpt );
                        st->fails++;
                    }
                    if ( mode == 3 || mode == 7 )
                    {
                        if ( seq < (uint32_t) s->expected )
                            cell->ooo++;
                        else if ( seq > (uint32_t) s->expected )
                        {
                            cell->missing += seq - (uint32_t) s->expected;
                            s->expected = (int64_t) seq + 1;
                        }
                        else
                            s->expected++;
                    }
                    else if ( mode == 4 )
                    {
                        if ( s->has_last )
                        {
                            if ( seq <= (uint32_t) s->last )
                                cell->superseded++;
                            else if ( seq > (uint32_t) s->last + 1u )
                                cell->missing += seq - (uint32_t) s->last - 1u;
                        }
                        s->last = (int64_t) seq;
                        s->has_last = 1;
                    }
                    else if ( mode == 1 )
                    {
                        if ( s->has_last )
                        {
                            if ( seq <= (uint32_t) s->last )
                                cell->superseded++;
                            else if ( seq > (uint32_t) s->last + 1u )
                                cell->lost += seq - (uint32_t) s->last - 1u;
                        }
                        s->last = (int64_t) seq;
                        s->has_last = 1;
                    }
                    else if ( mode == 2 || mode == 5 || mode == 6 )
                    {
                        if ( s->seen_valid && seq > s->seen_high + 1u )
                            cell->missing += seq - s->seen_high - 1u;
                    }
                }
            }
        }
    }
}

static double quantile( const double * values, int count, double q )
{
    if ( count <= 0 ) return 0.0;
    double * copy = (double *) malloc( (size_t) count * sizeof(double) );
    if ( !copy ) return 0.0;
    memcpy( copy, values, (size_t) count * sizeof(double) );
    for ( int i = 1; i < count; i++ )
    {
        double v = copy[i];
        int j = i - 1;
        while ( j >= 0 && copy[j] > v ) { copy[j + 1] = copy[j]; j--; }
        copy[j + 1] = v;
    }
    int index = (int) ( q * ( count - 1 ) + 0.5 );
    if ( index < 0 ) index = 0;
    if ( index >= count ) index = count - 1;
    double result = copy[index];
    free( copy );
    return result;
}

static long resident_kb( void );

// section 7: every counter by name, message and transport counters summed.
static void sample_counters( matrix_state_t * st, struct rocketnet_client_t * client, double seconds )
{
    if ( !client ) return;
    struct rocketnet_message_client_t * message_client = (struct rocketnet_message_client_t *) client;
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
    const int mc = rocketnet_counters_conserved( message, report, (int) sizeof(report) );
    const int tc = rocketnet_counters_conserved( transport_counters, report, (int) sizeof(report) );
    fprintf( st->res, "conserved\t%s\t%.1f\t%d\t%d\n", st->o->label, seconds, mc, tc );
    r_rss( st->res, st->o->label, seconds, resident_kb() * 4 );
    if ( !mc || !tc )
    {
        char excerpt[160];
        snprintf( excerpt, sizeof(excerpt), "conservation message=%d transport=%d", mc, tc );
        r_fail( st->res, st->o->label, (int) st->cycle, 0, 0, 0, 0, "counters-not-conserved", 0, 0, "client", "-", excerpt );
        st->fails++;
    }
}

static int last_stream_channel( int mode )
{
    return ( mode == 1 || mode == 3 || mode == 4 || mode == 7 ) ? 30 : 31;
}

// section 4, "The over-maximum send": once per window, per mode, channel 0
// and the last stream channel; the named counter must rise by exactly one.
static void over_maximum( matrix_state_t * st, struct rocketnet_client_t * client, int window )
{
    struct rocketnet_message_client_t * message_client = (struct rocketnet_message_client_t *) client;
    const struct rocketnet_counters_t * counters = rocketnet_message_client_counters( message_client );
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        if ( !( st->o->mode_mask & ( 1u << mode ) ) ) continue;
        const int max_m = (int) rocketnet_live_mode_max( &st->cfg, mode );
        const int channels[2] = { 0, last_stream_channel( mode ) };
        for ( int k = 0; k < 2; k++ )
        {
            const int channel = channels[k];
            const int counter = ( mode == 0 || mode == 1 ) ? ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED : ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED;
            const uint64_t before = rocketnet_counter_value( counters, counter );
            uint8_t * out = rocketnet_client_start_message( client, channel, (enum rocketnet_reliability_t) mode, max_m + 1 );
            if ( out )
            {
                rocketnet_client_finish_message( client, max_m + 1 );
                char excerpt[96];
                snprintf( excerpt, sizeof(excerpt), "start accepted %d bytes", max_m + 1 );
                r_fail( st->res, st->o->label, window, 0, mode, channel, 0, "over-max-not-refused", max_m + 1, 0, "client", "-", excerpt );
                st->fails++;
                rocketnet_client_abort_message( client );
                continue;
            }
            const uint64_t after = rocketnet_counter_value( counters, counter );
            cell_counts_t * cell = RN_CELL( st, 0, mode, channel, ROCKETNET_LIVE_BUCKET_OVER );
            cell->sent++;
            cell->over++;
            if ( after != before + 1 )
            {
                char excerpt[128];
                snprintf( excerpt, sizeof(excerpt), "counter=%s delta=%llu", rocketnet_counter_name( counter ),
                          (unsigned long long) ( after - before ) );
                r_fail( st->res, st->o->label, window, 0, mode, channel, 0, "over-counter-delta", max_m + 1,
                         0, "client", "-", excerpt );
                st->fails++;
            }
        }
    }
}

static void run_matrix( opts_t * o )
{
    FILE * res = stdout;
    FILE * status = NULL;
    if ( o->results_path )
    {
        res = rocketnet_test_fopen( o->results_path, "w" );
        if ( !res ) { perror( "client: results" ); exit( 1 ); }
    }
    if ( o->status_path )
    {
        status = rocketnet_test_fopen( o->status_path, "w" );
        if ( !status ) { perror( "client: status" ); exit( 1 ); }
    }

    matrix_state_t st;
    memset( &st, 0, sizeof(st) );
    st.o = o;
    st.res = res;
    st.status = status;
    st.cfg.seed = (uint32_t) o->seed;
    st.cfg.max_message_bytes = 262144;
    st.streams = (stream_t *) calloc( ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS, sizeof(stream_t) );
    st.cells = (cell_counts_t *) calloc( 2 * ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * ROCKETNET_LIVE_NUM_BUCKETS, sizeof(cell_counts_t) );
    st.rtt = (double *) calloc( 200000, sizeof(double) );
    st.loss = (double *) calloc( 200000, sizeof(double) );
    st.reconnect_ms = (double *) calloc( 4096, sizeof(double) );
    if ( !st.streams || !st.cells || !st.rtt || !st.loss || !st.reconnect_ms )
    {
        fprintf( stderr, "client: out of memory\n" );
        exit( 1 );
    }
    st.run_start = rocketnet_time();

    int timeout_seconds = ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS;
    double wait = 2.0 * timeout_seconds;
    printf( "client: matrix mode, timeout %d s, wait %.1f s, seed %u, label %s, modes 0x%x, channels %d\n",
            timeout_seconds, wait, st.cfg.seed, o->label, o->mode_mask, o->channels );

    int window = 0;
    double step = 1.0 / ( o->update_hz > 0 ? o->update_hz : 1000 );
    long long last_clock = 0;

    if ( o->stagger > 0 )
        rocketnet_sleep( o->stagger );

    for ( window = 0; window <= o->cycles && !quit; window++ )
    {
        int is_final = ( window == o->cycles );
        st.cycle = (uint32_t) window;

        // section 11: each client re-reads its clock at every mint
        const long long now = (long long) time( NULL );
        if ( last_clock && now < last_clock )
        {
            char excerpt[96];
            snprintf( excerpt, sizeof(excerpt), "clock went back %lld s", last_clock - now );
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
        cc.packets_per_update = 64;     // the budget, section 2

        enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &cc, &create_status );
        if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
        if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
        {
            rn_cli_client_stop_and_destroy( client );
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
            fprintf( status, "client\tconnecting\n" );

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
                break;
            rocketnet_sleep( step );
        }
        r_connect( res, o->label, window, connected, connect_ms );
        st.connect_count++;
        if ( connected )
        {
            st.connect_ok++;
            st.connect_ms_sum += connect_ms;
            if ( window > 0 && st.reconnect_count < 4096 )
                st.reconnect_ms[st.reconnect_count++] = connect_ms;
        }
        else
        {
            char counters[160];
            snprintf( counters, sizeof(counters), "state=%s denied_state=%d", rocketnet_client_state_name( state_at_fail ), state_at_fail );
            const int denial = state_at_fail < 0 ? state_at_fail : 0;
            r_fail( res, o->label, window, 0, 0, 0, 0, "connect-failed", 0, denial, "client", counters,
                    rocketnet_client_state_name( state_at_fail ) );
            r_event( res, o->label, window, state_at_fail < 0 ? "denial" : "connect-timeout",
                     rocketnet_client_state_name( state_at_fail ) );
            if ( state_at_fail < 0 ) st.denials++;
            st.fails++;
            rn_cli_client_stop_and_destroy( client );
            break;
        }
        if ( status )
            fprintf( status, "client\tconnected\n" );

        /* the stress window */
        memset( st.streams, 0, ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * sizeof(stream_t) );
        memset( st.cells, 0, 2 * ROCKETNET_LIVE_NUM_MODES * ROCKETNET_LIVE_NUM_CHANNELS * ROCKETNET_LIVE_NUM_BUCKETS * sizeof(cell_counts_t) );
        for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
            for ( int c = 0; c < ROCKETNET_LIVE_NUM_CHANNELS; c++ )
            {
                stream_t * s = RN_STREAM( &st, m, c );
                s->mode = m;
                s->channel = c;
            }

        double w0 = rocketnet_time();
        double last_counters = w0;
        double last_stat = w0;
        uint8_t buf[262144 + 16];
        int cap = (int) sizeof(buf);
        int aborted = 0;

        while ( !quit && rocketnet_time() - w0 < o->duration )
        {
            rocketnet_client_update( client, rocketnet_time() );
            const int state = rocketnet_client_state( client );
            if ( state < 0 )
            {
                char excerpt[128];
                // a negative state names how the connection ended
                snprintf( excerpt, sizeof(excerpt), "state=%s", rocketnet_client_state_name( state ) );
                r_fail( res, o->label, window, 0, 0, 0, 0, "unexpected-disconnect", 0, state, "client",
                        "-", excerpt );
                r_event( res, o->label, window, "unexpected-disconnect", rocketnet_client_state_name( state ) );
                st.fails++;
                aborted = 1;
                break;
            }
            /* send: one message per stream per pass, round robin */
            for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES && !quit; m++ )
            {
                if ( !( o->mode_mask & ( 1u << m ) ) ) continue;
                for ( int c = 0; c < o->channels; c++ )
                {
                    if ( !stream_channels( m, c ) ) continue;
                    stream_t * s = RN_STREAM( &st, m, c );
                    int r = send_message( client, m, c, st.cycle, s->sequence, buf, cap, &st );
                    if ( r > 0 )
                    {
                        s->sent++;
                        s->sequence++;
                        RN_CELL( &st, 0, m, c, rocketnet_live_bucket( &st.cfg, m, (uint32_t) r ) )->sent++;
                    }
                    else if ( r == -1 )
                    {
                        char excerpt[96];
                        snprintf( excerpt, sizeof(excerpt), "start refused mode=%d channel=%d", m, c );
                        r_fail( res, o->label, window, 0, m, c, s->sequence, "send-start-failed", 0, 0,
                                "client", "-", excerpt );
                        st.fails++;
                    }
                }
            }
            rocketnet_client_send_packets( client );
            receive_check( &st, client );

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
            // section 4: the over-maximum send, once per window per mode
            over_maximum( &st, client, window );

            /* drain: stop sending, wait for the server's progress (bounded) */
            double drain_timeout = 10.0;
            double d0 = rocketnet_time();
            while ( !quit && rocketnet_time() - d0 < drain_timeout )
            {
                rocketnet_client_update( client, rocketnet_time() );
                rocketnet_client_send_packets( client );
                receive_check( &st, client );
                if ( rocketnet_client_state( client ) < 0 )
                {
                    char excerpt[128];
                    snprintf( excerpt, sizeof(excerpt), "state=%s during drain", rocketnet_client_state_name( rocketnet_client_state( client ) ) );
                    r_fail( res, o->label, window, 0, 0, 0, 0, "unexpected-disconnect", 0,
                            rocketnet_client_state( client ), "client", "-", excerpt );
                    r_event( res, o->label, window, "unexpected-disconnect", "drain" );
                    st.fails++;
                    aborted = 1;
                    break;
                }
                rocketnet_sleep( step );
            }
        }

        // section 7: the per-window table, one row per (dir, mode, channel, bucket)
        for ( int dir = 0; dir < 2; dir++ )
            for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
            {
                if ( !( o->mode_mask & ( 1u << m ) ) ) continue;
                for ( int c = 0; c < o->channels; c++ )
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

        // section 7: counters and RSS at exit of the window
        sample_counters( &st, client, rocketnet_time() - st.run_start );

        /* Every in-process cycle uses orderly request/completion shutdown.
           Silent server loss needs a separate process-kill experiment. */
        const int ended = !aborted ? rocketnet_client_state( client ) : ROCKETNET_CLIENT_STATE_CONNECTED;
        if ( !aborted && ended < 0 )
        {
            char excerpt[96];
            snprintf( excerpt, sizeof(excerpt), "state=%s before clean close", rocketnet_client_state_name( (enum rocketnet_client_state_t) ended ) );
            r_fail( res, o->label, window, 0, 0, 0, 0, "unexpected-disconnect-reason", 0, ended, "client", "-", excerpt );
            r_event( res, o->label, window, "unexpected-disconnect", "reason" );
            st.fails++;
        }
        rocketnet_client_disconnect( client );
        rn_cli_client_wait_disconnected( client );
        r_cycle( res, o->label, window, "clean" );
        st.clean_count++;
        rn_cli_client_stop_and_destroy( client );

        if ( !is_final )
            rocketnet_sleep( wait );
    }

    // section 7, per bench: connect and reconnect times, RTT quantiles, loss
    double reconnect_max = 0.0;
    for ( int i = 0; i < st.reconnect_count; i++ )
        if ( st.reconnect_ms[i] > reconnect_max ) reconnect_max = st.reconnect_ms[i];
    r_bench( res, o->label, st.connect_count, st.connect_ok,
             st.connect_count ? st.connect_ms_sum / st.connect_count : 0.0,
             quantile( st.reconnect_ms, st.reconnect_count, 0.50 ),
             quantile( st.reconnect_ms, st.reconnect_count, 0.99 ),
             reconnect_max,
             quantile( st.rtt, st.rtt_count, 0.50 ),
             quantile( st.rtt, st.rtt_count, 0.99 ),
             quantile( st.loss, st.loss_count, 0.50 ),
             st.clean_count + st.silent_count, st.clean_count, st.silent_count, st.denials, st.fulls );

    /* counters at process exit */
    r_counters( res, o->label, "RUN_FAILURES", st.fails, rocketnet_time() - st.run_start );
    fprintf( res, "verdict\t%s\n", st.fails ? "FAIL" : "PASS" );

    free( st.streams );
    free( st.cells );
    free( st.rtt );
    free( st.loss );
    free( st.reconnect_ms );

    if ( status )
        fprintf( status, "client\tdone\n" );
    if ( res != stdout ) fclose( res );
    if ( status ) fclose( status );
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

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
        connect_address = o.address;

    if ( o.matrix )
    {
        run_matrix( &o );
        rocketnet_term();
        return 0;
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

    uint8_t * received = (uint8_t*) malloc( (size_t) rocketnet_client_transport_max_payload_bytes( client ) );
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
                break;
        }

        if ( state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            rocketnet_client_transport_send_packet( client, payload, sizeof(payload) );
            payloads_sent++;

            int received_bytes;
            while ( ( received_bytes = rocketnet_client_transport_receive_packet( client, received, rocketnet_client_transport_max_payload_bytes( client ) ) ) > 0 )
            {
                payloads_received++;
                if ( ( payloads_received % 100 ) == 0 )
                    printf( "client sent %llu and received %llu payloads\n",
                            (unsigned long long) payloads_sent, (unsigned long long) payloads_received );
            }
        }

        rocketnet_sleep( 0.01 );
    }

    printf( "\nclient disconnecting\n" );
    rocketnet_client_transport_disconnect( client );
    rocketnet_sleep( 0.1 );
    free( received );

    rn_native_client_transport_stop_and_destroy( client );

    rocketnet_term();

    return 0;
}
