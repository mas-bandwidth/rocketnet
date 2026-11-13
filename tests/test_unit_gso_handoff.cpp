/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_server_gso.h"

#include <stdint.h>
#include <stdio.h>

#define check rocketnet_unit_check

#if defined( __linux__ ) && ROCKETNET_DEV

#include <errno.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif // #ifndef UDP_SEGMENT

bool rocketnet_server_gso_test_receive_backoff();

// The GSO worker's handoffs (proofs/tla/RNGsoHandoff.tla), stepped on this thread
// with a faked transport: no socket, no worker thread, no clock.
static void test_server_gso_worker_handoff()
{
    check( rocketnet_server_gso_test_worker_handoff() );
}

REGISTER_UNIT_TEST( test_server_gso_worker_handoff );

// At stop, the datagrams the workers received that main never took are
// counted RECEIVE_DROP_STOPPED (proofs/tla/RNGsoHandoff.tla StopCount).
static void test_server_gso_stop_counts_unreceived()
{
    check( rocketnet_server_gso_test_stop_counts_unreceived() );
}

REGISTER_UNIT_TEST( test_server_gso_stop_counts_unreceived );

// A refused sendmmsg through the send-failure seam: an errno other than a
// full socket's drops one message and sends the rest.
static void test_server_gso_send_refusals()
{
    check( rocketnet_server_gso_test_send_refusals() );
}

REGISTER_UNIT_TEST( test_server_gso_send_refusals );

// A full socket mid-flush on a fake socket (proofs/tla/RNGsoHandoff.tla
// SentOrHeld): the bank keeps its unsent tail, nothing is dropped, and the
// next attempt sends the rest in order.
static void test_server_gso_send_full_keeps_rest()
{
    check( rocketnet_server_gso_test_send_full_keeps_rest() );
}

REGISTER_UNIT_TEST( test_server_gso_send_full_keeps_rest );

// A full socket after stop: the queue worker must leave its EAGAIN retry once
// quit is set, or the transport never reports stopped.
static void test_server_gso_send_retry_quit()
{
    check( rocketnet_server_gso_test_send_retry_quit() );
}

REGISTER_UNIT_TEST( test_server_gso_send_retry_quit );

// Per-queue staging (proofs/tla/RNGsoQueueBankLayout.tla): a destination's
// packets are staged on, and sent from, the queue its address hashes to.
static void test_server_gso_send_queues()
{
    check( rocketnet_server_gso_test_send_queues() );
}

// the case opens four queues, each holding its send banks and receive buffers
// at the transport's own sizes: the bound is the plan of those plus a margin
static long gso_send_queues_rss_bound_mb()
{
    return suite_rss_bound_mb( rocketnet_server_gso_test_open_bytes( 4 ), 64 );
}

SUITE_CASE_RSS_BOUND( test_server_gso_send_queues, gso_send_queues_rss_bound_mb );

REGISTER_UNIT_TEST( test_server_gso_send_queues );

#if ROCKETNET_XDP
// sdk/rocketnet_server_socket_linux.cpp, built with XDP only
int rocketnet_server_socket_send_queue( const struct rocketnet_address_t * to, int num_queues );
#endif // #if ROCKETNET_XDP

// The gso queue hash is the XDP transport's, so a client keeps its queue
// whichever of the two serves it: each answer is in range, one destination
// always maps to one queue, one queue means queue 0, and with XDP built the
// two functions agree on every destination tried.
static void test_server_gso_send_queue_hash()
{
    struct rocketnet_address_t client;
    memset( &client, 0, sizeof( client ) );
    client.type = ROCKETNET_ADDRESS_IPV4;
    client.data.ipv4[0] = 198;
    client.data.ipv4[1] = 51;
    client.data.ipv4[2] = 100;
    client.data.ipv4[3] = 7;
    client.port = 50000;

    check( rocketnet_server_gso_send_queue( &client, 1 ) == 0 );
    const int queue = rocketnet_server_gso_send_queue( &client, 4 );
    check( queue >= 0 && queue < 4 );
    check( rocketnet_server_gso_send_queue( &client, 4 ) == queue );

    bool seen[4] = { false, false, false, false };
    for ( int family = 0; family < 2; family++ )
    {
        memset( &client, 0, sizeof( client ) );
        client.type = family == 0 ? ROCKETNET_ADDRESS_IPV4 : ROCKETNET_ADDRESS_IPV6;
        client.data.ipv4[0] = 10;
        if ( family == 1 )
        {
            client.data.ipv6[0] = 0x2001;
            client.data.ipv6[7] = 9;
        }
        for ( int i = 0; i < 256; i++ )
        {
            client.port = (uint16_t) ( 1000 + i * 251 );
            for ( int queues = 1; queues <= ROCKETNET_GSO_MAX_QUEUES; queues++ )
            {
                const int q = rocketnet_server_gso_send_queue( &client, queues );
                check( q >= 0 && q < queues );
                if ( queues == 4 && q >= 0 && q < 4 )
                {
                    seen[q] = true;
                }
#if ROCKETNET_XDP
                check( q == rocketnet_server_socket_send_queue( &client, queues ) );
#endif // #if ROCKETNET_XDP
            }
        }
    }
    check( seen[0] && seen[1] && seen[2] && seen[3] );
}

