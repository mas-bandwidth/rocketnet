/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Soak: a client and server on loopback exchanging pattern-verified
    payloads of every size, reconnecting on a cycle, forever -- duration
    converts probability into certainty, and this is what finds the
    difficult problems. Run ./bin/soak and leave it; pass --seconds S or a
    number of iterations to bound it: ./bin/soak --seconds 60 or ./bin/soak 10000

    usage: soak [--seconds S] [--assert-flat] [positive iteration count]

    --seconds S ends the run after S seconds of wall clock (S a positive
    integer); beside an iteration count the run ends at whichever bound comes
    first. A zero, negative, non-numeric or missing S, an unknown flag and a
    second positional argument are each refused with a one-line reason on
    stderr and exit 2, before any run starts.

    At the end of every run, an interrupted (SIGINT) one included, soak prints
    its "soak finished:" line and then one "soak summary: <name> <value>" line
    each for: sent, client_sent, server_sent; received, client_received,
    server_received; verified, client_verified, server_verified; reconnects
    (the planned reconnect cycles); one line per disconnect reason seen, named
    by its ROCKETNET_CLIENT_STATE_* value from rocketnet.h; and
    client_<COUNTER> and server_<COUNTER> for every non-zero client and server
    transport counter, named by rocketnet_counter_name.

    soak exits 1 on any payload verify failure, any counter conservation
    failure and any disconnect that is not its own planned reconnect, naming
    which. Every REFUSAL counter must read 0, with one exception: when the
    link simulator duplicates packets (ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT
    or ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT nonzero), the duplicates
    it plants are refused as already received, so every <FAMILY>_ALREADY_RECEIVED
    refusal (payload, keep-alive, ack, fragment and disconnect, on the client
    and the server) is counted and printed, not failed.
    ROCKETNET_SOAK_FAULT_VERIFY=1 (off by default) corrupts the first
    received payload: the planted verify failure scripts/soak-args-selftest
    uses as its negative control.

    THE WARM CYCLE AND THE RESOURCE COUNTS (tests/process_resources.h). The
    warm cycle is the first planned reconnect cycle: 30 s of the first client,
    long enough for its sequence numbers to wrap every history and window the
    connection indexes by sequence, then its disconnect and destroy. Every
    sample of the comparison is taken between clients, with the last client
    destroyed and the server still running, so each sees the same objects
    alive: the post-warmup sample at the first planned reconnect, a peak
    sample at every later reconnect and every 10 s, and the end sample after
    the bound, once the last client is destroyed and before the server is.
    Each count prints as "soak resources: <name> start=<n> end=<n> peak=<n>"
    (fds, threads, resident_bytes; handles, threads, private_bytes on
    Windows); a run that ends inside the warm cycle, as every run under 30 s
    does, has no post-warmup sample and prints
    "soak resources: none (the run ended inside the warm cycle)".

    --assert-flat holds the counts to process_resources.h's flat rule: the
    fd (handle) and thread counts at the end equal their post-warmup counts,
    and the memory count grows by at most one page per ten minutes of run
    since the post-warmup sample (at least one page). A count that is not
    flat prints "soak: --assert-flat FAILED: <name> start=<n> end=<n>" and
    the run exits 1; so does a run that ends inside the warm cycle. A second
    --assert-flat, or one given a value (--assert-flat=...), is refused with
    exit 2 like the other malformed arguments.

    The granted transports print as "soak transport: server <name>" and
    "soak transport: client <name>" (rocketnet_server_transport_io_name and
    rocketnet_client_transport_io_name), and again as the summary lines
    server_transport_io and client_transport_io, so a pin
    (ROCKETNET_SERVER_TRANSPORT, ROCKETNET_CLIENT_TRANSPORT) is proven by the
    run rather than assumed. A client granted a transport other than the
    first client's is a failure.

    SOAK_SERVER_ADDRESS=<address>:<port> binds the server there instead of
    127.0.0.1 on a kernel-granted port (xdp discovers no port, so its cell
    names one), and on Linux SOAK_CLIENT_NETNS=/var/run/netns/<name> opens
    every client in that namespace, as load.c's LOAD_SERVER_ADDRESS and
    LOAD_CLIENT_NETNS do: the xdp cell of scripts/soak-per-transport.
