/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Soak: a client and server on loopback exchanging pattern-verified
    payloads of every size, reconnecting on a cycle, forever -- duration
    converts probability into certainty, and this is what finds the
    difficult problems. Run ./bin/soak and leave it; pass --seconds S or a
    number of iterations to bound it: ./bin/soak --seconds 60 or ./bin/soak 10000

    usage: soak [--seconds S] [--assert-flat] [--one-connection] [--rate N]
                [--assert-every S] [positive iteration count]

    --seconds S ends the run after S seconds of wall clock (S a positive
    integer); beside an iteration count the run ends at whichever bound comes
    first. A zero, negative, non-numeric or missing S, an unknown flag and a
    second positional argument are each refused with a one-line reason on
    stderr and exit 2, before any run starts.

    --one-connection holds one connection for the whole run: no planned
    reconnect, so any disconnect at all fails the run. With --rate it is the
    multi-day shape (scripts/soak-multiday): one connection past 2^32 packets,
    so the wire's 32-bit sequence field wraps (standard/TRANSPORT.md t4).

    --rate N sends N packets per second each way (N a positive integer, at
    most 1000000), paced against the wall clock from each connect; the
    transport layer has no send budget, so the rate is the only limit. Without
    it soak sends one packet each way per iteration, except that the client
    sends none in an iteration whose update deferred the hand-off of its fill
    ring (below): that packet is not dropped, it is the next one sent. The
    pacing never makes a
    refusal itself: each direction sends at most ROCKETNET_SOAK_RATE_BURST
    packets per iteration and holds its unacknowledged span (its highest sent
    session sequence less the largest the other side has acknowledged, read
    through the delivery capture) under ROCKETNET_SOAK_RATE_WINDOW, so a stall
    on either side never overruns the 4096-packet send history
    (SEND_HISTORY_OVERFLOW); and, with or without --rate, the client sends
    nothing more in an iteration once a hand-off of its fill ring is deferred
    (CLIENT_SEND_DEFERRED: the I/O thread still holds the other rings), so a
    stalled I/O thread never fills the ring (SEND_RING_FULL). What a stall
    holds back is still owed and sent after it. A packet counts as sent only
    when the transport staged it (start_packet and finish_packet).

    --assert-every S runs the counter discipline below (conservation, and
    every REFUSAL counter 0) on the client and the server every S seconds
    while the run goes (S a positive integer), not only at the end, together
    with the resource check: the process's peak resident memory may not grow
    by more than ROCKETNET_SOAK_RESIDENT_GROWTH_BYTES past its value at the
    first check (not on Windows). Each check prints one "soak assert:" line
    with the packets each way and the highest session sequence each way. The
    first violation ends the run, exit 1. With --rate a check also fails when
    either direction sent nothing since the previous check (the pacing
    stalled). After each "soak assert:" line soak prints one "soak assert
    counters:" line: every non-zero client and server counter, name=value,
    the hourly summary of scripts/soak-multiday carries it. A malformed --rate or --assert-every
    argument, a missing one or a repeated flag is refused like --seconds.

    ROCKETNET_DEV_SESSION_SEQUENCE_START (development builds only,
    sdk/rocketnet_test_seams.h) starts every session's sequence at its value
    instead of one, so a bounded run wraps the field in seconds:
    ROCKETNET_DEV_SESSION_SEQUENCE_START=4294966296 ./bin/soak --one-connection
    --rate 25000 --assert-every 10 --seconds 120.

    At the end of every run, an interrupted (SIGINT) one included, soak prints
    its "soak finished:" line and then one "soak summary: <name> <value>" line
    each for: sent, client_sent, server_sent; received, client_received,
    server_received; verified, client_verified, server_verified; reconnects
    (the planned reconnect cycles); client_highest_sent_sequence,
    server_highest_sent_sequence, client_highest_received_sequence and
    server_highest_received_sequence (the highest session sequences seen on a
    connected client, read through the delivery capture); one line per
    disconnect reason seen, named
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
    On Linux the post-warmup sample comes after the warm-up of
    tests/process_resources.h (process_resources_touch_all): every page of
    every private mapping faulted in once, so memory preallocated at create
    that one light client reaches only slowly (the server's receive buffer is
    sized for 1000 clients) is resident before the comparison starts; it
    prints as "soak resources: touched <bytes> bytes before the post-warmup
    sample" (or "soak resources: touch not available" where the kernel has
    no MADV_POPULATE_*). Every between-clients sample and every 10 s sample
    also prints "soak memory: <phase> t=<s> resident_bytes=<n>
    library_bytes=<n>", library_bytes being what the library holds through
    the allocator soak gives rocketnet_init; on Linux it adds
    "smaps_rss=<n> private_dirty=<n>" from /proc/self/smaps_rollup, and on
    Windows "threads=<n> os_threads=<n>"; phase is
    between (no client alive) or live (a client alive), and only between
    samples are compared.
    Every 10 s report also prints one "soak send: t=<s> staged_max=<n>
    deferred_run_max=<n> deferred_iterations=<n> unacked_span_max=<n>
    acked_packets=<n> refused_sends=<n> iteration_gap_max_ms=<ms>
    client_send_deferred=<n> client_send_ring_full=<n> library_bytes=<n>"
    line, the client send path over the interval read from what the library
    already keeps (soak_print_send): what tells a stalled I/O thread
    (deferred_run_max and staged_max climb toward the 256-packet fill ring)
    from a retire leak (unacked_span_max and library_bytes climb) and from
    memory the OS holds (library_bytes flat while resident_bytes moves).
    Each count prints as "soak resources: <name> start=<n> end=<n> peak=<n>"
    (fds, threads, resident_bytes; handles, threads, private_bytes,
    os_threads on Windows, where every thread proven the OS's by its start
    address also prints at the post-warmup and end samples as
    "soak resources: <sample> os_thread id=<n> start=ntdll+0x<offset>");
    library_bytes prints the same way on every platform; a run that ends inside the warm cycle, as every run under 30 s
    does, has no post-warmup sample and prints
    "soak resources: none (the run ended inside the warm cycle)".

    --assert-flat holds the counts to process_resources.h's flat rule: the
    fd (handle) and thread counts at the end equal their post-warmup counts,
    and library_bytes, the library's own allocator bytes, grows by at most
    one page per ten minutes of run since the post-warmup sample (at least
    one page). The process's memory count (resident_bytes, or private_bytes
    on Windows) is printed and never asserted: it counts what the OS and the
    C runtime hold (socket buffers, first touch of preallocated pages, the
    runtime's own heaps) beside the library, and the library answers only for
    its own; on Windows os_threads
    may fall and never rise, so the full thread count never grows. A resource
    read that fails (a thread walk that ends on an error among them) fails
    the run. A count that is not
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
#if !defined( _WIN32 )
#include <sys/resource.h>
#endif

// Internal transport tools mint the binary development token directly.
static int dev_token_binary( const char * address, uint64_t client_id, uint64_t timestamp, uint8_t * token, size_t token_bytes )
{
    return rocketnet_write_test_connect_token( address, client_id, timestamp, token, token_bytes ) == ROCKETNET_STATUS_OK;
}

#define ROCKETNET_SOAK_MAX_COUNTERS 256
#define ROCKETNET_SOAK_RATE_MAX 1000000ULL
#define ROCKETNET_SOAK_RATE_BURST 64           // packets each way per iteration at most: a quarter of the client's 256-slot fill ring
#define ROCKETNET_SOAK_RATE_WINDOW 1024        // unacknowledged sequences each way at most: a quarter of the 4096-packet send history
#define ROCKETNET_SOAK_RESIDENT_GROWTH_BYTES ( 64ULL * 1024 * 1024 )
#define ROCKETNET_SOAK_USAGE "usage: soak [--seconds S] [--assert-flat] [--one-connection] [--rate N] [--assert-every S] [positive iteration count]"
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

// The highest session sequences each way, read through the delivery capture
// while the client is connected; the running maxima survive the end of the
// connection.

struct soak_sequences_t
{
    uint64_t client_highest_sent;
    uint64_t server_highest_sent;
    uint64_t client_highest_received;
    uint64_t server_highest_received;
};

static void soak_capture_sequences( struct rocketnet_client_transport_t * client, struct rocketnet_server_transport_t * server, struct soak_sequences_t * sequences )
{
    if ( !client || rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        return;
    struct rocketnet_delivery_capture_t capture;
    memset( &capture, 0, sizeof( capture ) );
    rocketnet_client_transport_delivery_capture( client, &capture );
    if ( capture.connected )
    {
        if ( capture.highest_sent_sequence > sequences->client_highest_sent )
            sequences->client_highest_sent = capture.highest_sent_sequence;
        if ( capture.highest_received_sequence > sequences->client_highest_received )
            sequences->client_highest_received = capture.highest_received_sequence;
    }
    memset( &capture, 0, sizeof( capture ) );
    rocketnet_server_transport_delivery_capture( server, rocketnet_client_transport_client_index( client ), &capture );
    if ( capture.connected )
    {
        if ( capture.highest_sent_sequence > sequences->server_highest_sent )
            sequences->server_highest_sent = capture.highest_sent_sequence;
        if ( capture.highest_received_sequence > sequences->server_highest_received )
            sequences->server_highest_received = capture.highest_received_sequence;
    }
}

// the process's peak resident memory in bytes, 0 where it is not read
static uint64_t soak_peak_resident_bytes( void )
{
#if !defined( _WIN32 )
    struct rusage usage;
    if ( getrusage( RUSAGE_SELF, &usage ) != 0 )
        return 0;
#if defined( __APPLE__ )
    return (uint64_t) usage.ru_maxrss;                  // bytes on macOS
#else // #if defined( __APPLE__ )
    return (uint64_t) usage.ru_maxrss * 1024;           // kilobytes elsewhere
#endif // #if defined( __APPLE__ )
#else // #if !defined( _WIN32 )
    return 0;
#endif // #if !defined( _WIN32 )
}

// one optional flag's positive integer argument: exit 2 with a one-line
// reason when it is repeated, missing or malformed
static int soak_flag_value( int argc, char ** argv, int * i, int * have, uint64_t max_value, uint64_t * value )
{
    const char * flag = argv[*i];
    if ( *have )
    {
        fprintf( stderr, "soak: duplicate %s flag (" ROCKETNET_SOAK_USAGE ")\n", flag );
        return 0;
    }
    if ( *i + 1 >= argc )
    {
        fprintf( stderr, "soak: missing argument for %s (" ROCKETNET_SOAK_USAGE ")\n", flag );
        return 0;
    }
    ( *i )++;
    if ( !parse_positive_uint64( argv[*i], value ) || ( max_value && *value > max_value ) )
    {
        if ( max_value )
            fprintf( stderr, "soak: %s must be a positive integer at most %llu (" ROCKETNET_SOAK_USAGE ")\n", flag, (unsigned long long) max_value );
        else
            fprintf( stderr, "soak: %s must be a positive integer (" ROCKETNET_SOAK_USAGE ")\n", flag );
        return 0;
    }
    *have = 1;
    return 1;
}

// a counter's index by its rocketnet_counter_name, -1 when the build has none
static int soak_counter_index( const char * counter_name )
{
    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        if ( strcmp( rocketnet_counter_name( i ), counter_name ) == 0 )
            return i;
    }
    return -1;
}

// every non-zero counter of one end on the current line, name=value
static void soak_print_counters( const char * name, const struct rocketnet_counters_t * counters )
{
    printf( " %s", name );
    for ( int i = 0; i < rocketnet_num_counters(); i++ )
    {
        const uint64_t value = rocketnet_counter_value( counters, i );
        if ( value )
            printf( " %s=%llu", rocketnet_counter_name( i ), (unsigned long long) value );
    }
}

// the unacknowledged span of one end: its highest sent session sequence less
// the largest the other end has acknowledged, counted from the end's highest
// sent sequence at the connect (before the first acknowledgement the largest
// acknowledged reads 0); 0 when the capture is not connected
static uint64_t soak_unacked_span( const struct rocketnet_delivery_capture_t * capture, uint64_t sent_sequence_at_connect )
{
    if ( !capture->connected )
        return 0;
    const uint64_t acked = capture->largest_acked_sequence > sent_sequence_at_connect ? capture->largest_acked_sequence : sent_sequence_at_connect;
    return capture->highest_sent_sequence > acked ? capture->highest_sent_sequence - acked : 0;
}

// how many packets one direction may send this iteration: what the wall
// clock owes, at most a burst, and at most what keeps its unacknowledged span
// inside the window
static uint64_t soak_rate_send_count( uint64_t owed, uint64_t sent, uint64_t unacked_span )
{
    uint64_t count = owed > sent ? owed - sent : 0;
    if ( count > ROCKETNET_SOAK_RATE_BURST )
        count = ROCKETNET_SOAK_RATE_BURST;
    const uint64_t room = unacked_span < ROCKETNET_SOAK_RATE_WINDOW ? ROCKETNET_SOAK_RATE_WINDOW - unacked_span : 0;
    return count < room ? count : room;
}

// stages one pattern payload through the acquire/commit family, written at
// frame + payload_index; 1 only when the transport staged it
static int soak_client_send( struct rocketnet_client_transport_t * client, int payload_size )
{
    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_client_transport_start_packet( client, payload_size, &context );
    if ( !frame )
        return 0;
    generate_payload( frame + context.payload_index, payload_size );
    return rocketnet_client_transport_finish_packet( client, &context, payload_size ) > 0;
}

static int soak_server_send( struct rocketnet_server_transport_t * server, int client_index, int payload_size )
{
    struct rocketnet_packet_context_t context;
    uint8_t * frame = rocketnet_server_transport_start_packet( server, client_index, payload_size, &context );
    if ( !frame )
        return 0;
    generate_payload( frame + context.payload_index, payload_size );
    return rocketnet_server_transport_finish_packet( server, &context, payload_size ) > 0;
}

// The library's own allocations, counted: rocketnet_init is given an
// allocator that adds each block's bytes on allocate and takes them away on
// free, so every memory line can say how much of the process's memory the
// library holds through its allocator (library_bytes) beside what the OS and
// the C runtime hold. The library calls it from its own threads too, so the
// count moves by atomic adds. Each block carries its byte count in a header of
// SOAK_ALLOCATION_HEADER_BYTES, which keeps malloc's alignment.

#define SOAK_ALLOCATION_HEADER_BYTES 16

static volatile int64_t soak_library_bytes;

static void soak_library_bytes_add( int64_t bytes )
{
#if defined(_WIN32)
    InterlockedExchangeAdd64( (volatile LONG64 *) &soak_library_bytes, (LONG64) bytes );
#else
    __atomic_fetch_add( &soak_library_bytes, bytes, __ATOMIC_RELAXED );
#endif
}

static int64_t soak_library_bytes_read( void )
{
#if defined(_WIN32)
    return (int64_t) InterlockedCompareExchange64( (volatile LONG64 *) &soak_library_bytes, 0, 0 );
#else
    return __atomic_load_n( &soak_library_bytes, __ATOMIC_RELAXED );
#endif
}

static void * soak_library_malloc( void * context, size_t bytes )
{
    (void) context;
    uint8_t * block = (uint8_t *) malloc( bytes + SOAK_ALLOCATION_HEADER_BYTES );
    if ( !block )
        return NULL;
    memcpy( block, &bytes, sizeof( bytes ) );
    soak_library_bytes_add( (int64_t) bytes );
    return block + SOAK_ALLOCATION_HEADER_BYTES;
}

static void soak_library_free( void * context, void * p )
{
    (void) context;
    if ( !p )
        return;
    uint8_t * block = (uint8_t *) p - SOAK_ALLOCATION_HEADER_BYTES;
    size_t bytes = 0;
    memcpy( &bytes, block, sizeof( bytes ) );
    soak_library_bytes_add( -(int64_t) bytes );
    free( block );
}

// Windows only: names every thread the counts take as the OS's, with its
// start address, at one sample; a walk that fails here was already failed by
// the sample's own read

static void soak_print_os_threads( const char * sample_label )
{
#if defined(_WIN32)
    int64_t threads = 0;
    int64_t os_threads = 0;
    process_resources_walk_threads( &threads, &os_threads, sample_label );
#else
    (void) sample_label;
#endif
}

// one "soak memory:" line: the memory count of *sample, the library's own
// allocated bytes (a per-source figure beside the process's count, never
// compared), on Linux the smaps_rollup Rss and Private_Dirty read now, and on
// Windows the sample's threads and os_threads

static void soak_print_memory( const char * phase, double seconds, const struct process_resources_t * sample )
{
    int64_t rss = -1;
    int64_t private_dirty = -1;
    printf( "soak memory: %s t=%.0f %s=%lld library_bytes=%lld", phase, seconds, process_resources_name( PROCESS_RESOURCES_MEMORY ),
            (long long) sample->values[PROCESS_RESOURCES_MEMORY], (long long) soak_library_bytes_read() );
    if ( process_resources_smaps( &rss, &private_dirty ) )
        printf( " smaps_rss=%lld private_dirty=%lld", (long long) rss, (long long) private_dirty );
#if defined(_WIN32)
    printf( " threads=%lld os_threads=%lld", (long long) sample->values[PROCESS_RESOURCES_THREADS], (long long) sample->values[PROCESS_RESOURCES_OS_THREADS] );
#endif
    printf( "\n" );
    fflush( stdout );
}

// The client send path, per report interval, from what the library already
// keeps: the payloads soak staged into the client's fill ring since the last
// hand-off that was not deferred (staged; the fill ring holds 256 packets),
// the longest run of consecutive iterations whose update hand-off was
// deferred (CLIENT_SEND_DEFERRED moved: the I/O thread still held the other
// rings), the client's unacknowledged span (its highest sent session sequence
// less the largest the server acknowledged, from the delivery capture), the
// packet ids the client's updates reported acknowledged, the sends the
// transport refused (start_packet or finish_packet; soak retries the same
// payload on a later iteration, never drops it), the longest gap between two
// iteration starts, and the client's CLIENT_SEND_DEFERRED and SEND_RING_FULL
// counters. Printed and reset every report interval as one "soak send:" line.

struct soak_send_interval_t
{
    uint64_t staged;                    // now, carried across intervals
    uint64_t staged_max;
    uint64_t deferred_run;              // now, carried across intervals
    uint64_t deferred_run_max;
    uint64_t deferred_iterations;
    uint64_t unacked_span_max;
    uint64_t acked_packets;
    uint64_t refused_sends;
    double iteration_gap_max;
};

static void soak_send_interval_reset( struct soak_send_interval_t * send )
{
    send->staged_max = send->staged;
    send->deferred_run_max = send->deferred_run;
    send->deferred_iterations = 0;
    send->unacked_span_max = 0;
    send->acked_packets = 0;
    send->refused_sends = 0;
    send->iteration_gap_max = 0.0;
}

static void soak_print_send( double seconds, const struct soak_send_interval_t * send, struct rocketnet_client_transport_t * client, int counter_send_deferred, int counter_send_ring_full )
{
    const struct rocketnet_counters_t * counters = client ? rocketnet_client_transport_counters( client ) : NULL;
    printf( "soak send: t=%.0f staged_max=%llu deferred_run_max=%llu deferred_iterations=%llu unacked_span_max=%llu acked_packets=%llu "
            "refused_sends=%llu iteration_gap_max_ms=%.1f client_send_deferred=%llu client_send_ring_full=%llu library_bytes=%lld\n",
            seconds, (unsigned long long) send->staged_max, (unsigned long long) send->deferred_run_max,
            (unsigned long long) send->deferred_iterations, (unsigned long long) send->unacked_span_max,
            (unsigned long long) send->acked_packets, (unsigned long long) send->refused_sends, send->iteration_gap_max * 1000.0,
            (unsigned long long) ( counters && counter_send_deferred >= 0 ? rocketnet_counter_value( counters, counter_send_deferred ) : 0 ),
            (unsigned long long) ( counters && counter_send_ring_full >= 0 ? rocketnet_counter_value( counters, counter_send_ring_full ) : 0 ),
            (long long) soak_library_bytes_read() );
    fflush( stdout );
}

int main( int argc, char ** argv )
{
    // before anything is created: the allocator keeps the pages the warm-up
    // populates (tests/process_resources.h, THE WARM-UP)
    process_resources_keep_freed_pages();

    uint64_t max_iterations = 0;
    uint64_t max_seconds = 0;
    int have_iterations = 0;
    int have_seconds = 0;
    int assert_flat = 0;
    int one_connection = 0;
    uint64_t rate = 0;
    int have_rate = 0;
    uint64_t assert_every = 0;
    int have_assert_every = 0;

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--seconds" ) == 0 )
        {
            if ( have_seconds )
            {
                fprintf( stderr, "soak: duplicate --seconds flag (" ROCKETNET_SOAK_USAGE ")\n" );
                return 2;
            }
            if ( i + 1 >= argc )
            {
                fprintf( stderr, "soak: missing argument for --seconds (" ROCKETNET_SOAK_USAGE ")\n" );
                return 2;
            }
            i++;
            if ( !parse_positive_uint64( argv[i], &max_seconds ) )
            {
                fprintf( stderr, "soak: --seconds S must be a positive integer (" ROCKETNET_SOAK_USAGE ")\n" );
                return 2;
            }
            have_seconds = 1;
        }
        else if ( strcmp( argv[i], "--assert-flat" ) == 0 )
        {
            if ( assert_flat )
            {
                fprintf( stderr, "soak: duplicate --assert-flat flag (" ROCKETNET_SOAK_USAGE ")\n" );
                return 2;
            }
            assert_flat = 1;
        }
        else if ( strncmp( argv[i], "--assert-flat=", 14 ) == 0 )
        {
            fprintf( stderr, "soak: --assert-flat takes no value (" ROCKETNET_SOAK_USAGE ")\n" );
            return 2;
        }
        else if ( strcmp( argv[i], "--one-connection" ) == 0 )
        {
            if ( one_connection )
            {
                fprintf( stderr, "soak: duplicate --one-connection flag (" ROCKETNET_SOAK_USAGE ")\n" );
                return 2;
            }
            one_connection = 1;
        }
        else if ( strcmp( argv[i], "--rate" ) == 0 )
        {
            if ( !soak_flag_value( argc, argv, &i, &have_rate, ROCKETNET_SOAK_RATE_MAX, &rate ) )
                return 2;
        }
        else if ( strcmp( argv[i], "--assert-every" ) == 0 )
        {
            if ( !soak_flag_value( argc, argv, &i, &have_assert_every, 0, &assert_every ) )
                return 2;
        }
        else if ( argv[i][0] == '-' && ( argv[i][1] < '0' || argv[i][1] > '9' ) )
        {
            fprintf( stderr, "soak: unknown flag '%s' (" ROCKETNET_SOAK_USAGE ")\n", argv[i] );
            return 2;
        }
        else
        {
            if ( have_iterations )
            {
                fprintf( stderr, "soak: unexpected second positional argument '%s' (" ROCKETNET_SOAK_USAGE ")\n", argv[i] );
                return 2;
            }
            if ( !parse_positive_uint64( argv[i], &max_iterations ) )
            {
                fprintf( stderr, "soak: iteration count must be a positive integer (" ROCKETNET_SOAK_USAGE ")\n" );
                return 2;
            }
            have_iterations = 1;
        }
    }

    char fault_verify_env[256];
    int fault_verify_pending = ( rocketnet_test_getenv( "ROCKETNET_SOAK_FAULT_VERIFY", fault_verify_env, sizeof( fault_verify_env ) ) && fault_verify_env[0] != '\0' && strcmp( fault_verify_env, "0" ) != 0 ) ? 1 : 0;

    signal( SIGINT, interrupt_handler );

    if ( rocketnet_init( NULL, soak_library_malloc, soak_library_free ) != ROCKETNET_STATUS_OK )
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
    int resources_started = 0;
    int resources_ended = 0;
    double resources_start_time = 0.0;
    struct process_resources_t resources_start;
    struct process_resources_t resources_end;
    struct process_resources_t resources_peak;
    int64_t library_bytes_start = 0;
    int64_t library_bytes_end = 0;
    int64_t library_bytes_peak = 0;
    memset( &resources_start, 0, sizeof( resources_start ) );
    memset( &resources_end, 0, sizeof( resources_end ) );
    memset( &resources_peak, 0, sizeof( resources_peak ) );
    int client_payload_size = 1;
    int server_payload_size = 1;
    struct soak_sequences_t sequences;
    memset( &sequences, 0, sizeof( sequences ) );
    uint64_t resident_baseline = 0;
    uint64_t asserts = 0;
    // the pacing, per connection: packets each way sent since the connect,
    // and each end's highest sent session sequence at the connect
    uint64_t connection_client_sent = 0;
    uint64_t connection_server_sent = 0;
    uint64_t client_sent_sequence_at_connect = 0;
    uint64_t server_sent_sequence_at_connect = 0;
    uint64_t asserted_client_sent = 0;
    uint64_t asserted_server_sent = 0;
    const int counter_send_deferred = soak_counter_index( "CLIENT_SEND_DEFERRED" );
    if ( rate && counter_send_deferred < 0 )
    {
        printf( "soak: --rate needs the CLIENT_SEND_DEFERRED counter\n" );
        rn_native_server_transport_stop_and_destroy( server );
        rocketnet_term();
        return 1;
    }
    const int counter_send_ring_full = soak_counter_index( "SEND_RING_FULL" );
    struct soak_send_interval_t send_interval;
    memset( &send_interval, 0, sizeof( send_interval ) );
    const double start_time = rocketnet_time();
    double last_iteration_time = start_time;
    double last_report_time = start_time;
    double last_assert_time = start_time;
    double reconnect_time = start_time;
    double connect_time = start_time;

    while ( !quit && ( !max_iterations || iteration < max_iterations ) )
    {
        if ( max_seconds && rocketnet_time() - start_time >= (double) max_seconds )
        {
            deadline_reached = 1;
            break;
        }

        iteration++;
        {
            const double now = rocketnet_time();
            if ( now - last_iteration_time > send_interval.iteration_gap_max )
                send_interval.iteration_gap_max = now - last_iteration_time;
            last_iteration_time = now;
        }

        // reconnect on a cycle: the connect path is never a rare path

        if ( !client || ( !one_connection && rocketnet_time() - reconnect_time > 30.0 ) )
        {
            if ( client )
            {
                soak_capture_sequences( client, server, &sequences );
                reconnects++;
                rocketnet_client_transport_disconnect( client );
                soak_record_disconnect( disconnect_counts, rocketnet_client_transport_state( client ) );
                rocketnet_sleep( 0.05 );
                rocketnet_server_transport_update( server, rocketnet_time() );
                rocketnet_server_transport_send_packets( server );
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
                    // the warm-up: every preallocated page resident before
                    // the post-warmup sample (tests/process_resources.h)
                    const int64_t touched = process_resources_touch_all();
                    if ( touched >= 0 )
                        printf( "soak resources: touched %lld bytes before the post-warmup sample\n", (long long) touched );
                    else
                        printf( "soak resources: touch not available\n" );
                    if ( !process_resources_read( &sample ) )
                    {
                        printf( "soak: failed to read the process resource counts\n" );
                        failures++;
                        break;
                    }
                    resources_started = 1;
                    resources_start = sample;
                    resources_peak = sample;
                    resources_start_time = rocketnet_time();
                    soak_print_os_threads( "post-warmup" );
                    library_bytes_start = soak_library_bytes_read();
                    library_bytes_peak = library_bytes_start;
                }
                process_resources_peak( &resources_peak, &sample );
                if ( soak_library_bytes_read() > library_bytes_peak )
                    library_bytes_peak = soak_library_bytes_read();
                soak_print_memory( "between", rocketnet_time() - start_time, &sample );
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

            if ( rocketnet_client_transport_max_payload_bytes( client ) != ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES )
            {
                printf( "soak: unexpected client payload capacity\n" );
                failures++;
                break;
            }
            reconnect_time = rocketnet_time();
            connected = 0;
            send_interval.staged = 0;
            send_interval.deferred_run = 0;
        }

        // a deferred hand-off in update means the I/O thread still holds the
        // other rings: the client sends nothing more this iteration
        const uint64_t deferred_before_update = counter_send_deferred >= 0 ? rocketnet_counter_value( rocketnet_client_transport_counters( client ), counter_send_deferred ) : 0;
        rocketnet_client_transport_update( client, rocketnet_time() );
        rocketnet_server_transport_update( server, rocketnet_time() );
        const int client_ring_held = counter_send_deferred >= 0 && rocketnet_counter_value( rocketnet_client_transport_counters( client ), counter_send_deferred ) != deferred_before_update;
        {
            const uint64_t * acked_packet_ids = NULL;
            send_interval.acked_packets += (uint64_t) rocketnet_client_transport_acked_packets( client, &acked_packet_ids );
        }
        if ( client_ring_held )
        {
            send_interval.deferred_iterations++;
            send_interval.deferred_run++;
            if ( send_interval.deferred_run > send_interval.deferred_run_max )
                send_interval.deferred_run_max = send_interval.deferred_run;
        }
        else
        {
            send_interval.deferred_run = 0;
            send_interval.staged = 0;
        }

        // flush what the update staged (the handshake's replies, the
        // disconnects): a buffered transport, xdp, sends only at a flush

        rocketnet_server_transport_send_packets( server );

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
                connect_time = rocketnet_time();
                connection_client_sent = 0;
                connection_server_sent = 0;
                struct rocketnet_delivery_capture_t capture;
                memset( &capture, 0, sizeof( capture ) );
                rocketnet_client_transport_delivery_capture( client, &capture );
                client_sent_sequence_at_connect = capture.highest_sent_sequence;
                memset( &capture, 0, sizeof( capture ) );
                rocketnet_server_transport_delivery_capture( server, rocketnet_client_transport_client_index( client ), &capture );
                server_sent_sequence_at_connect = capture.highest_sent_sequence;
            }

            // one packet each way per iteration, or with --rate as many as
            // the wall clock owes since the connect, inside the burst and the
            // window (soak_rate_send_count)

            uint64_t client_send_count = client_ring_held ? 0 : 1;
            uint64_t server_send_count = 1;
            if ( rate )
            {
                const uint64_t owed = (uint64_t) ( ( rocketnet_time() - connect_time ) * (double) rate );
                struct rocketnet_delivery_capture_t client_capture;
                struct rocketnet_delivery_capture_t server_capture;
                memset( &client_capture, 0, sizeof( client_capture ) );
                memset( &server_capture, 0, sizeof( server_capture ) );
                rocketnet_client_transport_delivery_capture( client, &client_capture );
                rocketnet_server_transport_delivery_capture( server, rocketnet_client_transport_client_index( client ), &server_capture );
                client_send_count = client_ring_held ? 0 : soak_rate_send_count( owed, connection_client_sent, soak_unacked_span( &client_capture, client_sent_sequence_at_connect ) );
                server_send_count = soak_rate_send_count( owed, connection_server_sent, soak_unacked_span( &server_capture, server_sent_sequence_at_connect ) );
            }

            // every size cycles through, both directions, every byte checked;
            // each direction cycles its own sizes, so the receiver's check
            // holds whatever each direction sent

            const int client_index = rocketnet_client_transport_client_index( client );
            for ( uint64_t n = 0; n < client_send_count; n++ )
            {
                if ( !soak_client_send( client, client_payload_size ) )
                {
                    send_interval.refused_sends++;
                    break;
                }
                client_sent++;
                connection_client_sent++;
                send_interval.staged++;
                if ( send_interval.staged > send_interval.staged_max )
                    send_interval.staged_max = send_interval.staged;
                client_payload_size = client_payload_size < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? client_payload_size + 1 : 1;
            }
            if ( client_send_count )
            {
                const uint64_t deferred_before_send = counter_send_deferred >= 0 ? rocketnet_counter_value( rocketnet_client_transport_counters( client ), counter_send_deferred ) : 0;
                rocketnet_client_transport_send_packets( client );
                if ( counter_send_deferred < 0 || rocketnet_counter_value( rocketnet_client_transport_counters( client ), counter_send_deferred ) == deferred_before_send )
                    send_interval.staged = 0;
            }
            {
                struct rocketnet_delivery_capture_t client_capture;
                memset( &client_capture, 0, sizeof( client_capture ) );
                rocketnet_client_transport_delivery_capture( client, &client_capture );
                const uint64_t unacked_span = soak_unacked_span( &client_capture, client_sent_sequence_at_connect );
                if ( unacked_span > send_interval.unacked_span_max )
                    send_interval.unacked_span_max = unacked_span;
            }
            for ( uint64_t n = 0; n < server_send_count; n++ )
            {
                if ( !soak_server_send( server, client_index, server_payload_size ) )
                    break;
                server_sent++;
                connection_server_sent++;
                server_payload_size = server_payload_size < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? server_payload_size + 1 : 1;
            }
            rocketnet_server_transport_send_packets( server );

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

            static uint8_t received[ROCKETNET_TRANSPORT_MAX_PAYLOAD_BYTES];
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

        // the discipline while running: counters conserved and refusing
        // nothing on both ends, and peak resident memory flat

        if ( assert_every && rocketnet_time() - last_assert_time >= (double) assert_every )
        {
            last_assert_time = rocketnet_time();
            asserts++;
            soak_capture_sequences( client, server, &sequences );
            int violations = soak_check_counters( "client", rocketnet_client_transport_counters( client ) );
            violations += soak_check_counters( "server", rocketnet_server_transport_counters( server ) );
            const uint64_t resident = soak_peak_resident_bytes();
            if ( asserts == 1 )
                resident_baseline = resident;
            else if ( resident > resident_baseline + ROCKETNET_SOAK_RESIDENT_GROWTH_BYTES )
            {
                printf( "soak: RESOURCE CHECK FAILED: peak resident %llu bytes, %llu at the first check\n",
                        (unsigned long long) resident, (unsigned long long) resident_baseline );
                violations++;
            }
            if ( rate && ( client_sent == asserted_client_sent || server_sent == asserted_server_sent ) )
            {
                printf( "soak: RATE STALLED: client_sent %llu, server_sent %llu, unchanged since the previous check\n",
                        (unsigned long long) client_sent, (unsigned long long) server_sent );
                violations++;
            }
            asserted_client_sent = client_sent;
            asserted_server_sent = server_sent;
            printf( "soak assert: %.0f s, client_sent %llu, server_sent %llu, client_verified %llu, server_verified %llu, "
                    "client_highest_sent_sequence 0x%llx, server_highest_sent_sequence 0x%llx, peak_resident %llu, %s\n",
                    last_assert_time - start_time, (unsigned long long) client_sent, (unsigned long long) server_sent,
                    (unsigned long long) client_verified, (unsigned long long) server_verified,
                    (unsigned long long) sequences.client_highest_sent, (unsigned long long) sequences.server_highest_sent,
                    (unsigned long long) resident, violations ? "FAILED" : "ok" );
            printf( "soak assert counters:" );
            soak_print_counters( "client", rocketnet_client_transport_counters( client ) );
            soak_print_counters( "server", rocketnet_server_transport_counters( server ) );
            printf( "\n" );
            fflush( stdout );
            if ( violations )
            {
                counter_violations += violations;
                break;
            }
        }

        if ( rocketnet_time() - last_report_time > 10.0 )
        {
            last_report_time = rocketnet_time();
            if ( resources_started )
            {
                struct process_resources_t sample;
                if ( !process_resources_read( &sample ) )
                {
                    printf( "soak: failed to read the process resource counts\n" );
                    failures++;
                    break;
                }
                {
                    process_resources_peak( &resources_peak, &sample );
                    if ( resources_started && soak_library_bytes_read() > library_bytes_peak )
                        library_bytes_peak = soak_library_bytes_read();
                    soak_print_memory( client ? "live" : "between", rocketnet_time() - start_time, &sample );
                }
            }
            soak_print_send( rocketnet_time() - start_time, &send_interval, client, counter_send_deferred, counter_send_ring_full );
            soak_send_interval_reset( &send_interval );
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
            rocketnet_server_transport_send_packets( server );
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

    soak_capture_sequences( client, server, &sequences );

    if ( client )
    {
        if ( !unplanned_disconnect_recorded )
        {
            rocketnet_client_transport_disconnect( client );
            soak_record_disconnect( disconnect_counts, rocketnet_client_transport_state( client ) );
        }
        rocketnet_sleep( 0.05 );
        rocketnet_server_transport_update( server, rocketnet_time() );
        rocketnet_server_transport_send_packets( server );
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
        soak_print_os_threads( "end" );
        process_resources_peak( &resources_peak, &resources_end );
        library_bytes_end = soak_library_bytes_read();
        if ( library_bytes_end > library_bytes_peak )
            library_bytes_peak = library_bytes_end;
        soak_print_memory( "between", rocketnet_time() - start_time, &resources_end );
    }

    if ( resources_started && resources_ended )
    {
        const double seconds_since_start = rocketnet_time() - resources_start_time;
        for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
        {
            printf( "soak resources: %s start=%lld end=%lld peak=%lld\n", process_resources_name( i ),
                    (long long) resources_start.values[i], (long long) resources_end.values[i], (long long) resources_peak.values[i] );
            // the process's memory count is the OS's and the C runtime's as
            // much as the library's: printed, never asserted; the memory
            // flat rule holds the library's own bytes below
            if ( i == PROCESS_RESOURCES_MEMORY )
                continue;
            if ( assert_flat && !process_resources_flat( i, resources_start.values[i], resources_end.values[i], seconds_since_start ) )
            {
                printf( "soak: --assert-flat FAILED: %s start=%lld end=%lld\n", process_resources_name( i ),
                        (long long) resources_start.values[i], (long long) resources_end.values[i] );
                failures++;
            }
        }
        printf( "soak resources: library_bytes start=%lld end=%lld peak=%lld\n",
                (long long) library_bytes_start, (long long) library_bytes_end, (long long) library_bytes_peak );
        if ( assert_flat && !process_resources_flat( PROCESS_RESOURCES_MEMORY, library_bytes_start, library_bytes_end, seconds_since_start ) )
        {
            printf( "soak: --assert-flat FAILED: library_bytes start=%lld end=%lld (allowed growth %lld bytes over %.0f s)\n",
                    (long long) library_bytes_start, (long long) library_bytes_end,
                    (long long) process_resources_memory_allowance_bytes( seconds_since_start ), seconds_since_start );
            failures++;
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
    printf( "soak summary: client_highest_sent_sequence %llu\n", (unsigned long long) sequences.client_highest_sent );
    printf( "soak summary: server_highest_sent_sequence %llu\n", (unsigned long long) sequences.server_highest_sent );
    printf( "soak summary: client_highest_received_sequence %llu\n", (unsigned long long) sequences.client_highest_received );
    printf( "soak summary: server_highest_received_sequence %llu\n", (unsigned long long) sequences.server_highest_received );
    if ( assert_every )
        printf( "soak summary: asserts %llu\n", (unsigned long long) asserts );

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