REGISTER_UNIT_TEST( test_server_gso_send_queue_hash );

// A full socket once shutdown has begun, before stop: the server asks for
// stop only when send_idle is true, so the queue worker must give up on the
// full socket without quit and go on to the next bank.
bool rocketnet_server_gso_test_send_shutdown_full();

static void test_server_gso_send_shutdown_full()
{
    check( rocketnet_server_gso_test_send_shutdown_full() );
}

REGISTER_UNIT_TEST( test_server_gso_send_shutdown_full );

// The bounded retry once shutdown has begun: a socket that refuses fewer
// times than ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT allows takes every
// routable packet; one that refuses past it loses only the unsent tail, whose
// packets (not its aborted or already-dropped slots) are counted in
// SEND_DROPPED, and stop completes.
bool rocketnet_server_gso_test_send_shutdown_within_bound();
bool rocketnet_server_gso_test_send_shutdown_past_bound();

static void test_server_gso_send_shutdown_within_bound()
{
    check( rocketnet_server_gso_test_send_shutdown_within_bound() );
}

REGISTER_UNIT_TEST( test_server_gso_send_shutdown_within_bound );

static void test_server_gso_send_shutdown_past_bound()
{
    check( rocketnet_server_gso_test_send_shutdown_past_bound() );
}

REGISTER_UNIT_TEST( test_server_gso_send_shutdown_past_bound );

// The receive drain's read budget: full batches of zero-length, refused-source
// and undersized datagrams, far more than the budget, through the receive
// seam; each drain returns after ROCKETNET_GSO_RECV_QUEUE_SIZE messages.
static void test_server_gso_drain_budget()
{
    check( rocketnet_server_gso_test_drain_budget() );
}

REGISTER_UNIT_TEST( test_server_gso_drain_budget );

// a drain asks recvmmsg only for messages whose every segment fits the
// buffer, so nothing it reads is dropped as overflow
static void test_server_gso_drain_room()
{
    check( rocketnet_server_gso_test_drain_room() );
}

REGISTER_UNIT_TEST( test_server_gso_drain_room );

// receives whose UDP_GRO control was truncated (MSG_CTRUNC) are counted in
// GRO_CMSG_FAULTS once each after main's fold, as io_uring counts them; no
// socket, no thread, no clock
static void test_server_gso_gro_cmsg_faults_counted()
{
    check( rocketnet_server_gso_test_gro_cmsg_faults() );
}

REGISTER_UNIT_TEST( test_server_gso_gro_cmsg_faults_counted );

// the worker's prefilter before banking (sdk/rocketnet_receive_prefilter.h):
// garbage takes no slot and each drop is counted by reason
static void test_server_gso_drain_prefilter()
{
    check( rocketnet_server_gso_test_drain_prefilter() );
}

REGISTER_UNIT_TEST( test_server_gso_drain_prefilter );

static void test_server_gso_receive_backoff()
{
    check( rocketnet_server_gso_test_receive_backoff() );
}

REGISTER_UNIT_TEST( test_server_gso_receive_backoff );

// one segmented sendmmsg to one destination returns EINVAL. that run leaves
// as plain datagrams. segmentation stays on for every other destination, and
// turns off only when the threshold of destinations refused with no
// segmented send ever accepted: a kernel that cannot segment.

