/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_internal.h"
#include "rocketnet_server_uring.h"
#include "rocketnet_receive_prefilter.h" // ROCKETNET_RECEIVE_NUM_DROPS, in the built transport and the stub alike

#include <new>
#include <atomic>
#include <inttypes.h>
#include <string.h>

// The io_uring server transport. See rocketnet_server_uring.h for the shape;
// this file is the wiring: the sockets, one ring and one worker thread per
// queue, and main's side (publish and wake, drain, stop, the counters).
//
// Each queue's worker owns its ring: it is the only thread that submits to
// it or reaps it (rocketnet_uring_queue_worker_run). Main never touches a
// ring. Main and a worker meet only in the send handoff
// (rocketnet_uring_queue_send.h, under its mutex), the receive bank
// (rocketnet_uring_receive_queue.h, two atomic indexes), the worker's quit
// flag and the wake eventfd, and the counters (atomics, one writer each).
//
// The models are the modules': RNUringQueueBankLayout (the staging),
// RNUringQueueSend (the handoff), RNSendGatherLayout (the gather),
// RNUringQueueWorker (the worker loop), RNUringQueueReceive and
// RNUringQueueReceiveLayout (the receive bank), each bound to its code by a
// trace check. This file adds no state machine of its own: stop is each
// worker's Stop, stopped is each worker's Exit seen (its exited flag), and
// destroy joins before it frees.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING
#include <liburing.h>
#include "rocketnet_uring_codec.h"
// ROCKETNET_IO_URING also puts the io_uring row in the transport registry
// (rocketnet_driver.cpp), so it must mean the transport is built: a liburing
// without multishot receive is refused here, never compiled to a stub that the
// registry would still offer. CMake checks this at configure time
// (cmake/RocketNetDependencies.cmake) and leaves ROCKETNET_IO_URING unset.
#if defined( IORING_RECV_MULTISHOT )
#define ROCKETNET_URING_BUILT 1
#else // #if defined( IORING_RECV_MULTISHOT )
#error "ROCKETNET_IO_URING needs liburing 2.3 or later (IORING_RECV_MULTISHOT); build without ROCKETNET_IO_URING"
#endif // #if defined( IORING_RECV_MULTISHOT )
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined( ROCKETNET_IO_URING ) && ROCKETNET_IO_URING

#if defined( ROCKETNET_URING_BUILT ) && ROCKETNET_URING_BUILT

#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"
#include "rocketnet_server_gso.h"
#include "rocketnet_send_queue.h"
#include "rocketnet_uring_queue_send.h"
#include "rocketnet_uring_queue_worker.h"
#include "rocketnet_uring_receive_queue.h"

#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

// the most packets and bytes one segmented send carries: the gso transport's
// bounds (the kernel's UDP_MAX_SEGMENTS, the 16-bit UDP payload)

#define ROCKETNET_URING_MAX_SUPER_BYTES                 ROCKETNET_GSO_MAX_SUPER_BYTES

ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_MAX_SEGMENTS == ROCKETNET_GSO_MAX_SEGMENTS, "one coalesced receive is the gso transport's segment bound" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_MAX_SEGMENTS == ROCKETNET_URING_RECEIVE_MAX_SEGMENTS, "the receive module takes the transport's segment bound" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_RECV_BUFFERS <= ROCKETNET_URING_RECEIVE_MAX_BUFFERS, "a queue's provided buffers fit the kernel's ring" );

// one receive buffer holds the multishot header, the name area, the control
// area and the largest coalesced receive (the 16-bit UDP payload)

ROCKETNET_STATIC_ASSERT( sizeof( struct io_uring_recvmsg_out ) + sizeof( struct sockaddr_storage ) + ROCKETNET_URING_RECEIVE_CONTROL_BYTES + 65535 <= ROCKETNET_URING_RECV_BUFFER_BYTES,
                         "an io_uring receive buffer holds a whole UDP_GRO coalesced receive" );

#if ROCKETNET_DEV
// the test seam's own cancel (rocketnet_server_uring_test_fail_receive): a
// kind the worker never submits, taken out of the completion queue before
// the worker reads it
#define ROCKETNET_URING_TAG_TEST                                             7
#endif // #if ROCKETNET_DEV

struct rocketnet_server_uring_t;

// one queue: its socket, its ring, its receive bank, its worker and the
// worker's thread. Constructed by placement new with no parentheses: the
// fields below start from their initializers.

struct rocketnet_server_uring_queue_t
{
    struct rocketnet_server_uring_t * uring = NULL;
    int index = 0;
    int fd = -1;
    int wake_fd = -1;                           // the eventfd main writes to wake the worker (blocking: rocketnet_uring_queue_worker.h)
    bool ring_ready = false;
    uint64_t ring_mapped_bytes = 0;             // the ring's mappings, in the direct allocation count
    struct io_uring ring = {};
    struct rocketnet_uring_receive_queue_t * receive = NULL;
    bool worker_ready = false;
    struct rocketnet_uring_queue_worker_t worker;
    struct rocketnet_platform_thread_t * thread = NULL;

    // the worker thread stores it (release) after its loop's last access to
    // the ring; main loads it (acquire) in stopped

    alignas( 64 ) std::atomic<bool> exited{ false };

#if ROCKETNET_DEV
    std::atomic<int> test_receive_error{ 0 };   // main stores a pending error; the worker takes it and cancels its multishot
    int test_receive_cancelled = 0;             // the worker's: the error the terminating completion carries in place of -ECANCELED
#endif // #if ROCKETNET_DEV
};

struct rocketnet_server_uring_t
{
    void * context = NULL;

    int num_queues = 0;
    bool ipv6 = false;
    int features = 0;                           // ROCKETNET_SERVER_URING_FEATURE_*: what the create probe found
    int max_clients = 0;
    int bank_size = 0;                          // each queue's receive bank and the process view
    bool stopping = false;
    bool stop_counted = false;
    uint64_t receive_dropped_stopped = 0;       // main's: banked datagrams main never took, counted once at stopped
    int receive_start_queue = 0;                // main's: the queue the next drain starts at

    struct rocketnet_uring_send_queues_t * banks = NULL;
    struct rocketnet_uring_queue_send_t * send = NULL;
    struct rocketnet_server_uring_queue_t * queues = NULL;
    int queues_constructed = 0;

    struct rocketnet_server_socket_process_packets_t process_packets = {};

#if ROCKETNET_DEV
    bool test_stall_destroy = false;
    std::atomic<int> test_fail_sends{ 0 };
#endif // #if ROCKETNET_DEV
};

// ---------------------------------------------------------------------------

// address -> sockaddr at the boundary, the gso transport's rule: ports and
// address words are network order in the sockaddr and host order in
// rocketnet_address_t, and an ipv4 destination on a dual stack socket is sent
// v4-mapped. the receive direction is rocketnet_uring_sockaddr_to_address.

