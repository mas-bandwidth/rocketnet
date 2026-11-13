/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    harden_load: the server transport hardening load (docs/HARDEN_LOAD.md).
    A functional/bench tool, never a unit test: it uses real sockets, real
    time and real threads. Development builds only (ROCKETNET_DEV): it reads
    the library's counters through rocketnet_layers.h.

    One binary, four roles, each its own process, so the server and its load
    can sit on different machines or in different network namespaces:

      harden_load server   --address A [options]       the server, a 60 Hz game frame
      harden_load clients  --address A --key K [...]   connected players, verifying
      harden_load hostile  --address A --key K [...]   traffic from addresses that are not players
      harden_load netstat  --label L [--dev IF]        the kernel's UDP/ICMP/IP counters and one interface's

    Every role ends with "harden_term:" lines, name=value pairs a cell reader
    (scripts/harden-cell) puts into one set of equations: every packet sent is
    delivered or counted by a named counter, the library's, the kernel's or
    the interface's. Nothing is judged by "it did not crash".

    THE SERVER (public API, rocketnet.h, in the documented frame order:
    update, receive until NULL, write, send_packets). Each tick it writes to
    every connected client in the space game's shape (docs/HARDEN_LOAD.md
    "Space shape"): --base unreliable messages per client per tick, and on
    --burst-clients clients a tick (rotating, so every client bursts in turn)
    a burst of --burst messages, sizes cycling through --sizes (MTU-size and
    smaller, so the message layer packs some together); one reliable ordered
    message per client every --reliable-every ticks rides along, and one
    --big-bytes fragmented reliable ordered message every --big-every ticks.
    Every message carries magic, kind, client id, connection id, a per
    (connection, kind) sequence and a pattern the receiver checks byte for
    byte. With --fill-jobs N the unreliable messages are written on N job
    threads instead of the main thread: the wide fill of rocketnet_layers.h's
    threading contract, the game's own jobs filling between update and
    send_packets. Each job owns a contiguous run of the connected clients
    (one filler per client), writes each message in place as one SINGLE
    payload (rocketnet_server_transport_start_packet, then
    rocketnet_message_single_finish and finish_packet), and main joins them
    before it writes the reliable and big messages through the message
    server and calls send_packets. Inbound messages from the clients are
    verified the same way. That frame, the main thread blocked through all
    of networking, is --frame-shape blocking (the default), kept for
    comparison. --frame-shape space runs space game's server frame
    (harden_space_t below): the main thread does the first half of the
    network update, the world update, then splits the second half off into
    a task on --game-threads (space's ThreadPool over the vendored enkiTS)
    that writes every player's messages as jobs and stages them through the
    public API while the main thread runs to the end of its frame; they
    leave at the next frame's send_packets. --world-ms and --game-load are a
    stand-in game in either shape. Each
    second it prints "harden_tick:" (connected, frames, overruns past one
    frame, the work time per frame avg/p99/max split into update, receive,
    write and send_packets, the fill jobs' summed busy time per frame as
    jobs_core_ms, messages and packets per second, start refusals),
    and each --sample-seconds a "harden_sample:" (fds, threads, resident
    bytes, library bytes and every nonzero counter): the long-run mode reads
    those against process_resources.h's flat rule, the nightly soak's method.

    THE CLIENTS. --clients N public clients on --threads T threads, each
    updated at --hz. Joins are spread: --join-rate clients a second, never a
    synchronised mass join. The last --churn-clients of them churn: every
    1/--churn-rate seconds one churner takes the next event in turn: a clean
    leave and a later rejoin, a silent stop (the client is not updated past
    its token's timeout, so both ends time out) and a rejoin, or an
    immediate reconnect. The rest are stable: one that disconnects is a
    failure. Each client sends --messages unreliable 64-byte inputs a tick and
    a reliable ordered one every --reliable-every ticks. Every connection's
    transport counters are read once its transport has stopped, so the sum is
    every packet that client object ever received.

    THE HOSTILE LOAD. From --sockets sockets that are not players, at --pps
    in total shared by --threads senders, a mix of --kinds: garbage (random bytes), prefixed (the
    published test filter's prefilter header, then random bytes, so it passes
    the prefilter and reaches the typed ladders), truncated (a valid header
    cut short), badsig (a connection request whose signature is random),
    expired (a correctly signed token that expired an hour ago), reqreplay
    (one valid, unexpired request replayed from every hostile socket) and
    replay (session packets of a real player, captured on --capture-dev with
    AF_PACKET, sent again from the hostile sockets; Linux, CAP_NET_RAW). It
    counts what it sent per kind and every datagram the server sends back.

    Linux reads its kernel counters from /proc/net/snmp and the interface's
    from /sys/class/net; elsewhere they print as unavailable.
*/

#if defined( __linux__ ) && !defined( _GNU_SOURCE )
#define _GNU_SOURCE
#endif // #if defined( __linux__ ) && !defined( _GNU_SOURCE )

#include "rocketnet.h"
#include "rocketnet_layers.h"
#include "rocketnet_message.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_base64.h"
#include "rocketnet_read_write.h"
#include "tests/portable_crt.h"
#include "tests/process_resources.h"
#include "tests/load/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
// the RIO transport's lane probe, where the build has it (ROCKETNET_SERVER_RIO_TEST_LANES_MADE)
#include "rocketnet_server_rio.h"
typedef SOCKET harden_socket_t;
#define HARDEN_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int harden_socket_t;
#define HARDEN_INVALID_SOCKET ( -1 )
#endif // #if defined( _WIN32 )

#if defined( __linux__ )
#include <linux/if_ether.h>
#include <pthread.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#endif // #if defined( __linux__ )

#if !ROCKETNET_DEV
#error "harden_load reads the library's counters: build it against the development library (ROCKETNET_DEV=1)"
#endif // #if !ROCKETNET_DEV

#define HARDEN_PROTOCOL_ID UINT64_C( 0x48415244454e3031 )
#define HARDEN_MAGIC UINT32_C( 0x4844524e )
#define HARDEN_HEADER_BYTES 28
#define HARDEN_KIND_BURST 0
#define HARDEN_KIND_RELIABLE 1
#define HARDEN_KIND_BIG 2
#define HARDEN_KIND_INPUT 3
#define HARDEN_KIND_INPUT_RELIABLE 4
#define HARDEN_KINDS 5
#define HARDEN_CHANNEL_UNRELIABLE 0
#define HARDEN_MAX_FILL_JOBS 64
#define HARDEN_CHANNEL_RELIABLE 1
#define HARDEN_CHANNEL_BIG 2
#define HARDEN_MAX_SIZES 16
#define HARDEN_MAX_THREADS 64
#define HARDEN_INPUT_BYTES 256 // honest input: about 100-200 kbps a client
#define HARDEN_INPUT_RELIABLE_BYTES 200
#define HARDEN_COUNTER_SLOTS 512
#define HARDEN_SLOT_PACKETS_SENT HARDEN_COUNTER_SLOTS
#define HARDEN_SLOT_PACKETS_RECEIVED ( HARDEN_COUNTER_SLOTS + 1 )
#define HARDEN_CLIENT_SLOTS ( HARDEN_COUNTER_SLOTS + 2 )

// ---------------------------------------------------------------- time and text

static double harden_now()
{
    return (double) std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now().time_since_epoch() ).count() * 1e-9;
}

// the wall clock, shared by every role's process on one machine: the cut's
// phases are wall times so separate processes reach them together

static double harden_wall()
{
    return (double) std::chrono::duration_cast<std::chrono::microseconds>( std::chrono::system_clock::now().time_since_epoch() ).count() * 1e-6;
}

static void harden_utc( char * out, size_t bytes )
{
    const time_t t = time( NULL );
    struct tm parts;
#if defined( _WIN32 )
    gmtime_s( &parts, &t );
#else
    gmtime_r( &t, &parts );
#endif // #if defined( _WIN32 )
    strftime( out, bytes, "%Y-%m-%dT%H:%M:%SZ", &parts );
}

static void harden_print_term( const char * role, const char * name, int64_t value )
{
    printf( "harden_term: role=%s %s=%" PRId64 "\n", role, name, value );
}

static uint64_t harden_mix64( uint64_t x )
{
    x ^= x >> 33;
    x *= UINT64_C( 0xff51afd7ed558ccd );
    x ^= x >> 33;
    x *= UINT64_C( 0xc4ceb9fe1a85ec53 );
    x ^= x >> 33;
    return x;
}

// the pattern's word for the 8-byte block holding byte i: byte i is
// (uint8_t) ( word >> ( 8 * ( i % 8 ) ) ). One mix a block, not a byte: the
// writer runs on the server's main thread (or its fill jobs) inside the
// measured frame, so its cost is kept to a fraction of the seal's
static uint64_t harden_pattern_word( uint64_t client_id, uint32_t connection, uint32_t kind, uint32_t sequence, int block )
{
    return harden_mix64( client_id ^ ( (uint64_t) connection << 40 ) ^ ( (uint64_t) kind << 56 ) ^ ( (uint64_t) sequence << 8 ) ^ (uint64_t) block );
}

// ---------------------------------------------------------------- the message format

struct harden_message_t
{
    uint32_t kind;
    uint64_t client_id;
    uint32_t connection;
    uint32_t sequence;
    uint32_t bytes;
};

// --payload copy: every message's body is one pre-built pattern, copied in
// behind the header, so the writer costs a memcpy (a game copying its own
// state) instead of a pattern computed per message. The header still names
// the client, connection, kind and sequence, and the receiver still checks
// every byte, against the pre-built body. Both ends must use the same mode.
// Set once, before any thread starts

#define HARDEN_COPY_BODY_BYTES 65536

static bool harden_payload_copy = false;
static uint8_t harden_copy_body[HARDEN_COPY_BODY_BYTES];

static void harden_copy_body_build()
{
    for ( int i = 0; i < HARDEN_COPY_BODY_BYTES; i++ )
    {
        harden_copy_body[i] = (uint8_t) ( harden_mix64( UINT64_C( 0x5eedc0de ) ^ (uint64_t) ( i / 8 ) ) >> ( 8 * ( i % 8 ) ) );
    }
}

static void harden_write_message( uint8_t * p, int bytes, uint32_t kind, uint64_t client_id, uint32_t connection, uint32_t sequence )
{
    uint8_t * cursor = p;
    rocketnet_write_uint32( &cursor, HARDEN_MAGIC );
    rocketnet_write_uint32( &cursor, kind );
    rocketnet_write_uint64( &cursor, client_id );
    rocketnet_write_uint32( &cursor, connection );
    rocketnet_write_uint32( &cursor, sequence );
    rocketnet_write_uint32( &cursor, (uint32_t) bytes );
    if ( harden_payload_copy )
    {
        memcpy( p + HARDEN_HEADER_BYTES, harden_copy_body + HARDEN_HEADER_BYTES, (size_t) ( bytes - HARDEN_HEADER_BYTES ) );
        return;
    }
    uint64_t word = 0;
    for ( int i = HARDEN_HEADER_BYTES; i < bytes; i++ )
    {
        if ( i == HARDEN_HEADER_BYTES || i % 8 == 0 )
        {
            word = harden_pattern_word( client_id, connection, kind, sequence, i / 8 );
        }
        p[i] = (uint8_t) ( word >> ( 8 * ( i % 8 ) ) );
    }
}

// 1 when the message is whole and its pattern holds; the header into *out

static int harden_read_message( const uint8_t * p, int bytes, struct harden_message_t * out )
{
    if ( bytes < HARDEN_HEADER_BYTES )
    {
        return 0;
    }
    const uint8_t * cursor = p;
    const uint32_t magic = rocketnet_read_uint32( &cursor );
    out->kind = rocketnet_read_uint32( &cursor );
    out->client_id = rocketnet_read_uint64( &cursor );
    out->connection = rocketnet_read_uint32( &cursor );
    out->sequence = rocketnet_read_uint32( &cursor );
    out->bytes = rocketnet_read_uint32( &cursor );
    if ( magic != HARDEN_MAGIC || out->kind >= HARDEN_KINDS || out->bytes != (uint32_t) bytes )
    {
        return 0;
    }
    if ( harden_payload_copy )
    {
        return memcmp( p + HARDEN_HEADER_BYTES, harden_copy_body + HARDEN_HEADER_BYTES, (size_t) ( bytes - HARDEN_HEADER_BYTES ) ) == 0 ? 1 : 0;
    }
    uint64_t word = 0;
    for ( int i = HARDEN_HEADER_BYTES; i < bytes; i++ )
    {
        if ( i == HARDEN_HEADER_BYTES || i % 8 == 0 )
        {
            word = harden_pattern_word( out->client_id, out->connection, out->kind, out->sequence, i / 8 );
        }
        if ( p[i] != (uint8_t) ( word >> ( 8 * ( i % 8 ) ) ) )
        {
            return 0;
        }
    }
    return 1;
}

// one receive stream. Reliable ordered kinds must arrive exactly in order.
// Unreliable kinds promise neither delivery nor order, only at most once
// (rocketnet.h): a gap is counted lost until its message arrives late, a late
// arrival is counted reordered, and a second arrival of a sequence still in
// the window is a duplicate, an error

#define HARDEN_WINDOW 1024
#define HARDEN_FLUSH_SECONDS 0.1

struct harden_stream_t
{
    uint32_t connection = 0;
    uint32_t expected = 0;
    bool started = false;
    uint64_t seen[HARDEN_WINDOW / 64] = {};
};

struct harden_verify_t
{
    uint64_t delivered[HARDEN_KINDS] = {};
    uint64_t lost[HARDEN_KINDS] = {};
    uint64_t head[HARDEN_KINDS] = {};
    uint64_t reordered[HARDEN_KINDS] = {};
    uint64_t late_past_window[HARDEN_KINDS] = {};
    uint64_t errors = 0;
    uint64_t wrong_owner = 0;
    uint64_t reliable_gaps = 0;
    uint64_t back = 0;
    uint64_t duplicates = 0;
};

static bool harden_stream_seen( const harden_stream_t * stream, uint32_t sequence )
{
    return ( stream->seen[( sequence % HARDEN_WINDOW ) / 64] >> ( sequence % 64 ) ) & 1;
}

static void harden_stream_mark( harden_stream_t * stream, uint32_t sequence, bool seen )
{
    const uint64_t bit = UINT64_C( 1 ) << ( sequence % 64 );
    uint64_t & word = stream->seen[( sequence % HARDEN_WINDOW ) / 64];
    word = seen ? ( word | bit ) : ( word & ~bit );
}

static void harden_stream_take( harden_stream_t * stream, const harden_message_t * m, bool reliable, harden_verify_t * v )
{
    if ( !stream->started || stream->connection != m->connection )
    {
        // a new connection's first message: sequences start at 0, so what
        // came before it was sent before this end could take it
        *stream = harden_stream_t();
        stream->started = true;
        stream->connection = m->connection;
        v->head[m->kind] += m->sequence;
        stream->expected = m->sequence;
    }
    if ( m->sequence < stream->expected )
    {
        if ( reliable )
        {
            v->back++;
            v->errors++;
            return;
        }
        if ( stream->expected - m->sequence > HARDEN_WINDOW )
        {
            // older than the window: no record of whether it came before
            v->late_past_window[m->kind]++;
            v->lost[m->kind]--;
            v->delivered[m->kind]++;
            return;
        }
        if ( harden_stream_seen( stream, m->sequence ) )
        {
            v->duplicates++;
            v->errors++;
            return;
        }
        harden_stream_mark( stream, m->sequence, true );
        v->reordered[m->kind]++;
        v->lost[m->kind]--;
        v->delivered[m->kind]++;
        return;
    }
    if ( m->sequence > stream->expected )
    {
        if ( reliable )
        {
            v->reliable_gaps++;
            v->errors++;
        }
        v->lost[m->kind] += m->sequence - stream->expected;
    }
    // every sequence the window moves past is cleared, this one is seen
    const uint32_t clear = m->sequence - stream->expected < HARDEN_WINDOW ? m->sequence - stream->expected : HARDEN_WINDOW;
    for ( uint32_t i = 0; i < clear; i++ )
    {
        harden_stream_mark( stream, stream->expected + i, false );
    }
    harden_stream_mark( stream, m->sequence, true );
    stream->expected = m->sequence + 1;
    v->delivered[m->kind]++;
}

// ---------------------------------------------------------------- options

struct harden_options_t
{
    std::string role;
    std::string address = "127.0.0.1:40000";
    std::string transport;
    std::string key_hex;
    std::string label;
    std::string dev;
    std::string kinds = "garbage,prefixed,truncated,badsig,expired,reqreplay";
    std::string capture_dev;
    int max_clients = 1000;
    int clients = 1000;
    int threads = 8;
    int first_id = 0;
    double seconds = 60.0;
    double hz = 60.0;
    double join_rate = 100.0;
    int churn_clients = 0;
    double churn_rate = 0.0;
    int messages = 1;
    int input_bytes = HARDEN_INPUT_BYTES; // --input-bytes N: each player's unreliable input message a tick (honest input: about 300 B a tick, 100-200 kbps)
    std::string load;         // --load peak|average|custom: space game's real server-to-client load (see harden_apply_load); custom keeps the flags
    int base = 1;
    int burst = 15;
    int burst_clients = 0;
    int fill_jobs = 0;
    bool space_shape = false; // --frame-shape space: space game's server frame (see harden_space_t)
    int game_threads = 0;     // --game-threads N: the ThreadPool, N threads in all, the main thread first
    int pin_first_cpu = -1;   // --pin-first-cpu C: the main thread on core C, game thread i on C + i
    double world_ms = 2.0;    // --world-ms: the main thread's own share of the stand-in world update
    double game_load = 0.5;   // --game-load F: stand-in game jobs, F of each game thread per frame
    // departures from space's frame, one at a time (SPACE-FRAME-SHAPE.md "Departures"); the defaults are space exact
    bool space_send_wide = false;   // --space-send wide: the player jobs start and finish each client's messages themselves (no serial commit)
    bool space_update_split = false; // --space-update split: update_receive, update_client over client ranges as jobs, update_finish
    bool space_net_in_task = false;  // --space-net task: send_packets and the update run at the end of the split-off task, not in the first half
    bool space_split_top = false;    // --space-split top: the network task is split off at the top of the frame, before the world update, and runs in parallel with it
    bool payload_copy = false;
    int sizes[HARDEN_MAX_SIZES] = { 1200, 1200, 1200, 1100, 900, 600, 300 };
    int num_sizes = 7;
    int reliable_every = 1;
    int reliable_bytes = 200;
    int big_every = 60;
    int big_bytes = 3000;
    int sample_seconds = 60;
    double quiet_seconds = 1.5;
    double end_at = 0.0;
    int token_timeout = 5;
    int num_xdp_queues = 0;
    double pps = 100000.0;
    int sockets = 64;
    int warm_seconds = 60;
    double profile_after = 0.0;
    double profile_seconds = 0.0;
    std::string profile_out = "profile.txt";
    std::string logs;
    int netem_duplicates = 0;
    int frame_p99_us = 5000; // rocketnet's share of a 60 Hz game frame: the rest of the game runs in it too
    double min_delivery = 99.9;
    uint64_t protocol_id = HARDEN_PROTOCOL_ID;
};

// space game's real load: snapshot baselines and deltas as unreliable messages
// of about one MTU (one message = one packet), 15 a client a tick at the peak
// (about 10 Mb/s a client, 10 Gb/s server-wide) and 8 on average (5 to 6 Mb/s);
// no large messages; reliable only as a trickle, one small message a client
// every 3 seconds. The input each client sends is --input-bytes (256 B a tick).
static bool harden_apply_load( harden_options_t * o, const char * v )
{
    if ( strcmp( v, "peak" ) == 0 || strcmp( v, "average" ) == 0 )
    {
        o->load = v;
        o->base = strcmp( v, "peak" ) == 0 ? 15 : 8;
        o->burst = 0;
        o->burst_clients = 0;
        o->sizes[0] = 1200;
        o->num_sizes = 1;
        o->reliable_every = 180;
        o->reliable_bytes = 100;
        o->big_every = 0;
        return true;
    }
    if ( strcmp( v, "custom" ) == 0 )
    {
        o->load = v;
        return true;
    }
    fprintf( stderr, "harden_load: --load is peak, average or custom\n" );
    return false;
}

static bool harden_parse_sizes( const char * text, harden_options_t * o )
{
    o->num_sizes = 0;
    const char * p = text;
    while ( *p && o->num_sizes < HARDEN_MAX_SIZES )
    {
        char * end = NULL;
        const long v = strtol( p, &end, 10 );
        if ( end == p || v < HARDEN_HEADER_BYTES || v > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 16 )
        {
            return false;
        }
        o->sizes[o->num_sizes++] = (int) v;
        p = end;
        if ( *p == ',' )
        {
            p++;
        }
    }
    return o->num_sizes > 0 && *p == 0;
}

static bool harden_parse( int argc, char ** argv, harden_options_t * o )
{
    if ( argc < 2 )
    {
        return false;
    }
    o->role = argv[1];
    for ( int i = 2; i < argc; i++ )
    {
        const char * a = argv[i];
        const char * v = i + 1 < argc ? argv[i + 1] : NULL;
        if ( !v )
        {
            fprintf( stderr, "harden_load: %s needs a value\n", a );
            return false;
        }
        i++;
        if ( strcmp( a, "--address" ) == 0 )
        {
            o->address = v;
        }
        else if ( strcmp( a, "--transport" ) == 0 )
        {
            o->transport = v;
        }
        else if ( strcmp( a, "--key" ) == 0 )
        {
            o->key_hex = v;
        }
        else if ( strcmp( a, "--label" ) == 0 )
        {
            o->label = v;
        }
        else if ( strcmp( a, "--dev" ) == 0 )
        {
            o->dev = v;
        }
        else if ( strcmp( a, "--kinds" ) == 0 )
        {
            o->kinds = v;
        }
        else if ( strcmp( a, "--capture-dev" ) == 0 )
        {
            o->capture_dev = v;
        }
        else if ( strcmp( a, "--max-clients" ) == 0 )
        {
            o->max_clients = atoi( v );
        }
        else if ( strcmp( a, "--clients" ) == 0 )
        {
            o->clients = atoi( v );
        }
        else if ( strcmp( a, "--threads" ) == 0 )
        {
            o->threads = atoi( v );
        }
        else if ( strcmp( a, "--first-id" ) == 0 )
        {
            o->first_id = atoi( v );
        }
        else if ( strcmp( a, "--seconds" ) == 0 )
        {
            o->seconds = atof( v );
        }
        else if ( strcmp( a, "--hz" ) == 0 )
        {
            o->hz = atof( v );
        }
        else if ( strcmp( a, "--join-rate" ) == 0 )
        {
            o->join_rate = atof( v );
        }
        else if ( strcmp( a, "--churn-clients" ) == 0 )
        {
            o->churn_clients = atoi( v );
        }
        else if ( strcmp( a, "--churn-rate" ) == 0 )
        {
            o->churn_rate = atof( v );
        }
        else if ( strcmp( a, "--messages" ) == 0 )
        {
            o->messages = atoi( v );
        }
        else if ( strcmp( a, "--base" ) == 0 )
        {
            o->base = atoi( v );
        }
        else if ( strcmp( a, "--burst" ) == 0 )
        {
            o->burst = atoi( v );
        }
        else if ( strcmp( a, "--burst-clients" ) == 0 )
        {
            o->burst_clients = atoi( v );
        }
        else if ( strcmp( a, "--fill-jobs" ) == 0 )
        {
            o->fill_jobs = atoi( v );
        }
        else if ( strcmp( a, "--frame-shape" ) == 0 )
        {
            if ( strcmp( v, "space" ) == 0 )
            {
                o->space_shape = true;
            }
            else if ( strcmp( v, "blocking" ) != 0 )
            {
                fprintf( stderr, "harden_load: --frame-shape is blocking or space\n" );
                return false;
            }
        }
        else if ( strcmp( a, "--game-threads" ) == 0 )
        {
            o->game_threads = atoi( v );
        }
        else if ( strcmp( a, "--pin-first-cpu" ) == 0 )
        {
            o->pin_first_cpu = atoi( v );
        }
        else if ( strcmp( a, "--world-ms" ) == 0 )
        {
            o->world_ms = atof( v );
        }
        else if ( strcmp( a, "--game-load" ) == 0 )
        {
            o->game_load = atof( v );
        }
        else if ( strcmp( a, "--space-send" ) == 0 )
        {
            o->space_send_wide = strcmp( v, "wide" ) == 0;
            if ( !o->space_send_wide && strcmp( v, "serial" ) != 0 )
            {
                fprintf( stderr, "harden_load: --space-send is serial or wide\n" );
                return false;
            }
        }
        else if ( strcmp( a, "--load" ) == 0 )
        {
            if ( !harden_apply_load( o, v ) )
            {
                return false;
            }
        }
        else if ( strcmp( a, "--space-update" ) == 0 )
        {
            o->space_update_split = strcmp( v, "split" ) == 0;
            if ( !o->space_update_split && strcmp( v, "main" ) != 0 )
            {
                fprintf( stderr, "harden_load: --space-update is main or split\n" );
                return false;
            }
        }
        else if ( strcmp( a, "--space-net" ) == 0 )
        {
            o->space_net_in_task = strcmp( v, "task" ) == 0;
            if ( !o->space_net_in_task && strcmp( v, "main" ) != 0 )
            {
                fprintf( stderr, "harden_load: --space-net is main or task\n" );
                return false;
            }
        }
        else if ( strcmp( a, "--space-split" ) == 0 )
        {
            o->space_split_top = strcmp( v, "top" ) == 0;
            if ( !o->space_split_top && strcmp( v, "bottom" ) != 0 )
            {
                fprintf( stderr, "harden_load: --space-split is bottom (after the world update, space's order) or top\n" );
                return false;
            }
        }
        else if ( strcmp( a, "--input-bytes" ) == 0 )
        {
            o->input_bytes = atoi( v );
        }
        else if ( strcmp( a, "--payload" ) == 0 )
        {
            if ( strcmp( v, "copy" ) == 0 )
            {
                o->payload_copy = true;
            }
            else if ( strcmp( v, "pattern" ) != 0 )
            {
                fprintf( stderr, "harden_load: --payload is pattern or copy\n" );
                return false;
            }
        }
        else if ( strcmp( a, "--sizes" ) == 0 )
        {
            if ( !harden_parse_sizes( v, o ) )
            {
                fprintf( stderr, "harden_load: --sizes takes up to %d sizes from %d to %d bytes\n", HARDEN_MAX_SIZES, HARDEN_HEADER_BYTES, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 16 );
                return false;
            }
        }
        else if ( strcmp( a, "--reliable-every" ) == 0 )
        {
            o->reliable_every = atoi( v );
        }
        else if ( strcmp( a, "--reliable-bytes" ) == 0 )
        {
            o->reliable_bytes = atoi( v );
        }
        else if ( strcmp( a, "--big-every" ) == 0 )
        {
            o->big_every = atoi( v );
        }
        else if ( strcmp( a, "--big-bytes" ) == 0 )
        {
            o->big_bytes = atoi( v );
        }
        else if ( strcmp( a, "--sample-seconds" ) == 0 )
        {
            o->sample_seconds = atoi( v );
        }
        else if ( strcmp( a, "--quiet-seconds" ) == 0 )
        {
            o->quiet_seconds = atof( v );
        }
        else if ( strcmp( a, "--end-at" ) == 0 )
        {
            o->end_at = atof( v );
        }
        else if ( strcmp( a, "--token-timeout" ) == 0 )
        {
            o->token_timeout = atoi( v );
        }
        else if ( strcmp( a, "--num-xdp-queues" ) == 0 )
        {
            o->num_xdp_queues = atoi( v );
        }
        else if ( strcmp( a, "--pps" ) == 0 )
        {
            o->pps = atof( v );
        }
        else if ( strcmp( a, "--sockets" ) == 0 )
        {
            o->sockets = atoi( v );
        }
        else if ( strcmp( a, "--logs" ) == 0 )
        {
            o->logs = v;
        }
        else if ( strcmp( a, "--netem-duplicates" ) == 0 )
        {
            o->netem_duplicates = atoi( v );
        }
        else if ( strcmp( a, "--protocol-id" ) == 0 )
        {
            o->protocol_id = (uint64_t) strtoull( v, NULL, 0 );
        }
        else if ( strcmp( a, "--min-delivery" ) == 0 )
        {
            o->min_delivery = atof( v );
        }
        else if ( strcmp( a, "--frame-p99-us" ) == 0 )
        {
            o->frame_p99_us = atoi( v );
        }
        else if ( strcmp( a, "--warm-seconds" ) == 0 )
        {
            o->warm_seconds = atoi( v );
        }
        else if ( strcmp( a, "--profile-after" ) == 0 )
        {
            o->profile_after = atof( v );
        }
        else if ( strcmp( a, "--profile-seconds" ) == 0 )
        {
            o->profile_seconds = atof( v );
        }
        else if ( strcmp( a, "--profile-out" ) == 0 )
        {
            o->profile_out = v;
        }
        else
        {
            fprintf( stderr, "harden_load: unknown option %s\n", a );
            return false;
        }
    }
    if ( o->load.empty() && o->space_shape && !harden_apply_load( o, "peak" ) )
    {
        return false;
    }
    if ( o->max_clients < 1 || o->clients < 0 || o->threads < 1 || o->threads > HARDEN_MAX_THREADS || o->seconds <= 0.0 || o->hz <= 0.0 ||
         o->join_rate <= 0.0 || o->churn_clients < 0 || o->churn_clients > o->clients || o->churn_rate < 0.0 || o->messages < 0 ||
         o->base < 0 || o->burst < 0 || o->burst_clients < 0 || o->fill_jobs < 0 || o->fill_jobs > HARDEN_MAX_FILL_JOBS || o->reliable_every < 0 || o->reliable_bytes < HARDEN_HEADER_BYTES || o->input_bytes < HARDEN_HEADER_BYTES || o->input_bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 64 ||
         o->big_every < 0 || o->big_bytes < HARDEN_HEADER_BYTES || ( o->payload_copy && o->big_bytes > HARDEN_COPY_BODY_BYTES ) || o->sample_seconds < 1 || o->quiet_seconds < 0.1 || o->token_timeout < 1 ||
         o->pps < 0.0 || o->sockets < 1 || o->sockets > 4096 || o->warm_seconds < 0 || o->game_threads < 0 || o->game_threads > HARDEN_MAX_THREADS ||
         ( o->space_shape && ( o->game_threads < 2 || o->fill_jobs > 0 || ( o->space_send_wide && o->big_every > 0 ) ) ) || o->world_ms < 0.0 || o->game_load < 0.0 || o->game_load > 1.0 )
    {
        fprintf( stderr, "harden_load: an option is out of range\n" );
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- kernel and interface counters

struct harden_kernel_t
{
    bool read = false;
    int64_t udp_in = -1, udp_noports = -1, udp_inerrors = -1, udp_out = -1, udp_rcvbuf = -1, udp_sndbuf = -1, udp_incsum = -1;
    int64_t icmp_in = -1, icmp_out = -1;
    int64_t ip_inreceives = -1, ip_indiscards = -1, ip_outrequests = -1, ip_outdiscards = -1, ip_outnoroutes = -1;
};

#if defined( __linux__ )
// /proc/net/snmp holds a names line then a values line per protocol; returns
// the value under name on the protocol's lines, or -1

static int64_t harden_snmp_field( const char * names, const char * values, const char * name )
{
    std::vector<std::string> keys, nums;
    std::string token;
    for ( const char * p = names;; p++ )
    {
        if ( *p == ' ' || *p == '\n' || *p == 0 )
        {
            if ( !token.empty() )
            {
                keys.push_back( token );
            }
            token.clear();
            if ( *p == 0 )
            {
                break;
            }
        }
        else
        {
            token += *p;
        }
    }
    for ( const char * p = values;; p++ )
    {
        if ( *p == ' ' || *p == '\n' || *p == 0 )
        {
            if ( !token.empty() )
            {
                nums.push_back( token );
            }
            token.clear();
            if ( *p == 0 )
            {
                break;
            }
        }
        else
        {
            token += *p;
        }
    }
    for ( size_t i = 0; i < keys.size() && i < nums.size(); i++ )
    {
        if ( keys[i] == name )
        {
            return (int64_t) strtoll( nums[i].c_str(), NULL, 10 );
        }
    }
    return -1;
}
#endif // #if defined( __linux__ )

static harden_kernel_t harden_kernel_read()
{
    harden_kernel_t k;
#if defined( __linux__ )
    FILE * file = fopen( "/proc/net/snmp", "r" );
    if ( !file )
    {
        return k;
    }
    char names[2048] = { 0 };
    char line[2048];
    while ( fgets( line, sizeof( line ), file ) )
    {
        if ( strncmp( line, "Udp: ", 5 ) == 0 || strncmp( line, "Icmp: ", 6 ) == 0 || strncmp( line, "Ip: ", 4 ) == 0 )
        {
            if ( names[0] == 0 || strncmp( names, line, 4 ) != 0 )
            {
                snprintf( names, sizeof( names ), "%s", line );
                continue;
            }
            if ( strncmp( line, "Udp: ", 5 ) == 0 )
            {
                k.udp_in = harden_snmp_field( names, line, "InDatagrams" );
                k.udp_noports = harden_snmp_field( names, line, "NoPorts" );
                k.udp_inerrors = harden_snmp_field( names, line, "InErrors" );
                k.udp_out = harden_snmp_field( names, line, "OutDatagrams" );
                k.udp_rcvbuf = harden_snmp_field( names, line, "RcvbufErrors" );
                k.udp_sndbuf = harden_snmp_field( names, line, "SndbufErrors" );
                k.udp_incsum = harden_snmp_field( names, line, "InCsumErrors" );
            }
            else if ( strncmp( line, "Icmp: ", 6 ) == 0 )
            {
                k.icmp_in = harden_snmp_field( names, line, "InMsgs" );
                k.icmp_out = harden_snmp_field( names, line, "OutMsgs" );
            }
            else
            {
                k.ip_inreceives = harden_snmp_field( names, line, "InReceives" );
                k.ip_indiscards = harden_snmp_field( names, line, "InDiscards" );
                k.ip_outrequests = harden_snmp_field( names, line, "OutRequests" );
                k.ip_outdiscards = harden_snmp_field( names, line, "OutDiscards" );
                k.ip_outnoroutes = harden_snmp_field( names, line, "OutNoRoutes" );
            }
            names[0] = 0;
        }
    }
    fclose( file );
    k.read = k.udp_in >= 0 && k.icmp_in >= 0 && k.ip_inreceives >= 0;
#endif // #if defined( __linux__ )
    return k;
}

static int64_t harden_sys_value( const char * dev, const char * name )
{
#if defined( __linux__ )
    char path[256];
    snprintf( path, sizeof( path ), "/sys/class/net/%s/statistics/%s", dev, name );
    FILE * file = fopen( path, "r" );
    if ( !file )
    {
        return -1;
    }
    long long v = -1;
    if ( fscanf( file, "%lld", &v ) != 1 )
    {
        v = -1;
    }
    fclose( file );
    return (int64_t) v;
#else
    (void) dev;
    (void) name;
    return -1;
#endif // #if defined( __linux__ )
}

// the root qdisc's "Sent N bytes M pkt (dropped D" on the device, from tc;
// the packet count counts each GSO segment, the drop count each skb

struct harden_link_t
{
    int64_t tc_sent = 0, tc_dropped = 0;
    int64_t tx_packets = -1, tx_dropped = -1, rx_packets = -1, rx_dropped = -1;
    // a cloud NIC's own counters (ethtool -S), 0 where the device has none:
    // packets the provider queued or dropped past the instance's packet and
    // bandwidth allowances, printed beside the chain to name a gap on a real
    // link
    int64_t pps_allowance = 0, bw_in_allowance = 0, bw_out_allowance = 0;
};

#if defined( __linux__ )
static void harden_ethtool_read( const std::string & dev, harden_link_t * l )
{
    char command[256];
    snprintf( command, sizeof( command ), "ethtool -S %s 2>/dev/null", dev.c_str() );
    FILE * pipe = popen( command, "r" );
    if ( !pipe )
    {
        return;
    }
    char line[512];
    while ( fgets( line, sizeof( line ), pipe ) )
    {
        char name[128];
        long long value = 0;
        if ( sscanf( line, " %127[^:]: %lld", name, &value ) != 2 )
        {
            continue;
        }
        if ( strcmp( name, "pps_allowance_exceeded" ) == 0 )
        {
            l->pps_allowance = (int64_t) value;
        }
        else if ( strcmp( name, "bw_in_allowance_exceeded" ) == 0 )
        {
            l->bw_in_allowance = (int64_t) value;
        }
        else if ( strcmp( name, "bw_out_allowance_exceeded" ) == 0 )
        {
            l->bw_out_allowance = (int64_t) value;
        }
    }
    pclose( pipe );
}
#endif // #if defined( __linux__ )

static harden_link_t harden_link_read( const std::string & dev )
{
    harden_link_t l;
    if ( dev.empty() )
    {
        return l;
    }
#if defined( __linux__ )
    char command[256];
    snprintf( command, sizeof( command ), "tc -s qdisc show dev %s 2>/dev/null", dev.c_str() );
    FILE * pipe = popen( command, "r" );
    if ( pipe )
    {
        char line[512];
        bool first = true;
        while ( fgets( line, sizeof( line ), pipe ) )
        {
            const char * sent = strstr( line, "Sent " );
            if ( !sent || !first )
            {
                continue;
            }
            long long bytes = 0, packets = 0, dropped = 0;
            if ( sscanf( sent, "Sent %lld bytes %lld pkt (dropped %lld", &bytes, &packets, &dropped ) == 3 )
            {
                l.tc_sent = (int64_t) packets;
                l.tc_dropped = (int64_t) dropped;
                first = false;
            }
        }
        pclose( pipe );
    }
    harden_ethtool_read( dev, &l );
#endif // #if defined( __linux__ )
    l.tx_packets = harden_sys_value( dev.c_str(), "tx_packets" );
    l.tx_dropped = harden_sys_value( dev.c_str(), "tx_dropped" );
    l.rx_packets = harden_sys_value( dev.c_str(), "rx_packets" );
    l.rx_dropped = harden_sys_value( dev.c_str(), "rx_dropped" );
    return l;
}

// read once at the process's start, before any traffic of its own; the cut
// prints every term as its change since then

static harden_kernel_t harden_kernel_base;
static harden_link_t harden_link_base;

static void harden_print_cut_term( const char * role, const char * cut, const char * name, int64_t value )
{
    printf( "harden_term: role=%s %s_%s=%" PRId64 "\n", role, cut, name, value );
}

// one cut's network terms: this namespace's kernel counts and this end's
// device and qdisc, each as its change since the process started

static void harden_print_cut_network( const char * role, const char * cut, const harden_options_t & o )
{
    const harden_kernel_t k = harden_kernel_read();
    const harden_kernel_t & b = harden_kernel_base;
    harden_print_cut_term( role, cut, "kernel_read", k.read && b.read ? 1 : 0 );
    harden_print_cut_term( role, cut, "udp_in", k.udp_in - b.udp_in );
    harden_print_cut_term( role, cut, "udp_noports", k.udp_noports - b.udp_noports );
    harden_print_cut_term( role, cut, "udp_inerrors", k.udp_inerrors - b.udp_inerrors );
    harden_print_cut_term( role, cut, "udp_rcvbuf", k.udp_rcvbuf - b.udp_rcvbuf );
    harden_print_cut_term( role, cut, "udp_out", k.udp_out - b.udp_out );
    harden_print_cut_term( role, cut, "udp_sndbuf", k.udp_sndbuf - b.udp_sndbuf );
    harden_print_cut_term( role, cut, "udp_incsum", k.udp_incsum - b.udp_incsum );
    harden_print_cut_term( role, cut, "icmp_in", k.icmp_in - b.icmp_in );
    harden_print_cut_term( role, cut, "icmp_out", k.icmp_out - b.icmp_out );
    harden_print_cut_term( role, cut, "ip_inreceives", k.ip_inreceives - b.ip_inreceives );
    harden_print_cut_term( role, cut, "ip_indiscards", k.ip_indiscards - b.ip_indiscards );
    harden_print_cut_term( role, cut, "ip_outrequests", k.ip_outrequests - b.ip_outrequests );
    harden_print_cut_term( role, cut, "ip_outdiscards", k.ip_outdiscards - b.ip_outdiscards );
    if ( !o.dev.empty() )
    {
        const harden_link_t l = harden_link_read( o.dev );
        const harden_link_t & lb = harden_link_base;
        harden_print_cut_term( role, cut, "tc_sent", l.tc_sent - lb.tc_sent );
        harden_print_cut_term( role, cut, "tc_dropped", l.tc_dropped - lb.tc_dropped );
        harden_print_cut_term( role, cut, "dev_tx_packets", l.tx_packets - lb.tx_packets );
        harden_print_cut_term( role, cut, "dev_tx_dropped", l.tx_dropped - lb.tx_dropped );
        harden_print_cut_term( role, cut, "dev_rx_packets", l.rx_packets - lb.rx_packets );
        harden_print_cut_term( role, cut, "dev_rx_dropped", l.rx_dropped - lb.rx_dropped );
        harden_print_cut_term( role, cut, "nic_pps_allowance", l.pps_allowance - lb.pps_allowance );
        harden_print_cut_term( role, cut, "nic_bw_in_allowance", l.bw_in_allowance - lb.bw_in_allowance );
        harden_print_cut_term( role, cut, "nic_bw_out_allowance", l.bw_out_allowance - lb.bw_out_allowance );
    }
    fflush( stdout );
}

// sleeps until the wall clock reaches when

static void harden_sleep_until_wall( double when )
{
    for ( ;; )
    {
        const double left = when - harden_wall();
        if ( left <= 0.0 )
        {
            return;
        }
        rocketnet_sleep( left > 0.05 ? 0.05 : left );
    }
}

static int harden_netstat_main( const harden_options_t & o )
{
    char utc[32];
    harden_utc( utc, sizeof( utc ) );
    const harden_kernel_t k = harden_kernel_read();
    const char * label = o.label.empty() ? "netstat" : o.label.c_str();
    printf( "harden_netstat: label=%s utc=%s read=%d udp_in=%" PRId64 " udp_noports=%" PRId64 " udp_inerrors=%" PRId64 " udp_out=%" PRId64
            " udp_rcvbuf=%" PRId64 " udp_sndbuf=%" PRId64 " udp_incsum=%" PRId64 " icmp_in=%" PRId64 " icmp_out=%" PRId64
            " ip_inreceives=%" PRId64 " ip_indiscards=%" PRId64 " ip_outrequests=%" PRId64 " ip_outdiscards=%" PRId64 " ip_outnoroutes=%" PRId64,
            label, utc, k.read ? 1 : 0, k.udp_in, k.udp_noports, k.udp_inerrors, k.udp_out, k.udp_rcvbuf, k.udp_sndbuf, k.udp_incsum, k.icmp_in, k.icmp_out,
            k.ip_inreceives, k.ip_indiscards, k.ip_outrequests, k.ip_outdiscards, k.ip_outnoroutes );
    if ( !o.dev.empty() )
    {
        printf( " dev=%s dev_rx_packets=%" PRId64 " dev_tx_packets=%" PRId64 " dev_rx_dropped=%" PRId64 " dev_tx_dropped=%" PRId64,
                o.dev.c_str(), harden_sys_value( o.dev.c_str(), "rx_packets" ), harden_sys_value( o.dev.c_str(), "tx_packets" ),
                harden_sys_value( o.dev.c_str(), "rx_dropped" ), harden_sys_value( o.dev.c_str(), "tx_dropped" ) );
    }
    printf( "\n" );
    return k.read ? 0 : 1;
}

// ---------------------------------------------------------------- the library's bytes (the soak's method)

#define HARDEN_ALLOCATION_HEADER_BYTES 16

static std::atomic<int64_t> harden_library_allocator_bytes( 0 );

extern "C" uint64_t rocketnet_direct_allocation_bytes( void );

static void * harden_malloc( void * context, size_t bytes )
{
    (void) context;
    uint8_t * block = (uint8_t *) malloc( bytes + HARDEN_ALLOCATION_HEADER_BYTES );
    if ( !block )
    {
        return NULL;
    }
    memcpy( block, &bytes, sizeof( bytes ) );
    harden_library_allocator_bytes.fetch_add( (int64_t) bytes, std::memory_order_relaxed );
    return block + HARDEN_ALLOCATION_HEADER_BYTES;
}

static void harden_free( void * context, void * p )
{
    (void) context;
    if ( !p )
    {
        return;
    }
    uint8_t * block = (uint8_t *) p - HARDEN_ALLOCATION_HEADER_BYTES;
    size_t bytes = 0;
    memcpy( &bytes, block, sizeof( bytes ) );
    harden_library_allocator_bytes.fetch_sub( (int64_t) bytes, std::memory_order_relaxed );
    free( block );
}

static int64_t harden_library_bytes()
{
    return harden_library_allocator_bytes.load( std::memory_order_relaxed ) + (int64_t) rocketnet_direct_allocation_bytes();
}

static void harden_print_counters( const char * prefix, const char * role, const struct rocketnet_counters_t * counters, bool terms )
{
    for ( int i = 0; counters && i < rocketnet_num_counters(); i++ )
    {
        const uint64_t v = rocketnet_counter_value( counters, i );
        if ( v == 0 )
        {
            continue;
        }
        if ( terms )
        {
            printf( "harden_term: role=%s %s_%s=%" PRIu64 "\n", role, prefix, rocketnet_counter_name( i ), v );
        }
        else
        {
            printf( " %s=%" PRIu64, rocketnet_counter_name( i ), v );
        }
    }
}

// ---------------------------------------------------------------- keys

static bool harden_parse_key( const std::string & hex, uint8_t * key )
{
    if ( hex.size() != 2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES )
    {
        return false;
    }
    // two hex digits a byte, read by hand: no CRT scan, which MSVC deprecates
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        int value = 0;
        for ( int j = 0; j < 2; j++ )
        {
            const char c = hex[(size_t) ( 2 * i + j )];
            int digit = -1;
            if ( c >= '0' && c <= '9' )
            {
                digit = c - '0';
            }
            else if ( c >= 'a' && c <= 'f' )
            {
                digit = c - 'a' + 10;
            }
            else if ( c >= 'A' && c <= 'F' )
            {
                digit = c - 'A' + 10;
            }
            if ( digit < 0 )
            {
                return false;
            }
            value = value * 16 + digit;
        }
        key[i] = (uint8_t) value;
    }
    return true;
}

// ---------------------------------------------------------------- the server

struct harden_frame_window_t
{
    std::vector<double> work;
    double update_sum = 0, receive_sum = 0, write_sum = 0, send_sum = 0;
    double update_max = 0, receive_max = 0, write_max = 0, send_max = 0;
    uint64_t frames = 0, overruns = 0;
    uint64_t messages = 0, start_refused = 0, received = 0;
    double jobs_busy_sum = 0; // core seconds the fill jobs spent, summed over the jobs
    // the space frame shape (and the stand-in game in either shape)
    double join_sum = 0, join_max = 0;             // the main thread blocked at the join
    double task_sum = 0, task_max = 0;             // the split-off task, start to end
    double task_core_sum = 0, task_core_max = 0;   // core seconds of rocketnet's work on the game threads: write jobs + commit
    double commit_sum = 0, commit_max = 0;         // the commit inside the task: start/finish per staged message
    double latency_sum = 0, latency_max = 0;       // a frame's packets: built from the state to sent by send_packets
    uint64_t latency_frames = 0, task_frames = 0, task_late = 0;
    double input_age_sum = 0, input_age_max = 0;   // --space-net task: from the task's update finishing to the main thread handing those inputs to the world update
    uint64_t input_age_frames = 0;
    double game_core_sum = 0;                      // core seconds the stand-in game jobs ran
    double world_sum = 0, world_max = 0;           // the main thread's world update, its own share plus the game jobs it waits on
    double task_send_sum = 0, task_update_sum = 0, task_update_max = 0; // --space-net task: the task's send_packets and update (serial part)
    double net_thread[HARDEN_MAX_THREADS] = {};    // rocketnet's work by thread index (0 = the main thread's part in jobs)
    double range_thread[HARDEN_MAX_THREADS] = {};  // the part of it in range jobs (writes, update_client): no serial walk
};

static double harden_percentile( std::vector<double> & v, double p )
{
    if ( v.empty() )
    {
        return 0.0;
    }
    std::sort( v.begin(), v.end() );
    size_t i = (size_t) ( p * (double) ( v.size() - 1 ) + 0.5 );
    if ( i >= v.size() )
    {
        i = v.size() - 1;
    }
    return v[i];
}

struct harden_server_slot_t
{
    uint64_t client_id = 0;
    uint32_t connection = 0;
    uint32_t next[HARDEN_KINDS] = {};
    harden_stream_t inbound[HARDEN_KINDS];
};

// the wide fill: --fill-jobs threads, started once and woken each frame,
// write the unreliable messages between update and send_packets
// (rocketnet_layers.h, threading contract). main hands them the frame's plan,
// one item per connected client, and waits on the condition variable for all
// of them: the join the contract asks for. Job j owns items
// [n*j/N, n*(j+1)/N), so a client has one filler; its slot's sequence is
// touched by that job alone until main joins

struct harden_fill_item_t
{
    int slot;
    int count;
    uint64_t client_id;
    uint32_t connection;
};

struct harden_fill_job_t
{
    alignas( 64 ) uint64_t messages = 0;
    uint64_t start_refused = 0;
    uint64_t finish_refused = 0;
    double busy = 0.0; // seconds inside the fill range: the core time the game gives up to the job
};

struct harden_fill_pool_t
{
    std::mutex mutex;
    std::condition_variable start_cv;
    std::condition_variable done_cv;
    uint64_t generation = 0;
    int remaining = 0;
    bool quit = false;
    struct rocketnet_server_transport_t * transport = NULL;
    const harden_options_t * o = NULL;
    const std::vector<harden_fill_item_t> * plan = NULL;
    std::vector<harden_server_slot_t> * slots = NULL;
    std::vector<harden_fill_job_t> jobs;
    std::vector<std::thread> threads;
};

static void harden_fill_range( harden_fill_pool_t * pool, int job )
{
    const std::vector<harden_fill_item_t> & plan = *pool->plan;
    const harden_options_t & o = *pool->o;
    harden_fill_job_t & stats = pool->jobs[(size_t) job];
    const int num_jobs = (int) pool->jobs.size();
    const int begin = (int) ( (int64_t) plan.size() * job / num_jobs );
    const int end = (int) ( (int64_t) plan.size() * ( job + 1 ) / num_jobs );
    const double began = harden_now();
    for ( int k = begin; k < end; k++ )
    {
        const harden_fill_item_t & item = plan[(size_t) k];
        harden_server_slot_t & slot = ( *pool->slots )[(size_t) item.slot];
        for ( int n = 0; n < item.count; n++ )
        {
            // sizes cycle per client here, not across the server as on main
            const int bytes = o.sizes[slot.next[HARDEN_KIND_BURST] % (uint32_t) o.num_sizes];
            struct rocketnet_packet_context_t context;
            // start_packet returns the frame (as the message server reads it), not
            // the payload its header comment names: the payload is frame + payload_index
            uint8_t * frame = rocketnet_server_transport_start_packet( pool->transport, item.slot, bytes + 1, &context );
            if ( !frame )
            {
                stats.start_refused++;
                continue;
            }
            uint8_t * p = context.frame + context.payload_index;
            harden_write_message( p, bytes, HARDEN_KIND_BURST, item.client_id, item.connection, slot.next[HARDEN_KIND_BURST] );
            const int length = rocketnet_message_single_finish( p, context.payload_capacity, bytes );
            if ( length <= 0 )
            {
                rocketnet_server_transport_abort_packet( pool->transport, &context );
                stats.finish_refused++;
                continue;
            }
            if ( rocketnet_server_transport_finish_packet( pool->transport, &context, length ) <= 0 )
            {
                // dropped, not sent: the sequence is not taken, as a refused start on main
                stats.finish_refused++;
                continue;
            }
            slot.next[HARDEN_KIND_BURST]++;
            stats.messages++;
        }
    }
    stats.busy += harden_now() - began;
}

static void harden_fill_job_main( harden_fill_pool_t * pool, int job )
{
#if defined( __linux__ )
    // named, so a profile or ps -L finds the fill jobs
    char name[16];
    snprintf( name, sizeof( name ), "harden_fill%d", job );
    pthread_setname_np( pthread_self(), name );
#endif // #if defined( __linux__ )
    uint64_t seen = 0;
    for ( ;; )
    {
        {
            std::unique_lock<std::mutex> lock( pool->mutex );
            pool->start_cv.wait( lock, [&]
                                 { return pool->quit || pool->generation != seen; } );
            if ( pool->quit )
            {
                return;
            }
            seen = pool->generation;
        }
        harden_fill_range( pool, job );
        {
            std::lock_guard<std::mutex> lock( pool->mutex );
            pool->remaining--;
            if ( pool->remaining == 0 )
            {
                pool->done_cv.notify_one();
            }
        }
    }
}

// one fill phase: wake every job on the plan and wait for all of them
static void harden_fill_run( harden_fill_pool_t * pool, const std::vector<harden_fill_item_t> & plan )
{
    std::unique_lock<std::mutex> lock( pool->mutex );
    pool->plan = &plan;
    pool->remaining = (int) pool->jobs.size();
    pool->generation++;
    pool->start_cv.notify_all();
    pool->done_cv.wait( lock, [&]
                        { return pool->remaining == 0; } );
}

static void harden_fill_stop( harden_fill_pool_t * pool )
{
    {
        std::lock_guard<std::mutex> lock( pool->mutex );
        pool->quit = true;
    }
    pool->start_cv.notify_all();
    for ( std::thread & t : pool->threads )
    {
        t.join();
    }
    pool->threads.clear();
}

// ---------------------------------------------------------------- the space frame shape
//
// --frame-shape space runs space game's server frame (Apps/Server/Server.cpp,
// server_update_internal, at the framework import e3c684e; the record is
// SPACE-FRAME-SHAPE.md), one step for each of its steps:
//
//   join (S:588-601)       the main thread waits for last frame's network task,
//                          resets the pool, swaps the frame's buffers
//   first half (S:603-641) on the main thread: send_packets (last frame's
//                          staged packets leave), update, receive every batch
//                          and copy the inputs into game state
//   world (S:643-961)      on the main thread, going wide on the game threads:
//                          the reliable messages and a second send_packets
//                          (S:678-712), the main thread's own world work
//                          (--world-ms), the stand-in game jobs it waits on
//                          (--game-load), the snapshot (S:811-822): the frame's
//                          player plan, into its slot of the frame ring
//   split (S:963-971)      AddTask( the network task, this frame's plan ); the
//                          main thread carries on to the end of its frame
//   the task (S:91-127)    writes every player's packets as jobs across all
//                          the game threads into the game's own staging
//                          (PacketWriter WritePlayerPackets), waits for them,
//                          then stages each through rocketnet
//                          (start_message/finish_message, PacketWriter
//                          SendPackets). Space also builds baseline, delta and
//                          explosion packets between the add and the wait; the
//                          load test has none
//   the tail (S:1313-1369) the main thread paces to the frame edge
//
// The packets a frame stages leave at the next frame's send_packets: one
// frame of send latency, the price the owner accepts. The task is the
// server's only user from the split to the join (Server.cpp's comment at
// S:97-101), so this is the public API one thread at a time. Unlike space's
// loop, the per-second report reads the server after the join, not in the
// tail beside the task (S:1266-1309 read it beside the task).

struct harden_stage_t
{
    std::vector<uint8_t> data; // the player's messages this frame, back to back: game memory, not the library's
    int lengths[HARDEN_MAX_SIZES * 8] = {};
    int count = 0;
};

#define HARDEN_SPACE_RING 2 // the frame ring of plans (space's snapshots: frame % 128, SnapshotManager.h)

struct harden_space_t
{
    ThreadPool pool;
    struct rocketnet_server_t * server = NULL;
    const harden_options_t * o = NULL;
    std::vector<harden_server_slot_t> * slots = NULL;
    std::vector<harden_fill_item_t> plans[HARDEN_SPACE_RING];
    const std::vector<harden_fill_item_t> * plan = NULL; // the task's frame
    std::vector<harden_stage_t> stage;
    Task * task = NULL;
    // written by the task, read by the main thread after the join
    double task_began = 0, task_ended = 0, commit_seconds = 0;
    uint64_t messages = 0, refused = 0;
    uint64_t tick = 0; // the frame the task writes, for the reliable trickle each job writes with its own players
    struct alignas( 64 ) busy_t
    {
        double seconds = 0;
    };
    busy_t write_busy[HARDEN_MAX_THREADS]; // by enkiTS thread index: one writer per entry
    busy_t game_busy[HARDEN_MAX_THREADS];
    // every piece of rocketnet's work (and the packet writing) by thread
    // index, 0 the main thread: the even-spread report
    busy_t net_busy[HARDEN_MAX_THREADS];
    busy_t range_busy[HARDEN_MAX_THREADS]; // net_busy's range-job part
    struct alignas( 64 ) count_t
    {
        uint64_t count = 0;
    };
    count_t wide_messages[HARDEN_MAX_THREADS]; // --space-send wide: messages each thread queued
    count_t wide_refused[HARDEN_MAX_THREADS];
    double game_item_seconds = 0;
    double task_update_seconds = 0, task_send_seconds = 0; // --space-net task: the task's send_packets and update
    double task_sent_at = 0;                               // --space-net task: when the task's send_packets began
    double update_done_at = 0;                             // --space-net task: when the task's update finished (the batch the next receive reads)
};

// the per-client part of the server update, over a range of client slots:
// a plain range call any job system can make (rocketnet_server_update_client)
static void harden_space_update_clients( platform_task_args_t * args )
{
    harden_space_t * space = (harden_space_t *) args->data;
    const double began = harden_now();
    for ( int i = args->start; i < args->finish; i++ )
    {
        (void) rocketnet_server_update_client( space->server, i );
    }
    space->net_busy[args->thread_index].seconds += harden_now() - began;
    space->range_busy[args->thread_index].seconds += harden_now() - began;
}

// the server update, whole or split into receive, the client range jobs and
// finish. Returns the seconds the calling thread spent in its own serial part
// (the whole update, or receive plus finish); the jobs count their own
static double harden_space_update( harden_space_t * space, double time )
{
    const double began = harden_now();
    if ( !space->o->space_update_split )
    {
        rocketnet_server_update( space->server, time );
        return harden_now() - began;
    }
    rocketnet_server_update_receive( space->server, time );
    const double received = harden_now();
    Task * clients = space->pool.AddTask( harden_space_update_clients, space, space->o->max_clients );
    if ( clients )
    {
        clients->Wait();
    }
    const double finishing = harden_now();
    rocketnet_server_update_finish( space->server );
    return ( received - began ) + ( harden_now() - finishing );
}

static void harden_spin_for( double seconds )
{
    const double until = harden_now() + seconds;
    while ( harden_now() < until )
    {
    }
}

// PacketWriter's WritePlayerPacketThreadFunction: a range of players, each
// written into its own staging, nothing of the library's touched
static void harden_space_write_players( platform_task_args_t * args )
{
    harden_space_t * space = (harden_space_t *) args->data;
    const harden_options_t & o = *space->o;
    const std::vector<harden_fill_item_t> & plan = *space->plan;
    const double began = harden_now();
    if ( o.space_send_wide )
    {
        // departure 1 (wide): each job starts and finishes its own players'
        // messages, the packet built and sealed on this thread; no staging,
        // no serial commit. A refusal ends that player's frame, so its
        // sequences stay contiguous
        uint64_t messages = 0, refused = 0;
        for ( int k = args->start; k < args->finish; k++ )
        {
            const harden_fill_item_t & item = plan[(size_t) k];
            harden_server_slot_t & slot = ( *space->slots )[(size_t) item.slot];
            if ( o.reliable_every > 0 && space->tick % (uint64_t) o.reliable_every == 0 )
            {
                // the reliable trickle: written here, by the thread that owns this player this frame, because the
                // library's fill phase gives a player to one thread (main writing it first refuses the jobs' writes)
                uint8_t * r = rocketnet_server_start_message( space->server, (uint64_t) item.slot, HARDEN_CHANNEL_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, o.reliable_bytes );
                if ( r )
                {
                    harden_write_message( r, o.reliable_bytes, HARDEN_KIND_RELIABLE, item.client_id, item.connection, slot.next[HARDEN_KIND_RELIABLE]++ );
                    if ( rocketnet_server_finish_client_message( space->server, item.slot, o.reliable_bytes ) != 0 )
                    {
                        messages++;
                    }
                }
                else
                {
                    refused++;
                }
            }
            for ( int n = 0; n < item.count; n++ )
            {
                const uint32_t sequence = slot.next[HARDEN_KIND_BURST];
                const int bytes = o.sizes[sequence % (uint32_t) o.num_sizes];
                uint8_t * p = rocketnet_server_start_message( space->server, (uint64_t) item.slot, HARDEN_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
                if ( !p )
                {
                    refused += (uint64_t) ( item.count - n );
                    break;
                }
                harden_write_message( p, bytes, HARDEN_KIND_BURST, item.client_id, item.connection, sequence );
                if ( rocketnet_server_finish_client_message( space->server, item.slot, bytes ) == 0 )
                {
                    refused += (uint64_t) ( item.count - n );
                    break;
                }
                slot.next[HARDEN_KIND_BURST]++;
                messages++;
            }
        }
        space->wide_messages[args->thread_index].count += messages;
        space->wide_refused[args->thread_index].count += refused;
        space->net_busy[args->thread_index].seconds += harden_now() - began;
        space->range_busy[args->thread_index].seconds += harden_now() - began;
        return;
    }
    for ( int k = args->start; k < args->finish; k++ )
    {
        const harden_fill_item_t & item = plan[(size_t) k];
        const harden_server_slot_t & slot = ( *space->slots )[(size_t) item.slot];
        harden_stage_t & stage = space->stage[(size_t) item.slot];
        size_t offset = 0;
        stage.count = 0;
        for ( int n = 0; n < item.count && n < (int) ( sizeof( stage.lengths ) / sizeof( stage.lengths[0] ) ); n++ )
        {
            const uint32_t sequence = slot.next[HARDEN_KIND_BURST] + (uint32_t) n;
            const int bytes = o.sizes[sequence % (uint32_t) o.num_sizes];
            harden_write_message( stage.data.data() + offset, bytes, HARDEN_KIND_BURST, item.client_id, item.connection, sequence );
            stage.lengths[n] = bytes;
            stage.count++;
            offset += (size_t) bytes;
        }
    }
    space->write_busy[args->thread_index].seconds += harden_now() - began;
    space->net_busy[args->thread_index].seconds += harden_now() - began;
    space->range_busy[args->thread_index].seconds += harden_now() - began;
}

// the network task, space's AsyncThreadFunction
static void harden_space_task( platform_task_args_t * args )
{
    harden_space_t * space = (harden_space_t *) args->data;
    space->task_began = harden_now();
    Task * players = space->plan->empty() ? NULL : space->pool.AddTask( harden_space_write_players, space, (int) space->plan->size() );
    if ( players )
    {
        players->Wait();
    }
    // PacketWriter SendPackets: every staged packet committed on this thread,
    // the server's only user until the next frame joins this task
    const double commit_began = harden_now();
    for ( const harden_fill_item_t & item : *space->plan )
    {
        if ( space->o->space_send_wide )
        {
            break; // the jobs queued every message themselves
        }
        harden_server_slot_t & slot = ( *space->slots )[(size_t) item.slot];
        const harden_stage_t & stage = space->stage[(size_t) item.slot];
        size_t offset = 0;
        for ( int n = 0; n < stage.count; n++ )
        {
            const int bytes = stage.lengths[n];
            uint8_t * p = rocketnet_server_start_message( space->server, (uint64_t) item.slot, HARDEN_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
            if ( !p )
            {
                // the rest of this player's frame is dropped, so its sequences stay contiguous
                space->refused += (uint64_t) ( stage.count - n );
                break;
            }
            memcpy( p, stage.data.data() + offset, (size_t) bytes );
            rocketnet_server_finish_message( space->server, bytes );
            slot.next[HARDEN_KIND_BURST]++;
            space->messages++;
            offset += (size_t) bytes;
        }
    }
    const double committed = harden_now();
    space->commit_seconds = committed - commit_began;
    space->net_busy[args->thread_index].seconds += committed - commit_began;
    if ( space->o->space_net_in_task )
    {
        // departure 3: the frame's packets leave now, and the next frame's
        // update runs here too, so the main thread's first half is the read
        // of what this update received
        space->task_sent_at = committed;
        rocketnet_server_send_packets( space->server );
        const double sent = harden_now();
        space->task_send_seconds = sent - committed;
        const double serial = harden_space_update( space, sent );
        space->task_update_seconds = serial;
        space->update_done_at = harden_now();
        space->net_busy[args->thread_index].seconds += ( sent - committed ) + serial;
    }
    space->task_ended = harden_now();
}

// the stand-in game: jobs that each hold a core for a slice of the frame
static void harden_game_job( platform_task_args_t * args )
{
    harden_space_t * space = (harden_space_t *) args->data;
    const double began = harden_now();
    for ( int k = args->start; k < args->finish; k++ )
    {
        harden_spin_for( space->game_item_seconds );
    }
    space->game_busy[args->thread_index].seconds += harden_now() - began;
}

// the world update's stand-in, the same in both shapes: the game jobs are
// handed to the game threads first and the main thread does its own share
// while they run (as space's main thread does beside its object and collision
// jobs), then waits for them: the update takes the longer of the two, not the
// sum
static void harden_world_update( harden_space_t * space, const harden_options_t & o )
{
    Task * game = NULL;
    if ( o.game_load > 0.0 && space->pool.NumThreads() > 1 )
    {
        const int game_threads = space->pool.NumThreads() - 1;
        const int items = 8 * space->pool.NumThreads();
        space->game_item_seconds = o.game_load * ( 1.0 / o.hz ) * (double) game_threads / (double) items;
        game = space->pool.AddTask( harden_game_job, space, items );
    }
    harden_spin_for( o.world_ms * 1e-3 );
    if ( game )
    {
        game->Wait();
    }
}

static double harden_space_take_busy( harden_space_t::busy_t * busy )
{
    double sum = 0;
    for ( int i = 0; i < HARDEN_MAX_THREADS; i++ )
    {
        sum += busy[i].seconds;
        busy[i].seconds = 0;
    }
    return sum;
}

static void harden_print_resources( const char * tag, double t, const process_resources_t & r )
{
    printf( "harden_sample: tag=%s t=%.0f", tag, t );
    for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
    {
        printf( " %s=%" PRId64, process_resources_name( i ), r.values[i] );
    }
    printf( " library_bytes=%" PRId64, harden_library_bytes() );
}

// the server main thread's phase of the frame, for the profile to tag each
// sample with: one relaxed store at each boundary the tick already times
enum harden_main_phase_t
{
    HARDEN_MAIN_OTHER = 0,
    HARDEN_MAIN_UPDATE,
    HARDEN_MAIN_RECEIVE,
    HARDEN_MAIN_WRITE,
    HARDEN_MAIN_SEND,
    HARDEN_MAIN_SLEEP,
    HARDEN_MAIN_PHASES
};

static std::atomic<int> harden_main_phase{ HARDEN_MAIN_OTHER };

// THE WINDOWS PROFILE (--profile-after A --profile-seconds S, Windows only):
// the images the Windows rows run on carry no sampling profiler, so the
// server samples itself. A seconds into the run a profiler thread lists the
// process's threads with their start routines and CPU times, then for S
// seconds stops the main thread about once a millisecond, reads its program
// counter and lets it go; at the end it names each address through DbgHelp
// (loaded at run time, so nothing links it) and writes --profile-out: every
// thread's share of a core over the window with its start routine, and the
// main thread's flat profile (the function each sample stopped in, as perf
// report --no-children), whole and per phase of the frame (update, receive,
// write, send, sleep, other), each sample tagged with the phase main was in.
// Threads are listed with their creation time from the process's, so threads
// that share a start routine (the transport's lane workers, made in lane
// order) can be told apart. Each stop costs the main thread a few microseconds.
#if defined( _WIN32 )
#include <tlhelp32.h>
#include <dbghelp.h>

struct harden_profile_symbol_t
{
    SYMBOL_INFO info;
    char name[512];
};

typedef LONG( NTAPI * harden_nt_query_thread_t )( HANDLE, int, PVOID, ULONG, PULONG );
typedef DWORD( WINAPI * harden_sym_set_options_t )( DWORD );
typedef BOOL( WINAPI * harden_sym_initialize_t )( HANDLE, PCSTR, BOOL );
typedef BOOL( WINAPI * harden_sym_from_addr_t )( HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO );
typedef BOOL( WINAPI * harden_sym_cleanup_t )( HANDLE );

struct harden_profile_thread_t
{
    DWORD id = 0;
    HANDLE handle = NULL;
    uint64_t start_address = 0;
    uint64_t cpu_start = 0;
    uint64_t cpu_end = 0;
    uint64_t created = 0; // 100 ns units since the process was created
};

static uint64_t harden_profile_filetime( const FILETIME & t )
{
    return ( (uint64_t) t.dwHighDateTime << 32 ) | t.dwLowDateTime;
}

static uint64_t harden_profile_created( HANDLE thread )
{
    FILETIME created, exited, kernel, user;
    FILETIME process_created, process_exited, process_kernel, process_user;
    if ( !GetThreadTimes( thread, &created, &exited, &kernel, &user ) ||
         !GetProcessTimes( GetCurrentProcess(), &process_created, &process_exited, &process_kernel, &process_user ) )
    {
        return 0;
    }
    const uint64_t t = harden_profile_filetime( created );
    const uint64_t p = harden_profile_filetime( process_created );
    return t > p ? t - p : 0;
}

static uint64_t harden_profile_cpu( HANDLE thread )
{
    FILETIME created, exited, kernel, user;
    if ( !GetThreadTimes( thread, &created, &exited, &kernel, &user ) )
    {
        return 0;
    }
    const uint64_t k = ( (uint64_t) kernel.dwHighDateTime << 32 ) | kernel.dwLowDateTime;
    const uint64_t u = ( (uint64_t) user.dwHighDateTime << 32 ) | user.dwLowDateTime;
    return k + u; // 100 ns units
}

static double harden_profile_clock()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter( &now );
    QueryPerformanceFrequency( &frequency );
    return (double) now.QuadPart / (double) frequency.QuadPart;
}

struct harden_profile_t
{
    double after = 0.0;
    double seconds = 0.0;
    std::string out;
    DWORD main_id = 0;
    std::atomic<bool> stop{ false };
    std::thread thread;

    ~harden_profile_t()
    {
        stop.store( true );
        if ( thread.joinable() )
        {
            thread.join();
        }
    }
};

static void harden_profile_run( harden_profile_t * p )
{
    const double begun = harden_profile_clock();
    while ( harden_profile_clock() - begun < p->after )
    {
        if ( p->stop.load() )
        {
            return;
        }
        Sleep( 50 );
    }
    FILE * f = rocketnet_test_fopen( p->out.c_str(), "w" );
    if ( !f )
    {
        return;
    }
    const HMODULE ntdll = GetModuleHandleW( L"ntdll.dll" );
    const harden_nt_query_thread_t nt_query = ntdll ? reinterpret_cast<harden_nt_query_thread_t>( reinterpret_cast<void *>( GetProcAddress( ntdll, "NtQueryInformationThread" ) ) ) : NULL;

    // every thread of this process but the profiler, with its start routine
    std::vector<harden_profile_thread_t> threads;
    HANDLE main_thread = NULL;
    const HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if ( snapshot != INVALID_HANDLE_VALUE )
    {
        THREADENTRY32 entry;
        entry.dwSize = sizeof( entry );
        for ( BOOL more = Thread32First( snapshot, &entry ); more; more = Thread32Next( snapshot, &entry ) )
        {
            if ( entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId() )
            {
                continue;
            }
            harden_profile_thread_t t;
            t.id = entry.th32ThreadID;
            t.handle = OpenThread( THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t.id );
            if ( !t.handle )
            {
                continue;
            }
            if ( nt_query )
            {
                PVOID start = NULL;
                const int thread_query_set_win32_start_address = 9;
                if ( nt_query( t.handle, thread_query_set_win32_start_address, &start, sizeof( start ), NULL ) == 0 )
                {
                    t.start_address = (uint64_t) (uintptr_t) start;
                }
            }
            t.cpu_start = harden_profile_cpu( t.handle );
            t.created = harden_profile_created( t.handle );
            if ( t.id == p->main_id )
            {
                main_thread = t.handle;
            }
            threads.push_back( t );
        }
        CloseHandle( snapshot );
    }

    // the main thread's program counter, about once a millisecond, with the
    // phase of the frame it was in
    std::vector<std::pair<int, uint64_t>> samples;
    const double window_start = harden_profile_clock();
    double next = window_start;
    while ( main_thread && harden_profile_clock() - window_start < p->seconds && !p->stop.load() )
    {
        while ( harden_profile_clock() < next )
        {
            // spin: Sleep() resolves to the timer tick, about 15.6 ms
        }
        next += 0.001;
        if ( SuspendThread( main_thread ) == (DWORD) -1 )
        {
            break;
        }
        CONTEXT context;
        memset( &context, 0, sizeof( context ) );
        context.ContextFlags = CONTEXT_CONTROL;
        const int phase = harden_main_phase.load( std::memory_order_relaxed );
        if ( GetThreadContext( main_thread, &context ) )
        {
#if defined( _M_ARM64 )
            samples.push_back( std::make_pair( phase, (uint64_t) context.Pc ) );
#else
            samples.push_back( std::make_pair( phase, (uint64_t) context.Rip ) );
#endif // #if defined( _M_ARM64 )
        }
        ResumeThread( main_thread );
    }
    const double window = harden_profile_clock() - window_start;
    for ( harden_profile_thread_t & t : threads )
    {
        t.cpu_end = harden_profile_cpu( t.handle );
    }

    // names, through DbgHelp loaded at run time
    harden_sym_from_addr_t sym_from_addr = NULL;
    const HMODULE dbghelp = LoadLibraryW( L"dbghelp.dll" );
    if ( dbghelp )
    {
        const harden_sym_set_options_t sym_set_options = reinterpret_cast<harden_sym_set_options_t>( reinterpret_cast<void *>( GetProcAddress( dbghelp, "SymSetOptions" ) ) );
        const harden_sym_initialize_t sym_initialize = reinterpret_cast<harden_sym_initialize_t>( reinterpret_cast<void *>( GetProcAddress( dbghelp, "SymInitialize" ) ) );
        sym_from_addr = reinterpret_cast<harden_sym_from_addr_t>( reinterpret_cast<void *>( GetProcAddress( dbghelp, "SymFromAddr" ) ) );
        if ( sym_set_options && sym_initialize && sym_from_addr )
        {
            sym_set_options( 0x00000002 /* SYMOPT_UNDNAME */ | 0x00000004 /* SYMOPT_DEFERRED_LOADS */ );
            if ( !sym_initialize( GetCurrentProcess(), NULL, TRUE ) )
            {
                sym_from_addr = NULL;
            }
        }
        else
        {
            sym_from_addr = NULL;
        }
    }
    auto name_of = [&]( uint64_t address ) -> std::string
    {
        if ( sym_from_addr && address )
        {
            harden_profile_symbol_t symbol;
            memset( &symbol, 0, sizeof( symbol ) );
            symbol.info.SizeOfStruct = sizeof( SYMBOL_INFO );
            symbol.info.MaxNameLen = sizeof( symbol.name );
            DWORD64 displacement = 0;
            if ( sym_from_addr( GetCurrentProcess(), address, &displacement, &symbol.info ) )
            {
                return std::string( symbol.info.Name );
            }
        }
        char hex[32];
        snprintf( hex, sizeof( hex ), "0x%" PRIx64, address );
        return std::string( hex );
    };

    std::sort( threads.begin(), threads.end(), []( const harden_profile_thread_t & a, const harden_profile_thread_t & b )
               { return a.cpu_end - a.cpu_start > b.cpu_end - b.cpu_start; } );
    fprintf( f, "harden_profile: window_seconds=%.2f main_samples=%zu threads=%zu symbols=%s\n", window, samples.size(), threads.size(), sym_from_addr ? "dbghelp" : "none" );
    for ( const harden_profile_thread_t & t : threads )
    {
        const double share = window > 0.0 ? (double) ( t.cpu_end - t.cpu_start ) / ( window * 1.0e7 ) * 100.0 : 0.0;
        fprintf( f, "harden_profile_thread: id=%lu cpu=%.1f%% main=%d created_ms=%.1f start=%s\n", (unsigned long) t.id, share, t.id == p->main_id ? 1 : 0, (double) t.created / 1.0e4,
                 name_of( t.start_address ).c_str() );
    }

    // the flat profile of the samples in one phase (or every phase, -1)
    const char * phase_names[HARDEN_MAIN_PHASES] = { "other", "update", "receive", "write", "send", "sleep" };
    auto flat = [&]( int phase, std::vector<std::pair<std::string, int>> * counts ) -> int
    {
        std::vector<std::string> names;
        names.reserve( samples.size() );
        for ( const std::pair<int, uint64_t> & sample : samples )
        {
            if ( phase < 0 || sample.first == phase )
            {
                names.push_back( name_of( sample.second ) );
            }
        }
        std::sort( names.begin(), names.end() );
        for ( size_t i = 0; i < names.size(); )
        {
            size_t j = i;
            while ( j < names.size() && names[j] == names[i] )
            {
                j++;
            }
            counts->push_back( std::make_pair( names[i], (int) ( j - i ) ) );
            i = j;
        }
        std::sort( counts->begin(), counts->end(), []( const std::pair<std::string, int> & a, const std::pair<std::string, int> & b )
                   { return a.second > b.second; } );
        return (int) names.size();
    };
    for ( int phase = 0; phase < HARDEN_MAIN_PHASES; phase++ )
    {
        int n = 0;
        for ( const std::pair<int, uint64_t> & sample : samples )
        {
            n += sample.first == phase ? 1 : 0;
        }
        fprintf( f, "harden_profile_phase: %s %.2f%% samples=%d\n", phase_names[phase], samples.empty() ? 0.0 : 100.0 * n / (double) samples.size(), n );
    }
    {
        std::vector<std::pair<std::string, int>> counts;
        const int n = flat( -1, &counts );
        for ( size_t i = 0; i < counts.size() && i < 60; i++ )
        {
            fprintf( f, "harden_profile_main: %6.2f%% %s\n", n ? 100.0 * counts[i].second / (double) n : 0.0, counts[i].first.c_str() );
        }
    }
    for ( int phase = HARDEN_MAIN_UPDATE; phase <= HARDEN_MAIN_SEND; phase++ )
    {
        std::vector<std::pair<std::string, int>> counts;
        const int n = flat( phase, &counts );
        for ( size_t i = 0; i < counts.size() && i < 30; i++ )
        {
            fprintf( f, "harden_profile_in: phase=%s %6.2f%% %s\n", phase_names[phase], n ? 100.0 * counts[i].second / (double) n : 0.0, counts[i].first.c_str() );
        }
    }
    fclose( f );
    for ( harden_profile_thread_t & t : threads )
    {
        CloseHandle( t.handle );
    }

    // give back what DbgHelp opened, and stay alive until the server ends:
    // the resource judgement compares handles and threads from the
    // post-warmup sample to the end, and the profile must not move either
    if ( dbghelp )
    {
        const harden_sym_cleanup_t sym_cleanup = reinterpret_cast<harden_sym_cleanup_t>( reinterpret_cast<void *>( GetProcAddress( dbghelp, "SymCleanup" ) ) );
        if ( sym_from_addr && sym_cleanup )
        {
            sym_cleanup( GetCurrentProcess() );
        }
        FreeLibrary( dbghelp );
    }
    while ( !p->stop.load() )
    {
        Sleep( 50 );
    }
}
#endif // #if defined( _WIN32 )

static int harden_server_main( const harden_options_t & o )
{
    const char * role = "server";
    static const uint8_t dev_backend_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

    struct rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = o.protocol_id;
    config.max_clients = o.max_clients;
    config.per_client_send_kbps = ROCKETNET_SEND_KBPS_UNLIMITED;
    config.handshake_deadline_seconds = 10;
    if ( o.num_xdp_queues > 0 )
    {
        config.num_xdp_queues = o.num_xdp_queues;
    }
    memcpy( config.backend_public_key, dev_backend_key, sizeof( config.backend_public_key ) );
    if ( !o.transport.empty() )
    {
        config.transport = o.transport.c_str();
    }

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
#if defined( _WIN32 ) && defined( ROCKETNET_SERVER_RIO_TEST_LANES_MADE )
    const uint64_t rio_lanes_before = rocketnet_server_rio_test_lanes_made();
#endif // #if defined( _WIN32 ) && defined( ROCKETNET_SERVER_RIO_TEST_LANES_MADE )
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, o.address.c_str(), NULL, &config, &status );
    if ( !server || rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "harden_load server: create or start failed (status %d)\n", (int) status );
        printf( "harden_verdict: role=server verdict=FAIL reason=create\n" );
        return 1;
    }
#if defined( _WIN32 ) && defined( ROCKETNET_SERVER_RIO_TEST_LANES_MADE )
    // the lanes a rio server really made: the probe's delta across start
    // must be the lanes asked for (--xdp-queues, at least one)
    if ( strcmp( rocketnet_server_transport_name( server ), "rio" ) == 0 )
    {
        const uint64_t made = rocketnet_server_rio_test_lanes_made() - rio_lanes_before;
        const int asked = o.num_xdp_queues > 0 ? o.num_xdp_queues : 1;
        printf( "harden_rio_lanes: asked=%d made=%" PRIu64 "\n", asked, made );
        if ( made != (uint64_t) asked )
        {
            printf( "harden_verdict: role=server verdict=FAIL reason=rio_lanes asked=%d made=%" PRIu64 "\n", asked, made );
            return 1;
        }
    }
#endif // #if defined( _WIN32 ) && defined( ROCKETNET_SERVER_RIO_TEST_LANES_MADE )
    const char * granted = rocketnet_server_transport_name( server );
    if ( !o.transport.empty() && strcmp( granted, o.transport.c_str() ) != 0 )
    {
        printf( "harden_verdict: role=server verdict=FAIL reason=transport granted=%s pinned=%s\n", granted, o.transport.c_str() );
        return 1;
    }
    uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, key );
    char key_hex[2 * ROCKETNET_SERVER_PUBLIC_KEY_BYTES + 1];
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        snprintf( key_hex + 2 * i, 3, "%02x", key[i] );
    }
    char utc[32];
    harden_utc( utc, sizeof( utc ) );