struct gso_segment_socket_t
{
    int refuse_segment;
    uint16_t refuse_port;                   // nonzero: every segmented send to this port is refused, wherever it sits in the batch
    int refusals;
    int segmented;
    int plain_packets;
    uint16_t plain_ports[8];
    int plain_ports_n;
    uint16_t segment_port;
    int plain_fail;                         // nonzero: the next plain sendmmsg calls answer -1 with plain_errno
    int plain_errno;
};

static int gso_segment_stride( const struct msghdr * header )
{
    if ( header->msg_controllen < sizeof( struct cmsghdr ) )
    {
        return 0;
    }

    struct msghdr mutable_header = *header;
    for ( struct cmsghdr * cmsg = CMSG_FIRSTHDR( &mutable_header ); cmsg != NULL; cmsg = CMSG_NXTHDR( &mutable_header, cmsg ) )
    {
        if ( cmsg->cmsg_level == IPPROTO_UDP && cmsg->cmsg_type == UDP_SEGMENT && cmsg->cmsg_len >= CMSG_LEN( sizeof( uint16_t ) ) )
        {
            uint16_t stride = 0;
            memcpy( &stride, CMSG_DATA( cmsg ), sizeof( stride ) );
            return (int) stride;
        }
    }

    return 0;
}

static uint16_t gso_segment_port( const struct msghdr * header )
{
    if ( !header->msg_name || header->msg_namelen < (int) sizeof( struct sockaddr_in ) )
    {
        return 0;
    }

    const struct sockaddr_in * name = (const struct sockaddr_in *) header->msg_name;
    return ntohs( name->sin_port );
}

static int gso_segment_send( struct mmsghdr * messages, unsigned int count, void * user )
{
    struct gso_segment_socket_t * socket = (struct gso_segment_socket_t *) user;

    if ( count > 0 && gso_segment_stride( &messages[0].msg_hdr ) > 0 && socket->refuse_segment > 0 )
    {
        socket->refuse_segment--;
        socket->refusals++;
        errno = EINVAL;
        return -1;
    }

    if ( count > 0 && gso_segment_stride( &messages[0].msg_hdr ) == 0 && socket->plain_fail > 0 )
    {
        socket->plain_fail--;
        errno = socket->plain_errno;
        return -1;
    }

    for ( unsigned int i = 0; i < count; i++ )
    {
        const int stride = gso_segment_stride( &messages[i].msg_hdr );
        const uint16_t port = gso_segment_port( &messages[i].msg_hdr );
        if ( stride > 0 && socket->refuse_port != 0 && port == socket->refuse_port )
        {
            // sendmmsg: the messages before the refused one went; the refused
            // one is the call's error when it is the first
            if ( i > 0 )
            {
                return (int) i;
            }
            socket->refusals++;
            errno = EIO;
            return -1;
        }
        if ( stride > 0 )
        {
            socket->segmented++;
            socket->segment_port = port;
            continue;
        }

        check( messages[i].msg_hdr.msg_iovlen == 1 );
        if ( socket->plain_ports_n < 8 )
        {
            socket->plain_ports[socket->plain_ports_n++] = port;
        }
        socket->plain_packets++;
    }

    return (int) count;
}

static void gso_segment_stage( struct rocketnet_server_gso_t * gso, uint16_t port, int packets )
{
    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = port;

    for ( int i = 0; i < packets; i++ )
    {
        uint64_t packet_id = 0;
        uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
        check( packet );
        rocketnet_server_gso_finish_packet( gso, packet_id, packet, ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES );
    }
}

