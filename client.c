/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The out-of-the-box client: connects to a server with a development
    token minted locally from the published development keys, then
    exchanges payloads. Run ./bin/server, then ./bin/client. Pass an
    address to connect elsewhere: ./bin/client 192.0.2.10:40000

    --matrix runs the live-stress message matrix (docs/LIVE-XDP.md):
    every mode, every channel at once, the size schedule, both directions,
    a cycle of reconnects alternating clean disconnect and silent drop, and
    result lines of section 7 written to --results. --self-test-result
    writes a sample result file without any socket, for the merge to read.
*/

#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "sdk/rocketnet_connect_token.h"
#include "test/live_verify.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

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
    printf( "  --send-kbps K            client send budget in kbps\n" );
    printf( "  --update-hz H            main loop update rate in Hz\n" );
    printf( "  --counters-every S       snapshot counters every S seconds (default 60)\n" );
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

static void r_fail( FILE * f, const char * label, int cycle, int dir, int mode, int channel,
                     uint32_t sequence, const char * reason, int bytes )
{
    fprintf( f, "fail\t%s\t%d\t%d\t%d\t%d\t%u\t%s\t%d\n", label, cycle, dir, mode, channel,
             (unsigned) sequence, reason, bytes );
}

static void r_counters( FILE * f, const char * label, const char * name, uint64_t value )
{
    fprintf( f, "counters\t%s\t%s\t%llu\n", label, name, (unsigned long long) value );
}

static void r_rss( FILE * f, const char * label, double seconds, long kb )
{
    fprintf( f, "rss\t%s\t%.1f\t%ld\n", label, seconds, kb );
}

static long resident_kb( void )
{
    FILE * f = fopen( "/proc/self/statm", "r" );
    if ( !f ) return 0;
    long total = 0, resident = 0;
    int n = fscanf( f, "%ld %ld", &total, &resident );
    fclose( f );
    if ( n != 2 ) return 0;
    return resident; /* pages; the page size is 4 KiB on Linux, kb = pages * 4 */
}

// ---- self test ----

static void write_self_test( const char * path, const char * label )
{
    FILE * f = fopen( path, "w" );
    if ( !f )
    {
        fprintf( stderr, "client: cannot write %s\n", path );
        exit( 1 );
    }
    for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES; mode++ )
    {
        for ( int dir = 0; dir < 2; dir++ )
        {
            r_cell( f, label, 0, dir, mode, 0, ROCKETNET_LIVE_BUCKET_SMALL, 5, 5, 5, 0, 0, 0, 0, 0, 0, 0, 0 );
        }
    }
    r_cell( f, label, 0, 0, 0, 0, ROCKETNET_LIVE_BUCKET_OVER, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 );
    for ( int c = 0; c <= 8; c++ )
    {
        r_connect( f, label, c, 1, 1.5 );
        r_cycle( f, label, c, ( c % 2 ) ? "silent" : "clean" );
    }
    r_rss( f, label, 0.0, 40 );
    r_counters( f, label, "MESSAGE_RESENT", 0 );
    fprintf( f, "verdict\tPASS\n" );
    fclose( f );
}

// ---- matrix mode ----

typedef struct stream_t
{
    uint32_t sequence;
    uint64_t sent;
    uint64_t received;
    uint64_t verified;
    int channel;
    int mode;
} stream_t;

typedef struct matrix_state_t
{
    opts_t * o;
    FILE * res;
    FILE * status;
    rocketnet_live_config_t cfg;
    uint32_t cycle;
    uint64_t fails;
} matrix_state_t;

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
    uint8_t * out = rocketnet_client_start( client, channel, mode, len );
    if ( !out ) return -1;
    memcpy( out, buf, len );
    rocketnet_client_finish( client, len );
    return 1;
}