*/

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE     /* setns, for SOAK_CLIENT_NETNS */
#endif

#include "tests/native_shutdown.h"
#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_connect_token.h"
#include "tests/portable_crt.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h>
#include "tests/process_resources.h"
#if defined(__linux__)
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#endif

// Internal transport tools mint the binary development token directly.
static int dev_token_binary( const char * address, uint64_t client_id, uint64_t timestamp, uint8_t * token, size_t token_bytes )
{
    return rocketnet_write_test_connect_token( address, client_id, timestamp, token, token_bytes ) == ROCKETNET_STATUS_OK;
}

#define ROCKETNET_SOAK_MAX_COUNTERS 256
#define ROCKETNET_SOAK_DISCONNECT_REASONS ( ROCKETNET_CLIENT_STATE_DISCONNECTED - ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED + 1 )

static volatile sig_atomic_t quit;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

static void generate_payload( uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
        payload_data[i] = (uint8_t) ( start + i );
}

static int verify_payload( const uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
    {
        if ( payload_data[i] != (uint8_t) ( start + i ) )
            return 0;
    }
    return 1;
}

static int parse_positive_uint64( const char * arg, uint64_t * out )
{
    if ( !arg || arg[0] < '0' || arg[0] > '9' )
        return 0;
    char * end = NULL;
    errno = 0;
    unsigned long long value = strtoull( arg, &end, 10 );
    if ( !end || *end != '\0' || errno != 0 || value == 0 )
        return 0;
    *out = (uint64_t) value;
    return 1;
}

static const char * soak_disconnect_reason_name( int state )
{
    switch ( state )
    {
        case ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED:          return "ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED";
        case ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN:          return "ROCKETNET_CLIENT_STATE_INVALID_CONNECT_TOKEN";
        case ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT:           return "ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT:  return "ROCKETNET_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT:   return "ROCKETNET_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT:       return "ROCKETNET_CLIENT_STATE_COOKIE_REQUEST_TIMED_OUT";
        case ROCKETNET_CLIENT_STATE_CONNECTION_DENIED:              return "ROCKETNET_CLIENT_STATE_CONNECTION_DENIED";
        case ROCKETNET_CLIENT_STATE_DISCONNECTED:                   return "ROCKETNET_CLIENT_STATE_DISCONNECTED";
        default:                                                    return "ROCKETNET_CLIENT_STATE_UNKNOWN";
    }
}

static void soak_record_disconnect( uint64_t * disconnect_counts, int state )
{
    if ( state >= ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED && state <= ROCKETNET_CLIENT_STATE_DISCONNECTED )
        disconnect_counts[state - ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED]++;
}

static void soak_accumulate_counters( uint64_t * totals, const struct rocketnet_counters_t * counters )
{
    if ( !counters )
        return;
    const int n = rocketnet_num_counters();
    for ( int i = 0; i < n && i < ROCKETNET_SOAK_MAX_COUNTERS; i++ )
        totals[i] += rocketnet_counter_value( counters, i );
}

// Whether the link simulator duplicates packets on either end, read from the
// same env overlay the transports apply at create.

static int soak_duplicates_planted( void )
{
    const char * names[] = { "ROCKETNET_SERVER_SIMULATED_DUPLICATE_PERCENT", "ROCKETNET_CLIENT_SIMULATED_DUPLICATE_PERCENT" };
    for ( int i = 0; i < 2; i++ )
    {
        char value[256];
        if ( rocketnet_test_getenv( names[i], value, sizeof( value ) ) && value[0] != '\0' && strtol( value, NULL, 10 ) != 0 )
            return 1;
    }
    return 0;
}

// a planted duplicate of any sealed packet (payload, keep-alive, ack,
// fragment, disconnect) is refused as already received, counted by the
// family's <FAMILY>_ALREADY_RECEIVED refusal; only those refusals are
// expected, and only when the simulator plants duplicates

static int soak_refusal_planted_by_duplicates( const char * counter_name )
{
    const char suffix[] = "_ALREADY_RECEIVED";
    const size_t length = strlen( counter_name );
    const size_t suffix_length = sizeof(suffix) - 1;
    return length > suffix_length && strcmp( counter_name + length - suffix_length, suffix ) == 0;
}