#if defined( _WIN32 )
    harden_profile_t profile;
    if ( o.profile_seconds > 0.0 )
    {
        profile.after = o.profile_after;
        profile.seconds = o.profile_seconds;
        profile.out = o.profile_out;
        profile.main_id = GetCurrentThreadId();
        profile.thread = std::thread( harden_profile_run, &profile );
    }
#else
    if ( o.profile_seconds > 0.0 )
    {
        printf( "harden_profile: unavailable here (Windows only; perf samples Linux)\n" );
    }
#endif // #if defined( _WIN32 )
    bool warm_touched = false;
    // the soak's warm-up touch, before any player joins: run after the
    // players are in, it stalled the main thread for seconds (a 0.7% loss)
    {
        const int64_t touched = process_resources_touch_all();
        printf( "harden_sample: tag=touch bytes=%" PRId64 " before_join=1\n", touched );
        warm_touched = true;
    }
    printf( "harden_ready: address=%s port=%d transport=%s key=%s utc=%s max_clients=%d hz=%.0f base=%d burst=%d burst_clients=%d fill_jobs=%d load=%s payload=%s reliable_every=%d reliable_bytes=%d big_every=%d big_bytes=%d sizes=",
            o.address.c_str(), (int) rocketnet_server_port( server ), granted, key_hex, utc, o.max_clients, o.hz, o.base, o.burst, o.burst_clients, o.fill_jobs, o.load.empty() ? "blocking-default" : o.load.c_str(), o.payload_copy ? "copy" : "pattern",
            o.reliable_every, o.reliable_bytes, o.big_every, o.big_bytes );
    for ( int i = 0; i < o.num_sizes; i++ )
    {
        printf( "%s%d", i ? "," : "", o.sizes[i] );
    }
    printf( "\n" );
    fflush( stdout );

    struct rocketnet_message_server_t * message_server = rocketnet_server_message_server( server );
    struct rocketnet_server_transport_t * transport = message_server ? rocketnet_message_server_transport( message_server ) : NULL;

    std::vector<harden_server_slot_t> slots( (size_t) o.max_clients );
    harden_fill_pool_t pool;
    std::vector<harden_fill_item_t> plan;
    if ( o.fill_jobs > 0 )
    {
        if ( !transport )
        {
            printf( "harden_verdict: role=server verdict=FAIL reason=fill_jobs_need_transport\n" );
            return 1;
        }
        pool.transport = transport;
        pool.o = &o;
        pool.slots = &slots;
        pool.jobs.resize( (size_t) o.fill_jobs );
        for ( int j = 0; j < o.fill_jobs; j++ )
        {
            pool.threads.emplace_back( harden_fill_job_main, &pool, j );
        }
        plan.reserve( (size_t) o.max_clients );
    }
    // the game's ThreadPool (space's), in either shape when --game-threads is set
    harden_space_t space;
    if ( o.game_threads > 0 )
    {
        space.pool.Initialize( o.game_threads, o.pin_first_cpu );
        space.server = server;
        space.o = &o;
        space.slots = &slots;
        int largest = 0;
        for ( int i = 0; i < o.num_sizes; i++ )
        {
            largest = std::max( largest, o.sizes[i] );
        }
        const int most = std::max( o.base, o.burst );
        space.stage.resize( (size_t) o.max_clients );
        for ( harden_stage_t & s : space.stage )
        {
            s.data.resize( (size_t) ( most * largest ) );
        }
        for ( std::vector<harden_fill_item_t> & p : space.plans )
        {
            p.reserve( (size_t) o.max_clients );
        }
        printf( "harden_pool: shape=%s game_threads=%d first_cpu=%d pinned=%d realtime=%d world_ms=%.2f game_load=%.2f\n", o.space_shape ? "space" : "blocking", o.game_threads, o.pin_first_cpu,
                harden_pin.pinned.load(), harden_pin.realtime.load(), o.world_ms, o.game_load );
        fflush( stdout );
    }
    double last_plan_time = 0.0; // space: when the frame the task staged was built from game state
    double last_world_end = 0.0; // space: when the last world update finished (--space-split top: the state the task sends)
    harden_verify_t verify;
    harden_frame_window_t w;
    uint64_t total_messages = 0, total_start_refused = 0, total_frames = 0, total_overruns = 0;
    std::vector<double> all_work;
    all_work.reserve( (size_t) ( o.seconds * o.hz ) + 16 );
    double all_work_max = 0.0;

    const double step = 1.0 / o.hz;
    const double started = harden_now();
    double next = started;
    double last_report = started;
    double last_sample = started;
    uint64_t tick = 0;
    uint64_t last_packets_sent = 0;
    int size_cursor = 0;
    bool resources_started = false;
    process_resources_t resources_start, resources_peak;
    int64_t library_start = 0, library_peak = 0;
    double resources_started_at = 0.0;

    bool draining = false;
    std::vector<uint8_t> big( (size_t) o.big_bytes );

    // the end is two cuts, one per direction (the method of load's
    // counters_agree), each read while its direction is silent. An update
    // sends keep-alives, so silence is a frozen sender: no update, no send.
    //   T0 end_at        the clients freeze; the server stops writing and
    //                    runs on, so client to server drains
    //   T1 T0 + q        client to server cut: every role reads its terms;
    //                    then the server freezes
    //   T2 T1 + q/2      the clients run again, update and receive only, so
    //                    server to client drains into them
    //   T3 T2 + q        server to client cut: every role reads its terms
    //   T4 T3 + q/2      the cleanup: the server stops, the clients leave
    // The cleanup's traffic is outside the equations.
    const double end_at = o.end_at > 0.0 ? o.end_at : harden_wall() + o.seconds;
    const double cut_c2s_at = end_at + o.quiet_seconds;
    const double cut_s2c_at = cut_c2s_at + 1.5 * o.quiet_seconds;
    const double cleanup_at = cut_s2c_at + 0.5 * o.quiet_seconds;
    // one client's reliable ordered and big messages, on main through the
    // message server (after the join when the jobs fill)
    auto write_reliable_and_big = [&]( int i, uint64_t id, uint32_t connection, harden_server_slot_t & slot )
    {
        if ( o.reliable_every > 0 && tick % (uint64_t) o.reliable_every == 0 )
        {
            uint8_t * p = rocketnet_server_start_message( server, (uint64_t) i, HARDEN_CHANNEL_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, o.reliable_bytes );
            if ( p )
            {
                harden_write_message( p, o.reliable_bytes, HARDEN_KIND_RELIABLE, id, connection, slot.next[HARDEN_KIND_RELIABLE]++ );
                rocketnet_server_finish_message( server, o.reliable_bytes );
                w.messages++;
            }
            else
            {
                w.start_refused++;
            }
        }
        if ( o.big_every > 0 && ( tick + (uint64_t) i ) % (uint64_t) o.big_every == 0 )
        {
            uint8_t * p = rocketnet_server_start_message( server, (uint64_t) i, HARDEN_CHANNEL_BIG, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, o.big_bytes );
            if ( p )
            {
                harden_write_message( p, o.big_bytes, HARDEN_KIND_BIG, id, connection, slot.next[HARDEN_KIND_BIG]++ );
                rocketnet_server_finish_message( server, o.big_bytes );
                w.messages++;
            }
            else
            {
                w.start_refused++;
            }
        }
    };
    // the per-second report and the long-run samples. They read the server, so
    // in the space shape they run after the join, never beside the task
    auto report_and_sample = [&]() -> bool
    {
        const double t_now = harden_now();
        if ( t_now - last_report >= 1.0 )
        {
            struct rocketnet_server_stats_t stats;
            rocketnet_server_stats( server, &stats );
            const double span = t_now - last_report;
            const double f = w.frames ? (double) w.frames : 1.0;
            const uint64_t ring_full = transport ? rocketnet_counter_value( rocketnet_server_transport_counters( transport ), ROCKETNET_COUNTER_SEND_RING_FULL ) : 0;
            std::vector<double> work_copy = w.work;
            printf( "harden_tick: t=%.1f conn=%d frames=%" PRIu64 " overruns=%" PRIu64 " work_ms avg=%.3f p99=%.3f max=%.3f update=%.3f/%.3f receive=%.3f/%.3f write=%.3f/%.3f send=%.3f/%.3f"
                    " jobs_core_ms=%.3f msgs_ps=%.0f pkts_ps=%.0f in_msgs_ps=%.0f start_refused=%" PRIu64 " send_ring_full_total=%" PRIu64 "\n",
                    t_now - started, rocketnet_server_num_connected_clients( server ), w.frames, w.overruns,
                    1e3 * ( w.update_sum + w.receive_sum + w.write_sum + w.send_sum ) / f, 1e3 * harden_percentile( work_copy, 0.99 ), 1e3 * harden_percentile( work_copy, 1.0 ),
                    1e3 * w.update_sum / f, 1e3 * w.update_max, 1e3 * w.receive_sum / f, 1e3 * w.receive_max, 1e3 * w.write_sum / f, 1e3 * w.write_max,
                    1e3 * w.send_sum / f, 1e3 * w.send_max, 1e3 * w.jobs_busy_sum / f, (double) w.messages / span, (double) ( stats.packets_sent - last_packets_sent ) / span,
                    (double) w.received / span, w.start_refused, ring_full );
            if ( o.game_threads > 0 )
            {
                // the frame as the game sees it: main_rn is what rocketnet takes from the
                // main thread (work_ms above), task and task_core what it takes from the
                // game threads, join what the main thread waits for it, latency a frame's
                // packets from its game state to send_packets
                const double tf = w.task_frames ? (double) w.task_frames : 1.0;
                const double lf = w.latency_frames ? (double) w.latency_frames : 1.0;
                const double frame_core_ms = 1e3 * step * (double) o.game_threads;
                const double rn_core_ms = 1e3 * ( w.update_sum + w.receive_sum + w.write_sum + w.send_sum + w.task_core_sum + w.jobs_busy_sum ) / f;
                printf( "harden_frame: t=%.1f shape=%s threads=%d pinned=%d realtime=%d main_rn_ms p99=%.3f max=%.3f join_ms avg=%.3f max=%.3f task_ms avg=%.3f max=%.3f late=%" PRIu64
                        " task_core_ms avg=%.3f max=%.3f commit_ms avg=%.3f max=%.3f world_ms avg=%.3f max=%.3f game_core_ms=%.3f rn_core_ms=%.3f leftover_core_ms=%.3f latency_ms avg=%.3f max=%.3f input_age_ms avg=%.3f max=%.3f\n",
                        t_now - started, o.space_shape ? "space" : "blocking", o.game_threads, harden_pin.pinned.load(), harden_pin.realtime.load(), 1e3 * harden_percentile( work_copy, 0.99 ),
                        1e3 * harden_percentile( work_copy, 1.0 ), 1e3 * w.join_sum / f, 1e3 * w.join_max, 1e3 * w.task_sum / tf, 1e3 * w.task_max, w.task_late, 1e3 * w.task_core_sum / tf,
                        1e3 * w.task_core_max, 1e3 * w.commit_sum / tf, 1e3 * w.commit_max, 1e3 * w.world_sum / f, 1e3 * w.world_max, 1e3 * w.game_core_sum / f, rn_core_ms,
                        frame_core_ms - rn_core_ms - 1e3 * w.game_core_sum / f - 1e3 * o.world_ms * 1e-3, 1e3 * w.latency_sum / lf, 1e3 * w.latency_max, 1e3 * w.input_age_sum / ( w.input_age_frames ? (double) w.input_age_frames : 1.0 ),
                        1e3 * w.input_age_max );
                // one line per thread class: the main thread's network ms a frame
                // (wall, rocketnet's calls and the waits inside them), each game
                // thread's (busy in rocketnet's work), and main : mean game thread
                double gmin = 1e30, gmax = 0.0, gsum = 0.0;
                int gn = 0;
                for ( int i = 1; i < o.game_threads && i < HARDEN_MAX_THREADS; i++ )
                {
                    const double v = 1e3 * w.net_thread[i] / f;
                    gmin = std::min( gmin, v );
                    gmax = std::max( gmax, v );
                    gsum += v;
                    gn++;
                }
                const double gmean = gn ? gsum / gn : 0.0;
                const double main_ms = 1e3 * ( w.update_sum + w.receive_sum + w.write_sum + w.send_sum ) / f;
                printf( "harden_threads: t=%.1f send_mode=%s update_mode=%s net_mode=%s main_net_ms=%.3f main_in_jobs_ms=%.3f game_net_ms min=%.3f mean=%.3f max=%.3f ratio=%.3f"
                        " task_send_ms=%.3f task_update_ms avg=%.3f max=%.3f\n",
                        t_now - started, o.space_send_wide ? "wide" : "serial", o.space_update_split ? "split" : "main", o.space_net_in_task ? "task" : "main", main_ms,
                        1e3 * w.net_thread[0] / f, gn ? gmin : 0.0, gmean, gmax, gmean > 0.0 ? main_ms / gmean : 0.0, 1e3 * w.task_send_sum / tf, 1e3 * w.task_update_sum / tf,
                        1e3 * w.task_update_max );
                // each thread's network ms a frame, by thread index (0 = the
                // main thread's part in jobs), to see how even the split is
                printf( "harden_thread_net_ms: t=%.1f", t_now - started );
                for ( int i = 0; i < o.game_threads && i < HARDEN_MAX_THREADS; i++ )
                {
                    printf( " %.3f", 1e3 * w.net_thread[i] / f );
                }
                printf( "\n" );
                // the range jobs' part alone (no serial walk): the even-spread check
                printf( "harden_thread_range_ms: t=%.1f", t_now - started );
                for ( int i = 0; i < o.game_threads && i < HARDEN_MAX_THREADS; i++ )
                {
                    printf( " %.3f", 1e3 * w.range_thread[i] / f );
                }
                printf( "\n" );
            }
            fflush( stdout );
            last_packets_sent = stats.packets_sent;
            total_messages += w.messages;
            total_start_refused += w.start_refused;
            total_frames += w.frames;
            total_overruns += w.overruns;
            w = harden_frame_window_t();
            last_report = t_now;
        }

        // the long-run samples: the first after --warm-seconds is the
        // post-warmup sample (after the soak's warm-up touch), the rest peaks
        if ( t_now - started >= (double) o.warm_seconds && ( !resources_started || t_now - last_sample >= (double) o.sample_seconds ) )
        {
            process_resources_t r;
            if ( !resources_started && !warm_touched )
            {
                const int64_t touched = process_resources_touch_all();
                printf( "harden_sample: tag=touch bytes=%" PRId64 "\n", touched );
            }
            if ( !process_resources_read( &r ) )
            {
                printf( "harden_verdict: role=server verdict=FAIL reason=resource_read\n" );
                return false;
            }
            const char * tag = resources_started ? "peak" : "post_warmup";
            if ( !resources_started )
            {
                resources_start = r;
                resources_peak = r;
                library_start = harden_library_bytes();
                library_peak = library_start;
                resources_started = true;
                resources_started_at = t_now;
            }
            process_resources_peak( &resources_peak, &r );
            library_peak = std::max( library_peak, harden_library_bytes() );
            harden_print_resources( tag, t_now - started, r );
            printf( " counters:" );
            harden_print_counters( "transport", role, transport ? rocketnet_server_transport_counters( transport ) : NULL, false );
            printf( " message:" );
            harden_print_counters( "message", role, message_server ? rocketnet_message_server_counters( message_server ) : NULL, false );
            printf( "\n" );
            fflush( stdout );
            last_sample = t_now;
        }
        return true;
    };
    // receive until NULL, every input checked and copied into game state (the
    // slots' inbound streams): space's input parse (S:617-640)
    auto drain_receive = [&]()
    {
        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int m = 0; m < batch->num_messages; m++ )
            {
                harden_message_t header;
                const int index = batch->client_index[m];
                if ( !harden_read_message( batch->data[m], batch->bytes[m], &header ) || ( header.kind != HARDEN_KIND_INPUT && header.kind != HARDEN_KIND_INPUT_RELIABLE ) )
                {
                    verify.errors++;
                    continue;
                }
                if ( index < 0 || index >= o.max_clients || header.client_id != rocketnet_server_client_id( server, index ) )
                {
                    verify.wrong_owner++;
                    verify.errors++;
                    continue;
                }
                harden_stream_take( &slots[(size_t) index].inbound[header.kind], &header, header.kind == HARDEN_KIND_INPUT_RELIABLE, &verify );
                w.received++;
            }
        }
    };
    // the bursting clients rotate: a window of burst_clients slots among the
    // connected ones, advancing burst_clients each tick
    auto client_count = [&]( int bursting_seen, int connected_count ) -> int
    {
        int count = o.base;
        if ( o.burst_clients > 0 && connected_count > 0 )
        {
            const uint64_t window = ( tick * (uint64_t) o.burst_clients ) % (uint64_t) connected_count;
            const uint64_t position = ( (uint64_t) bursting_seen + (uint64_t) connected_count - window ) % (uint64_t) connected_count;
            if ( position < (uint64_t) o.burst_clients )
            {
                count = o.burst;
            }
        }
        return count;
    };
    for ( ;; )
    {
        const double frame_start = harden_now();
        if ( o.space_shape )
        {
            // ---- join (S:588-601): last frame's network task, then the
            // frame's buffers turn over (the plan ring by tick, as space's
            // snapshots by frame; ClearNewExplosions' A/B has no counterpart
            // here)
            const double t_join = harden_now();
            if ( space.task )
            {
                space.task->Wait();
                space.task = NULL;
                const double task_seconds = space.task_ended - space.task_began;
                const double task_core = harden_space_take_busy( space.write_busy ) + space.commit_seconds;
                w.task_sum += task_seconds;
                w.task_max = std::max( w.task_max, task_seconds );
                w.task_core_sum += task_core;
                w.task_core_max = std::max( w.task_core_max, task_core );
                w.commit_sum += space.commit_seconds;
                w.commit_max = std::max( w.commit_max, space.commit_seconds );
                w.task_frames++;
                if ( space.task_ended > t_join )
                {
                    w.task_late++; // the main thread reached the join before the task ended
                }
                w.messages += space.messages;
                w.start_refused += space.refused;
                space.messages = 0;
                space.refused = 0;
                for ( int i = 0; i < HARDEN_MAX_THREADS; i++ )
                {
                    w.messages += space.wide_messages[i].count;
                    w.start_refused += space.wide_refused[i].count;
                    space.wide_messages[i].count = 0;
                    space.wide_refused[i].count = 0;
                }
                w.task_send_sum += space.task_send_seconds;
                w.task_update_sum += space.task_update_seconds;
                w.task_update_max = std::max( w.task_update_max, space.task_update_seconds );
                space.task_send_seconds = 0;
                space.task_update_seconds = 0;
            }
            // every thread's rocketnet work since the last join (the first
            // half's update jobs included)
            for ( int i = 0; i < HARDEN_MAX_THREADS; i++ )
            {
                w.net_thread[i] += space.net_busy[i].seconds;
                space.net_busy[i].seconds = 0;
                w.range_thread[i] += space.range_busy[i].seconds;
                space.range_busy[i].seconds = 0;
            }
            const double t_joined = harden_now();
            space.pool.Reset();
            w.join_sum += t_joined - t_join;
            w.join_max = std::max( w.join_max, t_joined - t_join );
            if ( !report_and_sample() )
            {
                space.pool.Shutdown();
                return 1;
            }
            const double wall_now = harden_wall();
            if ( !draining && wall_now >= end_at )
            {
                draining = true;
                harden_utc( utc, sizeof( utc ) );
                printf( "harden_phase: role=server phase=writes_stop t=%.1f utc=%s\n", frame_start - started, utc );
                fflush( stdout );
            }
            if ( wall_now >= cut_c2s_at )
            {
                break;
            }

            // ---- first half, the main thread (S:603-641)
            const double t_first = harden_now();
            harden_main_phase.store( HARDEN_MAIN_SEND, std::memory_order_relaxed );
            if ( !o.space_net_in_task )
            {
                rocketnet_server_send_packets( server );
            }
            const double t_sent = harden_now();
            if ( last_plan_time > 0.0 )
            {
                // a frame's packets from its state to the send_packets that sends them
                const double sent_at = o.space_net_in_task ? space.task_sent_at : t_first;
                w.latency_sum += sent_at - last_plan_time;
                w.latency_max = std::max( w.latency_max, sent_at - last_plan_time );
                w.latency_frames++;
                last_plan_time = 0.0;
            }
            harden_main_phase.store( HARDEN_MAIN_UPDATE, std::memory_order_relaxed );
            if ( !o.space_net_in_task )
            {
                (void) harden_space_update( &space, frame_start );
            }
            const double t_update = harden_now();
            harden_main_phase.store( HARDEN_MAIN_RECEIVE, std::memory_order_relaxed );
            if ( o.space_net_in_task && space.update_done_at > 0.0 )
            {
                const double age = harden_now() - space.update_done_at;
                w.input_age_sum += age;
                w.input_age_max = std::max( w.input_age_max, age );
                w.input_age_frames++;
            }
            drain_receive();
            const double t_receive = harden_now();

            // ---- the world update, the main thread (S:643-961). First the
            // reliable messages and a second send_packets (S:678-712)
            harden_main_phase.store( HARDEN_MAIN_WRITE, std::memory_order_relaxed );
            std::vector<harden_fill_item_t> & frame_plan = space.plans[tick % HARDEN_SPACE_RING];
            frame_plan.clear();
            if ( !draining )
            {
                const int connected_count = rocketnet_server_num_connected_clients( server );
                int bursting_seen = 0;
                for ( int i = 0; i < o.max_clients; i++ )
                {
                    if ( !rocketnet_server_client_connected( server, i ) )
                    {
                        continue;
                    }
                    harden_server_slot_t & slot = slots[(size_t) i];
                    const uint64_t id = rocketnet_server_client_id( server, i );
                    const uint32_t connection = (uint32_t) rocketnet_server_client_connection_id( server, i );
                    if ( slot.client_id != id || slot.connection != connection )
                    {
                        slot = harden_server_slot_t();
                        slot.client_id = id;
                        slot.connection = connection;
                    }
                    frame_plan.push_back( harden_fill_item_t{ i, client_count( bursting_seen, connected_count ), id, connection } );
                    bursting_seen++;
                    if ( !o.space_send_wide )
                    {
                        write_reliable_and_big( i, id, connection, slot );
                    }
                }
            }
            const double t_reliable = harden_now();
            harden_main_phase.store( HARDEN_MAIN_SEND, std::memory_order_relaxed );
            if ( !o.space_net_in_task )
            {
                rocketnet_server_send_packets( server ); // S:711; with --space-net task the task's send_packets sends them
            }
            const double t_sent_reliable = harden_now();
            harden_main_phase.store( HARDEN_MAIN_OTHER, std::memory_order_relaxed );

            // --space-split top: the network task is split off here, before the
            // world update, and the main thread does not touch the server again
            // until the next join; the state it sends is the one the last world
            // update produced
            const bool split_here = o.space_split_top;
            if ( split_here && ( !frame_plan.empty() || o.space_net_in_task ) )
            {
                space.plan = &frame_plan;
                space.tick = tick;
                last_plan_time = last_world_end > 0.0 ? last_world_end : harden_now();
                space.task = space.pool.AddTask( harden_space_task, &space );
            }

            // the rest of the world: the main thread's own share, then the
            // stand-in game jobs across all the game threads, waited on
            const double t_world_began = harden_now();
            harden_world_update( &space, o );
            const double t_world = harden_now();
            last_world_end = t_world;
            w.game_core_sum += harden_space_take_busy( space.game_busy );
            w.world_sum += t_world - t_world_began;
            w.world_max = std::max( w.world_max, t_world - t_world_began );

            // ---- the split (S:963-971): the frame's plan is its snapshot
            if ( !split_here && ( !frame_plan.empty() || o.space_net_in_task ) )
            {
                space.plan = &frame_plan;
                space.tick = tick;
                last_plan_time = harden_now();
                space.task = space.pool.AddTask( harden_space_task, &space );
            }

            // what rocketnet took from the main thread this frame
            const double work = ( t_sent - t_first ) + ( t_receive - t_sent ) + ( t_sent_reliable - t_receive );
            w.work.push_back( work );
            all_work.push_back( work );
            all_work_max = std::max( all_work_max, work );
            w.send_sum += ( t_sent - t_first ) + ( t_sent_reliable - t_reliable );
            w.send_max = std::max( w.send_max, ( t_sent - t_first ) + ( t_sent_reliable - t_reliable ) );
            w.update_sum += t_update - t_sent;
            w.update_max = std::max( w.update_max, t_update - t_sent );
            w.receive_sum += t_receive - t_update;
            w.receive_max = std::max( w.receive_max, t_receive - t_update );
            w.write_sum += t_reliable - t_receive;
            w.write_max = std::max( w.write_max, t_reliable - t_receive );
            w.frames++;
            if ( harden_now() - frame_start > step )
            {
                w.overruns++;
            }
            tick++;

            // ---- the tail (S:1313-1369): the main thread paces to the frame
            // edge while the task runs
            next += step;
            const double wait = next - harden_now();
            if ( wait > 0.0 )
            {
                harden_main_phase.store( HARDEN_MAIN_SLEEP, std::memory_order_relaxed );
                rocketnet_sleep( wait );
                harden_main_phase.store( HARDEN_MAIN_OTHER, std::memory_order_relaxed );
            }
            else
            {
                next = harden_now();
            }
            continue;
        }
        const double wall = harden_wall();
        if ( !draining && wall >= end_at )
        {
            draining = true;
            harden_utc( utc, sizeof( utc ) );
            printf( "harden_phase: role=server phase=writes_stop t=%.1f utc=%s\n", frame_start - started, utc );
            fflush( stdout );
        }
        if ( wall >= cut_c2s_at )
        {
            break;
        }

        harden_main_phase.store( HARDEN_MAIN_UPDATE, std::memory_order_relaxed );
        rocketnet_server_update( server, frame_start );
        const double t_update = harden_now();
        harden_main_phase.store( HARDEN_MAIN_RECEIVE, std::memory_order_relaxed );
        drain_receive();
        const double t_received = harden_now();

        // the blocking shape with the stand-in game: the world update between
        // receive and write, the main thread blocked through all of networking
        // around it
        double world_seconds = 0.0;
        if ( o.game_threads > 0 )
        {
            harden_world_update( &space, o );
            world_seconds = harden_now() - t_received;
            w.game_core_sum += harden_space_take_busy( space.game_busy );
            w.world_sum += world_seconds;
            w.world_max = std::max( w.world_max, world_seconds );
        }
        const double t_receive = harden_now();
        harden_main_phase.store( HARDEN_MAIN_WRITE, std::memory_order_relaxed );

        if ( !draining )
        {
            const int connected_count = rocketnet_server_num_connected_clients( server );
            int bursting_seen = 0;
            for ( int i = 0; i < o.max_clients; i++ )
            {
                if ( !rocketnet_server_client_connected( server, i ) )
                {
                    continue;
                }
                harden_server_slot_t & slot = slots[(size_t) i];
                const uint64_t id = rocketnet_server_client_id( server, i );
                const uint32_t connection = (uint32_t) rocketnet_server_client_connection_id( server, i );
                if ( slot.client_id != id || slot.connection != connection )
                {
                    slot = harden_server_slot_t();
                    slot.client_id = id;
                    slot.connection = connection;
                }
                const int count = client_count( bursting_seen, connected_count );
                bursting_seen++;
                if ( o.fill_jobs > 0 )
                {
                    // the jobs write the unreliable messages; this client's
                    // reliable and big ones follow the join, in the next
                    // fill phase
                    plan.push_back( harden_fill_item_t{ i, count, id, connection } );
                    continue;
                }
                for ( int n = 0; n < count; n++ )
                {
                    const int bytes = o.sizes[size_cursor];
                    size_cursor = ( size_cursor + 1 ) % o.num_sizes;
                    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) i, HARDEN_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE, bytes );
                    if ( !p )
                    {
                        w.start_refused++;
                        continue;
                    }
                    harden_write_message( p, bytes, HARDEN_KIND_BURST, id, connection, slot.next[HARDEN_KIND_BURST]++ );
                    rocketnet_server_finish_message( server, bytes );
                    w.messages++;
                }
                write_reliable_and_big( i, id, connection, slot );
            }
            if ( o.fill_jobs > 0 )
            {
                harden_fill_run( &pool, plan );
                for ( harden_fill_job_t & job : pool.jobs )
                {
                    w.messages += job.messages;
                    w.start_refused += job.start_refused + job.finish_refused;
                    w.jobs_busy_sum += job.busy;
                    job = harden_fill_job_t();
                }
                // the fill phase ends here. A client has one filler per fill
                // phase (rocketnet_layers.h's threading contract, checked by
                // rocketnet_fill_phase in asserting builds): the job wrote
                // this client's unreliable messages, so main may write its
                // reliable and big ones only in the next phase. A main-thread
                // operation begins it; receive_messages is the one with
                // nothing to do, since update has not run since the receive
                // drained every batch
                if ( rocketnet_server_receive_messages( server ) != NULL )
                {
                    verify.errors++; // a batch after the drain: nothing should have arrived
                }
                for ( const harden_fill_item_t & item : plan )
                {
                    write_reliable_and_big( item.slot, item.client_id, item.connection, slots[(size_t) item.slot] );
                }
                plan.clear();
            }
        }
        const double t_write = harden_now();
        harden_main_phase.store( HARDEN_MAIN_SEND, std::memory_order_relaxed );

        rocketnet_server_send_packets( server );
        const double t_send = harden_now();
        harden_main_phase.store( HARDEN_MAIN_OTHER, std::memory_order_relaxed );

        // what rocketnet took from the main thread: the frame less the world update
        const double work = t_send - frame_start - world_seconds;
        w.work.push_back( work );
        all_work.push_back( work );
        all_work_max = std::max( all_work_max, work );
        w.update_sum += t_update - frame_start;
        w.receive_sum += t_received - t_update;
        w.write_sum += t_write - t_receive;
        w.send_sum += t_send - t_write;
        w.update_max = std::max( w.update_max, t_update - frame_start );
        w.receive_max = std::max( w.receive_max, t_received - t_update );
        w.write_max = std::max( w.write_max, t_write - t_receive );
        w.send_max = std::max( w.send_max, t_send - t_write );
        // a frame's packets, from its game state (the world update's end) to send_packets
        w.latency_sum += t_write - t_receive;
        w.latency_max = std::max( w.latency_max, t_write - t_receive );
        w.latency_frames++;
        w.frames++;
        if ( t_send - frame_start > step )
        {
            w.overruns++;
        }
        tick++;
        if ( !report_and_sample() )
        {
            harden_fill_stop( &pool );
            return 1;
        }

        next += step;
        const double wait = next - harden_now();
        if ( wait > 0.0 )
        {
            harden_main_phase.store( HARDEN_MAIN_SLEEP, std::memory_order_relaxed );
            rocketnet_sleep( wait );
            harden_main_phase.store( HARDEN_MAIN_OTHER, std::memory_order_relaxed );
        }
        else
        {
            next = harden_now();
        }
    }

    total_messages += w.messages;
    total_start_refused += w.start_refused;
    total_frames += w.frames;
    total_overruns += w.overruns;

    // the end sample, before stop: the same objects alive as at the
    // post-warmup sample (the server running, its clients gone or going)
    process_resources_t resources_end;
    const bool end_read = process_resources_read( &resources_end );
    const int64_t library_end = harden_library_bytes();
    // the fill jobs end with the frames, after the end sample (they are the
    // game's threads, alive at both samples): the cuts and the cleanup are main's
    harden_fill_stop( &pool );
    if ( o.game_threads > 0 )
    {
        // the space shape joined its last task before the loop ended; its
        // staged packets leave in the flush below
        space.pool.Shutdown();
    }
    const double run_seconds = harden_now() - resources_started_at;

    // T1, client to server cut: what the server took, then it freezes
    {
        struct rocketnet_server_stats_t cut;
        rocketnet_server_stats( server, &cut );
        harden_utc( utc, sizeof( utc ) );
        printf( "harden_phase: role=server phase=cut_c2s utc=%s\n", utc );
        harden_print_term( role, "c2s_packets_received", (int64_t) cut.packets_received );
        harden_print_counters( "c2s_transport", role, transport ? rocketnet_server_transport_counters( transport ) : NULL, true );
        harden_print_cut_network( role, "c2s", o );
    }
    // a send the transport kept for its next send_packets leaves now: a few
    // frames of sends only, then the freeze
    {
        const double flush_end = harden_now() + HARDEN_FLUSH_SECONDS;
        while ( harden_now() < flush_end )
        {
            rocketnet_server_send_packets( server );
            rocketnet_sleep( 1.0 / o.hz );
        }
    }
    // T3, server to client cut: what the frozen server sent
    harden_sleep_until_wall( cut_s2c_at );
    {
        struct rocketnet_server_stats_t cut;
        rocketnet_server_stats( server, &cut );
        harden_utc( utc, sizeof( utc ) );
        printf( "harden_phase: role=server phase=cut_s2c utc=%s\n", utc );
        harden_print_term( role, "s2c_packets_sent", (int64_t) cut.packets_sent );
        harden_print_counters( "s2c_transport", role, transport ? rocketnet_server_transport_counters( transport ) : NULL, true );
        harden_print_cut_network( role, "s2c", o );
    }
    // T4, the cleanup
    harden_sleep_until_wall( cleanup_at );

    rocketnet_server_stop( server );
    const double stop_deadline = harden_now() + 10.0;
    while ( !rocketnet_server_stopped( server ) && harden_now() < stop_deadline )
    {
        rocketnet_sleep( 0.001 );
    }

    struct rocketnet_server_stats_t stats;
    rocketnet_server_stats( server, &stats );
    message_server = rocketnet_server_message_server( server );
    transport = message_server ? rocketnet_message_server_transport( message_server ) : NULL;

    harden_utc( utc, sizeof( utc ) );
    printf( "harden_phase: role=server phase=end utc=%s stopped=%d\n", utc, rocketnet_server_stopped( server ) ? 1 : 0 );
    harden_print_term( role, "packets_sent", (int64_t) stats.packets_sent );
    harden_print_term( role, "packets_received", (int64_t) stats.packets_received );
    harden_print_term( role, "connections_accepted", (int64_t) stats.connections_accepted );
    harden_print_term( role, "connections_denied_full", (int64_t) stats.connections_denied_full );
    harden_print_term( role, "handshakes_dropped_over_budget", (int64_t) stats.handshakes_dropped_over_budget );
    harden_print_term( role, "messages_resent", (int64_t) stats.messages_resent );
    harden_print_term( role, "messages_dropped_over_budget", (int64_t) stats.messages_dropped_over_budget );
    harden_print_term( role, "messages_written", (int64_t) total_messages );
    harden_print_term( role, "start_refused", (int64_t) total_start_refused );
    harden_print_term( role, "frames", (int64_t) total_frames );
    harden_print_term( role, "overruns", (int64_t) total_overruns );
    harden_print_term( role, "work_p99_us", (int64_t) ( 1e6 * harden_percentile( all_work, 0.99 ) ) );
    harden_print_term( role, "work_p999_us", (int64_t) ( 1e6 * harden_percentile( all_work, 0.999 ) ) );
    harden_print_term( role, "work_max_us", (int64_t) ( 1e6 * all_work_max ) );
    for ( int k = HARDEN_KIND_INPUT; k <= HARDEN_KIND_INPUT_RELIABLE; k++ )
    {
        char name[64];
        snprintf( name, sizeof( name ), "in_delivered_kind%d", k );
        harden_print_term( role, name, (int64_t) verify.delivered[k] );
        snprintf( name, sizeof( name ), "in_lost_kind%d", k );
        harden_print_term( role, name, (int64_t) verify.lost[k] );
        snprintf( name, sizeof( name ), "in_head_kind%d", k );
        harden_print_term( role, name, (int64_t) verify.head[k] );
        snprintf( name, sizeof( name ), "in_reordered_kind%d", k );
        harden_print_term( role, name, (int64_t) verify.reordered[k] );
        snprintf( name, sizeof( name ), "in_late_past_window_kind%d", k );
        harden_print_term( role, name, (int64_t) verify.late_past_window[k] );
    }
    harden_print_term( role, "in_errors", (int64_t) verify.errors );
    harden_print_term( role, "in_reliable_gaps", (int64_t) verify.reliable_gaps );
    harden_print_term( role, "in_back", (int64_t) verify.back );
    harden_print_term( role, "in_duplicates", (int64_t) verify.duplicates );
    harden_print_term( role, "in_wrong_owner", (int64_t) verify.wrong_owner );
    harden_print_counters( "transport", role, transport ? rocketnet_server_transport_counters( transport ) : NULL, true );
    harden_print_counters( "message", role, message_server ? rocketnet_message_server_counters( message_server ) : NULL, true );
    char report[256] = { 0 };
    const int conserved = transport ? rocketnet_counters_conserved( rocketnet_server_transport_counters( transport ), report, (int) sizeof( report ) ) : 0;
    printf( "harden_term: role=server conserved=%d report=%s\n", conserved, conserved ? "ok" : report );

    bool flat = true;
    if ( resources_started && end_read )
    {
        for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
        {
            // the process's memory count is printed, never asserted (the
            // soak's rule): the library's own bytes are the memory guard
            const bool judged = i != PROCESS_RESOURCES_MEMORY;
            const bool ok = !judged || process_resources_flat( i, resources_start.values[i], resources_end.values[i], run_seconds );
            printf( "harden_resources: %s start=%" PRId64 " end=%" PRId64 " peak=%" PRId64 " judged=%d flat=%d\n", process_resources_name( i ),
                    resources_start.values[i], resources_end.values[i], resources_peak.values[i], judged ? 1 : 0, ok ? 1 : 0 );
            flat = flat && ok;
        }
        const bool library_flat = process_resources_flat( PROCESS_RESOURCES_MEMORY, library_start, library_end, run_seconds );
        printf( "harden_resources: library_bytes start=%" PRId64 " end=%" PRId64 " peak=%" PRId64 " judged=1 flat=%d\n", library_start, library_end, library_peak,
                library_flat ? 1 : 0 );
        flat = flat && library_flat;
    }
    else
    {
        printf( "harden_resources: none (the run ended inside --warm-seconds %d)\n", o.warm_seconds );
    }

    const bool failed = verify.errors != 0 || !conserved || !flat;
    printf( "harden_verdict: role=server verdict=%s errors=%" PRIu64 " conserved=%d flat=%d\n", failed ? "FAIL" : "PASS", verify.errors, conserved, flat ? 1 : 0 );
    fflush( stdout );
    rocketnet_server_destroy( server );
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------- the clients