static void test_server_gso_segment_refusal_keeps_other_destinations()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    check( gso );

    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_segment_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.refuse_segment = 1;
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    gso_segment_stage( gso, 40000, 2 );
    gso_segment_stage( gso, 40001, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );

    // the refused run left as two plain datagrams. the other destination
    // was still one segmented send, and the tier is still on. gso's sendmmsg
    // refuses synchronously with the bank still held, so the run is resent;
    // io_uring's refusal arrives in a CQE after the slots are handed back, so
    // there the run is dropped and counted, and segmentation is off from then
    // on (test_uring_segment_refusal_sends_plain_after).
    check( socket.plain_packets == 2 );
    check( socket.plain_ports_n == 2 );
    check( socket.plain_ports[0] == 40000 );
    check( socket.plain_ports[1] == 40000 );
    check( socket.segmented == 1 );
    check( socket.segment_port == 40001 );
    check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] == 4 );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );
    check( counters[ROCKETNET_COUNTER_GSO_GSO_SENDS] == 1 );

    // a later send to another destination is still segmented
    memset( &socket, 0, sizeof( socket ) );
    gso_segment_stage( gso, 40002, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 1 );
    check( socket.segment_port == 40002 );
    check( socket.plain_packets == 0 );
    check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );

    // the transport has segmented (40001 and 40002 were accepted), so
    // refusals by any number of other destinations stay theirs: each run is
    // resent plain and the tier stays on
    for ( int i = 0; i < 2 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD; i++ )
    {
        memset( &socket, 0, sizeof( socket ) );
        socket.refuse_segment = 100;
        gso_segment_stage( gso, (uint16_t) ( 41000 + i ), 2 );
        check( rocketnet_server_gso_test_flush_staged( gso ) );
        check( socket.plain_packets == 2 );
        check( socket.segmented == 0 );
        check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );
    }

    rocketnet_server_gso_test_close( gso );

    // a kernel that refuses every segmented send: no segmented send is ever
    // accepted. the threshold'th refusing destination is the one that clears
    // the tier, and that run is still resent.
    gso = rocketnet_server_gso_test_open( 1 );
    check( gso );
    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    for ( int i = 0; i < ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD - 1; i++ )
    {
        memset( &socket, 0, sizeof( socket ) );
        socket.refuse_segment = 100;
        gso_segment_stage( gso, (uint16_t) ( 41000 + i ), 2 );
        check( rocketnet_server_gso_test_flush_staged( gso ) );
        check( socket.plain_packets == 2 );
        check( socket.segmented == 0 );
        check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );
    }

    memset( &socket, 0, sizeof( socket ) );
    socket.refuse_segment = 100;
    gso_segment_stage( gso, 42000, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.plain_packets == 2 );
    check( socket.segmented == 0 );
    check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) == 0 );

    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );

    // the tier stays plain
    memset( &socket, 0, sizeof( socket ) );
    gso_segment_stage( gso, 43000, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 0 );
    check( socket.plain_packets == 2 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_segment_refusal_keeps_other_destinations );

// the worker gathers a bank by destination before it sends: packets to two
// destinations staged interleaved (a game server's update loop, one packet
// per client per pass) still leave as one segmented send per destination
static void test_server_gso_send_gathers_interleaved_destinations()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    check( gso );

    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_segment_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    for ( int i = 0; i < 3; i++ )
    {
        gso_segment_stage( gso, 40000, 1 );
        gso_segment_stage( gso, 40001, 1 );
    }
    check( rocketnet_server_gso_test_flush_staged( gso ) );

    check( socket.segmented == 2 );
    check( socket.plain_packets == 0 );

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] == 6 );
    check( counters[ROCKETNET_COUNTER_GSO_GSO_SENDS] == 2 );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );

    // segmentation off: the same bank leaves as six plain datagrams
    rocketnet_server_gso_test_set_features( gso, 0 );
    memset( &socket, 0, sizeof( socket ) );
    for ( int i = 0; i < 3; i++ )
    {
        gso_segment_stage( gso, 40000, 1 );
        gso_segment_stage( gso, 40001, 1 );
    }
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 0 );
    check( socket.plain_packets == 6 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_send_gathers_interleaved_destinations );

// the first port from first whose destination stages on queue q of num_queues
static uint16_t gso_segment_port_on_queue( uint16_t first, int q, int num_queues )
{
    for ( int port = first; port < 65535; port++ )
    {
        struct rocketnet_address_t to;
        memset( &to, 0, sizeof( to ) );
        to.type = ROCKETNET_ADDRESS_IPV4;
        to.data.ipv4[0] = 127;
        to.data.ipv4[3] = 1;
        to.port = (uint16_t) port;
        if ( rocketnet_server_gso_send_queue( &to, num_queues ) == q )
        {
            return (uint16_t) port;
        }
    }
    check( false );
    return 0;
}

// a segmented send one queue had accepted shows the transport can segment:
// refusals by any number of destinations on another queue stay theirs, and
// the tier stays on for every queue
static void test_server_gso_segment_accepted_on_any_queue()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 2 );
    check( gso );

    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_segment_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    gso_segment_stage( gso, gso_segment_port_on_queue( 40000, 0, 2 ), 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 1 );

    uint16_t port = 41000;
    for ( int i = 0; i < 2 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD; i++ )
    {
        port = gso_segment_port_on_queue( port, 1, 2 );
        memset( &socket, 0, sizeof( socket ) );
        socket.refuse_segment = 100;
        gso_segment_stage( gso, port, 2 );
        check( rocketnet_server_gso_test_flush_staged( gso ) );
        check( socket.plain_packets == 2 );
        check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );
        port++;
    }

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_segment_accepted_on_any_queue );