// The counter discipline, computed from the counter list. Conservation: every
// family balances, RECEIVED == the sum of its outcomes. Refusals: this is
// loopback with no injected fault, so every REFUSAL counter reads 0, except
// the already-received refusal of the duplicates the link simulator plants.
// Returns the number of violations, each printed by counter name.

static int soak_check_counters( const char * name, const struct rocketnet_counters_t * counters )
{
    const int duplicates_planted = soak_duplicates_planted();
    int violations = 0;

    char report[256];
    if ( !rocketnet_counters_conserved( counters, report, sizeof(report) ) )
    {
        printf( "soak: CONSERVATION FAILED at %s: %s\n", name, report );
        violations++;
    }

    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        if ( rocketnet_counter_class( i ) == ROCKETNET_COUNTER_CLASS_REFUSAL && rocketnet_counter_value( counters, i ) != 0 )
        {
            if ( duplicates_planted && soak_refusal_planted_by_duplicates( rocketnet_counter_name( i ) ) )
            {
                printf( "soak: planted duplicates refused at %s: %s = %llu\n", name, rocketnet_counter_name( i ),
                        (unsigned long long) rocketnet_counter_value( counters, i ) );
                continue;
            }
            printf( "soak: REFUSAL on a clean run at %s: %s = %llu\n", name, rocketnet_counter_name( i ),
                    (unsigned long long) rocketnet_counter_value( counters, i ) );
            violations++;
        }
    }

    return violations;
}

int main( int argc, char ** argv )
{
    uint64_t max_iterations = 0;
    uint64_t max_seconds = 0;
    int have_iterations = 0;
    int have_seconds = 0;
    int assert_flat = 0;

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--seconds" ) == 0 )
        {
            if ( have_seconds )
            {
                fprintf( stderr, "soak: duplicate --seconds flag (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n" );
                return 2;
            }
            if ( i + 1 >= argc )
            {
                fprintf( stderr, "soak: missing argument for --seconds (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n" );
                return 2;
            }
            i++;
            if ( !parse_positive_uint64( argv[i], &max_seconds ) )
            {
                fprintf( stderr, "soak: --seconds S must be a positive integer (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n" );
                return 2;
            }
            have_seconds = 1;
        }
        else if ( strcmp( argv[i], "--assert-flat" ) == 0 )
        {
            if ( assert_flat )
            {
                fprintf( stderr, "soak: duplicate --assert-flat flag (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n" );
                return 2;
            }
            assert_flat = 1;
        }
        else if ( strncmp( argv[i], "--assert-flat=", 14 ) == 0 )
        {
            fprintf( stderr, "soak: --assert-flat takes no value (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n" );
            return 2;
        }
        else if ( argv[i][0] == '-' && ( argv[i][1] < '0' || argv[i][1] > '9' ) )
        {
            fprintf( stderr, "soak: unknown flag '%s' (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n", argv[i] );
            return 2;
        }
        else
        {
            if ( have_iterations )
            {
                fprintf( stderr, "soak: unexpected second positional argument '%s' (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n", argv[i] );
                return 2;
            }
            if ( !parse_positive_uint64( argv[i], &max_iterations ) )
            {
                fprintf( stderr, "soak: iteration count must be a positive integer (usage: soak [--seconds S] [--assert-flat] [positive iteration count])\n" );
                return 2;
            }
            have_iterations = 1;
        }
    }

    char fault_verify_env[256];
    int fault_verify_pending = ( rocketnet_test_getenv( "ROCKETNET_SOAK_FAULT_VERIFY", fault_verify_env, sizeof( fault_verify_env ) ) && fault_verify_env[0] != '\0' && strcmp( fault_verify_env, "0" ) != 0 ) ? 1 : 0;

    signal( SIGINT, interrupt_handler );

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "error: failed to initialize rocketnet\n" );
        return 1;
    }

    // ROCKETNET_LOG_LEVEL, when set, keeps the level the library read from it
    // (an xdp cell's pre-flight names each unmet condition at warn)
    char log_level_env[256];
    if ( !rocketnet_test_getenv( "ROCKETNET_LOG_LEVEL", log_level_env, sizeof( log_level_env ) ) || log_level_env[0] == '\0' )
        rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 16;

    // Bind port zero and adopt what the kernel grants, the #107 shape. A soak
    // is the worst place for a fixed port: it is the one binary the estate
    // runs for DAYS, so it is the one most likely to still hold the port when
    // the next run starts, and CI runs it nightly beside whatever else is on
    // the box. A collision does not look like a collision either -- create
    // returns NULL and the run exits 1, which reads as a soak failure rather
    // than as a busy port.

    char server_bind[64];
    if ( !rocketnet_test_getenv( "SOAK_SERVER_ADDRESS", server_bind, sizeof( server_bind ) ) || server_bind[0] == '\0' )
        snprintf( server_bind, sizeof( server_bind ), "%s", "127.0.0.1" );

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, server_bind, &server_config);
    if ( !server )
    {
        printf( "soak: failed to create the server transport on %s\n", server_bind );
        rocketnet_term();
        return 1;
    }

    // the token names the host bound and the port granted, or the port the
    // address named when the transport reports none (xdp discovers no port)

    char server_host[64];
    snprintf( server_host, sizeof( server_host ), "%s", server_bind );
    char * server_port_text = strchr( server_host, ':' );
    int server_port = rocketnet_server_transport_port( server );
    if ( server_port_text )
    {
        *server_port_text = '\0';
        if ( server_port <= 0 )
            server_port = (int) strtol( server_port_text + 1, NULL, 10 );
    }
    char server_address[80];
    snprintf( server_address, sizeof(server_address), "%s:%d", server_host, server_port );

    char server_transport_name[64];
    snprintf( server_transport_name, sizeof( server_transport_name ), "%s", rocketnet_server_transport_io_name( server ) );
    printf( "soak transport: server %s\n", server_transport_name );
    char client_transport_name[64] = { 0 };

