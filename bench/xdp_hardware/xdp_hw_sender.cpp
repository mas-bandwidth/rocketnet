/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    xdp_hw_sender -- the hardware tier's sender. Two modes:

    flood: an AF_XDP transmitter on the sender port (a second port of the
    same NIC cabled back to the receiver's, in its own network namespace, or
    a port on a second machine on a direct cable). It emits RocketNet frames
    built by sdk/rocketnet_xdp_frame.h: PAYLOAD packets at the smallest wire
    size the server accepts, carrying the prefilter header the receiver's
    XDP program checks, from --flows distinct UDP source ports so receive
    side scaling spreads them across the receiver's queues. Without a
    session the server counts each one PAYLOAD_RECEIVED then
    PAYLOAD_NO_MAPPING (an EXPECTED outcome, not a refusal). Transmit only:
    no XDP program is attached on the sender port.

        xdp_hw_sender flood --iface <sender-iface> --queues <n> --cores <c0,c1,...>
                            --src <ipv4> --dst <ipv4:port> --dst-mac <aa:bb:..>
                            [--flows <n>] [--payload-bytes <n>]

        stdin:  send <pps> <seconds>   pace at pps for seconds, then stop,
                                       wait for every completion, and answer
                DONE pps=.. seconds=.. sent=.. submitted=.. back_pressure=..
                     tx_invalid=.. elapsed_ns=..
                quit

        sent counts completions: frames the driver reports transmitted.

    probe: a library client on its ordinary socket path, connected to the
    receiver with a development connect token, sending payloads that carry
    a sequence number and the CLOCK_MONOTONIC send time. The receiver reads
    the time on delivery; both ends share the host's clock in the
    self-contained setup, and the result is labelled software.

        xdp_hw_sender probe --server <ipv4:port> --count <n> --interval-us <us>

    Started and stopped by run.sh inside one held run; never left running.
*/

#include "rocketnet.h"
#include "rocketnet_xdp_frame.h"

#include <xdp/xsk.h>
#include <linux/if_xdp.h>
#include <linux/if_link.h>

#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define XDPHW_PROBE_MAGIC     "RNXDPHW1"
#define XDPHW_NUM_FRAMES      4096
#define XDPHW_FRAME_SIZE      XSK_UMEM__DEFAULT_FRAME_SIZE
#define XDPHW_BATCH           64
#define XDPHW_DRAIN_NS        2000000000ULL

static uint64_t monotonic_ns()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static bool parse_mac( const char * text, uint8_t * mac )
{
    unsigned int b[6];
    if ( sscanf( text, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5] ) != 6 )
        return false;
    for ( int i = 0; i < 6; i++ )
        mac[i] = (uint8_t) b[i];
    return true;
}

static bool read_mac( const char * iface, uint8_t * mac )
{
    char path[256];
    snprintf( path, sizeof( path ), "/sys/class/net/%s/address", iface );
    FILE * f = fopen( path, "r" );
    if ( !f )
        return false;
    char text[64] = { 0 };
    const bool ok = fgets( text, sizeof( text ), f ) != NULL && parse_mac( text, mac );
    fclose( f );
    return ok;
}

// ---------------------------------------------------------------- flood

struct queue_sender_t
{
    int queue;
    int core;
    void * buffer;
    struct xsk_umem * umem;
    struct xsk_ring_prod fill;
    struct xsk_ring_cons completion;
    struct xsk_socket * xsk;
    struct xsk_ring_prod tx;
    bool zerocopy;
    int frame_bytes;
    std::vector<uint64_t> free_frames;
    uint64_t outstanding;

    // per window, written by the thread, read by main after done
    uint64_t submitted;
    uint64_t completed;
    uint64_t back_pressure;
};

struct window_t
{
    std::atomic<uint64_t> generation{ 0 };
    std::atomic<int> done{ 0 };
    std::atomic<bool> quit{ false };
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    double pps_per_queue = 0.0;
};

static window_t window;

static void pin_to_core( int core )
{
    cpu_set_t set;
    CPU_ZERO( &set );
    CPU_SET( core, &set );
    pthread_setaffinity_np( pthread_self(), sizeof( set ), &set );
}