// a refused segmented run is resent plain, and that plain send fails too. a
// full socket stops the resend at the first unsent packet with nothing
// dropped; a hard error drops that one packet, counts it, and the rest of the
// run still leaves.
static void test_server_gso_plain_resend_failures()
{
    const int full_errors[2] = { EAGAIN, ENOBUFS };
    for ( int e = 0; e < 2; e++ )
    {
        struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
        check( gso );
        rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

        struct gso_segment_socket_t socket;
        memset( &socket, 0, sizeof( socket ) );
        socket.refuse_segment = 1;
        socket.plain_fail = 1;
        socket.plain_errno = full_errors[e];
        rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

        gso_segment_stage( gso, 44000, 2 );
        check( !rocketnet_server_gso_test_flush_staged( gso ) );
        check( socket.refusals == 1 );
        check( socket.plain_fail == 0 );
        check( socket.plain_packets == 0 );

        uint64_t counters[ROCKETNET_NUM_COUNTERS];
        rocketnet_server_gso_counters( gso, counters );
        check( counters[ROCKETNET_COUNTER_GSO_SEND_SYSCALLS] == 2 );
        check( counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] == 0 );
        check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );

        rocketnet_server_gso_test_close( gso );
    }

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    check( gso );
    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_segment_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.refuse_segment = 1;
    socket.plain_fail = 1;
    socket.plain_errno = EHOSTUNREACH;
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    gso_segment_stage( gso, 44001, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.refusals == 1 );
    check( socket.plain_fail == 0 );
    check( socket.plain_packets == 1 );
    check( socket.plain_ports[0] == 44001 );

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_SYSCALLS] == 3 );
    check( counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] == 1 );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 1 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_plain_resend_failures );

// a new destination refuses a segmented run every round, interleaved with
// accepted segmented runs to another destination: the first refusal warns,
// every later one is spam, and segmentation stays on

static int gso_refusal_warnings = 0;
static int gso_refusal_spam = 0;

static void gso_refusal_log( void * context, enum rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) length;
    if ( strstr( message, "live GSO send refused" ) == NULL )
    {
        return;
    }
    if ( level == ROCKETNET_LOG_LEVEL_SPAM )
    {
        gso_refusal_spam++;
    }
    else
    {
        gso_refusal_warnings++;
    }
}

static void test_server_gso_segment_refusal_warns_once()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    check( gso );

    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_segment_socket_t socket;
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    const enum rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_SPAM );
    rocketnet_log_function( gso_refusal_log, NULL );

    const int rounds = 4 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
    for ( int i = 0; i < rounds; i++ )
    {
        memset( &socket, 0, sizeof( socket ) );
        socket.refuse_segment = 1;
        gso_segment_stage( gso, (uint16_t) ( 40100 + i ), 2 );
        gso_segment_stage( gso, 40001, 2 );
        check( rocketnet_server_gso_test_flush_staged( gso ) );
        check( socket.plain_packets == 2 );
        check( socket.segmented == 1 );
        check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );
    }

    check( gso_refusal_warnings == 1 );
    check( gso_refusal_spam == rounds - 1 );

    rocketnet_server_gso_test_close( gso );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );
}

REGISTER_UNIT_TEST( test_server_gso_segment_refusal_warns_once );

// one destination refuses every segmented send (its route cannot take one:
// an IPsec policy, say), another accepts them. the other sends one packet an
// update, the normal game load, so no accepted segmented send lands between
// the refusals. many updates later the accepting destination's runs are
// still segmented, the refusing destination's packets all still leave,
// plain, nothing is dropped, and the refusing destination paid one refusal,
// not one a run.