static socklen_t rocketnet_server_uring_address_to_sockaddr( const rocketnet_address_t * input, bool socket_ipv6, struct sockaddr_storage * out )
{
    rocketnet_address_t to = *input;

    if ( socket_ipv6 )
    {
        if ( to.type == ROCKETNET_ADDRESS_IPV4 )
        {
            rocketnet_address_convert_ipv4_to_ipv6( &to );
        }

        struct sockaddr_in6 * addr = (struct sockaddr_in6 *) out;
        memset( addr, 0, sizeof( struct sockaddr_in6 ) );
        addr->sin6_family = AF_INET6;

        for ( int i = 0; i < 8; i++ )
        {
            ( (uint16_t *) &addr->sin6_addr )[i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }

        addr->sin6_port = rocketnet_platform_htons( to.port );
        return sizeof( struct sockaddr_in6 );
    }
    else
    {
        if ( to.type != ROCKETNET_ADDRESS_IPV4 )
        {
            return 0;
        }

        rocketnet_platform_address_to_sockaddr_in( &to, (struct sockaddr_in *) out );
        return sizeof( struct sockaddr_in );
    }
}

// one reuseport socket, bound as the gso transport binds its group: family from
// the bind address, dual stack when ipv6, the configured buffer sizes
// requested and the grant reported, the first socket discovering an
// ephemeral port the rest join, non-blocking, don't-fragment, close-on-exec.

static void rocketnet_server_uring_set_socket_option( void * user, int level, int option_name, int value )
{
    const int fd = *(const int *) user;
    setsockopt( fd, level, option_name, &value, sizeof( value ) );
}

static int rocketnet_server_uring_open_socket( const rocketnet_address_t * bind_address, int queue_index, int max_clients, uint16_t * bound_port )
{
    const bool ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;

    int fd = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP );
    if ( fd < 0 )
    {
        rocketnet_error( "io_uring transport: failed to create socket: %s", strerror( errno ) );
        return -1;
    }

    // the first queue binds before it joins a reuseport group, whether its
    // port is explicit or discovered. bound with SO_REUSEPORT, an explicit
    // port another reuseport socket of the same user already holds is joined,
    // not refused, and the kernel then shares that port's datagrams between
    // the two servers; an ephemeral bind may likewise be given a port another
    // reuseport group already holds (measured on Linux x64 gcc: 4 shared
    // ports in 50 x 64 reuseport ephemeral binds, none when the first socket
    // binds without it). Bound first, it takes a port no socket holds or the
    // bind fails with EADDRINUSE as the portable transport's does, and
    // SO_REUSEPORT set after the bind lets this server's other queues join it.

    const bool first_queue = queue_index == 0;

    int one = 1;
    if ( !first_queue && setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) != 0 )
    {
        rocketnet_error( "io_uring transport: failed to set SO_REUSEPORT: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( ipv6 )
    {
        int zero = 0;
        if ( setsockopt( fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof( zero ) ) != 0 )
        {
            rocketnet_error( "io_uring transport: failed to clear socket ipv6 only: %s", strerror( errno ) );
            close( fd );
            return -1;
        }
    }

    int send_buffer_size = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;

    // one update's inbound packets at the server's configured client count
    // (rocketnet_server_transport_receive_buffer_request; a request, and the
    // kernel charges only what it holds)
    int receive_buffer_size = rocketnet_server_transport_receive_buffer_request( "io_uring", max_clients );

    if ( setsockopt( fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof( int ) ) != 0 )
    {
        rocketnet_error( "io_uring transport: failed to set socket send buffer size: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size, sizeof( int ) ) != 0 )
    {
        rocketnet_error( "io_uring transport: failed to set socket receive buffer size: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    int granted_send = 0;
    int granted_receive = 0;
    socklen_t optlen = sizeof( int );
    getsockopt( fd, SOL_SOCKET, SO_SNDBUF, &granted_send, &optlen );
    optlen = sizeof( int );
    getsockopt( fd, SOL_SOCKET, SO_RCVBUF, &granted_receive, &optlen );

    granted_send /= 2;
    granted_receive /= 2;

    if ( queue_index == 0 )
    {
        if ( granted_send < send_buffer_size )
        {
            rocketnet_warn( "io_uring transport: socket send buffer granted %d of %d requested bytes (raise net.core.wmem_max)",
                            granted_send, send_buffer_size );
        }

        rocketnet_server_receive_buffer_granted( "io_uring", receive_buffer_size, granted_receive );
        if ( granted_send >= send_buffer_size && granted_receive >= receive_buffer_size )
        {
            rocketnet_info( "io_uring transport: socket buffers granted: send %d receive %d", granted_send, granted_receive );
        }
    }

    rocketnet_address_t addr = *bind_address;
    addr.port = *bound_port != 0 ? *bound_port : bind_address->port;

    struct sockaddr_storage sockaddr;
    socklen_t socklen = rocketnet_server_uring_address_to_sockaddr( &addr, ipv6, &sockaddr );

    if ( socklen == 0 || bind( fd, (struct sockaddr *) &sockaddr, socklen ) < 0 )
    {
        rocketnet_error( "io_uring transport: failed to bind socket (%s): %s", ipv6 ? "ipv6" : "ipv4", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( first_queue && setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof( one ) ) != 0 )
    {
        rocketnet_error( "io_uring transport: failed to set SO_REUSEPORT: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( *bound_port == 0 )
    {
        struct sockaddr_storage bound;
        memset( &bound, 0, sizeof( bound ) );
        socklen_t bound_length = sizeof( bound );
        if ( getsockname( fd, (struct sockaddr *) &bound, &bound_length ) != 0 )
        {
            rocketnet_error( "io_uring transport: failed to get bound socket port: %s", strerror( errno ) );
            close( fd );
            return -1;
        }

        rocketnet_address_t bound_address;
        if ( !rocketnet_uring_sockaddr_to_address( &bound, (int) bound_length, ipv6, &bound_address ) )
        {
            rocketnet_error( "io_uring transport: could not read bound socket address" );
            close( fd );
            return -1;
        }

        *bound_port = bound_address.port;
    }

    if ( fcntl( fd, F_SETFL, fcntl( fd, F_GETFL, 0 ) | O_NONBLOCK ) == -1 )
    {
        rocketnet_error( "io_uring transport: failed to set socket non-blocking: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    rocketnet_platform_socket_apply_dont_fragment( ipv6, rocketnet_server_uring_set_socket_option, &fd );

    return fd;
}


// ---------------------------------------------------------------------------

bool rocketnet_server_uring_available()
{
    // the probe: a ring, the five opcodes the workers submit, and a provided buffer ring.
    // -1 not yet probed, 0 no, 1 yes

    static std::atomic<int> available{ -1 };

    int known = available.load( std::memory_order_relaxed );
    if ( known >= 0 )
    {
        return known == 1;
    }

    bool ok = false;
    struct io_uring ring;
    if ( io_uring_queue_init( 8, &ring, 0 ) == 0 )
    {
        struct io_uring_probe * probe = io_uring_get_probe_ring( &ring );
        if ( probe )
        {
            ok = io_uring_opcode_supported( probe, IORING_OP_SENDMSG ) &&
                 io_uring_opcode_supported( probe, IORING_OP_RECVMSG ) &&
                 io_uring_opcode_supported( probe, IORING_OP_ASYNC_CANCEL ) &&
                 io_uring_opcode_supported( probe, IORING_OP_READ ) &&
                 io_uring_opcode_supported( probe, IORING_OP_POLL_ADD );

            io_uring_free_probe( probe );
        }

        if ( ok )
        {
            int error = 0;
            bool inverted_resv = false;
            struct io_uring_buf_ring * buf_ring = rocketnet_uring_receive_buf_ring_setup( &ring, 1, 0, &inverted_resv, &error );
            if ( buf_ring )
            {
                rocketnet_uring_receive_buf_ring_free( &ring, buf_ring, 1, 0, inverted_resv );
            }
            else
            {
                ok = false;
            }
        }

        io_uring_queue_exit( &ring );
    }

    available.store( ok ? 1 : 0, std::memory_order_relaxed );

    return ok;
}


// ---------------------------------------------------------------------------
// the worker's kernel: its own ring (rocketnet_uring_queue_worker_kernel_t).
// Every function runs on the queue's worker thread only.
// ---------------------------------------------------------------------------

static int rocketnet_server_uring_kernel_space( void * context )
{
    struct rocketnet_server_uring_queue_t * queue = (struct rocketnet_server_uring_queue_t *) context;
    return (int) io_uring_sq_space_left( &queue->ring );
}

static struct io_uring_sqe * rocketnet_server_uring_kernel_sqe( void * context )
{
    struct rocketnet_server_uring_queue_t * queue = (struct rocketnet_server_uring_queue_t *) context;
    return io_uring_get_sqe( &queue->ring );
}

#if ROCKETNET_DEV

// the test seams, on the worker thread before each submit: a pending
// receive failure becomes a cancel of the queue's multishot under the seam's
// own tag, and while sends are set to fail each send entry prepared and not
// yet submitted names no socket, so the kernel completes it -EBADF

static void rocketnet_server_uring_test_before_submit( struct rocketnet_server_uring_queue_t * queue )
{
    const int error = queue->test_receive_error.load( std::memory_order_acquire );
    if ( error != 0 && queue->test_receive_cancelled == 0 && io_uring_sq_space_left( &queue->ring ) > 0 )
    {
        struct io_uring_sqe * sqe = io_uring_get_sqe( &queue->ring );
        io_uring_prep_cancel64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue->index, 0 ), 0 );
        io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_TEST, queue->index, 0 ) );
        queue->test_receive_cancelled = error;
        queue->test_receive_error.store( 0, std::memory_order_relaxed );
    }

    std::atomic<int> & fail = queue->uring->test_fail_sends;
    for ( unsigned head = queue->ring.sq.sqe_head; head != queue->ring.sq.sqe_tail; head++ )
    {
        struct io_uring_sqe * sqe = &queue->ring.sq.sqes[head & queue->ring.sq.ring_mask];
        if ( sqe->opcode != IORING_OP_SENDMSG )
        {
            continue;
        }
        int count = fail.load( std::memory_order_relaxed );
        while ( count > 0 && !fail.compare_exchange_weak( count, count - 1, std::memory_order_relaxed ) )
        {
        }
        if ( count > 0 )
        {
            sqe->fd = -1;
        }
    }
}

#endif // #if ROCKETNET_DEV

static int rocketnet_server_uring_kernel_submit( void * context, bool wait )
{
    struct rocketnet_server_uring_queue_t * queue = (struct rocketnet_server_uring_queue_t *) context;
#if ROCKETNET_DEV
    rocketnet_server_uring_test_before_submit( queue );
#endif // #if ROCKETNET_DEV
    return wait ? io_uring_submit_and_wait( &queue->ring, 1 ) : io_uring_submit( &queue->ring );
}

static int rocketnet_server_uring_kernel_peek( void * context, struct rocketnet_uring_queue_worker_completion_t * completions, int max )
{
    struct rocketnet_server_uring_queue_t * queue = (struct rocketnet_server_uring_queue_t *) context;

    struct io_uring_cqe * cqe = NULL;

#if ROCKETNET_DEV
    // the seam's own cancel completes here and never reaches the worker; the
    // multishot's terminating completion carries the seam's error
    while ( io_uring_peek_cqe( &queue->ring, &cqe ) == 0 && cqe &&
            rocketnet_uring_tag_kind( cqe->user_data ) == ROCKETNET_URING_TAG_TEST )
    {
        io_uring_cqe_seen( &queue->ring, cqe );
    }
#endif // #if ROCKETNET_DEV

    // a peek consumes nothing, so one batch: the worker advances what it took
    struct io_uring_cqe * cqes[ROCKETNET_URING_WORKER_CQE_BATCH];
    const int want = max < ROCKETNET_URING_WORKER_CQE_BATCH ? max : ROCKETNET_URING_WORKER_CQE_BATCH;
    const unsigned got = io_uring_peek_batch_cqe( &queue->ring, cqes, (unsigned) want );
    for ( unsigned i = 0; i < got; i++ )
    {
#if ROCKETNET_DEV
        // the multishot's last completion after the seam's cancel carries the
        // seam's error, written in the ring so a second peek reads it too
        if ( queue->test_receive_cancelled != 0 && rocketnet_uring_tag_kind( cqes[i]->user_data ) == ROCKETNET_URING_TAG_RECV && cqes[i]->res == -ECANCELED &&
             ( cqes[i]->flags & IORING_CQE_F_MORE ) == 0 )
        {
            cqes[i]->res = queue->test_receive_cancelled;
            queue->test_receive_cancelled = 0;
        }
#endif // #if ROCKETNET_DEV
        completions[i].user_data = cqes[i]->user_data;
        completions[i].res = cqes[i]->res;
        completions[i].flags = cqes[i]->flags;
    }
    (void) cqe;
    return (int) got;
}

static void rocketnet_server_uring_kernel_advance( void * context, int count )
{
    struct rocketnet_server_uring_queue_t * queue = (struct rocketnet_server_uring_queue_t *) context;
    io_uring_cq_advance( &queue->ring, (unsigned) count );
}

// the worker's receive: the receive module's functions (rocketnet_uring_queue_worker_receive_t)

static bool rocketnet_server_uring_receive_needs_arm( void * context )
{
    return rocketnet_uring_receive_queue_needs_arm( (const struct rocketnet_uring_receive_queue_t *) context );
}

static bool rocketnet_server_uring_receive_arm( void * context, struct io_uring_sqe * sqe )
{
    return rocketnet_uring_receive_queue_arm( (struct rocketnet_uring_receive_queue_t *) context, sqe );
}

static void rocketnet_server_uring_receive_cqe( void * context, int32_t res, uint32_t flags )
{
    rocketnet_uring_receive_queue_cqe( (struct rocketnet_uring_receive_queue_t *) context, res, flags );
}

static void rocketnet_server_uring_receive_stop( void * context )
{
    rocketnet_uring_receive_queue_stop( (struct rocketnet_uring_receive_queue_t *) context );
}

static bool rocketnet_server_uring_receive_armed( void * context )
{
    return rocketnet_uring_receive_queue_armed( (const struct rocketnet_uring_receive_queue_t *) context );
}

// the queue's worker thread: the loop until it has stopped and nothing of
// its is in the kernel, then the exited flag (its last access to the queue)

static void rocketnet_server_uring_worker_thread( struct rocketnet_platform_thread_t *, void * data )
{
    struct rocketnet_server_uring_queue_t * queue = (struct rocketnet_server_uring_queue_t *) data;
    rocketnet_assert( queue );
    rocketnet_uring_queue_worker_run( &queue->worker );
    queue->exited.store( true, std::memory_order_release );
}

// ---------------------------------------------------------------------------

// a queue's receive config: the transport's geometry for max_clients

static void rocketnet_server_uring_receive_config( struct rocketnet_uring_receive_queue_config_t * config, int queue_index, int fd, int max_clients, bool ipv6, bool gro,
                                                   bool socket_drops, const uint8_t * packet_filter )
{
    memset( config, 0, sizeof( *config ) );
    config->queue_index = queue_index;
    config->fd = fd;
    config->bgid = queue_index;
    config->buffers = ROCKETNET_URING_RECV_BUFFERS;
    config->buffer_bytes = ROCKETNET_URING_RECV_BUFFER_BYTES;
    config->slots = rocketnet_server_uring_bank_packets( max_clients );
    config->ipv6 = ipv6;
    config->gro = gro;
    config->socket_drops = socket_drops;
    config->packet_filter = packet_filter;
}

uint64_t rocketnet_server_uring_allocation_bytes( int queues, int max_clients )
{
    if ( queues < 1 || queues > ROCKETNET_URING_MAX_QUEUES || max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        return 0;
    }

    struct rocketnet_uring_receive_queue_config_t config;
    rocketnet_server_uring_receive_config( &config, 0, -1, max_clients, true, true, true, NULL );
    const uint64_t receive = rocketnet_uring_receive_queue_allocation_bytes( &config );
    const uint64_t banks = rocketnet_uring_send_queues_allocation_bytes( queues, ROCKETNET_URING_SEND_QUEUE_SIZE );
    const uint64_t send = rocketnet_uring_queue_send_allocation_bytes( queues, ROCKETNET_URING_SEND_QUEUE_SIZE );
    const uint64_t view = rocketnet_server_process_packets_allocation_bytes( rocketnet_server_uring_bank_packets( max_clients ) );
    if ( !receive || !banks || !send || !view )
    {
        return 0;
    }

    uint64_t bytes = rocketnet_memory_allocations( { sizeof( rocketnet_server_uring_t ), (uint64_t) queues * sizeof( rocketnet_server_uring_queue_t ), banks, send } );
    // and each queue's worker thread: its platform thread record
    if ( !bytes || !rocketnet_memory_add( bytes, (uint64_t) queues, receive ) || !rocketnet_memory_add( bytes, 1, view ) ||
         !rocketnet_memory_add( bytes, (uint64_t) queues, rocketnet_platform_thread_allocation_bytes() ) )
    {
        return 0;
    }
    return bytes <= SIZE_MAX ? bytes : 0;
}

int rocketnet_server_uring_bank_size( const struct rocketnet_server_uring_t * uring )
{
    return uring ? uring->bank_size : 0;
}

// each queue's provided buffer ring, mapped outside the allocator, whole
// pages; the submission and completion rings are the kernel's and are not
// counted in the pre-create memory plan

uint64_t rocketnet_server_uring_mapped_bytes( int queues )
{
    if ( queues < 1 || queues > ROCKETNET_URING_MAX_QUEUES )
    {
        return 0;
    }
    return (uint64_t) queues * rocketnet_uring_receive_queue_mapped_bytes( ROCKETNET_URING_RECV_BUFFERS );
}

// ---------------------------------------------------------------------------

// frees everything create made, in the reverse order: every worker thread
// is stopped and joined first (a worker exits only once nothing of its is
// in the kernel), then each receive bank (its buffer ring unregistered from
// the queue's ring), each ring, each socket and eventfd, the send banks, the
// view, the transport. Create's failure path and destroy both end here.

static void rocketnet_server_uring_free( struct rocketnet_server_uring_t * uring )
{
    void * context = uring->context;

    for ( int i = 0; uring->queues && i < uring->queues_constructed; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];
        if ( queue->thread )
        {
            rocketnet_uring_queue_worker_stop( &queue->worker );
        }
    }

    for ( int i = 0; uring->queues && i < uring->queues_constructed; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];
        if ( queue->thread )
        {
            rocketnet_platform_thread_join( queue->thread );
            rocketnet_platform_thread_destroy( queue->thread );
            queue->thread = NULL;
        }
    }

    for ( int i = 0; uring->queues && i < uring->queues_constructed; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];

        if ( queue->receive )
        {
            rocketnet_uring_receive_queue_destroy( queue->receive );
            queue->receive = NULL;
        }

        if ( queue->ring_ready )
        {
            io_uring_queue_exit( &queue->ring );
            rocketnet_direct_allocation_remove( queue->ring_mapped_bytes );
            queue->ring_mapped_bytes = 0;
            queue->ring_ready = false;
        }

        if ( queue->wake_fd >= 0 )
        {
            close( queue->wake_fd );
            queue->wake_fd = -1;
        }

        if ( queue->fd >= 0 )
        {
            close( queue->fd );
            queue->fd = -1;
        }

        queue->~rocketnet_server_uring_queue_t();
    }

    if ( uring->queues )
    {
        rocketnet_free( context, uring->queues );
        uring->queues = NULL;
    }

    rocketnet_uring_queue_send_destroy( uring->send );
    uring->send = NULL;
    rocketnet_uring_send_queues_destroy( uring->banks );
    uring->banks = NULL;

    rocketnet_server_process_packets_destroy( context, &uring->process_packets );

    uring->~rocketnet_server_uring_t();
    rocketnet_free( context, uring );
}

// one queue's ring: ROCKETNET_URING_ENTRIES submission entries, a completion
// queue twice that

static bool rocketnet_server_uring_ring_create( struct rocketnet_server_uring_queue_t * queue )
{
    struct io_uring_params params;
    memset( &params, 0, sizeof( params ) );
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = ROCKETNET_URING_ENTRIES * 2;

    const int result = io_uring_queue_init_params( ROCKETNET_URING_ENTRIES, &queue->ring, &params );
    if ( result < 0 )
    {
        rocketnet_error( "io_uring transport: queue %d: io_uring_queue_init_params failed: %s", queue->index, strerror( -result ) );
        return false;
    }

    queue->ring_ready = true;

    // the kernel's rings, mapped by liburing: the submission ring, the
    // completion ring where it is a mapping of its own, and the entries

    queue->ring_mapped_bytes = (uint64_t) queue->ring.sq.ring_sz +
                               ( queue->ring.cq.ring_ptr != queue->ring.sq.ring_ptr ? (uint64_t) queue->ring.cq.ring_sz : 0 ) +
                               (uint64_t) params.sq_entries * sizeof( struct io_uring_sqe );
    rocketnet_direct_allocation_add( queue->ring_mapped_bytes );

    if ( ( params.features & IORING_FEAT_NODROP ) == 0 )
    {
        rocketnet_error( "io_uring transport: this kernel drops completions on overflow (no IORING_FEAT_NODROP)" );
        return false;
    }

    return true;
}

struct rocketnet_server_uring_t * rocketnet_server_uring_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int * status,
                                                                 const uint8_t * packet_filter )
{
    return rocketnet_server_uring_create_flags( context, bind_address, num_queues, max_clients, 0, status, packet_filter );
}

struct rocketnet_server_uring_t * rocketnet_server_uring_create_flags( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int * status,
                                                                       const uint8_t * packet_filter )
{
    rocketnet_assert( bind_address );
    rocketnet_assert( bind_address->type == ROCKETNET_ADDRESS_IPV4 || bind_address->type == ROCKETNET_ADDRESS_IPV6 );

    if ( num_queues <= 0 )
    {
        num_queues = ROCKETNET_URING_DEFAULT_QUEUES;
    }

    // refuse, never clamp: a queue count past this transport's geometry is a
    // configuration error named here, not a silently different server

    if ( num_queues > ROCKETNET_URING_MAX_QUEUES )
    {
        rocketnet_error( "io_uring transport num_queues %d is out of range (1 to %d)", num_queues, ROCKETNET_URING_MAX_QUEUES );
        return NULL;
    }

    if ( max_clients < 1 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        rocketnet_error( "io_uring transport max_clients %d is out of range (1 to %d)", max_clients, ROCKETNET_MAX_CLIENTS );
        return NULL;
    }

    if ( !rocketnet_server_uring_available() )
    {
        rocketnet_error( "io_uring transport refused: this kernel does not accept the io_uring ring, opcodes or provided buffer ring the transport needs" );
        return NULL;
    }

    struct rocketnet_server_uring_t * uring = (struct rocketnet_server_uring_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_uring_t ) );
    if ( !uring )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        return NULL;
    }

    new ( uring ) rocketnet_server_uring_t;

    uring->context = context;
    uring->num_queues = num_queues;
    uring->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    uring->max_clients = max_clients;
    uring->bank_size = rocketnet_server_uring_bank_packets( max_clients );

    uring->queues = (struct rocketnet_server_uring_queue_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_uring_queue_t ) * (size_t) num_queues );
    uring->banks = rocketnet_uring_send_queues_create( context, num_queues, ROCKETNET_URING_SEND_QUEUE_SIZE );
    uring->send = uring->banks ? rocketnet_uring_queue_send_create( context, uring->banks, ROCKETNET_URING_MAX_SEGMENTS, ROCKETNET_URING_MAX_SUPER_BYTES ) : NULL;
    if ( !uring->queues || !uring->banks || !uring->send || !rocketnet_server_process_packets_create( context, &uring->process_packets, uring->bank_size ) )
    {
        rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
        rocketnet_server_uring_free( uring );
        return NULL;
    }

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = new ( &uring->queues[i] ) rocketnet_server_uring_queue_t;
        queue->uring = uring;
        queue->index = i;
        uring->queues_constructed = i + 1;
    }

    // the reuseport group: the first socket may discover an ephemeral port,
    // the rest join it

    uint16_t bound_port = bind_address->port;

    for ( int i = 0; i < num_queues; i++ )
    {
        uring->queues[i].fd = rocketnet_server_uring_open_socket( bind_address, i, max_clients, &bound_port );
        if ( uring->queues[i].fd < 0 )
        {
            rocketnet_server_uring_free( uring );
            return NULL;
        }
    }

    bind_address->port = bound_port;

    // the UDP_SEGMENT probe, the gso transport's: every socket must accept
    // it, all or nothing. the probe writes segment size zero, the value every
    // socket already holds, so a refusal leaves nothing to roll back. failure
    // is a tier, not an error

    if ( ( flags & ROCKETNET_SERVER_URING_DISABLE_SEGMENT ) == 0 )
    {
        int segment_sockets = 0;
        for ( int i = 0; i < num_queues; i++ )
        {
            int zero = 0;
            if ( setsockopt( uring->queues[i].fd, IPPROTO_UDP, UDP_SEGMENT, &zero, sizeof( zero ) ) == 0 )
            {
                segment_sockets++;
            }
        }

        if ( segment_sockets == num_queues )
        {
            uring->features |= ROCKETNET_SERVER_URING_FEATURE_SEGMENT;
        }
    }

    // the UDP_GRO probe, the gso transport's: every socket of the group must
    // accept it, all or nothing, since a partial enable would leave socket
    // state the reported tier denies, so the ones that accepted roll back.
    // with it the multishot reserves a control area for the UDP_GRO cmsg and
    // one buffer may hold a coalesced receive of several datagrams

    if ( ( flags & ROCKETNET_SERVER_URING_DISABLE_GRO ) == 0 )
    {
        int gro_sockets = 0;
        for ( int i = 0; i < num_queues; i++ )
        {
            int one = 1;
            if ( setsockopt( uring->queues[i].fd, IPPROTO_UDP, UDP_GRO, &one, sizeof( one ) ) == 0 )
            {
                gro_sockets++;
            }
        }

        if ( gro_sockets == num_queues )
        {
            uring->features |= ROCKETNET_SERVER_URING_FEATURE_GRO;
        }
        else
        {
            for ( int i = 0; i < num_queues; i++ )
            {
                int zero = 0;
                setsockopt( uring->queues[i].fd, IPPROTO_UDP, UDP_GRO, &zero, sizeof( zero ) );
            }
        }
    }

    const bool segment = ( uring->features & ROCKETNET_SERVER_URING_FEATURE_SEGMENT ) != 0;
    const bool gro = ( uring->features & ROCKETNET_SERVER_URING_FEATURE_GRO ) != 0;

    // per queue: SO_RXQ_OVFL (each receive then carries the socket's drop
    // count, so the datagrams the kernel dropped at a full receive buffer are
    // counted by the receive module), the ring, the receive bank over its
    // provided buffer ring, the wake eventfd and the worker

    int socket_drop_queues = 0;

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];

        int one = 1;
        const bool socket_drops = setsockopt( queue->fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof( one ) ) == 0;
        if ( socket_drops )
        {
            socket_drop_queues++;
        }

        if ( !rocketnet_server_uring_ring_create( queue ) )
        {
            rocketnet_server_uring_free( uring );
            return NULL;
        }

        struct rocketnet_uring_receive_queue_config_t receive_config;
        rocketnet_server_uring_receive_config( &receive_config, i, queue->fd, max_clients, uring->ipv6, gro, socket_drops, packet_filter );
        bool receive_out_of_memory = false;
        queue->receive = rocketnet_uring_receive_queue_create( context, &queue->ring, &receive_config, &receive_out_of_memory );
        if ( !queue->receive )
        {
            if ( receive_out_of_memory )
            {
                rocketnet_create_status( status, ROCKETNET_STATUS_OUT_OF_MEMORY );
            }
            rocketnet_server_uring_free( uring );
            return NULL;
        }

        // blocking and not a semaphore: rocketnet_uring_queue_worker.h
        queue->wake_fd = eventfd( 0, EFD_CLOEXEC );
        if ( queue->wake_fd < 0 )
        {
            rocketnet_error( "io_uring transport: queue %d: eventfd failed: %s", i, strerror( errno ) );
            rocketnet_server_uring_free( uring );
            return NULL;
        }

        struct rocketnet_uring_queue_worker_config_t worker_config;
        memset( &worker_config, 0, sizeof( worker_config ) );
        worker_config.queue_index = i;
        worker_config.fd = queue->fd;
        worker_config.ipv6 = uring->ipv6;
        worker_config.wake_fd = queue->wake_fd;
        worker_config.max_in_kernel = ROCKETNET_URING_MAX_RUNS_IN_KERNEL;
        worker_config.max_retry = ROCKETNET_URING_SEND_RETRIES_AFTER_QUIT;
        worker_config.segment = segment;
        worker_config.send = uring->send;
        worker_config.receive.context = queue->receive;
        worker_config.receive.needs_arm = rocketnet_server_uring_receive_needs_arm;
        worker_config.receive.arm = rocketnet_server_uring_receive_arm;
        worker_config.receive.cqe = rocketnet_server_uring_receive_cqe;
        worker_config.receive.stop = rocketnet_server_uring_receive_stop;
        worker_config.receive.armed = rocketnet_server_uring_receive_armed;
        worker_config.kernel.context = queue;
        worker_config.kernel.space = rocketnet_server_uring_kernel_space;
        worker_config.kernel.sqe = rocketnet_server_uring_kernel_sqe;
        worker_config.kernel.submit = rocketnet_server_uring_kernel_submit;
        worker_config.kernel.peek = rocketnet_server_uring_kernel_peek;
        worker_config.kernel.advance = rocketnet_server_uring_kernel_advance;

        if ( !rocketnet_uring_queue_worker_init( &queue->worker, &worker_config ) )
        {
            rocketnet_error( "io_uring transport: queue %d: the worker refused its config", i );
            rocketnet_server_uring_free( uring );
            return NULL;
        }
        queue->worker_ready = true;
    }

    // the threads last: a thread that fails to start leaves the started
    // ones to the free, which stops and joins them

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];
        queue->thread = rocketnet_platform_thread_create( context, rocketnet_server_uring_worker_thread, queue, status );
        if ( !queue->thread )
        {
            rocketnet_error( "io_uring transport: queue %d: the worker thread did not start", i );
            rocketnet_server_uring_free( uring );
            return NULL;
        }
    }

    rocketnet_info( "io_uring transport: %d queue reuseport group on port %d, a worker and a %d entry ring per queue, %d receive buffers of %d bytes per queue, "
                    "%d receive slots and %d send slots per queue, UDP_SEGMENT %s, UDP_GRO %s, socket drop counts on %d of %d sockets, prefilter %s",
                    num_queues, bound_port, ROCKETNET_URING_ENTRIES, ROCKETNET_URING_RECV_BUFFERS, ROCKETNET_URING_RECV_BUFFER_BYTES, uring->bank_size,
                    ROCKETNET_URING_SEND_QUEUE_SIZE, segment ? "yes" : "no (one SENDMSG per packet)", gro ? "yes" : "no (one datagram per receive buffer)",
                    socket_drop_queues, num_queues, packet_filter ? "in the workers" : "off (raw datagrams)" );

    return uring;
}