#if defined(__linux__)
    int self_netns = -1;
    int client_netns = -1;
    char client_netns_path[256];
    if ( rocketnet_test_getenv( "SOAK_CLIENT_NETNS", client_netns_path, sizeof( client_netns_path ) ) && client_netns_path[0] != '\0' )
    {
        self_netns = open( "/proc/self/ns/net", O_RDONLY );
        client_netns = open( client_netns_path, O_RDONLY );
        if ( self_netns < 0 || client_netns < 0 )
        {
            printf( "soak: cannot open the namespaces /proc/self/ns/net and %s: %s\n", client_netns_path, strerror( errno ) );
            rn_native_server_transport_stop_and_destroy( server );
            rocketnet_term();
            return 1;
        }
    }
#endif

    struct rocketnet_client_transport_t * client = NULL;

    uint64_t iteration = 0;
    uint64_t connects = 0;
    uint64_t reconnects = 0;
    uint64_t client_sent = 0;
    uint64_t server_sent = 0;
    uint64_t client_received = 0;
    uint64_t server_received = 0;
    uint64_t payloads_verified = 0;
    uint64_t server_verified = 0;
    uint64_t client_verified = 0;
    uint64_t failures = 0;
    uint64_t disconnect_counts[ROCKETNET_SOAK_DISCONNECT_REASONS] = { 0 };
    uint64_t client_counters[ROCKETNET_SOAK_MAX_COUNTERS] = { 0 };
    int counter_violations = 0;
    int connected = 0;
    int unplanned_disconnect_recorded = 0;
    int deadline_reached = 0;
    int payload_size = 1;
    int resources_started = 0;
    int resources_ended = 0;
    double resources_start_time = 0.0;
    struct process_resources_t resources_start;
    struct process_resources_t resources_end;
    struct process_resources_t resources_peak;
    memset( &resources_start, 0, sizeof( resources_start ) );
    memset( &resources_end, 0, sizeof( resources_end ) );
    memset( &resources_peak, 0, sizeof( resources_peak ) );
    const double start_time = rocketnet_time();
    double last_report_time = start_time;
    double reconnect_time = start_time;

    while ( !quit && ( !max_iterations || iteration < max_iterations ) )
    {
        if ( max_seconds && rocketnet_time() - start_time >= (double) max_seconds )
        {
            deadline_reached = 1;
            break;
        }

        iteration++;

        // reconnect on a cycle: the connect path is never a rare path

        if ( !client || rocketnet_time() - reconnect_time > 30.0 )
        {
            if ( client )
            {
                reconnects++;
                rocketnet_client_transport_disconnect( client );
                soak_record_disconnect( disconnect_counts, rocketnet_client_transport_state( client ) );
                rocketnet_sleep( 0.05 );
                rocketnet_server_transport_update( server, rocketnet_time() );
                counter_violations += soak_check_counters( "client", rocketnet_client_transport_counters( client ) );
                soak_accumulate_counters( client_counters, rocketnet_client_transport_counters( client ) );
                rn_native_client_transport_stop_and_destroy( client );
                client = NULL;

                // between clients: the post-warmup sample at the first
                // planned reconnect, a peak sample at every later one

                struct process_resources_t sample;
                if ( !process_resources_read( &sample ) )
                {
                    printf( "soak: failed to read the process resource counts\n" );
                    failures++;
                    break;
                }
                if ( !resources_started )
                {
                    resources_started = 1;
                    resources_start = sample;
                    resources_peak = sample;
                    resources_start_time = rocketnet_time();
                }
                process_resources_peak( &resources_peak, &sample );
            }
            uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
            if ( !dev_token_binary( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token, sizeof( connect_token ) ) )
            {
                printf( "soak: failed to write dev connect token\n" );
                failures++;
                break;
            }
#if defined(__linux__)
            if ( client_netns >= 0 && setns( client_netns, CLONE_NEWNET ) != 0 )
            {
                printf( "soak: setns into %s failed: %s\n", client_netns_path, strerror( errno ) );
                failures++;
                break;
            }
#endif
            client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_time() );
#if defined(__linux__)
            if ( client_netns >= 0 && setns( self_netns, CLONE_NEWNET ) != 0 )
            {
                printf( "soak: setns back to the server namespace failed: %s\n", strerror( errno ) );
                failures++;
                break;
            }
#endif
            if ( !client )
            {
                printf( "soak: failed to create the client transport\n" );
                failures++;
                break;
            }
            const char * granted = rocketnet_client_transport_io_name( client );
            if ( client_transport_name[0] == '\0' )
            {
                snprintf( client_transport_name, sizeof( client_transport_name ), "%s", granted ? granted : "unknown" );
                printf( "soak transport: client %s\n", client_transport_name );
            }
            else if ( !granted || strcmp( granted, client_transport_name ) != 0 )
            {
                printf( "soak: client transport changed from %s to %s on a reconnect\n", client_transport_name, granted ? granted : "unknown" );
                failures++;
            }

            if ( rocketnet_client_transport_max_payload_bytes( client ) != ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES )
            {
                printf( "soak: unexpected client payload capacity\n" );
                failures++;
                break;
            }
            reconnect_time = rocketnet_time();
            connected = 0;
        }

        rocketnet_client_transport_update( client, rocketnet_time() );
        rocketnet_server_transport_update( server, rocketnet_time() );

        const int client_state = rocketnet_client_transport_state( client );
        if ( client_state < 0 || ( connected && client_state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED ) )
        {
            soak_record_disconnect( disconnect_counts, client_state );
            unplanned_disconnect_recorded = 1;
            printf( "soak: client connection failed (%s, %s, %d)\n",
                    soak_disconnect_reason_name( client_state ),
                    rocketnet_client_state_name( client_state ),
                    client_state );
            failures++;
            break;
        }

        if ( client_state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            if ( !connected )
            {
                connected = 1;
                connects++;
            }
            // every size cycles through, both directions, every byte checked

            uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
            generate_payload( payload, payload_size );
            rocketnet_client_transport_send_packet( client, payload, payload_size );
            client_sent++;
            rocketnet_server_transport_send_packet( server, rocketnet_client_transport_client_index( client ), payload, payload_size );
            rocketnet_server_transport_send_packets( server );
            server_sent++;
            payload_size = payload_size < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? payload_size + 1 : 1;

            const int num_payloads = rocketnet_server_transport_num_payloads( server );
            for ( int i = 0; i < num_payloads; i++ )
            {
                int payload_client_index, payload_bytes;
                uint8_t * server_payload = rocketnet_server_transport_payload( server, i, &payload_client_index, &payload_bytes );
                server_received++;
                if ( fault_verify_pending && payload_bytes > 0 )
                {
                    server_payload[0] ^= 0xff;
                    fault_verify_pending = 0;
                }
                if ( verify_payload( server_payload, payload_bytes ) )
                    { payloads_verified++; server_verified++; }
                else
                    { printf( "PAYLOAD VERIFY FAILED at server (%d bytes)\n", payload_bytes ); failures++; }
            }

            // sized by the default configured maximum -- the clients here
            // run the default config, asserted right after create; a custom
            // config would size this by
            // rocketnet_client_transport_max_payload_bytes instead

            static uint8_t received[ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES];
            int received_bytes;
            while ( ( received_bytes = rocketnet_client_transport_receive_packet( client, received, sizeof(received) ) ) > 0 )
            {
                client_received++;
                if ( fault_verify_pending )
                {
                    received[0] ^= 0xff;
                    fault_verify_pending = 0;
                }
                if ( verify_payload( received, received_bytes ) )
                    { payloads_verified++; client_verified++; }
                else
                    { printf( "PAYLOAD VERIFY FAILED at client (%d bytes)\n", received_bytes ); failures++; }
            }
        }

        if ( rocketnet_time() - last_report_time > 10.0 )
        {
            last_report_time = rocketnet_time();
            if ( resources_started )
            {
                struct process_resources_t sample;
                if ( process_resources_read( &sample ) )
                    process_resources_peak( &resources_peak, &sample );
            }
            printf( "soak: %llu iterations, %llu connects, %llu payloads verified, %llu failures\n",
                    (unsigned long long) iteration, (unsigned long long) connects,
                    (unsigned long long) payloads_verified, (unsigned long long) failures );
        }

        rocketnet_sleep( 0.001 );
    }

    // A bound can fall during a handshake: the first connect, or a planned
    // reconnect started in the last iterations. Finish it before the
    // teardown, with bounded updates: the server refuses a disconnect from a
    // client it has not yet connected (DISCONNECT_NOT_CONNECTED, its
    // slot-state check), and a clean run must never be refused.

    if ( client && !unplanned_disconnect_recorded && !connected )
    {
        const double handshake_deadline = rocketnet_time() + 5.0;
        int client_state = rocketnet_client_transport_state( client );
        while ( client_state >= 0 && client_state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && rocketnet_time() < handshake_deadline )
        {
            rocketnet_client_transport_update( client, rocketnet_time() );
            rocketnet_server_transport_update( server, rocketnet_time() );
            client_state = rocketnet_client_transport_state( client );
            rocketnet_sleep( 0.001 );
        }
        if ( client_state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            connected = 1;
            connects++;
        }
        else
        {
            soak_record_disconnect( disconnect_counts, client_state );
            unplanned_disconnect_recorded = 1;
            printf( "soak: the handshake in progress at the bound did not complete (%s, %d)\n",
                    rocketnet_client_state_name( client_state ), client_state );
            failures++;
        }
    }

    if ( !connects || !server_verified || !client_verified )
    {
        printf( "soak: insufficient coverage: require a connection and verified payloads in both directions\n" );
        failures++;
    }
    if ( !quit && max_iterations && iteration < max_iterations && !deadline_reached )
    {
        printf( "soak: bounded run ended before its iteration target\n" );
        failures++;
    }
    if ( !quit && max_seconds && !deadline_reached && ( !max_iterations || iteration < max_iterations ) )
    {
        printf( "soak: bounded run ended before its duration target\n" );
        failures++;
    }

    // the discipline at the end of the run, on both ends. the server has
    // processed everything it was handed and the client is idle, so every
    // arrival has taken its outcome

    if ( client )
    {
        if ( !unplanned_disconnect_recorded )
        {
            rocketnet_client_transport_disconnect( client );
            soak_record_disconnect( disconnect_counts, rocketnet_client_transport_state( client ) );
        }
        rocketnet_sleep( 0.05 );
        rocketnet_server_transport_update( server, rocketnet_time() );
        counter_violations += soak_check_counters( "client", rocketnet_client_transport_counters( client ) );
        soak_accumulate_counters( client_counters, rocketnet_client_transport_counters( client ) );
        rn_native_client_transport_stop_and_destroy( client );
        client = NULL;
    }
    counter_violations += soak_check_counters( "server", rocketnet_server_transport_counters( server ) );
    if ( counter_violations )
        failures += (uint64_t) counter_violations;
    else
        printf( "soak: conservation ok (server and every client)\n" );

    // the end sample, between clients as the post-warmup sample was: the
    // last client is destroyed, the server is still running

    if ( resources_started && process_resources_read( &resources_end ) )
    {
        resources_ended = 1;
        process_resources_peak( &resources_peak, &resources_end );
    }

    if ( resources_started && resources_ended )
    {
        const double seconds_since_start = rocketnet_time() - resources_start_time;
        for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
        {
            printf( "soak resources: %s start=%lld end=%lld peak=%lld\n", process_resources_name( i ),
                    (long long) resources_start.values[i], (long long) resources_end.values[i], (long long) resources_peak.values[i] );
            if ( assert_flat && !process_resources_flat( i, resources_start.values[i], resources_end.values[i], seconds_since_start ) )
            {
                printf( "soak: --assert-flat FAILED: %s start=%lld end=%lld", process_resources_name( i ),
                        (long long) resources_start.values[i], (long long) resources_end.values[i] );
                if ( i == PROCESS_RESOURCES_MEMORY )
                    printf( " (allowed growth %lld bytes over %.0f s)", (long long) process_resources_memory_allowance_bytes( seconds_since_start ), seconds_since_start );
                printf( "\n" );
                failures++;
            }
        }
    }
    else
    {
        printf( "soak resources: none (the run ended inside the warm cycle)\n" );
        if ( assert_flat )
        {
            printf( "soak: --assert-flat FAILED: no post-warmup and end samples (the run ended inside the warm cycle)\n" );
            failures++;
        }
    }


    printf( "\nsoak finished: %llu iterations, %llu connects, %llu payloads verified, %llu failures\n",
            (unsigned long long) iteration, (unsigned long long) connects,
            (unsigned long long) payloads_verified, (unsigned long long) failures );

    printf( "soak summary: sent %llu\n", (unsigned long long) ( client_sent + server_sent ) );
    printf( "soak summary: client_sent %llu\n", (unsigned long long) client_sent );
    printf( "soak summary: server_sent %llu\n", (unsigned long long) server_sent );
    printf( "soak summary: received %llu\n", (unsigned long long) ( client_received + server_received ) );
    printf( "soak summary: client_received %llu\n", (unsigned long long) client_received );
    printf( "soak summary: server_received %llu\n", (unsigned long long) server_received );
    printf( "soak summary: verified %llu\n", (unsigned long long) payloads_verified );
    printf( "soak summary: client_verified %llu\n", (unsigned long long) client_verified );
    printf( "soak summary: server_verified %llu\n", (unsigned long long) server_verified );
    printf( "soak summary: reconnects %llu\n", (unsigned long long) reconnects );
    printf( "soak summary: server_transport_io %s\n", server_transport_name );
    printf( "soak summary: client_transport_io %s\n", client_transport_name[0] ? client_transport_name : "none" );

    for ( int s = ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED; s <= ROCKETNET_CLIENT_STATE_DISCONNECTED; s++ )
    {
        const uint64_t count = disconnect_counts[s - ROCKETNET_CLIENT_STATE_CONNECT_TOKEN_EXPIRED];
        if ( count > 0 )
            printf( "soak summary: %s %llu\n", soak_disconnect_reason_name( s ), (unsigned long long) count );
    }

    const int num_counters = rocketnet_num_counters();
    for ( int i = 0; i < num_counters && i < ROCKETNET_SOAK_MAX_COUNTERS; i++ )
    {
        if ( client_counters[i] != 0 )
            printf( "soak summary: client_%s %llu\n", rocketnet_counter_name( i ), (unsigned long long) client_counters[i] );
    }

    const struct rocketnet_counters_t * server_counters = rocketnet_server_transport_counters( server );
    for ( int i = 0; i < num_counters; i++ )
    {
        const uint64_t value = rocketnet_counter_value( server_counters, i );
        if ( value != 0 )
            printf( "soak summary: server_%s %llu\n", rocketnet_counter_name( i ), (unsigned long long) value );
    }

    fflush( stdout );

    rocketnet_server_transport_stop( server );
    rocketnet_server_transport_update( server, rocketnet_time() );
    rn_native_server_transport_stop_and_destroy( server );

#if defined(__linux__)
    if ( self_netns >= 0 )
        close( self_netns );
    if ( client_netns >= 0 )
        close( client_netns );
#endif

    rocketnet_term();

    return failures ? 1 : 0;
}