static void reclaim( queue_sender_t & q )
{
    uint32_t index = 0;
    const uint32_t n = xsk_ring_cons__peek( &q.completion, XDPHW_NUM_FRAMES, &index );
    for ( uint32_t i = 0; i < n; i++ )
        q.free_frames.push_back( *xsk_ring_cons__comp_addr( &q.completion, index + i ) );
    if ( n )
    {
        xsk_ring_cons__release( &q.completion, n );
        q.outstanding -= n;
        q.completed += n;
    }
}

static void kick( queue_sender_t & q )
{
    if ( xsk_ring_prod__needs_wakeup( &q.tx ) )
        sendto( xsk_socket__fd( q.xsk ), NULL, 0, MSG_DONTWAIT, NULL, 0 );
}

static void queue_thread( queue_sender_t * q )
{
    pin_to_core( q->core );
    uint64_t seen = 0;
    while ( !window.quit.load() )
    {
        const uint64_t generation = window.generation.load();
        if ( generation == seen )
        {
            reclaim( *q );
            sched_yield();
            continue;
        }
        seen = generation;
        q->submitted = 0;
        q->completed = 0;
        q->back_pressure = 0;

        const uint64_t start = window.start_ns;
        const uint64_t end = window.end_ns;
        const double rate = window.pps_per_queue;

        while ( true )
        {
            const uint64_t now = monotonic_ns();
            if ( now >= end )
                break;
            reclaim( *q );
            const uint64_t due = (uint64_t) ( (double) ( now - start ) * rate / 1e9 );
            if ( due <= q->submitted )
                continue;
            uint32_t want = (uint32_t) std::min<uint64_t>( due - q->submitted, XDPHW_BATCH );
            uint32_t have = (uint32_t) std::min<uint64_t>( want, q->free_frames.size() );
            uint32_t index = 0;
            if ( have )
                have = xsk_ring_prod__reserve( &q->tx, have, &index );
            if ( have < want )
                q->back_pressure++;
            if ( !have )
            {
                kick( *q );
                continue;
            }
            for ( uint32_t i = 0; i < have; i++ )
            {
                struct xdp_desc * desc = xsk_ring_prod__tx_desc( &q->tx, index + i );
                desc->addr = q->free_frames.back();
                desc->len = (uint32_t) q->frame_bytes;
                q->free_frames.pop_back();
            }
            xsk_ring_prod__submit( &q->tx, have );
            q->outstanding += have;
            q->submitted += have;
            kick( *q );
        }

        // the window is over: no new frames; wait for every completion so
        // sent is a settled count, never one with frames in flight
        const uint64_t drain_deadline = monotonic_ns() + XDPHW_DRAIN_NS;
        while ( q->outstanding > 0 && monotonic_ns() < drain_deadline )
        {
            kick( *q );
            reclaim( *q );
        }
        window.done.fetch_add( 1 );
    }
}