// proofs/tla/RNUringQueueWorker.tla Stop, for every queue: quit, then the
// wake. Each worker keeps sending what was published, cancels its receive
// and exits once nothing of its is in the kernel.

void rocketnet_server_uring_stop( rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    if ( uring->stopping )
    {
        return;
    }
    uring->stopping = true;

    for ( int i = 0; i < uring->num_queues; i++ )
    {
        rocketnet_uring_queue_worker_stop( &uring->queues[i].worker );
    }
}

// true once every worker has exited (RNUringQueueWorker.tla Exit: nothing of
// its in the kernel). The first true counts what the workers banked and
// main never took as RECEIVE_DROP_STOPPED: main takes nothing after stop
// (rocketnet_server_uring_receive_packets), so nothing is counted twice.
// Never waits.

bool rocketnet_server_uring_stopped( rocketnet_server_uring_t * uring )
{
    if ( !uring || !uring->stopping )
    {
        return false;
    }

#if ROCKETNET_DEV
    if ( uring->test_stall_destroy )
    {
        return false;
    }
#endif // #if ROCKETNET_DEV

    for ( int i = 0; i < uring->num_queues; i++ )
    {
        if ( !uring->queues[i].exited.load( std::memory_order_acquire ) )
        {
            return false;
        }
    }

    if ( !uring->stop_counted )
    {
        uring->stop_counted = true;
        for ( int i = 0; i < uring->num_queues; i++ )
        {
            uring->receive_dropped_stopped += rocketnet_uring_receive_queue_waiting( uring->queues[i].receive );
        }
    }

    return true;
}