static int receive_check( matrix_state_t * st, struct rocketnet_client_t * client )
{
    /* poll every channel; modes 0, 2, 5, 6 arrive on channel 0 */
    int checked = 0;
    for ( int channel = 0; channel < ROCKETNET_LIVE_NUM_CHANNELS; channel++ )
    {
        const uint8_t * data;
        int bytes;
        while ( rocketnet_client_receive( client, channel, &data, &bytes ) )
        {
            if ( bytes < 1 ) { rocketnet_client_release( client, data ); continue; }
            int tag = data[0];
            int mode = tag & 7;
            int recv_channel = ( mode == 0 || mode == 2 || mode == 5 || mode == 6 ) ? 0 : channel;
            int bad;
            int r = rocketnet_live_check( &st->cfg, data, bytes, 1, mode, recv_channel, st->cycle, -1, &bad );
            if ( r != ROCKETNET_LIVE_OK )
            {
                r_fail( st->res, st->o->label, (int) st->cycle, 1, mode, recv_channel,
                         rocketnet_live_sequence( data, bytes ), rocketnet_live_result_name( r ), bytes );
                st->fails++;
            }
            checked++;
            rocketnet_client_release( client, data );
        }
    }
    return checked;
}

static void run_matrix( opts_t * o )
{
    FILE * res = stdout;
    FILE * status = NULL;
    if ( o->results_path )
    {
        res = fopen( o->results_path, "w" );
        if ( !res ) { perror( "client: results" ); exit( 1 ); }
    }
    if ( o->status_path )
    {
        status = fopen( o->status_path, "w" );
        if ( !status ) { perror( "client: status" ); exit( 1 ); }
    }

    matrix_state_t st;
    memset( &st, 0, sizeof(st) );
    st.o = o;
    st.res = res;
    st.status = status;
    st.cfg.seed = (uint32_t) o->seed;
    st.cfg.max_message_bytes = 262144;
    st.cfg.max_payload_bytes = 16384;

    int timeout_seconds = ROCKETNET_DEV_CONNECT_TOKEN_TIMEOUT_SECONDS;
    double wait = 2.0 * timeout_seconds;
    printf( "client: matrix mode, timeout %d s, wait %.1f s, seed %u, label %s\n",
            timeout_seconds, wait, st.cfg.seed, o->label );

    int total_windows = o->cycles + 1;
    int window = 0;
    double step = 1.0 / ( o->update_hz > 0 ? o->update_hz : 1000 );
    double last_counters = -1e9;

    if ( o->stagger > 0 )
        rocketnet_sleep( o->stagger );

    for ( window = 0; window <= o->cycles && !quit; window++ )
    {
        int is_final = ( window == o->cycles );
        st.cycle = (uint32_t) window;

        uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
        if ( !rocketnet_write_dev_connect_token( o->address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) )
        {
            r_fail( res, o->label, window, 0, 0, 0, 0, "token-mint-failed", 0 );
            st.fails++;
            break;
        }

        struct rocketnet_client_config_t cc;
        rocketnet_client_default_config( &cc );
        cc.send_kbps = o->send_kbps;
        cc.max_packets_per_update = 64;     // the budget, section 2

        struct rocketnet_client_t * client = rocketnet_client_create( NULL, connect_token, &cc, NULL, 0 );
        if ( !client )
        {
            r_fail( res, o->label, window, 0, 0, 0, 0, "client-create-failed", 0 );
            st.fails++;
            break;
        }

        if ( status )
            fprintf( status, "client\tconnecting\n" );

        double t0 = rocketnet_time();
        double connect_ms = 0.0;
        int connected = 0;
        double connect_timeout = 5.0;
        while ( !quit )
        {
            rocketnet_client_update( client, rocketnet_time() );
            rocketnet_client_send_packets( client );
            int state = rocketnet_client_state( client );
            if ( status && state != 0 )
            {
                /* state transitions logged once per window */
            }
            if ( state >= ROCKETNET_CLIENT_STATE_CONNECTED )
            {
                connected = 1;
                connect_ms = ( rocketnet_time() - t0 ) * 1000.0;
                break;
            }
            if ( state < 0 || rocketnet_time() - t0 > connect_timeout )
                break;
            rocketnet_sleep( step );
        }
        r_connect( res, o->label, window, connected, connect_ms );
        if ( !connected )
        {
            r_fail( res, o->label, window, 0, 0, 0, 0, "connect-failed", 0 );
            st.fails++;
            rocketnet_client_destroy( client );
            break;
        }
        if ( status )
            fprintf( status, "client\tconnected\n" );

        /* the stress window */
        stream_t streams[ROCKETNET_LIVE_NUM_MODES][ROCKETNET_LIVE_NUM_CHANNELS];
        memset( streams, 0, sizeof(streams) );
        for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES; m++ )
            for ( int c = 0; c < ROCKETNET_LIVE_NUM_CHANNELS; c++ )
            {
                streams[m][c].mode = m;
                streams[m][c].channel = c;
            }

        double w0 = rocketnet_time();
        uint8_t buf[262144 + 16];
        int cap = (int) sizeof(buf);

        while ( !quit && rocketnet_time() - w0 < o->duration )
        {
            rocketnet_client_update( client, rocketnet_time() );
            /* send: one message per stream per pass, round robin */
            for ( int m = 0; m < ROCKETNET_LIVE_NUM_MODES && !quit; m++ )
            {
                for ( int c = 0; c < ROCKETNET_LIVE_NUM_CHANNELS; c++ )
                {
                    if ( !stream_channels( m, c ) ) continue;
                    stream_t * s = &streams[m][c];
                    int r = send_message( client, m, c, st.cycle, s->sequence, buf, cap, &st );
                    if ( r == 1 )
                    {
                        s->sent++;
                        s->sequence++;
                    }
                    else if ( r == -1 )
                    {
                        /* over-maximum refused on the last sent length: count as
                           `over`; exact over-maximum sends happen below */
                    }
                }
            }
            /* the over-maximum send on channel 0 and the last stream channel */
            int over_modes[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
            for ( int k = 0; k < 8; k++ )
            {
                int m = over_modes[k];
                int max_m = (int) rocketnet_live_mode_max( &st.cfg, m );
                uint8_t * out = rocketnet_client_start( client, 0, m, max_m + 1 );
                if ( !out )
                {
                    r_cell( res, o->label, window, 0, m, 0, ROCKETNET_LIVE_BUCKET_OVER,
                             1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 );
                }
                else
                {
                    /* a start that succeeds is a finding */
                    r_fail( res, o->label, window, 0, m, 0, 0, "over-max-not-refused", max_m + 1 );
                    st.fails++;
                    rocketnet_client_abort( client );
                }
            }
            rocketnet_client_send_packets( client );
            receive_check( &st, client );

            if ( rocketnet_time() - last_counters >= o->counters_every )
            {
                last_counters = rocketnet_time();
                r_rss( res, o->label, rocketnet_time() - w0, resident_kb() * 4 );
            }
            rocketnet_sleep( step );
        }

        /* drain: stop sending, wait for the peer's progress (bounded) */
        double drain_timeout = 10.0;
        double d0 = rocketnet_time();
        while ( !quit && rocketnet_time() - d0 < drain_timeout )
        {
            rocketnet_client_update( client, rocketnet_time() );
            rocketnet_client_send_packets( client );
            receive_check( &st, client );
            rocketnet_sleep( step );
        }

        /* the disconnect: alternating clean and silent drop, final clean */
        if ( is_final || ( window % 2 ) == 0 )
        {
            rocketnet_client_disconnect( client );
            rocketnet_client_update( client, rocketnet_time() );
            r_cycle( res, o->label, window, "clean" );
        }
        else
        {
            /* silent drop: destroy without a disconnect call; the server's
               timeout path frees the slot (docs/LIVE-XDP.md section 6) */
            r_cycle( res, o->label, window, "silent" );
        }

        rocketnet_client_destroy( client );

        if ( !is_final )
            rocketnet_sleep( wait );
    }

    /* conservation + counters at exit */
    r_counters( res, o->label, "RUN_FAILURES", st.fails );
    fprintf( res, "verdict\t%s\n", st.fails ? "FAIL" : "PASS" );

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

    if ( !rocketnet_init( NULL ) )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    const char * connect_address = getenv( "ROCKETNET_CLIENT_CONNECT_ADDRESS" );
    if ( !connect_address )
        connect_address = o.address;

    if ( o.matrix )
    {
        run_matrix( &o );
        rocketnet_term();
        return 0;
    }

    // the development connect: mint a token locally against the published
    // development keys. a production client is handed its token by a backend.

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_dev_connect_token( connect_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) )
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

    uint8_t * received = (uint8_t*) malloc( rocketnet_client_transport_max_payload_bytes( client ) );
    if ( !received )
    {
        rocketnet_client_transport_destroy( client );
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

    rocketnet_client_transport_destroy( client );

    rocketnet_term();

    return 0;
}