static void test_server_gso_segment_refusal_is_per_destination()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    check( gso );

    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_segment_socket_t socket;
    memset( &socket, 0, sizeof( socket ) );
    socket.refuse_port = 40000;
    rocketnet_server_gso_test_set_send( gso, gso_segment_send, &socket );

    const int rounds = 4 * ROCKETNET_SEGMENT_CONSECUTIVE_REFUSAL_THRESHOLD;
    for ( int i = 0; i < rounds; i++ )
    {
        gso_segment_stage( gso, 40000, 2 );
        gso_segment_stage( gso, 40001, 1 );
        check( rocketnet_server_gso_test_flush_staged( gso ) );
    }

    check( socket.refusals == 1 );
    check( socket.plain_packets == 3 * rounds );
    check( ( rocketnet_server_gso_features( gso ) & ROCKETNET_SERVER_GSO_FEATURE_SEGMENT ) != 0 );

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] == (uint64_t) ( 3 * rounds ) );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );

    // the accepting destination's run is one segmented send, and the
    // refusing destination's run in the same batch leaves plain, unrefused
    memset( &socket, 0, sizeof( socket ) );
    socket.refuse_port = 40000;
    gso_segment_stage( gso, 40001, 2 );
    gso_segment_stage( gso, 40000, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 1 );
    check( socket.segment_port == 40001 );
    check( socket.plain_packets == 2 );
    check( socket.refusals == 0 );

    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_segment_refusal_is_per_destination );