// joins every worker (each has exited: stopped said so), then frees

void rocketnet_server_uring_destroy( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    const bool stopped = rocketnet_server_uring_stopped( uring );
    rocketnet_assert( stopped );
    if ( !stopped )
    {
        return;
    }

    rocketnet_server_uring_free( uring );
}

int rocketnet_server_uring_num_queues( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    return uring->num_queues;
}

// the create probe's bits; SEGMENT clears once any queue's worker has turned
// segmentation off (rocketnet_server_gso.h's refusal threshold)

int rocketnet_server_uring_features( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    int features = uring->features;
    for ( int i = 0; i < uring->num_queues; i++ )
    {
        if ( rocketnet_uring_queue_worker_counter( &uring->queues[i].worker, ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS ) != 0 )
        {
            features &= ~ROCKETNET_SERVER_URING_FEATURE_SEGMENT;
        }
    }
    return features;
}

// every queue's module counters, summed into the transport's

void rocketnet_server_uring_counters( struct rocketnet_server_uring_t * uring, uint64_t * counters )
{
    rocketnet_assert( uring );
    rocketnet_assert( counters );

    memset( counters, 0, ROCKETNET_SERVER_URING_NUM_COUNTERS * sizeof( uint64_t ) );

    for ( int i = 0; i < uring->num_queues; i++ )
    {
        const struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];

        uint64_t receive[ROCKETNET_URING_RECEIVE_NUM_COUNTERS];
        rocketnet_uring_receive_queue_counters( queue->receive, receive );

        uint64_t worker[ROCKETNET_URING_WORKER_NUM_COUNTERS];
        for ( int c = 0; c < ROCKETNET_URING_WORKER_NUM_COUNTERS; c++ )
        {
            worker[c] = rocketnet_uring_queue_worker_counter( &queue->worker, c );
        }

        const uint64_t sent = rocketnet_uring_queue_send_counter( uring->send, i, ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT );
        const uint64_t dropped = rocketnet_uring_queue_send_counter( uring->send, i, ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED );
        const uint64_t abandoned = rocketnet_uring_queue_send_counter( uring->send, i, ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED );

        counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT] += sent;
        counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED] += receive[ROCKETNET_URING_RECEIVE_COUNTER_BANKED];
        counters[ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS] += worker[ROCKETNET_URING_WORKER_COUNTER_SUBMITS];
        counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED] += dropped + abandoned;
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_OVERFLOW];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SOURCE] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS] += receive[ROCKETNET_URING_RECEIVE_COUNTER_NO_BUFFERS];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS] += receive[ROCKETNET_URING_RECEIVE_COUNTER_REARMS];
        counters[ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED] += worker[ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED];
        counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DEFERRED] += worker[ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_NO_BUFFER] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_NO_BUFFER];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARM_FAILED] += receive[ROCKETNET_URING_RECEIVE_COUNTER_REARM_FAILED];
        counters[ROCKETNET_SERVER_URING_COUNTER_GSO_SENDS] += worker[ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS];
        counters[ROCKETNET_SERVER_URING_COUNTER_GSO_FALLBACKS] += worker[ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS];
        counters[ROCKETNET_SERVER_URING_COUNTER_GRO_BATCHES] += receive[ROCKETNET_URING_RECEIVE_COUNTER_GRO_BATCHES];
        counters[ROCKETNET_SERVER_URING_COUNTER_GRO_CMSG_FAULTS] += receive[ROCKETNET_URING_RECEIVE_COUNTER_GRO_CMSG_FAULTS];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SOCKET] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOCKET];
        counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_ERRORS] += receive[ROCKETNET_URING_RECEIVE_COUNTER_ERRORS];

        // a run the socket family cannot name is dropped by the worker
        // without a send: rocketnet_uring_queue_send_complete( FAILED )
        // already counted its packets in the send module's DROPPED, so the
        // worker's own SEND_DROPPED is not added again
    }
}

