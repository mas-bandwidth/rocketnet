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
    using test/live_verify.h (sections 3 to 5). It runs at the max_clients
    the run sets, dumps counters and resident memory each minute, exits when
    its controlling ssh or stdin closes, and refuses to start when the XDP
    driver is pinned and the interface reports the in-tree driver (section 9).
*/

#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_platform.h"
#include "rocketnet_counters.h"
#include "test/live_verify.h"

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>

#define ROCKETNET_SERVER_LABEL_MAX           64
#define ROCKETNET_SERVER_DIRECTIONS           2
#define ROCKETNET_SERVER_TINY                 ROCKETNET_LIVE_HEADER_BYTES
#define ROCKETNET_SERVER_MAX_SEQUENCES       256
#define ROCKETNET_SERVER_OVER_MAX_DELTA      1
#define ROCKETNET_SERVER_HELLO_NIBBLE        0xD0u

static volatile int quit;

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
        "  --max-packets-per-update N  the message config's max_packets_per_update\n"
        "  --update-hz H           update rate of the main loop\n"
        "  --counters-every S      snapshot counters, RSS and transport stats every S seconds\n"
        "  --results PATH          where the result lines go\n"
        "  --status PATH           one line per state change, for the orchestrator\n"
        "  --transport T           pin the transport (e.g. xdp); the in-tree driver refuses\n"
        "  --help                  this message\n"
        "environment:\n"
        "  ROCKETNET_SERVER_TRANSPORT   the transport candidate list\n"
        "  ROCKETNET_SERVER_PRINT_COUNTERS=1  every server counter at stop\n" );
}

static void set_status( const char * path, const char * line )
{
    if ( !path )
        return;
    FILE * f = fopen( path, "w" );
    if ( f )
    {
        fprintf( f, "%s\n", line );
        fclose( f );
    }
}

static uint64_t resident_bytes( void )
{
    FILE * f = fopen( "/proc/self/statm", "r" );
    if ( !f )
        return 0;
    long total = 0, resident = 0;
    if ( fscanf( f, "%ld %ld", &total, &resident ) != 2 )
        resident = 0;
    fclose( f );
    const long page = sysconf( _SC_PAGESIZE );
    return (uint64_t) resident * (uint64_t) ( page > 0 ? page : 4096 );
}

// the card: the server exits when its controlling ssh or stdin closes
static void check_stdin_closed( void )
{
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
}

static void dump_counters( const struct rocketnet_counters_t * counters )
{
    const int n = rocketnet_num_counters();
    for ( int i = 0; i < n; i++ )
        printf( "rocketnet counter %s %llu\n", rocketnet_counter_name( i ), (unsigned long long) rocketnet_counter_value( counters, i ) );
    printf( "rocketnet rss %llu\n", (unsigned long long) resident_bytes() );
    fflush( stdout );
}

// the interface that carries an IPv4 address, via `ip -o -4 addr show`
static int interface_for_address( const char * address, char * out, size_t out_size )
{
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
}