enum harden_client_phase_t
{
    HARDEN_PHASE_WAITING = 0,   // not yet joined
    HARDEN_PHASE_LIVE,          // connect called, updated every tick
    HARDEN_PHASE_SILENT,        // churn: not updated until silent_until
    HARDEN_PHASE_LEAVING,       // disconnect called, waiting for disconnected
    HARDEN_PHASE_AWAY,          // churn: disconnected, rejoins at rejoin_at
    HARDEN_PHASE_DONE           // end of run: disconnected and read
};

struct harden_client_t
{
    struct rocketnet_client_t * client = NULL;
    uint64_t client_id = 0;
    bool churner = false;
    harden_client_phase_t phase = HARDEN_PHASE_WAITING;
    double join_at = 0.0;
    double silent_until = 0.0;
    double rejoin_at = 0.0;
    int pending_event = -1;
    uint32_t connection_serial = 0;    // this end's count of its connects, for its inputs
    uint32_t next_input = 0;
    uint32_t next_input_reliable = 0;
    uint64_t tick = 0;
    bool was_connected = false;
    double connect_called_at = 0.0;
    harden_stream_t streams[HARDEN_KINDS];
};

struct harden_client_totals_t
{
    std::mutex mutex;
    harden_verify_t verify;
    uint64_t counters[HARDEN_CLIENT_SLOTS] = {};
    uint64_t c2s_counters[HARDEN_CLIENT_SLOTS] = {};
    uint64_t s2c_counters[HARDEN_CLIENT_SLOTS] = {};
    int c2s_threads = 0;
    int s2c_threads = 0;
    double end_at = 0.0;
    uint64_t joins = 0, connects = 0, connect_seconds_max_us = 0;
    uint64_t clean_leaves = 0, silent_stops = 0, reconnects = 0;
    uint64_t silent_timed_out = 0, unplanned_ends = 0, stable_ends = 0, end_of_run_ends = 0;
    uint64_t end_states[32] = {};
    uint64_t inputs_sent = 0, inputs_refused = 0;
    uint64_t connections_read = 0;
    std::atomic<int> connected[HARDEN_MAX_THREADS];
    std::atomic<uint64_t> delivered_live[HARDEN_MAX_THREADS];
    std::atomic<bool> stop_traffic{ false };
    std::atomic<bool> quit{ false };