// the builtin transport's receive_drops hook: the size drops of every
// queue, split at the receive ceiling (the receive module counts them before
// the prefilter, so no datagram is in both these and the drop totals)

void rocketnet_server_uring_receive_drops( struct rocketnet_server_uring_t * uring, uint64_t * too_short, uint64_t * too_large )
{
    rocketnet_assert( uring );
    rocketnet_assert( too_short );
    rocketnet_assert( too_large );

    uint64_t size = 0;
    uint64_t large = 0;
    for ( int i = 0; i < uring->num_queues; i++ )
    {
        uint64_t receive[ROCKETNET_URING_RECEIVE_NUM_COUNTERS];
        rocketnet_uring_receive_queue_counters( uring->queues[i].receive, receive );
        size += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE];
        large += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_TOO_LARGE];
    }
    *too_large = large;
    *too_short = size - large;
}

void rocketnet_server_uring_receive_drop_totals( struct rocketnet_server_uring_t * uring, uint64_t * values )
{
    rocketnet_assert( uring );
    rocketnet_assert( values );

    memset( values, 0, ROCKETNET_RECEIVE_NUM_DROPS * sizeof( uint64_t ) );
    for ( int i = 0; i < uring->num_queues; i++ )
    {
        rocketnet_uring_receive_queue_drops( uring->queues[i].receive, values );
    }
    values[ROCKETNET_RECEIVE_DROP_STOPPED] += uring->receive_dropped_stopped;
    values[ROCKETNET_RECEIVE_ACCEPT] = 0;
}

