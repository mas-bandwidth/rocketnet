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

// A full socket after stop: the TX worker must leave its EAGAIN retry once
// send_quit is set, or the transport never reports stopped.
static void test_server_gso_send_retry_quit()
{
    check( rocketnet_server_gso_test_send_retry_quit() );
}

REGISTER_UNIT_TEST( test_server_gso_send_retry_quit );

// A full socket once shutdown has begun, before stop: the server asks for
// stop only when send_idle is true, so the TX worker must give up on the
// full socket without send_quit and go on to the next bank.
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

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == 4 );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 0 );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_GSO_SENDS] == 1 );

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
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 0 );

    // the tier stays plain
    memset( &socket, 0, sizeof( socket ) );
    gso_segment_stage( gso, 43000, 2 );
    check( rocketnet_server_gso_test_flush_staged( gso ) );
    check( socket.segmented == 0 );
    check( socket.plain_packets == 2 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_segment_refusal_keeps_other_destinations );

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

        uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
        rocketnet_server_gso_counters( gso, counters );
        check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_SYSCALLS] == 2 );
        check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == 0 );
        check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 0 );

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

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_SYSCALLS] == 3 );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == 1 );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 1 );

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

    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( gso, counters );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_PACKETS_SENT] == (uint64_t) ( 3 * rounds ) );
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 0 );

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
    check( counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED] == 0 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_segment_refusal_is_per_destination );

// Four full receive queues, and the real per-update budget, which two queues
// exhaust (8192 + 8192 is past 16000). The queues the first update consumes
// are published full again: a walk that always starts at 0 then stays on 0
// and 1, and queues 2 and 3 never deliver. Across two updates every queue
// must. No socket, no thread, no clock.
static void test_server_gso_receive_queue_rotation()
{
    const int queues = 4;
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( queues );
    check( gso );

    struct rocketnet_server_socket_process_packets_t * view = rocketnet_server_gso_process_packets( gso );
    check( view->capacity > ROCKETNET_GSO_RECV_QUEUE_SIZE );
    check( view->capacity < 2 * ROCKETNET_GSO_RECV_QUEUE_SIZE );

    int seen[4] = { 0, 0, 0, 0 };

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
    check( seen[0] > 0 );
    check( seen[1] > 0 );
    check( seen[2] == 0 );
    check( seen[3] == 0 );

    // 0 and 1 have a free buffer again; 2 and 3 still hold the ready buffer
    // the first update did not claim.
    check( rocketnet_server_gso_test_publish_marked( gso, 0, (uint8_t) 1, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );
    check( rocketnet_server_gso_test_publish_marked( gso, 1, (uint8_t) 2, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );
    check( !rocketnet_server_gso_test_publish_marked( gso, 2, (uint8_t) 3, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );
    check( !rocketnet_server_gso_test_publish_marked( gso, 3, (uint8_t) 4, (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE ) );

    rocketnet_server_gso_receive_packets( gso );
    check( view->num_packets == view->capacity );
    for ( int i = 0; i < view->num_packets; i++ )
    {
        const int marker = (int) view->packet_data[i][0];
        check( marker >= 1 && marker <= queues );
        seen[marker - 1]++;
    }

    check( seen[0] > 0 );
    check( seen[1] > 0 );
    check( seen[2] > 0 );
    check( seen[3] > 0 );

    rocketnet_server_gso_test_close( gso );
}

REGISTER_UNIT_TEST( test_server_gso_receive_queue_rotation );

#endif