    harden_client_totals_t()
    {
        for ( int t = 0; t < HARDEN_MAX_THREADS; t++ )
        {
            connected[t].store( 0 );
            delivered_live[t].store( 0 );
        }
    }
};

static bool harden_make_token( const harden_options_t & o, const uint8_t * key, uint64_t client_id, int timeout_seconds, char * token )
{
    return rocketnet_create_dev_connect_token( o.address.c_str(), NULL, key, 3600, timeout_seconds, client_id, o.protocol_id, (uint64_t) time( NULL ), NULL, token ) == ROCKETNET_STATUS_OK;
}

// reads the connection's transport counters into the totals, once its
// transport has stopped (disconnected is true): the counters are the
// connection's, and the next connect destroys it

// the layer a client counter belongs to, for its printed name
static const char * harden_counter_layer( int k )
{
    return strncmp( rocketnet_counter_name( k ), "MESSAGE_", 8 ) == 0 ? "message" : "transport";
}

static void harden_read_connection( harden_client_t * c, uint64_t * counters, uint64_t * read_count )
{
    struct rocketnet_message_client_t * message = rocketnet_client_message_client( c->client );
    struct rocketnet_client_transport_t * transport = message ? rocketnet_message_client_transport( message ) : NULL;
    const struct rocketnet_counters_t * block = transport ? rocketnet_client_transport_counters( transport ) : NULL;
    if ( !block )
    {
        return;
    }
    for ( int k = 0; k < rocketnet_num_counters() && k < HARDEN_COUNTER_SLOTS; k++ )
    {
        counters[k] += rocketnet_counter_value( block, k );
    }
    // the message layer's own refusals (a wire violation ends the connection
    // as PROTOCOL_VIOLATION): its counters share the one index space, so they
    // add into the same slots and print as message_
    const struct rocketnet_counters_t * message_block = rocketnet_message_client_counters( message );
    if ( message_block && message_block != block )
    {
        for ( int k = 0; k < rocketnet_num_counters() && k < HARDEN_COUNTER_SLOTS; k++ )
        {
            counters[k] += rocketnet_counter_value( message_block, k );
        }
    }
    struct rocketnet_client_transport_stats_t stats;
    memset( &stats, 0, sizeof( stats ) );
    rocketnet_client_transport_stats( transport, &stats );
    counters[HARDEN_SLOT_PACKETS_SENT] += stats.packets_sent;
    counters[HARDEN_SLOT_PACKETS_RECEIVED] += stats.packets_received;
    ( *read_count )++;
}