// ---------------------------------------------------------------------------
// send
// ---------------------------------------------------------------------------

// any fill thread: the destination's queue (the XDP hash), one claim of its
// producer bank (proofs/tla/RNUringQueueBankLayout.tla Claim); NULL when that
// bank is full

uint8_t * rocketnet_server_uring_start_packet( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( uring );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    uint8_t * frame = rocketnet_uring_send_queues_start( uring->banks, to, packet_id );
    if ( !frame )
    {
        rocketnet_spam( "io_uring transport send staging full. packet dropped" );
    }
    return frame;
}

void rocketnet_server_uring_finish_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( uring );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    // a pointer that names no slot of the id's producer bank is refused and
    // counted (refused_pointer); nothing is written
    rocketnet_uring_send_queues_finish( uring->banks, packet_id, packet_data, packet_bytes );
}

void rocketnet_server_uring_abort_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( uring );

    if ( !packet_data )
    {
        return;
    }

    rocketnet_uring_send_queues_abort( uring->banks, packet_id, packet_data );
}

// main, the fill joined: each queue's producer published to its worker
// (proofs/tla/RNUringQueueSend.tla Publish) and the worker woken
// (RNUringQueueWorker.tla Signal). A queue with no free bank keeps its
// producer, with its slots, for a later call.

