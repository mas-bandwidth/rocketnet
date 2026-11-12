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
    byte. Inbound messages from the clients are verified the same way. Each
    second it prints "harden_tick:" (connected, frames, overruns past one
    frame, the work time per frame avg/p99/max split into update, receive,
    write and send_packets, messages and packets per second, start refusals),
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
    in total, a mix of --kinds: garbage (random bytes), prefixed (the
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
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_base64.h"
#include "rocketnet_read_write.h"
#include "tests/process_resources.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#include <winsock2.h>
#include <ws2tcpip.h>
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
#define HARDEN_CHANNEL_RELIABLE 1
#define HARDEN_CHANNEL_BIG 2
#define HARDEN_MAX_SIZES 16
#define HARDEN_MAX_THREADS 64
#define HARDEN_INPUT_BYTES 64
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

static uint8_t harden_pattern_byte( uint64_t client_id, uint32_t connection, uint32_t kind, uint32_t sequence, int i )
{
    const uint64_t word = harden_mix64( client_id ^ ( (uint64_t) connection << 40 ) ^ ( (uint64_t) kind << 56 ) ^ ( (uint64_t) sequence << 8 ) ^ (uint64_t) ( i / 8 ) );
    return (uint8_t) ( word >> ( 8 * ( i % 8 ) ) );
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

static void harden_write_message( uint8_t * p, int bytes, uint32_t kind, uint64_t client_id, uint32_t connection, uint32_t sequence )
{
    uint8_t * cursor = p;
    rocketnet_write_uint32( &cursor, HARDEN_MAGIC );
    rocketnet_write_uint32( &cursor, kind );
    rocketnet_write_uint64( &cursor, client_id );
    rocketnet_write_uint32( &cursor, connection );
    rocketnet_write_uint32( &cursor, sequence );
    rocketnet_write_uint32( &cursor, (uint32_t) bytes );
    for ( int i = HARDEN_HEADER_BYTES; i < bytes; i++ )
    {
        p[i] = harden_pattern_byte( client_id, connection, kind, sequence, i );
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
    for ( int i = HARDEN_HEADER_BYTES; i < bytes; i++ )
    {
        if ( p[i] != harden_pattern_byte( out->client_id, out->connection, out->kind, out->sequence, i ) )
        {
            return 0;
        }
    }
    return 1;
}

// one receive stream: unreliable kinds count gaps as loss and a step back as
// an error; reliable ordered kinds must arrive exactly in order

struct harden_stream_t
{
    uint32_t connection = 0;
    uint32_t expected = 0;
    bool started = false;
};

struct harden_verify_t
{
    uint64_t delivered[HARDEN_KINDS] = {};
    uint64_t lost[HARDEN_KINDS] = {};
    uint64_t head[HARDEN_KINDS] = {};
    uint64_t errors = 0;
    uint64_t wrong_owner = 0;
    uint64_t reliable_gaps = 0;
    uint64_t back = 0;
};

static void harden_stream_take( harden_stream_t * stream, const harden_message_t * m, bool reliable, harden_verify_t * v )
{
    if ( !stream->started || stream->connection != m->connection )
    {
        // a new connection's first message: sequences start at 0, so what
        // came before it was sent before this end could take it
        stream->started = true;
        stream->connection = m->connection;
        v->head[m->kind] += m->sequence;
        stream->expected = m->sequence;
    }
    if ( m->sequence < stream->expected )
    {
        v->back++;
        v->errors++;
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
    int base = 1;
    int burst = 15;
    int burst_clients = 0;
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
    std::string logs;
    int netem_duplicates = 0;
    int frame_p99_us = 12000;
};

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
        else if ( strcmp( a, "--frame-p99-us" ) == 0 )
        {
            o->frame_p99_us = atoi( v );
        }
        else if ( strcmp( a, "--warm-seconds" ) == 0 )
        {
            o->warm_seconds = atoi( v );
        }
        else
        {
            fprintf( stderr, "harden_load: unknown option %s\n", a );
            return false;
        }
    }
    if ( o->max_clients < 1 || o->clients < 0 || o->threads < 1 || o->threads > HARDEN_MAX_THREADS || o->seconds <= 0.0 || o->hz <= 0.0 ||
         o->join_rate <= 0.0 || o->churn_clients < 0 || o->churn_clients > o->clients || o->churn_rate < 0.0 || o->messages < 0 ||
         o->base < 0 || o->burst < 0 || o->burst_clients < 0 || o->reliable_every < 0 || o->reliable_bytes < HARDEN_HEADER_BYTES ||
         o->big_every < 0 || o->big_bytes < HARDEN_HEADER_BYTES || o->sample_seconds < 1 || o->quiet_seconds < 0.1 || o->token_timeout < 1 ||
         o->pps < 0.0 || o->sockets < 1 || o->sockets > 4096 || o->warm_seconds < 0 )
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
};

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

static void harden_print_cut_network( const char * role, const harden_options_t & o )
{
    const harden_kernel_t k = harden_kernel_read();
    const harden_kernel_t & b = harden_kernel_base;
    printf( "harden_term: role=%s cut_kernel_read=%d\n", role, k.read && b.read ? 1 : 0 );
    harden_print_term( role, "cut_udp_in", k.udp_in - b.udp_in );
    harden_print_term( role, "cut_udp_noports", k.udp_noports - b.udp_noports );
    harden_print_term( role, "cut_udp_inerrors", k.udp_inerrors - b.udp_inerrors );
    harden_print_term( role, "cut_udp_rcvbuf", k.udp_rcvbuf - b.udp_rcvbuf );
    harden_print_term( role, "cut_udp_out", k.udp_out - b.udp_out );
    harden_print_term( role, "cut_udp_sndbuf", k.udp_sndbuf - b.udp_sndbuf );
    harden_print_term( role, "cut_udp_incsum", k.udp_incsum - b.udp_incsum );
    harden_print_term( role, "cut_icmp_in", k.icmp_in - b.icmp_in );
    harden_print_term( role, "cut_icmp_out", k.icmp_out - b.icmp_out );
    harden_print_term( role, "cut_ip_inreceives", k.ip_inreceives - b.ip_inreceives );
    harden_print_term( role, "cut_ip_indiscards", k.ip_indiscards - b.ip_indiscards );
    harden_print_term( role, "cut_ip_outrequests", k.ip_outrequests - b.ip_outrequests );
    harden_print_term( role, "cut_ip_outdiscards", k.ip_outdiscards - b.ip_outdiscards );
    if ( !o.dev.empty() )
    {
        const harden_link_t l = harden_link_read( o.dev );
        const harden_link_t & lb = harden_link_base;
        harden_print_term( role, "cut_tc_sent", l.tc_sent - lb.tc_sent );
        harden_print_term( role, "cut_tc_dropped", l.tc_dropped - lb.tc_dropped );
        harden_print_term( role, "cut_dev_tx_packets", l.tx_packets - lb.tx_packets );
        harden_print_term( role, "cut_dev_tx_dropped", l.tx_dropped - lb.tx_dropped );
        harden_print_term( role, "cut_dev_rx_packets", l.rx_packets - lb.rx_packets );
        harden_print_term( role, "cut_dev_rx_dropped", l.rx_dropped - lb.rx_dropped );
    }
}

// the kernel's UDP counts again, a quiet interval after the cut: equal to
// the cut's unless something sent across it

static void harden_print_settle( const char * role )
{
    const harden_kernel_t k = harden_kernel_read();
    const harden_kernel_t & b = harden_kernel_base;
    harden_print_term( role, "settle_udp_in", k.udp_in - b.udp_in );
    harden_print_term( role, "settle_udp_out", k.udp_out - b.udp_out );
    harden_print_term( role, "settle_udp_noports", k.udp_noports - b.udp_noports );
    harden_print_term( role, "settle_udp_inerrors", k.udp_inerrors - b.udp_inerrors );
    fflush( stdout );
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
    for ( int i = 0; i < ROCKETNET_SERVER_PUBLIC_KEY_BYTES; i++ )
    {
        unsigned value = 0;
        if ( sscanf( hex.c_str() + 2 * i, "%2x", &value ) != 1 )
        {
            return false;
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

static void harden_print_resources( const char * tag, double t, const process_resources_t & r )
{
    printf( "harden_sample: tag=%s t=%.0f", tag, t );
    for ( int i = 0; i < PROCESS_RESOURCES_COUNT; i++ )
    {
        printf( " %s=%" PRId64, process_resources_name( i ), r.values[i] );
    }
    printf( " library_bytes=%" PRId64, harden_library_bytes() );
}

static int harden_server_main( const harden_options_t & o )
{
    const char * role = "server";
    static const uint8_t dev_backend_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

    struct rocketnet_server_config_t config;
    rocketnet_server_default_config( &config );
    config.protocol_id = HARDEN_PROTOCOL_ID;
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
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, o.address.c_str(), NULL, &config, &status );
    if ( !server || rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "harden_load server: create or start failed (status %d)\n", (int) status );
        printf( "harden_verdict: role=server verdict=FAIL reason=create\n" );
        return 1;
    }
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
    printf( "harden_ready: address=%s port=%d transport=%s key=%s utc=%s max_clients=%d hz=%.0f base=%d burst=%d burst_clients=%d reliable_every=%d reliable_bytes=%d big_every=%d big_bytes=%d sizes=",
            o.address.c_str(), (int) rocketnet_server_port( server ), granted, key_hex, utc, o.max_clients, o.hz, o.base, o.burst, o.burst_clients,
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

    // the end is a cut (the method of load's counters_agree): at end_at the
    // writes stop and the clients stop sending, so client to server drains;
    // at end_at + quiet the server stops sending too, so server to client
    // drains; at end_at + 2 quiet every process reads its terms with nothing
    // in flight. The cleanup traffic after the cut is outside the equations.
    const double end_at = o.end_at > 0.0 ? o.end_at : harden_wall() + o.seconds;
    bool sending = true;
    for ( ;; )
    {
        const double frame_start = harden_now();
        const double wall = harden_wall();
        if ( !draining && wall >= end_at )
        {
            draining = true;
            harden_utc( utc, sizeof( utc ) );
            printf( "harden_phase: role=server phase=writes_stop t=%.1f utc=%s\n", frame_start - started, utc );
            fflush( stdout );
        }
        if ( sending && wall >= end_at + o.quiet_seconds )
        {
            sending = false;
            harden_utc( utc, sizeof( utc ) );
            printf( "harden_phase: role=server phase=sends_stop t=%.1f utc=%s\n", frame_start - started, utc );
            fflush( stdout );
        }
        if ( wall >= end_at + 2.0 * o.quiet_seconds )
        {
            break;
        }

        rocketnet_server_update( server, frame_start );
        const double t_update = harden_now();

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
        const double t_receive = harden_now();

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
                // the bursting clients rotate: a window of burst_clients slots
                // among the connected ones, advancing burst_clients each tick
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
                bursting_seen++;
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
            }
        }
        const double t_write = harden_now();

        if ( sending )
        {
            rocketnet_server_send_packets( server );
        }
        const double t_send = harden_now();

        const double work = t_send - frame_start;
        w.work.push_back( work );
        all_work.push_back( work );
        all_work_max = std::max( all_work_max, work );
        w.update_sum += t_update - frame_start;
        w.receive_sum += t_receive - t_update;
        w.write_sum += t_write - t_receive;
        w.send_sum += t_send - t_write;
        w.update_max = std::max( w.update_max, t_update - frame_start );
        w.receive_max = std::max( w.receive_max, t_receive - t_update );
        w.write_max = std::max( w.write_max, t_write - t_receive );
        w.send_max = std::max( w.send_max, t_send - t_write );
        w.frames++;
        if ( work > step )
        {
            w.overruns++;
        }
        tick++;

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
                    " msgs_ps=%.0f pkts_ps=%.0f in_msgs_ps=%.0f start_refused=%" PRIu64 " send_ring_full_total=%" PRIu64 "\n",
                    t_now - started, rocketnet_server_num_connected_clients( server ), w.frames, w.overruns,
                    1e3 * ( w.update_sum + w.receive_sum + w.write_sum + w.send_sum ) / f, 1e3 * harden_percentile( work_copy, 0.99 ), 1e3 * harden_percentile( work_copy, 1.0 ),
                    1e3 * w.update_sum / f, 1e3 * w.update_max, 1e3 * w.receive_sum / f, 1e3 * w.receive_max, 1e3 * w.write_sum / f, 1e3 * w.write_max,
                    1e3 * w.send_sum / f, 1e3 * w.send_max, (double) w.messages / span, (double) ( stats.packets_sent - last_packets_sent ) / span,
                    (double) w.received / span, w.start_refused, ring_full );
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
            if ( !resources_started )
            {
                const int64_t touched = process_resources_touch_all();
                printf( "harden_sample: tag=touch bytes=%" PRId64 "\n", touched );
            }
            if ( !process_resources_read( &r ) )
            {
                printf( "harden_verdict: role=server verdict=FAIL reason=resource_read\n" );
                return 1;
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

    total_messages += w.messages;
    total_start_refused += w.start_refused;
    total_frames += w.frames;
    total_overruns += w.overruns;

    // the end sample, before stop: the same objects alive as at the
    // post-warmup sample (the server running, its clients gone or going)
    process_resources_t resources_end;
    const bool end_read = process_resources_read( &resources_end );
    const int64_t library_end = harden_library_bytes();
    const double run_seconds = harden_now() - resources_started_at;

    // the cut: nothing in flight either way; every term read here
    {
        struct rocketnet_server_stats_t cut;
        rocketnet_server_stats( server, &cut );
        harden_utc( utc, sizeof( utc ) );
        printf( "harden_phase: role=server phase=cut utc=%s\n", utc );
        harden_print_term( role, "cut_packets_sent", (int64_t) cut.packets_sent );
        harden_print_term( role, "cut_packets_received", (int64_t) cut.packets_received );
        harden_print_counters( "cut_transport", role, transport ? rocketnet_server_transport_counters( transport ) : NULL, true );
        harden_print_cut_network( role, o );
        fflush( stdout );
    }
    // the clients read their own cut now; the server's stop (its disconnects)
    // waits until they have
    rocketnet_sleep( o.quiet_seconds );
    harden_print_settle( role );

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
    }
    harden_print_term( role, "in_errors", (int64_t) verify.errors );
    harden_print_term( role, "in_reliable_gaps", (int64_t) verify.reliable_gaps );
    harden_print_term( role, "in_back", (int64_t) verify.back );
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
    uint64_t cut_counters[HARDEN_CLIENT_SLOTS] = {};
    int cut_threads = 0;
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
    return rocketnet_create_dev_connect_token( o.address.c_str(), NULL, key, 3600, timeout_seconds, client_id, HARDEN_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, token ) == ROCKETNET_STATUS_OK;
}

// reads the connection's transport counters into the totals, once its
// transport has stopped (disconnected is true): the counters are the
// connection's, and the next connect destroys it

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
    bool cut_done = false;
    while ( !totals->quit.load() )
    {
        const double t = harden_now();
        const double wall = harden_wall();
        const bool cleanup = totals->stop_traffic.load();
        // from end_at to the cleanup: no writes, no connects, no send_packets
        // (the cut's first half); the cleanup after the cut sends again
        const bool stop_traffic = cleanup || wall >= totals->end_at;
        const bool sends = cleanup || wall < totals->end_at;
        // the last churn event leaves room for a silent stop to time out
        // and its client to come back before the end
        const bool churn_open = wall < totals->end_at - (double) o->token_timeout - 6.0;
        int connected = 0;

        if ( !cut_done && wall >= totals->end_at + 2.0 * o->quiet_seconds )
        {
            // the cut: the connections read so far, and every one still open
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
            for ( int k = 0; k < HARDEN_CLIENT_SLOTS; k++ )
            {
                totals->cut_counters[k] += cut[(size_t) k];
            }
            totals->cut_threads++;
            cut_done = true;
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
                            uint8_t * p = rocketnet_client_start_message( c.client, HARDEN_CHANNEL_UNRELIABLE, ROCKETNET_RELIABILITY_UNRELIABLE, HARDEN_INPUT_BYTES );
                            if ( !p )
                            {
                                inputs_refused++;
                                break;
                            }
                            harden_write_message( p, HARDEN_INPUT_BYTES, HARDEN_KIND_INPUT, c.client_id, c.connection_serial, c.next_input++ );
                            rocketnet_client_finish_message( c.client, HARDEN_INPUT_BYTES );
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
    }
    totals->verify.errors += verify.errors;
    totals->verify.wrong_owner += verify.wrong_owner;
    totals->verify.reliable_gaps += verify.reliable_gaps;
    totals->verify.back += verify.back;
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
    printf( "harden_clients: address=%s clients=%d threads=%d first_id=%d hz=%.0f join_rate=%.0f churn_clients=%d churn_rate=%.2f messages=%d token_timeout=%d seconds=%.0f utc=%s\n",
            o.address.c_str(), o.clients, o.threads, o.first_id, o.hz, o.join_rate, o.churn_clients, o.churn_rate, o.messages, o.token_timeout, o.seconds, utc );
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
    for ( ;; )
    {
        rocketnet_sleep( 0.05 );
        {
            std::lock_guard<std::mutex> lock( totals.mutex );
            if ( totals.cut_threads == o.threads )
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

    // the cut: every thread has read its connections, nothing is in flight
    harden_utc( utc, sizeof( utc ) );
    printf( "harden_phase: role=clients phase=cut utc=%s\n", utc );
    harden_print_term( role, "cut_packets_sent", (int64_t) totals.cut_counters[HARDEN_SLOT_PACKETS_SENT] );
    harden_print_term( role, "cut_packets_received", (int64_t) totals.cut_counters[HARDEN_SLOT_PACKETS_RECEIVED] );
    for ( int k = 0; k < rocketnet_num_counters() && k < HARDEN_COUNTER_SLOTS; k++ )
    {
        if ( totals.cut_counters[k] )
        {
            printf( "harden_term: role=clients cut_transport_%s=%" PRIu64 "\n", rocketnet_counter_name( k ), totals.cut_counters[k] );
        }
    }
    harden_print_cut_network( role, o );
    fflush( stdout );

    // the cleanup, after the server's own cut read: every client leaves
    rocketnet_sleep( o.quiet_seconds );
    harden_print_settle( role );
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
    }
    harden_print_term( role, "errors", (int64_t) v.errors );
    harden_print_term( role, "reliable_gaps", (int64_t) v.reliable_gaps );
    harden_print_term( role, "back", (int64_t) v.back );
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
            printf( "harden_term: role=clients transport_%s=%" PRIu64 "\n", rocketnet_counter_name( k ), totals.counters[k] );
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
    if ( rocketnet_create_dev_connect_token( o.address.c_str(), NULL, key, expire_seconds, 5, UINT64_C( 0x7fff000000000000 ) + ( timestamp & 0xffff ), HARDEN_PROTOCOL_ID, timestamp, NULL, token_text ) != ROCKETNET_STATUS_OK )
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
// with AF_PACKET, UDP to the server's port
static bool harden_capture( const harden_options_t & o, int want, std::vector<std::vector<uint8_t>> * out )
{
    struct sockaddr_in server;
    if ( !harden_parse_address( o.address, &server ) )
    {
        return false;
    }
    const int s = socket( AF_PACKET, SOCK_DGRAM, htons( ETH_P_IP ) );
    if ( s < 0 )
    {
        fprintf( stderr, "harden_load hostile: replay needs AF_PACKET (CAP_NET_RAW): %s\n", strerror( errno ) );
        return false;
    }
    struct sockaddr_ll bind_address;
    memset( &bind_address, 0, sizeof( bind_address ) );
    bind_address.sll_family = AF_PACKET;
    bind_address.sll_protocol = htons( ETH_P_IP );
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
        if ( ip->protocol != IPPROTO_UDP || ip->daddr != server.sin_addr.s_addr )
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

    uint64_t sent[HARDEN_HOSTILE_KINDS] = {};
    uint64_t send_errors = 0, received = 0, received_bytes = 0, sent_bytes = 0;
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    static const uint8_t session_types[] = { ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_RESPONSE_PACKET };
    const double started = harden_now();
    double last_report = started;
    uint64_t due_total = 0, sent_total = 0, last_sent_total = 0;
    int socket_cursor = 0, kind_cursor = 0;
    uint8_t packet[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 128];
    // the hostile load ends a second before the cut, so its last packets and
    // the answers to them are in before the terms are read
    const double hostile_end = o.end_at > 0.0 ? o.end_at - 1.0 : harden_wall() + o.seconds;
    while ( harden_wall() < hostile_end )
    {
        const double t = harden_now();
        due_total = (uint64_t) ( ( t - started ) * o.pps );
        int burst = 0;
        while ( sent_total < due_total && burst < 4096 && num_enabled > 0 )
        {
            int kind = kind_cursor;
            while ( !enabled[kind] )
            {
                kind = ( kind + 1 ) % HARDEN_HOSTILE_KINDS;
            }
            kind_cursor = ( kind + 1 ) % HARDEN_HOSTILE_KINDS;
            int bytes = 0;
            const uint8_t * data = packet;
            switch ( kind )
            {
                case 0:
                {
                    bytes = 1 + rand() % 1400;
                    for ( int i = 0; i < bytes; i++ )
                    {
                        packet[i] = (uint8_t) rand();
                    }
                    break;
                }
                case 1:
                {
                    const uint8_t type = session_types[(size_t) rand() % sizeof( session_types )];
                    uint8_t * p = packet;
                    rocketnet_write_prefilter_header( &p, type, filter );
                    bytes = rocketnet_session_min_wire_bytes( type ) + rand() % 1200;
                    for ( int i = ROCKETNET_PREFILTER_BYTES; i < bytes; i++ )
                    {
                        packet[i] = (uint8_t) rand();
                    }
                    break;
                }
                case 2:
                {
                    const uint8_t type = session_types[(size_t) rand() % sizeof( session_types )];
                    uint8_t * p = packet;
                    rocketnet_write_prefilter_header( &p, type, filter );
                    const int minimum = rocketnet_session_min_wire_bytes( type );
                    bytes = 1 + rand() % ( minimum > 1 ? minimum - 1 : 1 );
                    for ( int i = ROCKETNET_PREFILTER_BYTES; i < bytes; i++ )
                    {
                        packet[i] = (uint8_t) rand();
                    }
                    break;
                }
                case 3:
                {
                    const std::vector<uint8_t> & r = h.requests_badsig[(size_t) rand() % h.requests_badsig.size()];
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
                    const std::vector<uint8_t> & r = h.captured[(size_t) rand() % h.captured.size()];
                    data = r.data();
                    bytes = (int) r.size();
                    break;
                }
            }
            const harden_socket_t s = sockets[(size_t) socket_cursor];
            socket_cursor = ( socket_cursor + 1 ) % (int) sockets.size();
            const int result = (int) sendto( s, (const char *) data, (size_t) bytes, 0, (const struct sockaddr *) &server, sizeof( server ) );
            if ( result == bytes )
            {
                sent[kind]++;
                sent_bytes += (uint64_t) bytes;
            }
            else
            {
                send_errors++;
            }
            sent_total++;
            burst++;
        }
        // read whatever the server answered
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
        if ( t - last_report >= 1.0 )
        {
            printf( "harden_hostile_tick: t=%.1f sent_ps=%.0f received_total=%" PRIu64 " send_errors=%" PRIu64 "\n", t - started,
                    (double) ( sent_total - last_sent_total ) / ( t - last_report ), received, send_errors );
            fflush( stdout );
            last_sent_total = sent_total;
            last_report = t;
        }
        rocketnet_sleep( 0.001 );
    }

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

static bool harden_terms_load( const std::string & path, harden_terms_t * terms )
{
    FILE * file = fopen( path.c_str(), "r" );
    if ( !file )
    {
        fprintf( stderr, "harden_load reconcile: cannot read %s\n", path.c_str() );
        return false;
    }
    char line[1024];
    while ( fgets( line, sizeof( line ), file ) )
    {
        char role[64], name[256];
        long long value = 0;
        if ( sscanf( line, "harden_term: role=%63s %255[^=]=%lld", role, name, &value ) == 3 )
        {
            terms->names.push_back( std::string( role ) + "." + name );
            terms->values.push_back( (int64_t) value );
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
    int64_t library_sent, kernel_out, kernel_sndbuf, qdisc_dropped, device_dropped;
    int64_t arrived_in, arrived_inerrors, arrived_noports, arrived_ipdiscards;
    int64_t library_received;
};

static bool harden_chain_print( const char * direction, const harden_chain_t & c, int duplicates_expected )
{
    const int64_t send_gap = c.library_sent - ( c.kernel_out + c.kernel_sndbuf );
    const int64_t path_gap = c.kernel_out - ( c.arrived_in + c.arrived_inerrors + c.arrived_noports + c.arrived_ipdiscards + c.qdisc_dropped + c.device_dropped );
    const int64_t receive_gap = c.arrived_in - c.library_received;
    // netem's duplicates are not counted anywhere: they are the path's
    // excess of arrivals, and must be zero unless the row duplicates
    const bool path_ok = duplicates_expected ? path_gap <= 0 : path_gap == 0;
    const bool ok = send_gap == 0 && receive_gap == 0 && path_ok;
    printf( "harden_chain: %s library_sent=%" PRId64 " = kernel_out=%" PRId64 " + kernel_sndbuf_errors=%" PRId64 " (send_gap=%" PRId64 ")\n", direction, c.library_sent, c.kernel_out,
            c.kernel_sndbuf, send_gap );
    printf( "harden_chain: %s kernel_out=%" PRId64 " = arrived_udp_in=%" PRId64 " + arrived_udp_inerrors=%" PRId64 " + arrived_udp_noports=%" PRId64 " + arrived_ip_indiscards=%" PRId64
            " + qdisc_dropped=%" PRId64 " + device_tx_dropped=%" PRId64 " (path_gap=%" PRId64 "%s)\n",
            direction, c.kernel_out, c.arrived_in, c.arrived_inerrors, c.arrived_noports, c.arrived_ipdiscards, c.qdisc_dropped, c.device_dropped, path_gap,
            duplicates_expected && path_gap < 0 ? ", netem duplicates" : "" );
    printf( "harden_chain: %s arrived_udp_in=%" PRId64 " = library_received=%" PRId64 " (receive_gap=%" PRId64 ")\n", direction, c.arrived_in, c.library_received, receive_gap );
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
    if ( !t.has( "server.cut_packets_sent" ) || !t.has( "clients.cut_packets_sent" ) )
    {
        printf( "harden_reconcile: verdict=FAIL reason=missing_cut (a role ended before its cut)\n" );
        return 1;
    }

    harden_chain_t up;
    up.library_sent = t.get( "clients.cut_packets_sent" ) + t.get( "hostile.sent" );
    up.kernel_out = t.get( "clients.cut_udp_out" );
    up.kernel_sndbuf = t.get( "clients.cut_udp_sndbuf" );
    up.qdisc_dropped = t.get( "clients.cut_tc_dropped" );
    up.device_dropped = t.get( "clients.cut_dev_tx_dropped" );
    up.arrived_in = t.get( "server.cut_udp_in" );
    up.arrived_inerrors = t.get( "server.cut_udp_inerrors" );
    up.arrived_noports = t.get( "server.cut_udp_noports" );
    up.arrived_ipdiscards = t.get( "server.cut_ip_indiscards" );
    up.library_received = t.get( "server.cut_packets_received" );

    harden_chain_t down;
    down.library_sent = t.get( "server.cut_packets_sent" );
    down.kernel_out = t.get( "server.cut_udp_out" );
    down.kernel_sndbuf = t.get( "server.cut_udp_sndbuf" );
    down.qdisc_dropped = t.get( "server.cut_tc_dropped" );
    down.device_dropped = t.get( "server.cut_dev_tx_dropped" );
    down.arrived_in = t.get( "clients.cut_udp_in" );
    down.arrived_inerrors = t.get( "clients.cut_udp_inerrors" );
    down.arrived_noports = t.get( "clients.cut_udp_noports" );
    down.arrived_ipdiscards = t.get( "clients.cut_ip_indiscards" );
    down.library_received = t.get( "clients.cut_packets_received" ) + t.get( "hostile.received" );

    const bool up_ok = harden_chain_print( "clients_to_server", up, o.netem_duplicates );
    const bool down_ok = harden_chain_print( "server_to_clients", down, o.netem_duplicates );

    // the players: delivery verified, nobody stable dropped, the frame held
    const int64_t p99 = t.get( "server.work_p99_us" );
    const bool frame_ok = p99 <= (int64_t) o.frame_p99_us;
    const bool players_ok = t.get( "clients.errors" ) == 0 && t.get( "server.in_errors" ) == 0 && t.get( "clients.stable_ends" ) == 0 && t.get( "clients.connects" ) > 0;
    const int64_t burst_delivered = t.get( "clients.delivered_kind0" );
    const int64_t burst_lost = t.get( "clients.lost_kind0" );
    printf( "harden_players: connects=%" PRId64 " stable_ends=%" PRId64 " unplanned_ends=%" PRId64 " client_errors=%" PRId64 " server_in_errors=%" PRId64
            " unreliable_delivered=%" PRId64 " unreliable_lost=%" PRId64 " (%.4f%%) reliable_delivered=%" PRId64 " big_delivered=%" PRId64 " verdict=%s\n",
            t.get( "clients.connects" ), t.get( "clients.stable_ends" ), t.get( "clients.unplanned_ends" ), t.get( "clients.errors" ), t.get( "server.in_errors" ),
            burst_delivered, burst_lost, burst_delivered + burst_lost > 0 ? 100.0 * (double) burst_lost / (double) ( burst_delivered + burst_lost ) : 0.0,
            t.get( "clients.delivered_kind1" ), t.get( "clients.delivered_kind2" ), players_ok ? "PASS" : "FAIL" );
    printf( "harden_frame: work_p99_us=%" PRId64 " work_p999_us=%" PRId64 " work_max_us=%" PRId64 " overruns=%" PRId64 " frames=%" PRId64 " bound_p99_us=%d verdict=%s\n", p99,
            t.get( "server.work_p999_us" ), t.get( "server.work_max_us" ), t.get( "server.overruns" ), t.get( "server.frames" ), o.frame_p99_us, frame_ok ? "PASS" : "FAIL" );
    const bool ok = up_ok && down_ok && frame_ok && players_ok;
    printf( "harden_reconcile: verdict=%s chains=%s players=%s frame=%s\n", ok ? "PASS" : "FAIL", up_ok && down_ok ? "PASS" : "FAIL", players_ok ? "PASS" : "FAIL",
            frame_ok ? "PASS" : "FAIL" );
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