static bool setup_queue( queue_sender_t & q, const char * iface, const uint8_t * src_mac, const uint8_t * dst_mac,
                         const rocketnet_address_t * src, const rocketnet_address_t * dst, int flows, int payload_bytes )
{
    const size_t size = (size_t) XDPHW_NUM_FRAMES * XDPHW_FRAME_SIZE;
    q.buffer = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if ( q.buffer == MAP_FAILED )
    {
        printf( "FAIL mmap umem queue=%d\n", q.queue );
        return false;
    }

    struct xsk_umem_config umem_config;
    memset( &umem_config, 0, sizeof( umem_config ) );
    umem_config.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_config.comp_size = XDPHW_NUM_FRAMES;
    umem_config.frame_size = XDPHW_FRAME_SIZE;
    umem_config.frame_headroom = 0;
    int result = xsk_umem__create( &q.umem, q.buffer, size, &q.fill, &q.completion, &umem_config );
    if ( result )
    {
        printf( "FAIL umem queue=%d: %s\n", q.queue, strerror( -result ) );
        return false;
    }

    // zero copy first; a fall to copy mode is reported, and run.sh's sender
    // line names the mode the run used

    struct xsk_socket_config config;
    memset( &config, 0, sizeof( config ) );
    config.rx_size = 0;
    config.tx_size = XDPHW_NUM_FRAMES;
    config.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
    config.bind_flags = XDP_ZEROCOPY | XDP_USE_NEED_WAKEUP;
    result = xsk_socket__create( &q.xsk, iface, q.queue, q.umem, NULL, &q.tx, &config );
    q.zerocopy = ( result == 0 );
    if ( result )
    {
        config.bind_flags = XDP_COPY | XDP_USE_NEED_WAKEUP;
        result = xsk_socket__create( &q.xsk, iface, q.queue, q.umem, NULL, &q.tx, &config );
    }
    if ( result )
    {
        printf( "FAIL xsk queue=%d: %s\n", q.queue, strerror( -result ) );
        return false;
    }

    // prebuild every frame; the hot loop only hands addresses to the ring

    const int header_bytes = rocketnet_xdp_frame_header_bytes( src->type );
    const uint8_t filter[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    for ( int i = 0; i < XDPHW_NUM_FRAMES; i++ )
    {
        uint8_t * frame = (uint8_t*) q.buffer + (size_t) i * XDPHW_FRAME_SIZE;
        uint8_t * payload = frame + header_bytes;
        memset( payload, 0, (size_t) payload_bytes );
        payload[0] = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
        payload[1] = 0;                                  // the filter epoch, 0 in 1.0
        memcpy( payload + 2, filter, ROCKETNET_FILTER_VALUE_BYTES );
        for ( int j = 2 + ROCKETNET_FILTER_VALUE_BYTES; j < payload_bytes; j++ )
            payload[j] = (uint8_t) ( i + j );
        rocketnet_address_t source = *src;
        source.port = (uint16_t) ( 40000 + ( ( i + q.queue * 7 ) % flows ) );
        const int frame_bytes = rocketnet_xdp_frame_build( frame, XDPHW_FRAME_SIZE, src_mac, dst_mac, &source, dst, payload_bytes );
        if ( frame_bytes <= 0 )
        {
            printf( "FAIL frame build queue=%d\n", q.queue );
            return false;
        }
        q.frame_bytes = frame_bytes;
        q.free_frames.push_back( (uint64_t) i * XDPHW_FRAME_SIZE );
    }
    q.outstanding = 0;
    return true;
}

static int flood( int argc, char ** argv )
{
    const char * iface = NULL;
    const char * src_text = NULL;
    const char * dst_text = NULL;
    const char * dst_mac_text = NULL;
    std::string cores_text;
    int queues = 1;
    int flows = 64;
    int payload_bytes = ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES;

    for ( int i = 2; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--iface" ) == 0 && i + 1 < argc ) iface = argv[++i];
        else if ( strcmp( argv[i], "--queues" ) == 0 && i + 1 < argc ) queues = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--cores" ) == 0 && i + 1 < argc ) cores_text = argv[++i];
        else if ( strcmp( argv[i], "--src" ) == 0 && i + 1 < argc ) src_text = argv[++i];
        else if ( strcmp( argv[i], "--dst" ) == 0 && i + 1 < argc ) dst_text = argv[++i];
        else if ( strcmp( argv[i], "--dst-mac" ) == 0 && i + 1 < argc ) dst_mac_text = argv[++i];
        else if ( strcmp( argv[i], "--flows" ) == 0 && i + 1 < argc ) flows = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--payload-bytes" ) == 0 && i + 1 < argc ) payload_bytes = atoi( argv[++i] );
        else { printf( "FAIL unknown argument %s\n", argv[i] ); return 2; }
    }
    if ( !iface || !src_text || !dst_text || !dst_mac_text || queues <= 0 || flows <= 0 )
    {
        printf( "FAIL usage: flood --iface --queues --cores --src --dst --dst-mac [--flows] [--payload-bytes]\n" );
        return 2;
    }
    if ( payload_bytes < ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES || payload_bytes > ROCKETNET_MAX_PACKET_BYTES )
    {
        printf( "FAIL --payload-bytes outside [%d, %d]\n", ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES, ROCKETNET_MAX_PACKET_BYTES );
        return 2;
    }

    std::vector<int> cores;
    for ( size_t at = 0; at < cores_text.size(); )
    {
        size_t comma = cores_text.find( ',', at );
        if ( comma == std::string::npos ) comma = cores_text.size();
        cores.push_back( atoi( cores_text.substr( at, comma - at ).c_str() ) );
        at = comma + 1;
    }
    if ( (int) cores.size() != queues )
    {
        printf( "FAIL --cores must name one core per sender queue (%d)\n", queues );
        return 2;
    }

    uint8_t src_mac[6], dst_mac[6];
    if ( !read_mac( iface, src_mac ) || !parse_mac( dst_mac_text, dst_mac ) )
    {
        printf( "FAIL mac addresses\n" );
        return 2;
    }

    char src_with_port[128];
    snprintf( src_with_port, sizeof( src_with_port ), "%s:40000", src_text );
    rocketnet_address_t src, dst;
    if ( !rocketnet_address_parse( &src, src_with_port ) || !rocketnet_address_parse( &dst, dst_text ) || src.type != ROCKETNET_ADDRESS_IPV4 || dst.type != ROCKETNET_ADDRESS_IPV4 )
    {
        printf( "FAIL addresses: --src <ipv4> --dst <ipv4:port>\n" );
        return 2;
    }

    std::vector<queue_sender_t> senders( queues );
    for ( int q = 0; q < queues; q++ )
    {
        senders[q].queue = q;
        senders[q].core = cores[q];
        if ( !setup_queue( senders[q], iface, src_mac, dst_mac, &src, &dst, flows, payload_bytes ) )
            return 1;
        printf( "TXMODE queue=%d core=%d zerocopy=%d frame_bytes=%d\n", q, cores[q], senders[q].zerocopy ? 1 : 0, senders[q].frame_bytes );
    }
    printf( "READY queues=%d flows=%d payload_bytes=%d\n", queues, flows, payload_bytes );

    std::vector<std::thread> threads;
    for ( int q = 0; q < queues; q++ )
        threads.emplace_back( queue_thread, &senders[q] );

    char line[256];
    while ( fgets( line, sizeof( line ), stdin ) )
    {
        double pps = 0.0, seconds = 0.0;
        if ( sscanf( line, "send %lf %lf", &pps, &seconds ) == 2 && pps > 0.0 && seconds > 0.0 )
        {
            window.done.store( 0 );
            window.pps_per_queue = pps / (double) queues;
            window.start_ns = monotonic_ns();
            window.end_ns = window.start_ns + (uint64_t) ( seconds * 1e9 );
            window.generation.fetch_add( 1 );
            while ( window.done.load() < queues )
                usleep( 1000 );
            const uint64_t elapsed = monotonic_ns() - window.start_ns;
            uint64_t submitted = 0, completed = 0, back_pressure = 0, tx_invalid = 0, outstanding = 0;
            for ( queue_sender_t & q : senders )
            {
                submitted += q.submitted;
                completed += q.completed;
                back_pressure += q.back_pressure;
                outstanding += q.outstanding;
                struct xdp_statistics stats;
                socklen_t bytes = sizeof( stats );
                if ( getsockopt( xsk_socket__fd( q.xsk ), SOL_XDP, XDP_STATISTICS, &stats, &bytes ) == 0 )
                    tx_invalid += stats.tx_invalid_descs;
            }
            printf( "DONE pps=%.0f seconds=%.3f sent=%" PRIu64 " submitted=%" PRIu64 " outstanding=%" PRIu64 " back_pressure=%" PRIu64 " tx_invalid=%" PRIu64 " elapsed_ns=%" PRIu64 "\n",
                pps, seconds, completed, submitted, outstanding, back_pressure, tx_invalid, elapsed );
            fflush( stdout );
        }
        else if ( strncmp( line, "quit", 4 ) == 0 )
        {
            break;
        }
        else
        {
            printf( "UNKNOWN %s", line );
            fflush( stdout );
        }
    }

    window.quit.store( true );
    for ( std::thread & t : threads )
        t.join();
    for ( queue_sender_t & q : senders )
    {
        xsk_socket__delete( q.xsk );
        xsk_umem__delete( q.umem );
        munmap( q.buffer, (size_t) XDPHW_NUM_FRAMES * XDPHW_FRAME_SIZE );
    }
    printf( "STOPPED\n" );
    return 0;
}

// ---------------------------------------------------------------- probe

static int probe( int argc, char ** argv )
{
    const char * server = NULL;
    int count = 10000;
    int interval_us = 1000;
    for ( int i = 2; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--server" ) == 0 && i + 1 < argc ) server = argv[++i];
        else if ( strcmp( argv[i], "--count" ) == 0 && i + 1 < argc ) count = atoi( argv[++i] );
        else if ( strcmp( argv[i], "--interval-us" ) == 0 && i + 1 < argc ) interval_us = atoi( argv[++i] );
        else { printf( "FAIL unknown argument %s\n", argv[i] ); return 2; }
    }
    if ( !server || count <= 0 || interval_us <= 0 )
    {
        printf( "FAIL usage: probe --server <ipv4:port> --count <n> --interval-us <us>\n" );
        return 2;
    }

    if ( !rocketnet_init( NULL ) )
    {
        printf( "FAIL init\n" );
        return 1;
    }

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    if ( !rocketnet_write_dev_connect_token( server, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) )
    {
        printf( "FAIL dev connect token\n" );
        rocketnet_term();
        return 1;
    }
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, NULL, rocketnet_time() );
    memset( connect_token, 0, sizeof( connect_token ) );
    if ( !client )
    {
        printf( "FAIL client create\n" );
        rocketnet_term();
        return 1;
    }

    const double connect_deadline = rocketnet_time() + 10.0;
    while ( rocketnet_client_transport_state( client ) != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
    {
        rocketnet_client_transport_update( client, rocketnet_time() );
        if ( rocketnet_client_transport_state( client ) < 0 || rocketnet_time() > connect_deadline )
        {
            printf( "FAIL probe did not connect (state %d)\n", rocketnet_client_transport_state( client ) );
            rocketnet_client_transport_destroy( client );
            rocketnet_term();
            return 1;
        }
        usleep( 1000 );
    }

    uint8_t payload[32];
    memset( payload, 0, sizeof( payload ) );
    memcpy( payload, XDPHW_PROBE_MAGIC, 8 );
    uint64_t next = monotonic_ns();
    for ( uint64_t seq = 0; seq < (uint64_t) count; seq++ )
    {
        while ( monotonic_ns() < next )
            rocketnet_client_transport_update( client, rocketnet_time() );
        memcpy( payload + 8, &seq, 8 );
        const uint64_t sent_ns = monotonic_ns();
        memcpy( payload + 16, &sent_ns, 8 );
        rocketnet_client_transport_send_packet( client, payload, sizeof( payload ) );
        next += (uint64_t) interval_us * 1000ULL;
    }

    rocketnet_client_transport_disconnect( client );
    const double linger = rocketnet_time() + 0.2;
    while ( rocketnet_time() < linger )
    {
        rocketnet_client_transport_update( client, rocketnet_time() );
        usleep( 1000 );
    }
    rocketnet_client_transport_destroy( client );
    rocketnet_term();
    printf( "PROBE sent=%d interval_us=%d clock=CLOCK_MONOTONIC\n", count, interval_us );
    return 0;
}

int main( int argc, char ** argv )
{
    setvbuf( stdout, NULL, _IOLBF, 0 );
    if ( argc >= 2 && strcmp( argv[1], "flood" ) == 0 )
        return flood( argc, argv );
    if ( argc >= 2 && strcmp( argv[1], "probe" ) == 0 )
        return probe( argc, argv );
    fprintf( stderr, "usage: xdp_hw_sender flood ... | probe ...  (see the comment at the top of xdp_hw_sender.cpp)\n" );
    return 2;
}