void rocketnet_server_uring_send_packets( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    for ( int i = 0; i < uring->num_queues; i++ )
    {
        if ( rocketnet_uring_queue_send_publish( uring->send, i ) )
        {
            rocketnet_uring_queue_worker_wake( &uring->queues[i].worker );
        }
    }
}

int rocketnet_server_uring_send_queues( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    return uring->num_queues;
}

int rocketnet_server_uring_send_queue_of( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to )
{
    rocketnet_assert( uring );
    rocketnet_assert( to );
    return rocketnet_send_queue_of( to, uring->num_queues );
}

int rocketnet_server_uring_send_room_queue( struct rocketnet_server_uring_t * uring, int queue )
{
    rocketnet_assert( uring );
    rocketnet_assert( queue >= 0 && queue < uring->num_queues );
    if ( uring->stopping || queue < 0 || queue >= uring->num_queues )
    {
        return 0;
    }
    const int staged = rocketnet_uring_send_queues_staged( uring->banks, queue );
    return staged >= ROCKETNET_URING_SEND_QUEUE_SIZE ? 0 : ROCKETNET_URING_SEND_QUEUE_SIZE - staged;
}

// proofs/tla/ServerStopSpread.tla Room: the fullest queue's producer room

int rocketnet_server_uring_send_room( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    int room = ROCKETNET_URING_SEND_QUEUE_SIZE;
    for ( int i = 0; i < uring->num_queues; i++ )
    {
        const int free = rocketnet_server_uring_send_room_queue( uring, i );
        if ( free < room )
        {
            room = free;
        }
    }
    return room;
}

// proofs/tla/ServerStopSpread.tla RequestIoStop: on every queue nothing
// staged and no bank published and not yet retired