static void harden_clients_thread( const harden_options_t * o, const uint8_t * key, harden_client_totals_t * totals, int thread_index, double started )
{
    const int begin = o->clients * thread_index / o->threads;
    const int end = o->clients * ( thread_index + 1 ) / o->threads;
    std::vector<harden_client_t> clients( (size_t) ( end - begin ) );
    std::vector<uint64_t> counters( HARDEN_CLIENT_SLOTS, 0 );
    harden_verify_t verify;
    uint64_t connections_read = 0, inputs_sent = 0, inputs_refused = 0, joins = 0, connects = 0, connect_max_us = 0;
    uint64_t clean_leaves = 0, silent_stops = 0, reconnects = 0, silent_timed_out = 0, unplanned_ends = 0, stable_ends = 0, end_of_run_ends = 0;
    uint64_t end_states[32] = {};

    struct rocketnet_client_config_t config;
    rocketnet_client_default_config( &config );

    for ( size_t i = 0; i < clients.size(); i++ )
    {
        const int global = begin + (int) i;
        harden_client_t & c = clients[i];
        c.client_id = (uint64_t) o->first_id + (uint64_t) global + 1;
        c.churner = global >= o->clients - o->churn_clients;
        c.join_at = started + (double) global / o->join_rate;
        enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
        c.client = rocketnet_client_create( NULL, &config, &status );
        if ( !c.client )
        {
            fprintf( stderr, "harden_load clients: create failed (status %d)\n", (int) status );
            verify.errors++;
        }
    }

    // the churn schedule: the churners of every thread share one clock, each
    // thread takes the events whose number falls to it, round robin over its
    // own churners
    int my_churners = 0;
    for ( const harden_client_t & c : clients )
    {
        my_churners += c.churner ? 1 : 0;
    }
    const double churn_interval = o->churn_rate > 0.0 ? (double) o->threads / o->churn_rate : 0.0;
    double next_churn = started + (double) o->clients / o->join_rate + 5.0 + churn_interval * (double) thread_index / (double) o->threads;
    int churn_cursor = 0;
    int churn_event = 0;

    const double step = 1.0 / o->hz;
    double next = harden_now();
    // the two cuts (harden_server_main has the timeline): frozen from end_at
    // to T2, the client to server read half way through that; update and
    // receive only from T2, the server to client read at T3
    const double end_at = totals->end_at;
    const double read_c2s_at = end_at + 0.5 * o->quiet_seconds;
    const double thaw_at = end_at + 1.5 * o->quiet_seconds;
    const double read_s2c_at = thaw_at + o->quiet_seconds;
    bool read_c2s = false, read_s2c = false;
    while ( !totals->quit.load() )
    {
        const double t = harden_now();
        const double wall = harden_wall();
        const bool cleanup = totals->stop_traffic.load();
        // from end_at to the cleanup: no writes, no connects, no send_packets;
        // the cleanup sends again
        const bool stop_traffic = cleanup || wall >= end_at;
        const bool sends = cleanup || wall < end_at;
        // a hand-off the transport deferred leaves with the next send_packets
        // (CLIENT_SEND_DEFERRED), so the clients flush for a few frames, sends
        // only, before they freeze
        const bool flushing = !cleanup && wall >= end_at && wall < end_at + HARDEN_FLUSH_SECONDS;
        const bool frozen = !cleanup && wall >= end_at + HARDEN_FLUSH_SECONDS && wall < thaw_at;
        // the last churn event leaves room for a silent stop to time out
        // and its client to come back before the end
        const bool churn_open = wall < end_at - (double) o->token_timeout - 6.0;
        int connected = 0;

        if ( ( !read_c2s && wall >= read_c2s_at ) || ( !read_s2c && wall >= read_s2c_at ) )
        {
            // a cut: the connections read so far, and every one still open
            const bool c2s = !read_c2s;
            std::vector<uint64_t> cut = counters;
            for ( harden_client_t & c : clients )
            {
                if ( c.client && ( c.phase == HARDEN_PHASE_LIVE || c.phase == HARDEN_PHASE_SILENT || c.phase == HARDEN_PHASE_LEAVING ) )
                {
                    uint64_t ignored = 0;
                    harden_read_connection( &c, cut.data(), &ignored );
                }
            }
            std::lock_guard<std::mutex> lock( totals->mutex );
            uint64_t * into = c2s ? totals->c2s_counters : totals->s2c_counters;
            for ( int k = 0; k < HARDEN_CLIENT_SLOTS; k++ )
            {
                into[k] += cut[(size_t) k];
            }
            if ( c2s )
            {
                totals->c2s_threads++;
                read_c2s = true;
            }
            else
            {
                totals->s2c_threads++;
                read_s2c = true;
            }
        }

        if ( frozen )
        {
            // a frozen client sends nothing, keep-alives included
            rocketnet_sleep( step );
            next = harden_now();
            continue;
        }

        if ( churn_interval > 0.0 && my_churners > 0 && churn_open && !stop_traffic && t >= next_churn )
        {
            next_churn += churn_interval;
            for ( int tries = 0; tries < (int) clients.size(); tries++ )
            {
                harden_client_t & c = clients[(size_t) churn_cursor];
                churn_cursor = ( churn_cursor + 1 ) % (int) clients.size();
                if ( c.churner && c.phase == HARDEN_PHASE_LIVE && rocketnet_client_state( c.client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
                {
                    const int event = churn_event++ % 3;
                    if ( event == 0 )
                    {
                        clean_leaves++;
                        c.pending_event = 0;
                        rocketnet_client_disconnect( c.client );
                        c.phase = HARDEN_PHASE_LEAVING;
                    }
                    else if ( event == 1 )
                    {
                        silent_stops++;
                        c.pending_event = 1;
                        c.phase = HARDEN_PHASE_SILENT;
                        c.silent_until = t + (double) o->token_timeout + 2.0;
                    }
                    else
                    {
                        reconnects++;
                        c.pending_event = 2;
                        rocketnet_client_disconnect( c.client );
                        c.phase = HARDEN_PHASE_LEAVING;
                    }
                    break;
                }
            }
        }

        for ( harden_client_t & c : clients )
        {
            if ( !c.client )
            {
                continue;
            }
            if ( c.phase == HARDEN_PHASE_WAITING || c.phase == HARDEN_PHASE_AWAY )
            {
                const double due = c.phase == HARDEN_PHASE_WAITING ? c.join_at : c.rejoin_at;
                if ( t < due || stop_traffic )
                {
                    continue;
                }
                char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
                if ( !harden_make_token( *o, key, c.client_id, c.churner ? o->token_timeout : 10, token ) || rocketnet_client_connect( c.client, token ) != ROCKETNET_STATUS_OK )
                {
                    verify.errors++;
                    c.phase = HARDEN_PHASE_DONE;
                    continue;
                }
                joins++;
                c.connection_serial++;
                c.next_input = 0;
                c.next_input_reliable = 0;
                c.was_connected = false;
                c.connect_called_at = t;
                c.phase = HARDEN_PHASE_LIVE;
            }
            if ( c.phase == HARDEN_PHASE_SILENT )
            {
                if ( t < c.silent_until )
                {
                    continue;
                }
                // back from silence: the client sees its own timeout on this
                // update, then disconnects and rejoins
                c.phase = HARDEN_PHASE_LIVE;
            }
            if ( c.phase == HARDEN_PHASE_DONE )
            {
                continue;
            }

            if ( flushing )
            {
                if ( c.phase == HARDEN_PHASE_LIVE || c.phase == HARDEN_PHASE_LEAVING )
                {
                    rocketnet_client_send_packets( c.client );
                }
                continue;
            }
            rocketnet_client_update( c.client, t );
            const struct rocketnet_message_batch_t * batch;
            while ( ( batch = rocketnet_client_receive_messages( c.client ) ) != NULL )
            {
                for ( int m = 0; m < batch->num_messages; m++ )
                {
                    harden_message_t header;
                    if ( !harden_read_message( batch->data[m], batch->bytes[m], &header ) || header.kind > HARDEN_KIND_BIG )
                    {
                        verify.errors++;
                        continue;
                    }
                    if ( header.client_id != c.client_id )
                    {
                        verify.wrong_owner++;
                        verify.errors++;
                        continue;
                    }
                    harden_stream_take( &c.streams[header.kind], &header, header.kind != HARDEN_KIND_BURST, &verify );
                }
            }

            const int state = (int) rocketnet_client_state( c.client );
            if ( c.phase == HARDEN_PHASE_LIVE )
            {
                if ( state == ROCKETNET_CLIENT_STATE_CONNECTED )
                {
                    connected++;
                    if ( !c.was_connected )
                    {
                        c.was_connected = true;
                        connects++;
                        const uint64_t us = (uint64_t) ( 1e6 * ( t - c.connect_called_at ) );
                        connect_max_us = std::max( connect_max_us, us );
                    }
                    if ( !stop_traffic )
                    {
                        for ( int m = 0; m < o->messages; m++ )
                        {
                            uint8_t * p = rocketnet_client_start_message( c.client, HARDEN_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE, o->input_bytes );
                            if ( !p )
                            {
                                inputs_refused++;
                                break;
                            }
                            harden_write_message( p, o->input_bytes, HARDEN_KIND_INPUT, c.client_id, c.connection_serial, c.next_input++ );
                            rocketnet_client_finish_message( c.client, o->input_bytes );
                            inputs_sent++;
                        }
                        if ( o->reliable_every > 0 && c.tick % (uint64_t) ( 30 * o->reliable_every ) == 0 )
                        {
                            uint8_t * p = rocketnet_client_start_message( c.client, HARDEN_CHANNEL_RELIABLE, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, HARDEN_INPUT_RELIABLE_BYTES );
                            if ( p )
                            {
                                harden_write_message( p, HARDEN_INPUT_RELIABLE_BYTES, HARDEN_KIND_INPUT_RELIABLE, c.client_id, c.connection_serial, c.next_input_reliable++ );
                                rocketnet_client_finish_message( c.client, HARDEN_INPUT_RELIABLE_BYTES );
                                inputs_sent++;
                            }
                            else
                            {
                                inputs_refused++;
                            }
                        }
                    }
                    c.tick++;
                }
                else if ( state < 0 )
                {
                    // an ended connection: planned (a churner back from silence)
                    // or not (a failure, named by its state)
                    end_states[-state < 32 ? -state : 31]++;
                    if ( c.churner && c.pending_event == 1 && state == ROCKETNET_CLIENT_STATE_CONNECTION_TIMED_OUT )
                    {
                        silent_timed_out++;
                    }
                    else if ( stop_traffic )
                    {
                        // after end_at: the server's stop at the end of the run
                        end_of_run_ends++;
                    }
                    else
                    {
                        unplanned_ends++;
                        if ( !c.churner )
                        {
                            stable_ends++;
                        }
                    }
                    rocketnet_client_disconnect( c.client );
                    c.phase = HARDEN_PHASE_LEAVING;
                }
            }
            if ( sends )
            {
                rocketnet_client_send_packets( c.client );
            }

            if ( c.phase == HARDEN_PHASE_LEAVING && rocketnet_client_disconnected( c.client ) )
            {
                harden_read_connection( &c, counters.data(), &connections_read );
                const bool immediate = c.pending_event == 2;
                c.pending_event = -1;
                if ( c.churner && !stop_traffic )
                {
                    c.phase = HARDEN_PHASE_AWAY;
                    c.rejoin_at = t + ( immediate ? 0.0 : 1.0 );
                }
                else
                {
                    c.phase = stop_traffic ? HARDEN_PHASE_DONE : HARDEN_PHASE_AWAY;
                    c.rejoin_at = t + 1.0;
                }
            }
        }

        if ( cleanup )
        {
            // the end: every live client disconnects cleanly, and each
            // connection is read once its transport has stopped
            for ( harden_client_t & c : clients )
            {
                if ( c.client && ( c.phase == HARDEN_PHASE_LIVE || c.phase == HARDEN_PHASE_SILENT ) )
                {
                    rocketnet_client_disconnect( c.client );
                    c.phase = HARDEN_PHASE_LEAVING;
                }
                else if ( c.phase == HARDEN_PHASE_WAITING || c.phase == HARDEN_PHASE_AWAY )
                {
                    c.phase = HARDEN_PHASE_DONE;
                }
            }
        }

        totals->connected[thread_index].store( connected );
        uint64_t delivered = 0;
        for ( int k = 0; k < HARDEN_KINDS; k++ )
        {
            delivered += verify.delivered[k];
        }
        totals->delivered_live[thread_index].store( delivered );

        next += step;
        const double wait = next - harden_now();
        if ( wait > 0.0 )
        {
            rocketnet_sleep( wait );
        }
        else
        {
            next = harden_now();
        }
    }

    // anything still leaving is waited for (bounded), read and destroyed
    const double deadline = harden_now() + 10.0;
    for ( harden_client_t & c : clients )
    {
        if ( !c.client )
        {
            continue;
        }
        if ( c.phase != HARDEN_PHASE_DONE && c.phase != HARDEN_PHASE_WAITING && c.phase != HARDEN_PHASE_AWAY )
        {
            if ( c.phase != HARDEN_PHASE_LEAVING )
            {
                rocketnet_client_disconnect( c.client );
            }
            while ( !rocketnet_client_disconnected( c.client ) && harden_now() < deadline )
            {
                rocketnet_client_update( c.client, harden_now() );
                while ( rocketnet_client_receive_messages( c.client ) != NULL )
                {
                }
                rocketnet_sleep( 0.001 );
            }
            if ( rocketnet_client_disconnected( c.client ) )
            {
                harden_read_connection( &c, counters.data(), &connections_read );
            }
            else
            {
                verify.errors++;
                fprintf( stderr, "harden_load clients: client %" PRIu64 " never reported disconnected\n", c.client_id );
            }
        }
        if ( rocketnet_client_disconnected( c.client ) )
        {
            rocketnet_client_destroy( c.client );
        }
        c.client = NULL;
    }

    std::lock_guard<std::mutex> lock( totals->mutex );
    for ( int k = 0; k < HARDEN_KINDS; k++ )
    {
        totals->verify.delivered[k] += verify.delivered[k];
        totals->verify.lost[k] += verify.lost[k];
        totals->verify.head[k] += verify.head[k];
        totals->verify.reordered[k] += verify.reordered[k];
        totals->verify.late_past_window[k] += verify.late_past_window[k];
    }
    totals->verify.errors += verify.errors;
    totals->verify.wrong_owner += verify.wrong_owner;
    totals->verify.reliable_gaps += verify.reliable_gaps;
    totals->verify.back += verify.back;
    totals->verify.duplicates += verify.duplicates;
    for ( int k = 0; k < HARDEN_CLIENT_SLOTS; k++ )
    {
        totals->counters[k] += counters[(size_t) k];
    }
    totals->connections_read += connections_read;
    totals->inputs_sent += inputs_sent;
    totals->inputs_refused += inputs_refused;
    totals->joins += joins;
    totals->connects += connects;
    totals->connect_seconds_max_us = std::max( totals->connect_seconds_max_us, connect_max_us );
    totals->clean_leaves += clean_leaves;
    totals->silent_stops += silent_stops;
    totals->reconnects += reconnects;
    totals->silent_timed_out += silent_timed_out;
    totals->unplanned_ends += unplanned_ends;
    totals->stable_ends += stable_ends;
    totals->end_of_run_ends += end_of_run_ends;
    for ( int s = 0; s < 32; s++ )
    {
        totals->end_states[s] += end_states[s];
    }
}

static int harden_clients_main( const harden_options_t & o )
{
    const char * role = "clients";
    uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !harden_parse_key( o.key_hex, key ) )
    {
        fprintf( stderr, "harden_load clients: --key takes the server's 64 hex digit public key (its harden_ready line)\n" );
        return 2;
    }
    char utc[32];
    harden_utc( utc, sizeof( utc ) );
    printf( "harden_clients: address=%s clients=%d threads=%d first_id=%d hz=%.0f join_rate=%.0f churn_clients=%d churn_rate=%.2f messages=%d input_bytes=%d token_timeout=%d seconds=%.0f utc=%s\n",
            o.address.c_str(), o.clients, o.threads, o.first_id, o.hz, o.join_rate, o.churn_clients, o.churn_rate, o.messages, o.input_bytes, o.token_timeout, o.seconds, utc );
    fflush( stdout );

    static harden_client_totals_t totals;
    totals.end_at = o.end_at > 0.0 ? o.end_at : harden_wall() + o.seconds;
    const double started = harden_now();
    std::vector<std::thread> threads;
    for ( int t = 0; t < o.threads; t++ )
    {
        threads.emplace_back( harden_clients_thread, &o, key, &totals, t, started );
    }

    double last_report = started;
    uint64_t last_delivered = 0;
    for ( int cut = 0; cut < 2; cut++ )
    {
        // wait for every thread's read of this cut, printing the players
        // each second meanwhile
        for ( ;; )
        {
            rocketnet_sleep( 0.02 );
            {
                std::lock_guard<std::mutex> lock( totals.mutex );
                if ( ( cut == 0 ? totals.c2s_threads : totals.s2c_threads ) == o.threads )
                {
                    break;
                }
            }
            const double t_now = harden_now();
            if ( t_now - last_report < 1.0 )
            {
                continue;
            }
            int connected = 0;
            uint64_t delivered = 0;
            for ( int t = 0; t < o.threads; t++ )
            {
                connected += totals.connected[t].load();
                delivered += totals.delivered_live[t].load();
            }
            printf( "harden_clients_tick: t=%.1f connected=%d delivered_ps=%.0f\n", t_now - started, connected, (double) ( delivered - last_delivered ) / ( t_now - last_report ) );
            fflush( stdout );
            last_delivered = delivered;
            last_report = t_now;
        }
        // the threads have read their connections; the namespace's kernel
        // terms now, while this cut's direction is still silent
        const char * name = cut == 0 ? "c2s" : "s2c";
        const uint64_t * counts = cut == 0 ? totals.c2s_counters : totals.s2c_counters;
        harden_utc( utc, sizeof( utc ) );
        printf( "harden_phase: role=clients phase=cut_%s utc=%s\n", name, utc );
        harden_print_cut_term( role, name, cut == 0 ? "packets_sent" : "packets_received", (int64_t) counts[cut == 0 ? HARDEN_SLOT_PACKETS_SENT : HARDEN_SLOT_PACKETS_RECEIVED] );
        for ( int k = 0; k < rocketnet_num_counters() && k < HARDEN_COUNTER_SLOTS; k++ )
        {
            if ( counts[k] )
            {
                printf( "harden_term: role=clients %s_%s_%s=%" PRIu64 "\n", name, harden_counter_layer( k ), rocketnet_counter_name( k ), counts[k] );
            }
        }
        harden_print_cut_network( role, name, o );
    }

    // T4, the cleanup: every client leaves
    harden_sleep_until_wall( totals.end_at + 3.0 * o.quiet_seconds );
    harden_utc( utc, sizeof( utc ) );
    printf( "harden_phase: role=clients phase=leave utc=%s\n", utc );
    fflush( stdout );
    totals.stop_traffic.store( true );
    rocketnet_sleep( 2.0 );
    totals.quit.store( true );
    for ( std::thread & t : threads )
    {
        t.join();
    }

    harden_utc( utc, sizeof( utc ) );
    printf( "harden_phase: role=clients phase=end utc=%s\n", utc );
    const harden_verify_t & v = totals.verify;
    for ( int k = HARDEN_KIND_BURST; k <= HARDEN_KIND_BIG; k++ )
    {
        char name[64];
        snprintf( name, sizeof( name ), "delivered_kind%d", k );
        harden_print_term( role, name, (int64_t) v.delivered[k] );
        snprintf( name, sizeof( name ), "lost_kind%d", k );
        harden_print_term( role, name, (int64_t) v.lost[k] );
        snprintf( name, sizeof( name ), "head_kind%d", k );
        harden_print_term( role, name, (int64_t) v.head[k] );
        snprintf( name, sizeof( name ), "reordered_kind%d", k );
        harden_print_term( role, name, (int64_t) v.reordered[k] );
        snprintf( name, sizeof( name ), "late_past_window_kind%d", k );
        harden_print_term( role, name, (int64_t) v.late_past_window[k] );
    }
    harden_print_term( role, "errors", (int64_t) v.errors );
    harden_print_term( role, "reliable_gaps", (int64_t) v.reliable_gaps );
    harden_print_term( role, "back", (int64_t) v.back );
    harden_print_term( role, "duplicates", (int64_t) v.duplicates );
    harden_print_term( role, "wrong_owner", (int64_t) v.wrong_owner );
    harden_print_term( role, "connections_read", (int64_t) totals.connections_read );
    harden_print_term( role, "joins", (int64_t) totals.joins );
    harden_print_term( role, "connects", (int64_t) totals.connects );
    harden_print_term( role, "connect_max_us", (int64_t) totals.connect_seconds_max_us );
    harden_print_term( role, "clean_leaves", (int64_t) totals.clean_leaves );
    harden_print_term( role, "silent_stops", (int64_t) totals.silent_stops );
    harden_print_term( role, "silent_timed_out", (int64_t) totals.silent_timed_out );
    harden_print_term( role, "reconnects", (int64_t) totals.reconnects );
    harden_print_term( role, "unplanned_ends", (int64_t) totals.unplanned_ends );
    harden_print_term( role, "stable_ends", (int64_t) totals.stable_ends );
    harden_print_term( role, "end_of_run_ends", (int64_t) totals.end_of_run_ends );
    harden_print_term( role, "inputs_sent", (int64_t) totals.inputs_sent );
    harden_print_term( role, "inputs_refused", (int64_t) totals.inputs_refused );
    for ( int s = 0; s < 32; s++ )
    {
        if ( totals.end_states[s] )
        {
            printf( "harden_term: role=clients end_state_%s=%" PRIu64 "\n", rocketnet_client_state_name( ( enum rocketnet_client_state_t ) - s ), totals.end_states[s] );
        }
    }
    for ( int k = 0; k < rocketnet_num_counters() && k < HARDEN_COUNTER_SLOTS; k++ )
    {
        if ( totals.counters[k] )
        {
            printf( "harden_term: role=clients %s_%s=%" PRIu64 "\n", harden_counter_layer( k ), rocketnet_counter_name( k ), totals.counters[k] );
        }
    }
    harden_print_term( role, "packets_sent", (int64_t) totals.counters[HARDEN_SLOT_PACKETS_SENT] );
    harden_print_term( role, "packets_received", (int64_t) totals.counters[HARDEN_SLOT_PACKETS_RECEIVED] );
    const bool failed = v.errors != 0 || totals.stable_ends != 0 || totals.connects == 0;
    printf( "harden_verdict: role=clients verdict=%s errors=%" PRIu64 " stable_ends=%" PRIu64 " connects=%" PRIu64 "\n", failed ? "FAIL" : "PASS", v.errors,
            totals.stable_ends, totals.connects );
    fflush( stdout );
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------- the hostile load

#define HARDEN_HOSTILE_KINDS 7

static const char * harden_hostile_kind_names[HARDEN_HOSTILE_KINDS] = { "garbage", "prefixed", "truncated", "badsig", "expired", "reqreplay", "replay" };

struct harden_hostile_t
{
    std::vector<std::vector<uint8_t>> requests_badsig;
    std::vector<uint8_t> request_expired;
    std::vector<uint8_t> request_valid;
    std::vector<std::vector<uint8_t>> captured;
};

static void harden_close_socket( harden_socket_t s )
{
#if defined( _WIN32 )
    closesocket( s );
#else
    close( s );
#endif // #if defined( _WIN32 )
}

static bool harden_parse_address( const std::string & text, struct sockaddr_in * out )
{
    memset( out, 0, sizeof( *out ) );
    const size_t colon = text.rfind( ':' );
    if ( colon == std::string::npos )
    {
        return false;
    }
    out->sin_family = AF_INET;
    out->sin_port = htons( (uint16_t) atoi( text.c_str() + colon + 1 ) );
    return inet_pton( AF_INET, text.substr( 0, colon ).c_str(), &out->sin_addr ) == 1;
}

// a connection request whose token says what the kind needs: the request's
// layout is the client's own (rocketnet_write_connection_request_packet)

static bool harden_build_request( const harden_options_t & o, const uint8_t * key, uint64_t timestamp, int expire_seconds, bool corrupt_signature, std::vector<uint8_t> * out )
{
    char token_text[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( rocketnet_create_dev_connect_token( o.address.c_str(), NULL, key, expire_seconds, 5, UINT64_C( 0x7fff000000000000 ) + ( timestamp & 0xffff ), o.protocol_id, timestamp, NULL, token_text ) != ROCKETNET_STATUS_OK )
    {
        return false;
    }
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( rocketnet_base64_decode_data( token_text, token, sizeof( token ) ) != ROCKETNET_CONNECT_TOKEN_BYTES )
    {
        return false;
    }
    struct rocketnet_connect_token_t parts;
    rocketnet_read_connect_token( &parts, token );
    if ( corrupt_signature )
    {
        for ( int i = 0; i < ROCKETNET_SIGNATURE_BYTES; i++ )
        {
            parts.signature[i] = (uint8_t) rand();
        }
    }
    uint8_t salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t ephemeral[32];
    for ( size_t i = 0; i < sizeof( salt ); i++ )
    {
        salt[i] = (uint8_t) rand();
    }
    for ( size_t i = 0; i < sizeof( ephemeral ); i++ )
    {
        ephemeral[i] = (uint8_t) ( rand() | 1 );
    }
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    out->assign( (size_t) ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, 0 );
    rocketnet_write_connection_request_packet( out->data(), filter, NULL, salt, ephemeral, parts.signed_connect_data, parts.signature );
    return true;
}

#if defined( __linux__ )
// replay: up to want session packets of real players, read off the wire
// with AF_PACKET, UDP to the server's port. ETH_P_ALL, not ETH_P_IP: Linux hands a
// host's own outgoing frames only to ETH_P_ALL packet sockets, and the players
// send from this host; the IP protocol and address checks below drop the rest
static bool harden_capture( const harden_options_t & o, int want, std::vector<std::vector<uint8_t>> * out )
{
    struct sockaddr_in server;
    if ( !harden_parse_address( o.address, &server ) )
    {
        return false;
    }
    const int s = socket( AF_PACKET, SOCK_DGRAM, htons( ETH_P_ALL ) );
    if ( s < 0 )
    {
        fprintf( stderr, "harden_load hostile: replay needs AF_PACKET (CAP_NET_RAW): %s\n", strerror( errno ) );
        return false;
    }
    struct sockaddr_ll bind_address;
    memset( &bind_address, 0, sizeof( bind_address ) );
    bind_address.sll_family = AF_PACKET;
    bind_address.sll_protocol = htons( ETH_P_ALL );
    bind_address.sll_ifindex = (int) if_nametoindex( o.capture_dev.c_str() );
    if ( bind_address.sll_ifindex == 0 || bind( s, (struct sockaddr *) &bind_address, sizeof( bind_address ) ) != 0 )
    {
        fprintf( stderr, "harden_load hostile: cannot capture on --capture-dev %s\n", o.capture_dev.c_str() );
        close( s );
        return false;
    }
    struct timeval timeout = { 1, 0 };
    setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof( timeout ) );
    const double deadline = harden_now() + 30.0;
    uint8_t buffer[2048];
    while ( (int) out->size() < want && harden_now() < deadline )
    {
        const ssize_t n = recv( s, buffer, sizeof( buffer ), 0 );
        if ( n < (ssize_t) ( sizeof( struct iphdr ) + sizeof( struct udphdr ) ) )
        {
            continue;
        }
        const struct iphdr * ip = (const struct iphdr *) buffer;
        if ( ip->version != 4 || ip->protocol != IPPROTO_UDP || ip->daddr != server.sin_addr.s_addr )
        {
            continue;
        }
        const size_t ihl = (size_t) ip->ihl * 4;
        const struct udphdr * udp = (const struct udphdr *) ( buffer + ihl );
        if ( udp->dest != server.sin_port )
        {
            continue;
        }
        const size_t payload = (size_t) ntohs( udp->len ) - sizeof( struct udphdr );
        const uint8_t * data = buffer + ihl + sizeof( struct udphdr );
        if ( ihl + sizeof( struct udphdr ) + payload > (size_t) n || payload < ROCKETNET_PREFILTER_BYTES || data[0] != ROCKETNET_CONNECTION_PAYLOAD_PACKET )
        {
            continue;
        }
        out->push_back( std::vector<uint8_t>( data, data + payload ) );
    }
    close( s );
    return !out->empty();
}
#endif // #if defined( __linux__ )

// one hostile sender: its sockets, its share of the rate, its own generator
// (xorshift64; rand() is shared state and was the first version's ceiling)
// and its own counts, summed by the role after the join
struct harden_hostile_worker_t
{
    const harden_hostile_t * h = nullptr;
    const bool * enabled = nullptr;
    int num_enabled = 0;
    std::vector<harden_socket_t> sockets;
    struct sockaddr_in server = {};
    double pps = 0.0;
    double started = 0.0;
    double hostile_end = 0.0;
    uint64_t random = 1;
    uint64_t sent[HARDEN_HOSTILE_KINDS] = {};
    uint64_t send_errors = 0, received = 0, received_bytes = 0, sent_bytes = 0;
    std::atomic<uint64_t> progress_sent{ 0 }, progress_received{ 0 }, progress_errors{ 0 };

    harden_hostile_worker_t() = default;
    harden_hostile_worker_t( const harden_hostile_worker_t & ) = delete;
    harden_hostile_worker_t & operator=( const harden_hostile_worker_t & ) = delete;
};

static uint64_t harden_hostile_next( harden_hostile_worker_t * w )
{
    uint64_t x = w->random;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    w->random = x;
    return x;
}

static void harden_hostile_fill( harden_hostile_worker_t * w, uint8_t * p, int from, int to )
{
    int i = from;
    while ( i < to )
    {
        uint64_t r = harden_hostile_next( w );
        for ( int b = 0; b < 8 && i < to; b++, i++ )
        {
            p[i] = (uint8_t) r;
            r >>= 8;
        }
    }
}

static void harden_hostile_send_loop( harden_hostile_worker_t * w )
{
    static const uint8_t session_types[] = { ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_RESPONSE_PACKET };
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    const harden_hostile_t & h = *w->h;
    uint64_t sent_total = 0;
    int socket_cursor = 0, kind_cursor = 0;
    uint8_t packet[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 128];
    while ( harden_wall() < w->hostile_end )
    {
        const uint64_t due_total = (uint64_t) ( ( harden_now() - w->started ) * w->pps );
        int burst = 0;
        while ( sent_total < due_total && burst < 4096 && w->num_enabled > 0 )
        {
            int kind = kind_cursor;
            while ( !w->enabled[kind] )
            {
                kind = ( kind + 1 ) % HARDEN_HOSTILE_KINDS;
            }
            kind_cursor = ( kind + 1 ) % HARDEN_HOSTILE_KINDS;
            const uint64_t pick = harden_hostile_next( w );
            int bytes = 0;
            const uint8_t * data = packet;
            switch ( kind )
            {
                case 0:
                {
                    bytes = 1 + (int) ( pick % 1400 );
                    harden_hostile_fill( w, packet, 0, bytes );
                    break;
                }
                case 1:
                {
                    const uint8_t type = session_types[pick % sizeof( session_types )];
                    uint8_t * p = packet;
                    rocketnet_write_prefilter_header( &p, type, filter );
                    bytes = rocketnet_session_min_wire_bytes( type ) + (int) ( ( pick >> 16 ) % 1200 );
                    harden_hostile_fill( w, packet, ROCKETNET_PREFILTER_BYTES, bytes );
                    break;
                }
                case 2:
                {
                    const uint8_t type = session_types[pick % sizeof( session_types )];
                    uint8_t * p = packet;
                    rocketnet_write_prefilter_header( &p, type, filter );
                    const int minimum = rocketnet_session_min_wire_bytes( type );
                    bytes = 1 + (int) ( ( pick >> 16 ) % (uint64_t) ( minimum > 1 ? minimum - 1 : 1 ) );
                    harden_hostile_fill( w, packet, ROCKETNET_PREFILTER_BYTES, bytes );
                    break;
                }
                case 3:
                {
                    const std::vector<uint8_t> & r = h.requests_badsig[(size_t) ( pick % h.requests_badsig.size() )];
                    data = r.data();
                    bytes = (int) r.size();
                    break;
                }
                case 4:
                {
                    data = h.request_expired.data();
                    bytes = (int) h.request_expired.size();
                    break;
                }
                case 5:
                {
                    data = h.request_valid.data();
                    bytes = (int) h.request_valid.size();
                    break;
                }
                default:
                {
                    const std::vector<uint8_t> & r = h.captured[(size_t) ( pick % h.captured.size() )];
                    data = r.data();
                    bytes = (int) r.size();
                    break;
                }
            }
            const harden_socket_t s = w->sockets[(size_t) socket_cursor];
            socket_cursor = ( socket_cursor + 1 ) % (int) w->sockets.size();
            const int result = (int) sendto( s, (const char *) data, (size_t) bytes, 0, (const struct sockaddr *) &w->server, sizeof( w->server ) );
            if ( result == bytes )
            {
                w->sent[kind]++;
                w->sent_bytes += (uint64_t) bytes;
            }
            else
            {
                w->send_errors++;
            }
            sent_total++;
            burst++;
        }
        // read whatever the server answered
        for ( harden_socket_t s : w->sockets )
        {
            for ( ;; )
            {
                const int n = (int) recv( s, (char *) packet, (int) sizeof( packet ), 0 );
                if ( n < 0 )
                {
                    break;
                }
                w->received++;
                w->received_bytes += (uint64_t) n;
            }
        }
        w->progress_sent.store( sent_total, std::memory_order_relaxed );
        w->progress_received.store( w->received, std::memory_order_relaxed );
        w->progress_errors.store( w->send_errors, std::memory_order_relaxed );
        // sleep only when caught up with the rate
        if ( sent_total >= due_total )
        {
            rocketnet_sleep( 0.001 );
        }
    }
}

static int harden_hostile_main( const harden_options_t & o )
{
    const char * role = "hostile";
    uint8_t key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    if ( !harden_parse_key( o.key_hex, key ) )
    {
        fprintf( stderr, "harden_load hostile: --key takes the server's public key\n" );
        return 2;
    }
    bool enabled[HARDEN_HOSTILE_KINDS] = {};
    int num_enabled = 0;
    {
        std::string list = o.kinds + ",";
        size_t start = 0;
        for ( size_t comma = list.find( ',' ); comma != std::string::npos; comma = list.find( ',', start ) )
        {
            const std::string name = list.substr( start, comma - start );
            start = comma + 1;
            bool known = false;
            for ( int k = 0; k < HARDEN_HOSTILE_KINDS; k++ )
            {
                if ( name == harden_hostile_kind_names[k] )
                {
                    enabled[k] = true;
                    known = true;
                }
            }
            if ( !known )
            {
                fprintf( stderr, "harden_load hostile: unknown kind '%s'\n", name.c_str() );
                return 2;
            }
        }
        for ( int k = 0; k < HARDEN_HOSTILE_KINDS; k++ )
        {
            num_enabled += enabled[k] ? 1 : 0;
        }
    }
    struct sockaddr_in server;
    if ( !harden_parse_address( o.address, &server ) )
    {
        fprintf( stderr, "harden_load hostile: --address must be an IPv4 address:port\n" );
        return 2;
    }

    srand( (unsigned) time( NULL ) );
    harden_hostile_t h;
    const uint64_t now = (uint64_t) time( NULL );
    for ( int i = 0; i < 64; i++ )
    {
        std::vector<uint8_t> request;
        if ( !harden_build_request( o, key, now, 3600, true, &request ) )
        {
            fprintf( stderr, "harden_load hostile: could not build a request\n" );
            return 1;
        }
        h.requests_badsig.push_back( request );
    }
    if ( !harden_build_request( o, key, now - 7200, 60, false, &h.request_expired ) || !harden_build_request( o, key, now, 3600, false, &h.request_valid ) )
    {
        fprintf( stderr, "harden_load hostile: could not build a request\n" );
        return 1;
    }
    if ( enabled[6] )
    {
#if defined( __linux__ )
        if ( o.capture_dev.empty() || !harden_capture( o, 4096, &h.captured ) )
        {
            printf( "harden_verdict: role=hostile verdict=FAIL reason=replay_capture (--capture-dev and CAP_NET_RAW, with players on the wire)\n" );
            return 1;
        }
        printf( "harden_hostile: captured=%zu session packets for replay\n", h.captured.size() );
#else
        printf( "harden_verdict: role=hostile verdict=FAIL reason=replay_needs_linux\n" );
        return 1;
#endif // #if defined( __linux__ )
    }

    std::vector<harden_socket_t> sockets;
    for ( int i = 0; i < o.sockets; i++ )
    {
        harden_socket_t s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
        if ( s == HARDEN_INVALID_SOCKET )
        {
            fprintf( stderr, "harden_load hostile: socket failed\n" );
            return 1;
        }
#if defined( _WIN32 )
        u_long nonblocking = 1;
        ioctlsocket( s, FIONBIO, &nonblocking );
#else
        fcntl( s, F_SETFL, fcntl( s, F_GETFL, 0 ) | O_NONBLOCK );
#endif // #if defined( _WIN32 )
        sockets.push_back( s );
    }

    char utc[32];
    harden_utc( utc, sizeof( utc ) );
    printf( "harden_hostile: address=%s pps=%.0f sockets=%d kinds=%s seconds=%.0f utc=%s\n", o.address.c_str(), o.pps, o.sockets, o.kinds.c_str(), o.seconds, utc );
    fflush( stdout );

    // --threads senders, each with its own share of the sockets and the rate,
    // its own generator and its own counts; one thread's sendto and the
    // per-byte rand() of the first version topped out near 200k a second
    const int num_workers = std::max( 1, std::min( o.threads, (int) sockets.size() ) );
    std::vector<harden_hostile_worker_t> workers( (size_t) num_workers );
    for ( int i = 0; i < (int) sockets.size(); i++ )
    {
        workers[(size_t) ( i % num_workers )].sockets.push_back( sockets[(size_t) i] );
    }
    const double started = harden_now();
    // the hostile load ends a second before the cut, so its last packets and
    // the answers to them are in before the terms are read
    const double hostile_end = o.end_at > 0.0 ? o.end_at - 1.0 : harden_wall() + o.seconds;
    const uint64_t seed = (uint64_t) time( NULL );
    std::vector<std::thread> threads;
    for ( int w = 0; w < num_workers; w++ )
    {
        harden_hostile_worker_t & worker = workers[(size_t) w];
        worker.h = &h;
        worker.enabled = enabled;
        worker.num_enabled = num_enabled;
        worker.server = server;
        worker.pps = o.pps / num_workers;
        worker.started = started;
        worker.hostile_end = hostile_end;
        worker.random = harden_mix64( seed ^ ( (uint64_t) ( w + 1 ) << 32 ) ) | 1;
        threads.emplace_back( harden_hostile_send_loop, &worker );
    }
    uint64_t last_sent_total = 0;
    double last_report = started;
    while ( harden_wall() < hostile_end )
    {
        rocketnet_sleep( 0.05 );
        const double t = harden_now();
        if ( t - last_report >= 1.0 )
        {
            uint64_t sent_total = 0, received_now = 0, errors_now = 0;
            for ( const harden_hostile_worker_t & worker : workers )
            {
                sent_total += worker.progress_sent.load( std::memory_order_relaxed );
                received_now += worker.progress_received.load( std::memory_order_relaxed );
                errors_now += worker.progress_errors.load( std::memory_order_relaxed );
            }
            printf( "harden_hostile_tick: t=%.1f sent_ps=%.0f received_total=%" PRIu64 " send_errors=%" PRIu64 "\n", t - started,
                    (double) ( sent_total - last_sent_total ) / ( t - last_report ), received_now, errors_now );
            fflush( stdout );
            last_sent_total = sent_total;
            last_report = t;
        }
    }
    for ( std::thread & thread : threads )
    {
        thread.join();
    }
    uint64_t sent[HARDEN_HOSTILE_KINDS] = {};
    uint64_t send_errors = 0, received = 0, received_bytes = 0, sent_bytes = 0;
    for ( const harden_hostile_worker_t & worker : workers )
    {
        for ( int k = 0; k < HARDEN_HOSTILE_KINDS; k++ )
        {
            sent[k] += worker.sent[k];
        }
        send_errors += worker.send_errors;
        received += worker.received;
        received_bytes += worker.received_bytes;
        sent_bytes += worker.sent_bytes;
    }
    uint8_t packet[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 128];

    // a last read for late answers, then close every socket
    const double tail = harden_now() + 2.0;
    while ( harden_now() < tail )
    {
        for ( harden_socket_t s : sockets )
        {
            for ( ;; )
            {
                const int n = (int) recv( s, (char *) packet, (int) sizeof( packet ), 0 );
                if ( n < 0 )
                {
                    break;
                }
                received++;
                received_bytes += (uint64_t) n;
            }
        }
        rocketnet_sleep( 0.01 );
    }
    for ( harden_socket_t s : sockets )
    {
        harden_close_socket( s );
    }

    harden_utc( utc, sizeof( utc ) );
    printf( "harden_phase: role=hostile phase=end utc=%s\n", utc );
    uint64_t sent_sum = 0;
    for ( int k = 0; k < HARDEN_HOSTILE_KINDS; k++ )
    {
        char name[64];
        snprintf( name, sizeof( name ), "sent_%s", harden_hostile_kind_names[k] );
        harden_print_term( role, name, (int64_t) sent[k] );
        sent_sum += sent[k];
    }
    harden_print_term( role, "sent", (int64_t) sent_sum );
    harden_print_term( role, "sent_bytes", (int64_t) sent_bytes );
    harden_print_term( role, "send_errors", (int64_t) send_errors );
    harden_print_term( role, "received", (int64_t) received );
    harden_print_term( role, "received_bytes", (int64_t) received_bytes );
    printf( "harden_verdict: role=hostile verdict=%s sent=%" PRIu64 " send_errors=%" PRIu64 "\n", sent_sum > 0 ? "PASS" : "FAIL", sent_sum, send_errors );
    fflush( stdout );
    return sent_sum > 0 ? 0 : 1;
}

// ---------------------------------------------------------------- the reconcile

// every "harden_term: role=R name=value" line of the logs, as R.name

struct harden_terms_t
{
    std::vector<std::string> names;
    std::vector<int64_t> values;

    bool has( const std::string & name ) const
    {
        return std::find( names.begin(), names.end(), name ) != names.end();
    }

    int64_t get( const std::string & name ) const
    {
        for ( size_t i = 0; i < names.size(); i++ )
        {
            if ( names[i] == name )
            {
                return values[i];
            }
        }
        return 0;
    }
};

// "harden_term: role=<role> <name>=<integer>", read by hand (no CRT scan,
// which MSVC deprecates): the role runs to the first space, the name from the
// next non-space to the first '=', and the value is a decimal integer
static bool harden_parse_term( const char * line, std::string * role, std::string * name, int64_t * value )
{
    static const char prefix[] = "harden_term: role=";
    if ( strncmp( line, prefix, sizeof( prefix ) - 1 ) != 0 )
    {
        return false;
    }
    const char * cursor = line + sizeof( prefix ) - 1;
    const char * role_end = cursor;
    while ( *role_end && *role_end != ' ' && *role_end != '\t' && *role_end != '\n' )
    {
        role_end++;
    }
    if ( role_end == cursor || ( *role_end != ' ' && *role_end != '\t' ) )
    {
        return false;
    }
    role->assign( cursor, (size_t) ( role_end - cursor ) );
    cursor = role_end;
    while ( *cursor == ' ' || *cursor == '\t' )
    {
        cursor++;
    }
    const char * equals = strchr( cursor, '=' );
    if ( !equals || equals == cursor )
    {
        return false;
    }
    name->assign( cursor, (size_t) ( equals - cursor ) );
    char * end = NULL;
    const long long parsed = strtoll( equals + 1, &end, 10 );
    if ( end == equals + 1 )
    {
        return false;
    }
    *value = (int64_t) parsed;
    return true;
}

static bool harden_terms_load( const std::string & path, harden_terms_t * terms )
{
    FILE * file = rocketnet_test_fopen( path.c_str(), "r" );
    if ( !file )
    {
        fprintf( stderr, "harden_load reconcile: cannot read %s\n", path.c_str() );
        return false;
    }
    char line[1024];
    while ( fgets( line, sizeof( line ), file ) )
    {
        std::string role, name;
        int64_t value = 0;
        if ( harden_parse_term( line, &role, &name, &value ) )
        {
            terms->names.push_back( role + "." + name );
            terms->values.push_back( value );
        }
    }
    fclose( file );
    return true;
}

// one direction's chain, every term named: what the sending library counted,
// what the sending namespace's kernel took or refused, what the qdisc and the
// device dropped on the way, what the receiving namespace's kernel saw, and
// what the receiving library took

struct harden_chain_t
{
    int64_t library_sent, kernel_out, kernel_sndbuf, qdisc_sent, qdisc_dropped, device_dropped;
    int64_t arrived_in, arrived_inerrors, arrived_noports, arrived_ipdiscards, arrived_device_dropped;
    int64_t library_received;
    // refused by the receiving server transport before its protocol counts the
    // packet received: a server transport's own receive drops (too short or
    // too large) land in PREFILTER_TOO_SHORT and PREFILTER_TOO_LARGE, outside
    // PACKETS_RECEIVED; the protocol's prefilter counts into the same two
    // inside it, so a transport that passes short datagrams up makes this
    // term too large and the chain red (library_gap < 0), never green
    int64_t library_refused_before_received = 0;
};

static bool harden_chain_print( const char * direction, const harden_chain_t & c, int duplicates_expected )
{
    const int64_t send_gap = c.library_sent - ( c.kernel_out + c.kernel_sndbuf );
    const int64_t path_gap = c.kernel_out - ( c.arrived_in + c.arrived_inerrors + c.arrived_noports + c.arrived_ipdiscards + c.qdisc_dropped + c.device_dropped + c.arrived_device_dropped );
    const int64_t receive_gap = c.arrived_in - ( c.library_received + c.library_refused_before_received );
    // netem's duplicates are not counted anywhere: they are the path's
    // excess of arrivals, and must be zero unless the row duplicates
    // the judged equation is the libraries': every packet the sender's
    // library counted sent is counted received by the receiver's library or
    // dropped by a named counter. The kernel's UDP counts step by step are
    // printed beside it: they count an skb, so a GSO send of N datagrams is
    // one in them (and in qdisc drops), and a step gap there names where a
    // library gap went
    const int64_t named_drops = c.kernel_sndbuf + c.arrived_inerrors + c.arrived_noports + c.arrived_ipdiscards + c.qdisc_dropped + c.device_dropped + c.arrived_device_dropped + c.library_refused_before_received;
    const int64_t library_gap = c.library_sent - ( c.library_received + named_drops );
    // the kernel's drops count skbs, and a GSO skb carries several datagrams:
    // a qdisc drop, a send buffer error, or a GSO skb arriving whole (a veth
    // and a UDP_GRO socket keep it whole) at a closed port or a full socket
    // names one where the libraries lost several. The datagrams riding in a
    // GSO skb beyond its first are the send_gap (OutDatagrams counts the skb),
    // so while any of those drops, a gap of at most that many is a dropped
    // GSO skb's later segments; the gap is never below zero
    const int64_t skb_drops = c.qdisc_dropped + c.kernel_sndbuf + c.arrived_noports + c.arrived_inerrors;
    const int64_t gso_slack = skb_drops > 0 && send_gap > 0 ? send_gap : 0;
    const bool ok = duplicates_expected ? library_gap <= 0 : library_gap >= 0 && library_gap <= gso_slack;
    printf( "harden_chain: %s library_sent=%" PRId64 " = library_received=%" PRId64 " + named_drops=%" PRId64 " (library_gap=%" PRId64 "%s", direction, c.library_sent,
            c.library_received, named_drops, library_gap, duplicates_expected && library_gap < 0 ? ", netem duplicates" : "" );
    if ( !duplicates_expected && library_gap > 0 && gso_slack > 0 )
    {
        printf( ", at most gso_slack=%" PRId64 ": later segments of dropped GSO skbs", gso_slack );
    }
    printf( ")\n" );
    printf( "harden_chain: %s library_sent=%" PRId64 " = kernel_out=%" PRId64 " + kernel_sndbuf_errors=%" PRId64 " (send_gap=%" PRId64 ")\n", direction, c.library_sent, c.kernel_out,
            c.kernel_sndbuf, send_gap );
    printf( "harden_chain: %s kernel_out=%" PRId64 " = arrived_udp_in=%" PRId64 " + arrived_udp_inerrors=%" PRId64 " + arrived_udp_noports=%" PRId64 " + arrived_ip_indiscards=%" PRId64
            " + qdisc_dropped=%" PRId64 " + device_tx_dropped=%" PRId64 " + device_rx_dropped=%" PRId64 " (path_gap=%" PRId64 "%s)\n",
            direction, c.kernel_out, c.arrived_in, c.arrived_inerrors, c.arrived_noports, c.arrived_ipdiscards, c.qdisc_dropped, c.device_dropped, c.arrived_device_dropped, path_gap,
            duplicates_expected && path_gap < 0 ? ", netem duplicates" : "" );
    printf( "harden_chain: %s arrived_udp_in=%" PRId64 " = library_received=%" PRId64 " + library_refused_before_received=%" PRId64 " (receive_gap=%" PRId64 ")\n", direction,
            c.arrived_in, c.library_received, c.library_refused_before_received, receive_gap );
    // the qdisc counts each GSO segment it sends and each skb it drops: with
    // nothing dropped it is the datagrams that left this end, so a library
    // gap is the sender's (wire_gap > 0) or the receiver's (wire_gap 0)
    printf( "harden_chain: %s library_sent=%" PRId64 " = qdisc_sent_segments=%" PRId64 " + qdisc_dropped=%" PRId64 " (wire_gap=%" PRId64 ")\n", direction, c.library_sent, c.qdisc_sent,
            c.qdisc_dropped, c.library_sent - c.qdisc_sent - c.qdisc_dropped );
    printf( "harden_chain: %s verdict=%s\n", direction, ok ? "PASS" : "FAIL" );
    return ok;
}

static int harden_reconcile_main( const harden_options_t & o )
{
    harden_terms_t t;
    std::string list = o.logs + ",";
    size_t start = 0;
    for ( size_t comma = list.find( ',' ); comma != std::string::npos; comma = list.find( ',', start ) )
    {
        const std::string path = list.substr( start, comma - start );
        start = comma + 1;
        if ( !path.empty() && !harden_terms_load( path, &t ) )
        {
            return 2;
        }
    }
    if ( !t.has( "server.s2c_packets_sent" ) || !t.has( "clients.c2s_packets_sent" ) || !t.has( "server.c2s_packets_received" ) || !t.has( "clients.s2c_packets_received" ) )
    {
        printf( "harden_reconcile: verdict=FAIL reason=missing_cut (a role ended before its cut)\n" );
        return 1;
    }

    harden_chain_t up;
    up.library_sent = t.get( "clients.c2s_packets_sent" ) + t.get( "hostile.sent" );
    up.kernel_out = t.get( "clients.c2s_udp_out" );
    up.kernel_sndbuf = t.get( "clients.c2s_udp_sndbuf" );
    up.qdisc_sent = t.get( "clients.c2s_tc_sent" );
    up.qdisc_dropped = t.get( "clients.c2s_tc_dropped" );
    up.device_dropped = t.get( "clients.c2s_dev_tx_dropped" );
    up.arrived_in = t.get( "server.c2s_udp_in" );
    up.arrived_inerrors = t.get( "server.c2s_udp_inerrors" );
    up.arrived_noports = t.get( "server.c2s_udp_noports" );
    up.arrived_ipdiscards = t.get( "server.c2s_ip_indiscards" );
    up.arrived_device_dropped = t.get( "server.c2s_dev_rx_dropped" );
    up.library_received = t.get( "server.c2s_packets_received" );
    // a transport that prefilters off the main thread (IOCP's completion
    // workers, for one) counts each datagram it refuses into the public
    // PACKETS_RECEIVED and its reason, and main's stats never see it: the
    // difference is the refusals before main. A transport that drops on its
    // own receive path counts the drop in PREFILTER_TOO_SHORT/TOO_LARGE and
    // in neither received count
    const int64_t refused_off_main = t.get( "server.c2s_transport_PACKETS_RECEIVED" ) - up.library_received;
    if ( refused_off_main > 0 )
    {
        up.library_refused_before_received = refused_off_main;
    }
    else
    {
        up.library_refused_before_received = t.get( "server.c2s_transport_PREFILTER_TOO_SHORT" ) + t.get( "server.c2s_transport_PREFILTER_TOO_LARGE" );
    }

    harden_chain_t down;
    down.library_sent = t.get( "server.s2c_packets_sent" );
    down.kernel_out = t.get( "server.s2c_udp_out" );
    down.kernel_sndbuf = t.get( "server.s2c_udp_sndbuf" );
    down.qdisc_sent = t.get( "server.s2c_tc_sent" );
    down.qdisc_dropped = t.get( "server.s2c_tc_dropped" );
    down.device_dropped = t.get( "server.s2c_dev_tx_dropped" );
    down.arrived_in = t.get( "clients.s2c_udp_in" );
    down.arrived_inerrors = t.get( "clients.s2c_udp_inerrors" );
    down.arrived_noports = t.get( "clients.s2c_udp_noports" );
    down.arrived_ipdiscards = t.get( "clients.s2c_ip_indiscards" );
    down.arrived_device_dropped = t.get( "clients.s2c_dev_rx_dropped" );
    down.library_received = t.get( "clients.s2c_packets_received" ) + t.get( "hostile.received" );

    const bool up_ok = harden_chain_print( "clients_to_server", up, o.netem_duplicates );
    const bool down_ok = harden_chain_print( "server_to_clients", down, o.netem_duplicates );
    // a cloud NIC's allowance counters, not judged (the provider counts a
    // packet queued or dropped): nonzero beside a library gap names the link
    printf( "harden_nic: clients pps_allowance=%" PRId64 " bw_in_allowance=%" PRId64 " bw_out_allowance=%" PRId64 " server pps_allowance=%" PRId64 " bw_in_allowance=%" PRId64
            " bw_out_allowance=%" PRId64 "\n",
            t.get( "clients.s2c_nic_pps_allowance" ), t.get( "clients.s2c_nic_bw_in_allowance" ), t.get( "clients.s2c_nic_bw_out_allowance" ), t.get( "server.s2c_nic_pps_allowance" ),
            t.get( "server.s2c_nic_bw_in_allowance" ), t.get( "server.s2c_nic_bw_out_allowance" ) );

    // the frame: rocketnet's whole span on the game thread, from the frame's
    // start through update, receive, the write (the wait for the fill jobs
    // included) and send, measured once per frame with nothing between the
    // phases left out. Its p99 is held to the aim (--frame-p99-us), which is
    // reported, not judged: the cell's verdict is the chains and the players
    const int64_t p99 = t.get( "server.work_p99_us" );
    const bool frame_ok = p99 <= (int64_t) o.frame_p99_us;
    // delivery: unreliable messages each way, delivered against delivered +
    // lost, held to --min-delivery percent
    const int64_t down_delivered = t.get( "clients.delivered_kind0" );
    const int64_t down_lost = t.get( "clients.lost_kind0" );
    const int64_t up_delivered = t.get( "server.in_delivered_kind3" );
    const int64_t up_lost = t.get( "server.in_lost_kind3" );
    const double down_percent = down_delivered + down_lost > 0 ? 100.0 * (double) down_delivered / (double) ( down_delivered + down_lost ) : 0.0;
    const double up_percent = up_delivered + up_lost > 0 ? 100.0 * (double) up_delivered / (double) ( up_delivered + up_lost ) : 0.0;
    const bool delivery_ok = down_percent >= o.min_delivery && up_percent >= o.min_delivery;
    const bool players_ok = t.get( "clients.errors" ) == 0 && t.get( "server.in_errors" ) == 0 && t.get( "clients.stable_ends" ) == 0 && t.get( "clients.connects" ) > 0 && delivery_ok;
    printf( "harden_players: connects=%" PRId64 " stable_ends=%" PRId64 " unplanned_ends=%" PRId64 " client_errors=%" PRId64 " server_in_errors=%" PRId64
            " s2c_unreliable_delivered=%" PRId64 " lost=%" PRId64 " reordered=%" PRId64 " (%.4f%%) c2s_unreliable_delivered=%" PRId64 " lost=%" PRId64 " reordered=%" PRId64
            " (%.4f%%) bound=%.3f%% reliable_delivered=%" PRId64 " big_delivered=%" PRId64 " verdict=%s\n",
            t.get( "clients.connects" ), t.get( "clients.stable_ends" ), t.get( "clients.unplanned_ends" ), t.get( "clients.errors" ), t.get( "server.in_errors" ),
            down_delivered, down_lost, t.get( "clients.reordered_kind0" ), down_percent, up_delivered, up_lost, t.get( "server.in_reordered_kind3" ), up_percent, o.min_delivery,
            t.get( "clients.delivered_kind1" ), t.get( "clients.delivered_kind2" ), players_ok ? "PASS" : "FAIL" );
    printf( "harden_frame: work_p99_us=%" PRId64 " work_max_us=%" PRId64 " work_p999_us=%" PRId64 " overruns=%" PRId64 " frames=%" PRId64 " aim_p99_us=%d aim=%s\n", p99,
            t.get( "server.work_max_us" ), t.get( "server.work_p999_us" ), t.get( "server.overruns" ), t.get( "server.frames" ), o.frame_p99_us, frame_ok ? "MET" : "MISSED" );
    const bool ok = up_ok && down_ok && players_ok;
    printf( "harden_reconcile: verdict=%s chains=%s players=%s frame_aim=%s\n", ok ? "PASS" : "FAIL", up_ok && down_ok ? "PASS" : "FAIL", players_ok ? "PASS" : "FAIL",
            frame_ok ? "MET" : "MISSED" );
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------- main

static void harden_usage()
{
    fprintf( stderr, "usage: harden_load server|clients|hostile|netstat|reconcile [options] (docs/HARDEN_LOAD.md)\n" );
}

int main( int argc, char ** argv )
{
    harden_options_t o;
    if ( !harden_parse( argc, argv, &o ) )
    {
        harden_usage();
        return 2;
    }
    setvbuf( stdout, NULL, _IOLBF, 1 << 16 );
    if ( o.role == "netstat" )
    {
        return harden_netstat_main( o );
    }
    if ( o.role == "reconcile" )
    {
        return harden_reconcile_main( o );
    }
    harden_kernel_base = harden_kernel_read();
    harden_link_base = harden_link_read( o.dev );
    harden_payload_copy = o.payload_copy;
    if ( harden_payload_copy )
    {
        harden_copy_body_build();
    }

#if defined( _WIN32 )
    WSADATA wsa;
    if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )
    {
        return 1;
    }
#endif // #if defined( _WIN32 )

    process_resources_keep_freed_pages();
    if ( rocketnet_init( NULL, harden_malloc, harden_free ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "harden_load: rocketnet_init failed\n" );
        return 1;
    }
    int result = 2;
    if ( o.role == "server" )
    {
        result = harden_server_main( o );
    }
    else if ( o.role == "clients" )
    {
        result = harden_clients_main( o );
    }
    else if ( o.role == "hostile" )
    {
        result = harden_hostile_main( o );
    }
    else
    {
        harden_usage();
    }
    rocketnet_term();
    return result;
}