// Full receive queues, two more than the real per-update budget reaches: the
// budget ends inside a queue (at 1000 clients it is less than one 32768-slot
// queue), so the first update claims that queue and stops, and the last two
// queues are left unclaimed. The queues the first update consumes are
// published full again: a walk that always starts at 0 then stays on the
// claimed queues, and the last two never deliver. Across as many updates as
// there are queues, every queue must. No socket, no thread, no clock.
static void test_server_gso_receive_queue_rotation()
{
    const int budget = rocketnet_server_receive_packets_per_update( ROCKETNET_DEFAULT_MAX_CLIENTS );
    const int claimed = ( budget + ROCKETNET_GSO_RECV_QUEUE_SIZE - 1 ) / ROCKETNET_GSO_RECV_QUEUE_SIZE;
    const int queues = claimed + 2;
    check( queues <= ROCKETNET_GSO_MAX_QUEUES );

    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( queues );
    check( gso );

    // the budget ends inside a queue, so the first update claims that queue
    // too and stops there

    struct rocketnet_server_socket_process_packets_t * view = rocketnet_server_gso_process_packets( gso );
    check( view->capacity == budget );
    check( view->capacity > ( claimed - 1 ) * ROCKETNET_GSO_RECV_QUEUE_SIZE );
    check( view->capacity < claimed * ROCKETNET_GSO_RECV_QUEUE_SIZE );

    int seen[ROCKETNET_GSO_MAX_QUEUES] = {};

    for ( int q = 0; q < queues; q++ )
    {
        check( rocketnet_server_gso_test_publish_marked( gso, q, (uint8_t) ( q + 1 ), (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );
    }

    rocketnet_server_gso_receive_packets( gso );
    check( view->num_packets == view->capacity );
    for ( int i = 0; i < view->num_packets; i++ )
    {
        const int marker = (int) view->packet_data[i][0];
        check( marker >= 1 && marker <= queues );
        seen[marker - 1]++;
    }
    for ( int q = 0; q < claimed; q++ )
    {
        check( seen[q] > 0 );
    }
    check( seen[claimed] == 0 );
    check( seen[claimed + 1] == 0 );

    // the claimed queues have a free buffer again, the others still hold the
    // ready buffer the first update did not claim. Each update claims the
    // next queue after the last it filled: every queue delivers within as
    // many updates as there are queues.
    for ( int round = 1; round < queues; round++ )
    {
        for ( int q = 0; q < queues; q++ )
        {
            // a queue still holding its ready buffer refuses; that is the point
            rocketnet_server_gso_test_publish_marked( gso, q, (uint8_t) ( q + 1 ), (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE );
        }
        rocketnet_server_gso_receive_packets( gso );
        check( view->num_packets == view->capacity );
        for ( int i = 0; i < view->num_packets; i++ )
        {
            const int marker = (int) view->packet_data[i][0];
            check( marker >= 1 && marker <= queues );
            seen[marker - 1]++;
        }
    }

    for ( int q = 0; q < queues; q++ )
    {
        check( seen[q] > 0 );
    }

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_receive_queue_rotation );

// The segmented runs a flush builds, read back from the messages it hands
// sendmmsg: a run to one destination holds packets of the first one's size,
// a shorter packet joins it as the tail and closes it, and every gathered
// datagram points at its own staged bytes in staging order.

#define GSO_RUN_MAX_MESSAGES 8
#define GSO_RUN_MAX_IOVECS 8

struct gso_run_socket_t
{
    int messages;
    int iovecs[GSO_RUN_MAX_MESSAGES];
    int stride[GSO_RUN_MAX_MESSAGES];
    uint16_t port[GSO_RUN_MAX_MESSAGES];
    int bytes[GSO_RUN_MAX_MESSAGES][GSO_RUN_MAX_IOVECS];
    uint8_t first[GSO_RUN_MAX_MESSAGES][GSO_RUN_MAX_IOVECS];
    uint8_t last[GSO_RUN_MAX_MESSAGES][GSO_RUN_MAX_IOVECS];
};

static int gso_run_send( struct mmsghdr * messages, unsigned int count, void * user )
{
    struct gso_run_socket_t * socket = (struct gso_run_socket_t *) user;

    for ( unsigned int m = 0; m < count; m++ )
    {
        check( socket->messages < GSO_RUN_MAX_MESSAGES );
        const int index = socket->messages++;
        const struct msghdr * header = &messages[m].msg_hdr;
        check( header->msg_iovlen <= GSO_RUN_MAX_IOVECS );

        socket->iovecs[index] = (int) header->msg_iovlen;
        socket->stride[index] = gso_segment_stride( header );
        socket->port[index] = gso_segment_port( header );
        for ( int i = 0; i < (int) header->msg_iovlen; i++ )
        {
            const struct iovec * iov = &header->msg_iov[i];
            const uint8_t * data = (const uint8_t *) iov->iov_base;
            socket->bytes[index][i] = (int) iov->iov_len;
            socket->first[index][i] = data && iov->iov_len > 0 ? data[0] : 0;
            socket->last[index][i] = data && iov->iov_len > 0 ? data[iov->iov_len - 1] : 0;
        }
    }

    return (int) count;
}

static void gso_run_stage( struct rocketnet_server_gso_t * gso, uint16_t port, int bytes, uint8_t tag )
{
    struct rocketnet_address_t to;
    memset( &to, 0, sizeof( to ) );
    to.type = ROCKETNET_ADDRESS_IPV4;
    to.data.ipv4[0] = 127;
    to.data.ipv4[3] = 1;
    to.port = port;

    uint64_t packet_id = 0;
    uint8_t * packet = rocketnet_server_gso_start_packet( gso, &to, &packet_id );
    check( packet );
    // the slot is a frame: the wire packet starts at the wire offset
    memset( packet + ROCKETNET_FRAME_WIRE_OFFSET, tag, (size_t) bytes );
    rocketnet_server_gso_finish_packet( gso, packet_id, packet, bytes );
}

static void test_server_gso_segment_runs_gather_their_own_packets()
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    check( gso );
    rocketnet_server_gso_test_set_features( gso, ROCKETNET_SERVER_GSO_FEATURE_SEGMENT );

    struct gso_run_socket_t * socket = new gso_run_socket_t();
    rocketnet_server_gso_test_set_send( gso, gso_run_send, socket );

    // one packet to 39999, then 200, 150 and 200 bytes to 40000: the 150 is
    // the first run's tail and closes it, so the last 200 is a run of its own

    gso_run_stage( gso, 39999, 120, 0x10 );
    gso_run_stage( gso, 40000, 200, 0x21 );
    gso_run_stage( gso, 40000, 150, 0x22 );
    gso_run_stage( gso, 40000, 200, 0x23 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );

    check( socket->messages == 3 );

    check( socket->port[0] == 39999 && socket->iovecs[0] == 1 && socket->stride[0] == 0 );
    check( socket->bytes[0][0] == 120 && socket->first[0][0] == 0x10 && socket->last[0][0] == 0x10 );

    check( socket->port[1] == 40000 && socket->iovecs[1] == 2 && socket->stride[1] == 200 );
    check( socket->bytes[1][0] == 200 && socket->first[1][0] == 0x21 && socket->last[1][0] == 0x21 );
    check( socket->bytes[1][1] == 150 && socket->first[1][1] == 0x22 && socket->last[1][1] == 0x22 );

    check( socket->port[2] == 40000 && socket->iovecs[2] == 1 && socket->stride[2] == 0 );
    check( socket->bytes[2][0] == 200 && socket->first[2][0] == 0x23 && socket->last[2][0] == 0x23 );

    uint64_t counters[ROCKETNET_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_COUNTER_GSO_PACKETS_SENT] == 4 );
    check( counters[ROCKETNET_COUNTER_GSO_SEND_DROPPED] == 0 );
    check( counters[ROCKETNET_COUNTER_GSO_GSO_SENDS] == 1 );

    rocketnet_server_gso_test_close( gso );
    delete socket;
}

REGISTER_UNIT_TEST( test_server_gso_segment_runs_gather_their_own_packets );

// The socket buffer decision (rocketnet_server_gso_buffer_step): a grant at or
// past the request is taken; a short grant asks once more with the force
// option; a grant still short after it is named at start.
static void test_server_gso_buffer_step()
{
    check( rocketnet_server_gso_buffer_step( 25000000, 25000000, false ) == ROCKETNET_SERVER_GSO_BUFFER_GRANTED );
    check( rocketnet_server_gso_buffer_step( 25000000, 26000000, false ) == ROCKETNET_SERVER_GSO_BUFFER_GRANTED );
    check( rocketnet_server_gso_buffer_step( 25000000, 4194304, false ) == ROCKETNET_SERVER_GSO_BUFFER_FORCE );
    check( rocketnet_server_gso_buffer_step( 25000000, 4194304, true ) == ROCKETNET_SERVER_GSO_BUFFER_SHORT );
    check( rocketnet_server_gso_buffer_step( 25000000, 25000000, true ) == ROCKETNET_SERVER_GSO_BUFFER_GRANTED );
    check( rocketnet_server_gso_buffer_step( 1000000, 0, false ) == ROCKETNET_SERVER_GSO_BUFFER_FORCE );
}

REGISTER_UNIT_TEST( test_server_gso_buffer_step );

// The start line for each buffer: asked and got always; a short grant is a
// warning naming why the force option did not raise it and the exact sysctl
// and value the operator sets; a grant met, plainly or by force, is not.
static void test_server_gso_buffer_note()
{
    char line[512];

    // the a02 host: rmem_max 4 MB, no CAP_NET_ADMIN
    check( rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), true, 25000000, 4194304, true, EPERM ) );
    check( strstr( line, "receive buffer asked 25000000 bytes, got 4194304" ) != NULL );
    check( strstr( line, "SO_RCVBUFFORCE refused: the process lacks CAP_NET_ADMIN" ) != NULL );
    check( strstr( line, "set sysctl net.core.rmem_max=25000000" ) != NULL );

    // the same on the send side names its own option and sysctl
    check( rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), false, 1000000, 212992, true, EPERM ) );
    check( strstr( line, "send buffer asked 1000000 bytes, got 212992" ) != NULL );
    check( strstr( line, "SO_SNDBUFFORCE refused" ) != NULL );
    check( strstr( line, "set sysctl net.core.wmem_max=1000000" ) != NULL );
    check( strstr( line, "rmem_max" ) == NULL );

    // another refusal is named by its errno
    check( rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), true, 25000000, 4194304, true, EINVAL ) );
    char errno_text[64];
    snprintf( errno_text, sizeof( errno_text ), "SO_RCVBUFFORCE refused: errno %d", EINVAL );
    check( strstr( line, errno_text ) != NULL );

    // force taken and the kernel still short
    check( rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), true, 25000000, 4194304, true, 0 ) );
    check( strstr( line, "SO_RCVBUFFORCE taken and still short" ) != NULL );

    // met by the plain request: information, no sysctl
    check( !rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), true, 25000000, 25000000, false, 0 ) );
    check( strstr( line, "receive buffer asked 25000000 bytes, got 25000000" ) != NULL );
    check( strstr( line, "sysctl" ) == NULL );

    // met by the force option: information that says so
    check( !rocketnet_server_gso_buffer_note( line, (int) sizeof( line ), true, 25000000, 25000000, true, 0 ) );
    check( strstr( line, "past net.core.rmem_max with SO_RCVBUFFORCE" ) != NULL );

    // a line that does not fit is cut and terminated
    char small[16];
    memset( small, 'x', sizeof( small ) );
    check( rocketnet_server_gso_buffer_note( small, (int) sizeof( small ), true, 25000000, 4194304, true, EPERM ) );
    check( small[sizeof( small ) - 1] == '\0' );
}

REGISTER_UNIT_TEST( test_server_gso_buffer_note );

#endif