bool rocketnet_server_uring_send_idle( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    for ( int i = 0; i < uring->num_queues; i++ )
    {
        if ( !rocketnet_uring_queue_send_idle( uring->send, i ) )
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// receive
// ---------------------------------------------------------------------------

// every queue's bank drained into the view (proofs/tla/RNUringQueueReceive.tla
// Drain, which every update takes on every queue: it also releases what the
// last drain exposed), from a rotating first queue so no queue is always
// served last when the view fills. The queue after the one that filled the
// view starts the next call (the gso transport's rule).

void rocketnet_server_uring_receive_packets( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    struct rocketnet_server_socket_process_packets_t * view = &uring->process_packets;
    view->num_packets = 0;

    if ( uring->stopping )
    {
        return;
    }

    const int num_queues = uring->num_queues;
    int filled_queue = -1;
    for ( int step = 0; step < num_queues; step++ )
    {
        const int queue_index = ( uring->receive_start_queue + step ) % num_queues;
        const int before = view->num_packets;
        const int room = view->capacity - before;
        const int taken = rocketnet_uring_receive_queue_drain( uring->queues[queue_index].receive, room, &view->from[before], &view->packet_bytes[before],
                                                               &view->packet_data[before] );
        view->num_packets = before + taken;
        if ( filled_queue < 0 && room > 0 && view->num_packets >= view->capacity )
        {
            filled_queue = queue_index;
        }
    }

    const int origin = filled_queue >= 0 ? filled_queue : uring->receive_start_queue;
    uring->receive_start_queue = ( origin + 1 ) % num_queues;
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_uring_process_packets( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    return &uring->process_packets;
}

// ---------------------------------------------------------------------------
// test seams
// ---------------------------------------------------------------------------

#if ROCKETNET_DEV

void rocketnet_server_uring_test_stall_destroy( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    uring->test_stall_destroy = true;
}

void rocketnet_server_uring_test_resume_stop( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    uring->test_stall_destroy = false;
}

void rocketnet_server_uring_test_fail_receive( struct rocketnet_server_uring_t * uring, int queue_index, int error )
{
    rocketnet_assert( uring );
    rocketnet_assert( queue_index >= 0 && queue_index < uring->num_queues );
    rocketnet_assert( error < 0 );
    uring->queues[queue_index].test_receive_error.store( error, std::memory_order_release );
    rocketnet_uring_queue_worker_wake( &uring->queues[queue_index].worker );
}

void rocketnet_server_uring_test_fail_sends( struct rocketnet_server_uring_t * uring, int count )
{
    rocketnet_assert( uring );
    uring->test_fail_sends.store( count, std::memory_order_relaxed );
}

bool rocketnet_server_uring_test_send_idle( struct rocketnet_server_uring_t * uring )
{
    return rocketnet_server_uring_send_idle( uring );
}

#endif // #if ROCKETNET_DEV

#else // #if defined( ROCKETNET_URING_BUILT ) && ROCKETNET_URING_BUILT

// built without liburing (or not on Linux): every entry refuses or reads
// empty, and create logs the refusal

struct rocketnet_server_uring_t
{
    int unused;
};

bool rocketnet_server_uring_available()
{
    return false;
}

uint64_t rocketnet_server_uring_allocation_bytes( int queues, int max_clients )
{
    (void) queues;
    (void) max_clients;
    return 0;
}

int rocketnet_server_uring_bank_size( const struct rocketnet_server_uring_t * )
{
    return 0;
}

uint64_t rocketnet_server_uring_mapped_bytes( int queues )
{
    (void) queues;
    return 0;
}

struct rocketnet_server_uring_t * rocketnet_server_uring_create( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int * status,
                                                                 const uint8_t * packet_filter )
{
    (void) status;
    (void) context;
    (void) bind_address;
    (void) num_queues;
    (void) max_clients;
    (void) packet_filter;
    rocketnet_error( "io_uring transport refused: built without liburing" );
    return NULL;
}

struct rocketnet_server_uring_t * rocketnet_server_uring_create_flags( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int * status,
                                                                       const uint8_t * packet_filter )
{
    (void) flags;
    return rocketnet_server_uring_create( context, bind_address, num_queues, max_clients, status, packet_filter );
}

int rocketnet_server_uring_features( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return 0;
}

void rocketnet_server_uring_stop( struct rocketnet_server_uring_t * ) {}
bool rocketnet_server_uring_stopped( struct rocketnet_server_uring_t * ) { return true; }

void rocketnet_server_uring_destroy( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
}

#if ROCKETNET_DEV
void rocketnet_server_uring_test_resume_stop( struct rocketnet_server_uring_t * ) {}

void rocketnet_server_uring_test_stall_destroy( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
}
#endif // #if ROCKETNET_DEV

int rocketnet_server_uring_num_queues( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return 0;
}

void rocketnet_server_uring_counters( struct rocketnet_server_uring_t * uring, uint64_t * counters )
{
    (void) uring;
    if ( counters )
    {
        memset( counters, 0, ROCKETNET_SERVER_URING_NUM_COUNTERS * sizeof( uint64_t ) );
    }
}

void rocketnet_server_uring_receive_drops( struct rocketnet_server_uring_t * uring, uint64_t * too_short, uint64_t * too_large )
{
    (void) uring;
    *too_short = 0;
    *too_large = 0;
}

void rocketnet_server_uring_receive_drop_totals( struct rocketnet_server_uring_t * uring, uint64_t * values )
{
    (void) uring;
    memset( values, 0, ROCKETNET_RECEIVE_NUM_DROPS * sizeof( uint64_t ) );
}

uint8_t * rocketnet_server_uring_start_packet( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to, uint64_t * packet_id )
{
    (void) uring;
    (void) to;
    (void) packet_id;
    return NULL;
}

void rocketnet_server_uring_finish_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    (void) uring;
    (void) packet_id;
    (void) packet_data;
    (void) packet_bytes;
}

void rocketnet_server_uring_abort_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data )
{
    (void) uring;
    (void) packet_id;
    (void) packet_data;
}

void rocketnet_server_uring_send_packets( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
}

int rocketnet_server_uring_send_room( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return 0;
}

int rocketnet_server_uring_send_queues( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return 1;
}

int rocketnet_server_uring_send_queue_of( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to )
{
    (void) uring;
    (void) to;
    return 0;
}

int rocketnet_server_uring_send_room_queue( struct rocketnet_server_uring_t * uring, int queue )
{
    (void) uring;
    (void) queue;
    return 0;
}

bool rocketnet_server_uring_send_idle( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return true;
}

void rocketnet_server_uring_receive_packets( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_uring_process_packets( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return NULL;
}

#if ROCKETNET_DEV
void rocketnet_server_uring_test_fail_receive( struct rocketnet_server_uring_t *, int, int ) {}
void rocketnet_server_uring_test_fail_sends( struct rocketnet_server_uring_t *, int ) {}
bool rocketnet_server_uring_test_send_idle( struct rocketnet_server_uring_t * ) { return true; }
#endif // #if ROCKETNET_DEV

#endif // #if defined( ROCKETNET_URING_BUILT ) && ROCKETNET_URING_BUILT

int rocketnet_server_uring_linux_cpp_dummy = 0;