// Section 9: the in-tree i40e reports the running kernel's version (the same
// string as `uname -r`); the out-of-tree build reports its own release number.
static int in_tree_driver( const char * nic )
{
    char cmd[256];
    snprintf( cmd, sizeof cmd, "ethtool -i %s 2>/dev/null | awk '/^driver:/{d=$2} /^version:/{v=$2} END{print d, v}'", nic );
    FILE * f = popen( cmd, "r" );
    if ( !f )
        return 1;
    char driver[64] = {0}, version[64] = {0};
    if ( fscanf( f, "%63s %63s", driver, version ) != 2 )
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

static uint64_t server_counter( const struct rocketnet_counters_t * counters, int index )
{
    return counters ? counters->values[index] : 0;
}

static void matrix_fail( struct server_client_t * c, int mode, int channel, int64_t sequence, const char * reason, int bytes )
{
    if ( c->failures < 64 )
        printf( "fail %s %u 1 %d %d %lld %s %d\n", c->label, c->cycle, mode, channel, (long long) sequence, reason, bytes );
    c->failures++;
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
    (void) server; (void) client_index;
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
    const int result = rocketnet_live_check( &( (struct rocketnet_live_config_t) { .seed = 0, .max_message_bytes = 262144, .max_payload_bytes = 16384 } ), data, bytes, 0, mode, channel, c->cycle, expected, &offset );
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
    s->seen[q / 64] |= 1ull << ( q % 64 );
    s->last = q;
    s->received++;
    if ( matrix_ordered( mode ) )
        s->expected++;
    c->verified++;
}

static void matrix_receive( struct rocketnet_message_server_t * server, struct server_client_t * c, int client_index )
{
    for ( int channel = 0; channel < ROCKETNET_LIVE_NUM_CHANNELS; channel++ )
    {
        const uint8_t * data = NULL;
        int bytes = 0;
        while ( rocketnet_message_server_receive( server, client_index, channel, &data, &bytes ) )
        {
            matrix_arrival( server, c, client_index, channel, data, bytes );
            rocketnet_message_server_release( server, client_index, data );
        }
    }
}

static uint32_t matrix_send_one( struct rocketnet_message_server_t * server, int client_index, int mode, int channel,
                                 uint32_t cycle, uint32_t sequence, uint32_t length )
{
    uint8_t * p = rocketnet_message_server_start( server, client_index, channel, mode, (int) length );
    if ( !p )
        return 0;
    const struct rocketnet_live_config_t live = { .seed = 0, .max_message_bytes = 262144, .max_payload_bytes = 16384 };
    const int written = rocketnet_live_generate( &live, 1, mode, channel, cycle, sequence, p, (int) length );
    if ( written != (int) length )
    {
        rocketnet_message_server_abort( server, client_index );
        return 0;
    }
    return rocketnet_message_server_finish( server, client_index, written );
}

static void matrix_client_window( struct rocketnet_message_server_t * server, int client_index, struct server_client_t * c )
{
    client_streams_reset( &(struct rocketnet_live_config_t) { .seed = 0, .max_message_bytes = 262144, .max_payload_bytes = 16384 }, c );
    matrix_over_maximum( server, c, client_index, &(struct rocketnet_live_config_t) { .seed = 0, .max_message_bytes = 262144, .max_payload_bytes = 16384 } );

    int all_done = 0;
    while ( !all_done && !quit )
    {
        rocketnet_message_server_update( server, rocketnet_time() );
        matrix_receive( server, c, client_index );
        all_done = 1;
        for ( int mode = 0; mode < ROCKETNET_LIVE_NUM_MODES && all_done; mode++ )
        {
            for ( int channel = 0; channel < matrix_channels( mode ); channel++ )
            {
                struct server_stream_t * s = &c->streams[mode][channel];
                while ( s->next < s->total && s->next < ROCKETNET_SERVER_MAX_SEQUENCES )
                {
                    const uint32_t length = rocketnet_live_length( &(struct rocketnet_live_config_t) { .seed = 0, .max_message_bytes = 262144, .max_payload_bytes = 16384 }, mode, channel, s->next );
                    const uint32_t serial = matrix_send_one( server, client_index, mode, channel, c->cycle, s->next, length );
                    if ( serial == 0 )
                        break;
                    s->next++;
                    s->sent++;
                    if ( length < ROCKETNET_SERVER_TINY )
                        c->tiny_sent[mode][length]++;
                }
                if ( s->next < s->total )
                    all_done = 0;
            }
        }
        rocketnet_message_server_send_packets( server );
        rocketnet_platform_sleep( 0.0005 );
    }
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    int matrix = 0;
    const char * address = NULL;
    const char * label = "server";
    const char * transport = NULL;
    const char * results_path = NULL;
    const char * status_path = NULL;
    int max_clients = 1000;
    int max_packets_per_update = 64;
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
        else if ( strcmp( a, "--max-packets-per-update" ) == 0 && i + 1 < argc ) max_packets_per_update = (int) strtol( argv[++i], NULL, 10 );
        else if ( strcmp( a, "--update-hz" ) == 0 && i + 1 < argc ) update_hz = strtod( argv[++i], NULL );
        else if ( strcmp( a, "--counters-every" ) == 0 && i + 1 < argc ) counters_every = strtod( argv[++i], NULL );
        else if ( strcmp( a, "--results" ) == 0 && i + 1 < argc ) results_path = argv[++i];
        else if ( strcmp( a, "--status" ) == 0 && i + 1 < argc ) status_path = argv[++i];
        else if ( strcmp( a, "--transport" ) == 0 && i + 1 < argc ) transport = argv[++i];
        else if ( a[0] != '-' && !address ) address = a;
        else
        {
            fprintf( stderr, "server: unknown argument: %s\n", a );
            usage();
            return 1;
        }
    }

    if ( !address )
        address = "127.0.0.1:40000";

    if ( !rocketnet_init( NULL ) )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    struct rocketnet_server_transport_config_t config;
    rocketnet_server_transport_dev_config( &config );
    config.max_clients = max_clients;

    const char * env_transport = getenv( "ROCKETNET_SERVER_TRANSPORT" );
    if ( transport )
        snprintf( config.driver, sizeof config.driver, "%s", transport );
    else if ( env_transport )
        snprintf( config.driver, sizeof config.driver, "%s", env_transport );

    // section 9: when xdp is pinned, refuse the in-tree driver
    if ( strcmp( config.driver, "xdp" ) == 0 )
    {
        char nic[64] = {0};
        if ( interface_for_address( address, nic, sizeof nic ) && in_tree_driver( nic ) )
        {
            fprintf( stderr, "server: refusing: interface %s reports the in-tree driver\n", nic );
            rocketnet_term();
            return 1;
        }
    }

    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );
    message_config.max_packets_per_update = max_packets_per_update;

    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, address, &config, &message_config, NULL, 0, send_kbps );
    if ( !server )
    {
        rocketnet_term();
        return 1;
    }

    struct rocketnet_server_transport_t * transport_obj = rocketnet_message_server_transport( server );
    printf( "server transport %s\n", rocketnet_server_transport_driver( transport_obj ) );
    printf( "server max_clients %d\n", config.max_clients );
    set_status( status_path, "listening" );

    if ( !matrix )
    {
        // the payload echo, exactly as before
        struct rocketnet_frame_sync_t * frame_sync = rocketnet_frame_sync_create( NULL, 60.0 );
        if ( !frame_sync )
        {
            rocketnet_message_server_destroy( server );
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
            rocketnet_message_server_destroy( server );
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
            for ( int i = 0; i < max_clients; i++ )
            {
                if ( !rocketnet_message_server_client_connected( server, i ) )
                    continue;
                matrix_receive( server, &clients[i], i );
                if ( !clients[i].window_done )
                {
                    matrix_client_window( server, i, &clients[i] );
                    clients[i].window_done = 1;
                }
            }
            const double now = rocketnet_time();
            if ( counters_every > 0.0 && now - last_counters >= counters_every )
            {
                dump_counters( rocketnet_message_server_counters( server ) );
                last_counters = now;
            }
            check_stdin_closed();
            rocketnet_platform_sleep( 0.0005 );
        }
        free( clients );
    }

    printf( "\nserver stopping\n" );
    set_status( status_path, "stopping" );
    rocketnet_message_server_destroy( server );

    const char * print_counters = getenv( "ROCKETNET_SERVER_PRINT_COUNTERS" );
    (void) print_counters;
    rocketnet_term();

    set_status( status_path, "stopped" );
    return 0;
}
