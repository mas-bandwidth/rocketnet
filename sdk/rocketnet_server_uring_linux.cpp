/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_internal.h"
#include "rocketnet_server_uring.h"

#include <new>
#include <atomic>
#include <inttypes.h>
#include <string.h>

// The io_uring server transport. See rocketnet_server_uring.h for the shape;
// this file is the machinery. tla/RNUringDriver.tla models it: each function
// that implements a model action names the action.
//
// One io_uring per transport, touched only by the main thread; no worker
// thread. The receive side is one multishot RECVMSG per socket in the
// SO_REUSEPORT group over that socket's provided buffer ring: the kernel
// fills ring buffers and posts one CQE per datagram, and the main thread
// copies each datagram into the FILL bank and returns the buffer to the
// ring. receive_packets swaps the FILL and EXPOSED banks, so the batch the
// transport reads never changes until the next receive_packets, even though
// receive CQEs are also reaped inside send_packets.
//
// The send side is the gso transport's staging: game threads reserve a frame
// slot with one atomic add between flushes, and send_packets submits one
// SENDMSG per slot with bytes, each slot carrying its own msghdr, iovec and
// destination, then reaps ready completions within its completion budget
// without waiting. The reservation index goes back to zero only in a call that leaves
// no send without its CQE and no staged slot unsubmitted: until its CQE the
// kernel owns a slot's frame and msghdr, so a call that returns first leaves
// the index where it is and a later call retires the rest.
//
// Every call is bounded: receive_packets reaps at most
// ROCKETNET_URING_RECEIVE_BUDGET completions and send_packets at most
// ROCKETNET_URING_SEND_BUDGET, each from its own budget; completions past a
// budget stay in the completion queue for the next call.

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && defined(ROCKETNET_IO_URING) && ROCKETNET_IO_URING
#include <liburing.h>
#include "rocketnet_uring_codec.h"
#if defined(IORING_RECV_MULTISHOT)
#define ROCKETNET_URING_BUILT 1
#endif
#endif

#if defined(ROCKETNET_URING_BUILT) && ROCKETNET_URING_BUILT

#include "rocketnet_constants.h"
#include "rocketnet_platform.h"
#include "rocketnet_wire.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>

// one receive bank holds a whole process batch, so the swap never drops
// below the process arrays

#define ROCKETNET_URING_BANK_SIZE              ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS

ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_BANK_SIZE >= ROCKETNET_URING_SEND_BUDGET,
                         "receive budget twice send: one send reap fits the receive bank" );

// completions taken per peek

#define ROCKETNET_URING_CQE_BATCH                                          256

// Only private synchronous create-failure cleanup waits for kernel ownership.
#define ROCKETNET_URING_ABORT_CREATE_WAIT_SECONDS 0.001

// each slot's info is the slot's msghdr, iovec and destination, which the
// kernel reads until the send's CQE. finish threads write packet_bytes, so a
// slot's info is cache line aligned.

struct alignas(64) rocketnet_server_uring_send_info_t
{
    rocketnet_address_t to;
    int packet_bytes;
    struct msghdr message;
    struct iovec iovec;
    struct sockaddr_storage name;
};

struct rocketnet_server_uring_send_buffer_t
{
    uint8_t padding_0[4096];

    std::atomic<int> num_packets{0};

    uint8_t padding_1[4096];

    rocketnet_server_uring_send_info_t info[ROCKETNET_URING_SEND_QUEUE_SIZE];

    uint8_t padding_2[4096];

    alignas(16) uint8_t data[ROCKETNET_URING_SEND_QUEUE_SIZE][ROCKETNET_FRAME_STRIDE];

    uint8_t padding_3[4096];
};

struct rocketnet_server_uring_bank_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_URING_BANK_SIZE];
    int packet_bytes[ROCKETNET_URING_BANK_SIZE];
    alignas(16) uint8_t packet_data[ROCKETNET_URING_BANK_SIZE][ROCKETNET_FRAME_STRIDE];
};

struct rocketnet_server_uring_queue_t
{
    int fd;
    bool armed;                                 // a multishot RECVMSG is in the kernel (or queued to submit)
    bool cancel_pending;                        // destroy's cancel SQE has no CQE yet
    struct io_uring_buf_ring * buf_ring;
    bool buf_ring_inverted_resv;                // registered through the inverted resv check (see setup_buf_ring)
    struct msghdr receive_message;              // the multishot's msghdr: the name area reserved, no control
    alignas(64) uint8_t buffers[ROCKETNET_URING_RECV_BUFFERS][ROCKETNET_URING_RECV_BUFFER_BYTES];
};

struct rocketnet_server_uring_t
{
    void * context;

    int num_queues;
    bool ipv6;
    bool ring_ready;
    bool destroying;
    bool test_stall_destroy;                    // test completion remains pending until explicitly released
    rocketnet_address_t bind_address;

    struct io_uring ring;

    struct rocketnet_server_uring_queue_t * queues;

    std::atomic<uint64_t> packet_id{0};

    uint64_t send_rotation;                     // main thread only: rotates submit chunks across the group's sockets
    int sends_pending;                          // sends submitted, by this send_packets or an earlier one, with no CQE reaped
    int send_submitted;                         // staged slots below this were submitted by an earlier send_packets
    int fill_bank;                              // the bank receive CQEs are copied into; the other is exposed

    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];

    // the part of RECEIVE_DROPPED_SIZE above the receive ceiling or truncated
    // in its buffer; the rest is below the smallest packet a server reads
    uint64_t receive_dropped_too_large;

    struct rocketnet_server_uring_send_buffer_t send;

    struct rocketnet_server_uring_bank_t banks[2];

    struct rocketnet_server_socket_process_packets_t process_packets;

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
    // the test seam: a queue whose multishot is cancelled so its terminating
    // CQE carries this error in place of -ECANCELED (zero: none pending), and
    // the next send SQEs prepared with no destination, so the kernel answers
    // each with a failed CQE
    int test_receive_error[ROCKETNET_URING_MAX_QUEUES];
    int test_fail_sends;
#endif

#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    // the unit suite's fake completion source (rocketnet_server_uring_test_create):
    // non-NULL means no kernel ring. its submission entries queue here until
    // a submit hands their user_data to the source, and a peek copies its
    // completions here
    const struct rocketnet_server_uring_test_source_t * test_source;
    struct io_uring_sqe test_sqes[ROCKETNET_URING_ENTRIES];
    int test_sqes_queued;
    int test_wait_calls;
    struct rocketnet_server_uring_test_completion_t test_completions[ROCKETNET_URING_CQE_BATCH];

    // the no-kernel-ring seam: a fake transport whose receive_packets runs only
    // the re-arm loop, and the next this many arm calls find no submission
    // entry (the failed re-arm the CQE handler can hit)
    bool test_no_ring;
    int test_arm_find_no_sqe;
#endif
};

// the ring operations, each through the fake completion source when the
// unit suite's seam is compiled in and the transport has one

#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
static_assert( sizeof(struct rocketnet_server_uring_test_completion_t) == sizeof(struct io_uring_cqe), "a fake completion is laid out as a cqe" );
#define ROCKETNET_URING_FAKE( uring ) ( (uring)->test_source != NULL )
#else
#define ROCKETNET_URING_FAKE( uring ) false
#endif

static struct io_uring_sqe * rocketnet_server_uring_ring_get_sqe( struct rocketnet_server_uring_t * uring )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( ROCKETNET_URING_FAKE( uring ) )
    {
        if ( uring->test_sqes_queued >= ROCKETNET_URING_ENTRIES )
            return NULL;
        struct io_uring_sqe * sqe = &uring->test_sqes[uring->test_sqes_queued++];
        memset( sqe, 0, sizeof(*sqe) );
        return sqe;
    }
#endif
    return io_uring_get_sqe( &uring->ring );
}

static unsigned rocketnet_server_uring_ring_sq_space( struct rocketnet_server_uring_t * uring )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( ROCKETNET_URING_FAKE( uring ) )
        return (unsigned) ( ROCKETNET_URING_ENTRIES - uring->test_sqes_queued );
#endif
    return io_uring_sq_space_left( &uring->ring );
}

static unsigned rocketnet_server_uring_ring_sq_ready( struct rocketnet_server_uring_t * uring )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( ROCKETNET_URING_FAKE( uring ) )
        return (unsigned) uring->test_sqes_queued;
#endif
    return io_uring_sq_ready( &uring->ring );
}

static int rocketnet_server_uring_ring_submit( struct rocketnet_server_uring_t * uring )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( ROCKETNET_URING_FAKE( uring ) )
    {
        const int count = uring->test_sqes_queued;
        uring->test_sqes_queued = 0;
        for ( int i = 0; i < count; i++ )
            uring->test_source->submitted( uring->test_source->context, uring->test_sqes[i].user_data );
        return count;
    }
#endif
    return io_uring_submit( &uring->ring );
}

static unsigned rocketnet_server_uring_ring_peek( struct rocketnet_server_uring_t * uring, struct io_uring_cqe ** cqes, unsigned max )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( ROCKETNET_URING_FAKE( uring ) )
    {
        if ( max > ROCKETNET_URING_CQE_BATCH )
            max = ROCKETNET_URING_CQE_BATCH;
        int count = uring->test_source->peek( uring->test_source->context, uring->test_completions, (int) max );
        if ( count < 0 )
            count = 0;
        if ( count > (int) max )
            count = (int) max;
        for ( int i = 0; i < count; i++ )
            cqes[i] = (struct io_uring_cqe*) (void*) &uring->test_completions[i];
        return (unsigned) count;
    }
#endif
    return io_uring_peek_batch_cqe( &uring->ring, cqes, max );
}

static void rocketnet_server_uring_ring_advance( struct rocketnet_server_uring_t * uring, unsigned count )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( ROCKETNET_URING_FAKE( uring ) )
    {
        uring->test_source->advance( uring->test_source->context, (int) count );
        return;
    }
#endif
    io_uring_cq_advance( &uring->ring, count );
}

static unsigned rocketnet_server_uring_ring_cq_ready( struct rocketnet_server_uring_t * uring )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( ROCKETNET_URING_FAKE( uring ) )
    {
        const int ready = uring->test_source->ready( uring->test_source->context );
        return ready > 0 ? (unsigned) ready : 0;
    }
#endif
    return io_uring_cq_ready( &uring->ring );
}

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

        struct sockaddr_in6 * addr = (struct sockaddr_in6*) out;
        memset( addr, 0, sizeof(struct sockaddr_in6) );
        addr->sin6_family = AF_INET6;
        for ( int i = 0; i < 8; i++ )
        {
            ( (uint16_t*) &addr->sin6_addr ) [i] = rocketnet_platform_htons( to.data.ipv6[i] );
        }
        addr->sin6_port = rocketnet_platform_htons( to.port );
        return sizeof(struct sockaddr_in6);
    }
    else
    {
        if ( to.type != ROCKETNET_ADDRESS_IPV4 )
            return 0;

        rocketnet_platform_address_to_sockaddr_in( &to, (struct sockaddr_in*) out );
        return sizeof(struct sockaddr_in);
    }
}

// one reuseport socket, bound as the gso transport binds its group: family from
// the bind address, dual stack when ipv6, the configured buffer sizes
// requested and the grant reported, the first socket discovering an
// ephemeral port the rest join, non-blocking, don't-fragment.

static int rocketnet_server_uring_open_socket( const rocketnet_address_t * bind_address, int queue_index, uint16_t * bound_port )
{
    const bool ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;

    int fd = socket( ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if ( fd < 0 )
    {
        rocketnet_error( "io_uring transport: failed to create socket: %s", strerror( errno ) );
        return -1;
    }

    int one = 1;
    if ( setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one) ) != 0 )
    {
        rocketnet_error( "io_uring transport: failed to set SO_REUSEPORT: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( ipv6 )
    {
        int zero = 0;
        if ( setsockopt( fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero) ) != 0 )
        {
            rocketnet_error( "io_uring transport: failed to clear socket ipv6 only: %s", strerror( errno ) );
            close( fd );
            return -1;
        }
    }

    int send_buffer_size = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    int receive_buffer_size = ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE;

    if ( setsockopt( fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(int) ) != 0 )
    {
        rocketnet_error( "io_uring transport: failed to set socket send buffer size: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( setsockopt( fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size, sizeof(int) ) != 0 )
    {
        rocketnet_error( "io_uring transport: failed to set socket receive buffer size: %s", strerror( errno ) );
        close( fd );
        return -1;
    }

    int granted_send = 0;
    int granted_receive = 0;
    socklen_t optlen = sizeof(int);
    getsockopt( fd, SOL_SOCKET, SO_SNDBUF, &granted_send, &optlen );
    optlen = sizeof(int);
    getsockopt( fd, SOL_SOCKET, SO_RCVBUF, &granted_receive, &optlen );
    granted_send /= 2;
    granted_receive /= 2;

    if ( queue_index == 0 )
    {
        if ( granted_send < send_buffer_size || granted_receive < receive_buffer_size )
        {
            rocketnet_warn( "io_uring transport: socket buffers granted below request: send %d/%d receive %d/%d (raise net.core.wmem_max/rmem_max)",
                            granted_send, send_buffer_size, granted_receive, receive_buffer_size );
        }
        else
        {
            rocketnet_info( "io_uring transport: socket buffers granted: send %d receive %d", granted_send, granted_receive );
        }
    }

    rocketnet_address_t addr = *bind_address;
    addr.port = *bound_port != 0 ? *bound_port : bind_address->port;

    struct sockaddr_storage sockaddr;
    socklen_t socklen = rocketnet_server_uring_address_to_sockaddr( &addr, ipv6, &sockaddr );

    if ( socklen == 0 || bind( fd, (struct sockaddr*) &sockaddr, socklen ) < 0 )
    {
        rocketnet_error( "io_uring transport: failed to bind socket (%s): %s", ipv6 ? "ipv6" : "ipv4", strerror( errno ) );
        close( fd );
        return -1;
    }

    if ( *bound_port == 0 )
    {
        struct sockaddr_storage bound;
        memset( &bound, 0, sizeof(bound) );
        socklen_t bound_length = sizeof(bound);
        if ( getsockname( fd, (struct sockaddr*) &bound, &bound_length ) != 0 )
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

    if ( ipv6 )
    {
        int df = 1;
        setsockopt( fd, IPPROTO_IPV6, IPV6_DONTFRAG, &df, sizeof(df) );
    }
    else
    {
        int pmtud = IP_PMTUDISC_DO;
        setsockopt( fd, IPPROTO_IP, IP_MTU_DISCOVER, &pmtud, sizeof(pmtud) );
    }

    return fd;
}

// ---------------------------------------------------------------------------

// the provided buffer ring. Ubuntu 24.04 kernels 6.8.0-136 through 6.8.0-148
// carry a broken backport in io_uring/kbuf.c: the resv check in
// io_register_pbuf_ring and io_unregister_pbuf_ring is inverted, so every
// correct registration (resv all zero) is refused with EINVAL and one with a
// nonzero resv is taken. When the plain form is refused with EINVAL, register
// once more with resv[0] = 1: a correct kernel refuses that too, so the retry
// only succeeds where the check is inverted, and the ring is the same ring
// either way. Unregister then has to carry the same nonzero resv.

static struct io_uring_buf_ring * rocketnet_server_uring_setup_buf_ring( struct io_uring * ring, unsigned int entries, int bgid, bool * inverted_resv, int * error )
{
    *inverted_resv = false;
    struct io_uring_buf_ring * buf_ring = io_uring_setup_buf_ring( ring, entries, bgid, 0, error );
    if ( buf_ring || *error != -EINVAL )
        return buf_ring;

    const size_t bytes = (size_t) entries * sizeof(struct io_uring_buf);
    void * memory = mmap( NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0 );
    if ( memory == MAP_FAILED )
        return NULL;                                // keep the EINVAL of the plain form

    struct io_uring_buf_reg reg;
    memset( &reg, 0, sizeof(reg) );
    reg.ring_addr = (uint64_t) (uintptr_t) memory;
    reg.ring_entries = entries;
    reg.bgid = (uint16_t) bgid;
    reg.resv[0] = 1;
    if ( syscall( __NR_io_uring_register, ring->ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1 ) != 0 )
    {
        munmap( memory, bytes );
        return NULL;                                // a correct kernel: the EINVAL of the plain form stands
    }

    buf_ring = (struct io_uring_buf_ring *) memory;
    io_uring_buf_ring_init( buf_ring );
    *inverted_resv = true;
    *error = 0;
    return buf_ring;
}

static void rocketnet_server_uring_free_buf_ring( struct io_uring * ring, struct io_uring_buf_ring * buf_ring, unsigned int entries, int bgid, bool inverted_resv )
{
    if ( !inverted_resv )
    {
        io_uring_free_buf_ring( ring, buf_ring, entries, bgid );
        return;
    }

    struct io_uring_buf_reg reg;
    memset( &reg, 0, sizeof(reg) );
    reg.bgid = (uint16_t) bgid;
    reg.resv[0] = 1;
    syscall( __NR_io_uring_register, ring->ring_fd, IORING_UNREGISTER_PBUF_RING, &reg, 1 );
    munmap( buf_ring, (size_t) entries * sizeof(struct io_uring_buf) );
}

// ---------------------------------------------------------------------------

bool rocketnet_server_uring_available()
{
    // the probe: a ring, the three opcodes, and a provided buffer ring.
    // -1 not yet probed, 0 no, 1 yes

    static std::atomic<int> available{-1};

    int known = available.load( std::memory_order_relaxed );
    if ( known >= 0 )
        return known == 1;

    bool ok = false;
    struct io_uring ring;
    if ( io_uring_queue_init( 8, &ring, 0 ) == 0 )
    {
        struct io_uring_probe * probe = io_uring_get_probe_ring( &ring );
        if ( probe )
        {
            ok = io_uring_opcode_supported( probe, IORING_OP_SENDMSG ) &&
                 io_uring_opcode_supported( probe, IORING_OP_RECVMSG ) &&
                 io_uring_opcode_supported( probe, IORING_OP_ASYNC_CANCEL );
            io_uring_free_probe( probe );
        }
        if ( ok )
        {
            int error = 0;
            bool inverted_resv = false;
            struct io_uring_buf_ring * buf_ring = rocketnet_server_uring_setup_buf_ring( &ring, 1, 0, &inverted_resv, &error );
            if ( buf_ring )
                rocketnet_server_uring_free_buf_ring( &ring, buf_ring, 1, 0, inverted_resv );
            else
                ok = false;
        }
        io_uring_queue_exit( &ring );
    }

    available.store( ok ? 1 : 0, std::memory_order_relaxed );
    return ok;
}

uint64_t rocketnet_server_uring_allocation_bytes( int queues )
{
    if ( queues < 1 || queues > ROCKETNET_URING_MAX_QUEUES )
        return 0;
    uint64_t bytes = rocketnet_memory_allocations( { sizeof(rocketnet_server_uring_t),
        (uint64_t) queues * sizeof(rocketnet_server_uring_queue_t) } );
    return bytes <= SIZE_MAX ? bytes : 0;
}

// an SQE, submitting the queued ones first when the submission queue is full

static struct io_uring_sqe * rocketnet_server_uring_get_sqe( struct rocketnet_server_uring_t * uring )
{
    struct io_uring_sqe * sqe = rocketnet_server_uring_ring_get_sqe( uring );
    if ( !sqe )
    {
        rocketnet_server_uring_ring_submit( uring );
        uring->counters[ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS]++;
        sqe = rocketnet_server_uring_ring_get_sqe( uring );
    }
    return sqe;
}

// arm the queue's multishot RECVMSG over its provided buffer ring (bgid =
// queue). the model's armed: the SQE is queued here and submitted by the
// caller's next submit.

static bool rocketnet_server_uring_arm( struct rocketnet_server_uring_t * uring, int queue_index )
{
    struct rocketnet_server_uring_queue_t * queue = &uring->queues[queue_index];
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( uring->test_arm_find_no_sqe > 0 )
    {
        uring->test_arm_find_no_sqe--;
        return false;
    }
    if ( uring->test_no_ring )
    {
        queue->armed = true;
        return true;
    }
#endif
    struct io_uring_sqe * sqe = rocketnet_server_uring_get_sqe( uring );
    if ( !sqe )
    {
        rocketnet_error( "io_uring transport: no submission entry to arm queue %d", queue_index );
        return false;
    }
    io_uring_prep_recvmsg_multishot( sqe, queue->fd, &queue->receive_message, 0 );
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = (uint16_t) queue_index;
    io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue_index, 0 ) );
    queue->armed = true;
    return true;
}

// one receive CQE's datagram into the FILL bank (RNUringDriver Reap, the
// "recv" case: always the fill bank, whether reaped here from
// receive_packets or from inside send_packets), then its buffer back to the
// ring (RNUringDriver ReturnBuffer)

static void rocketnet_server_uring_receive_cqe( struct rocketnet_server_uring_t * uring, int queue_index, const struct io_uring_cqe * cqe )
{
    struct rocketnet_server_uring_queue_t * queue = &uring->queues[queue_index];

    const int buffer_id = rocketnet_uring_buffer_id( cqe->flags );
    if ( buffer_id < 0 || buffer_id >= ROCKETNET_URING_RECV_BUFFERS )
    {
        uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_NO_BUFFER]++;
        return;
    }

    const uint8_t * buffer = queue->buffers[buffer_id];

    const uint8_t * name = NULL;
    const uint8_t * payload = NULL;
    int name_bytes = 0;
    int payload_bytes = 0;

    const int verdict = rocketnet_uring_recvmsg_parse( buffer, cqe->res, (int) sizeof(struct sockaddr_storage), 0,
                                                       &name, &name_bytes, &payload, &payload_bytes );

    if ( verdict == ROCKETNET_URING_RECV_NAME_TRUNCATED )
    {
        uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SOURCE]++;
    }
    else if ( verdict != ROCKETNET_URING_RECV_OK )
    {
        uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE]++;
        if ( verdict == ROCKETNET_URING_RECV_PAYLOAD_TRUNCATED )
            uring->receive_dropped_too_large++;
    }
    else
    {
        struct sockaddr_storage source;
        memset( &source, 0, sizeof(source) );
        memcpy( &source, name, (size_t) name_bytes );

        rocketnet_address_t from;
        if ( !rocketnet_uring_sockaddr_to_address( &source, name_bytes, uring->ipv6, &from ) )
        {
            uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SOURCE]++;
        }
        else if ( payload_bytes < ROCKETNET_WIRE_SERVER_MIN_PACKET_BYTES || payload_bytes > ROCKETNET_RECEIVE_CEILING )
        {
            uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE]++;
            if ( payload_bytes > ROCKETNET_RECEIVE_CEILING )
                uring->receive_dropped_too_large++;
        }
        else
        {
            struct rocketnet_server_uring_bank_t * fill = &uring->banks[uring->fill_bank];
            if ( fill->num_packets >= ROCKETNET_URING_BANK_SIZE )
            {
                uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW]++;
            }
            else
            {
                const int index = fill->num_packets++;
                fill->from[index] = from;
                fill->packet_bytes[index] = payload_bytes;
                memcpy( fill->packet_data[index], payload, (size_t) payload_bytes );
                uring->counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED]++;
            }
        }
    }

    // RNUringDriver ReturnBuffer: the datagram is in a bank (or counted
    // dropped), so the buffer goes back to the ring

    io_uring_buf_ring_add( queue->buf_ring, queue->buffers[buffer_id], ROCKETNET_URING_RECV_BUFFER_BYTES,
                           (unsigned short) buffer_id, io_uring_buf_ring_mask( ROCKETNET_URING_RECV_BUFFERS ), 0 );
    io_uring_buf_ring_advance( queue->buf_ring, 1 );
}

// RNUringDriver Reap(c), the "term" case: the multishot ended, so it is
// disarmed. the one-shot re-arm is not guaranteed (it can find no submission
// entry), so receive_packets re-arms a disarmed queue; each failed arm here
// is counted too.

static void rocketnet_server_uring_multishot_ended( struct rocketnet_server_uring_t * uring, int queue_index )
{
    struct rocketnet_server_uring_queue_t * queue = &uring->queues[queue_index];

    queue->armed = false;

    if ( !uring->destroying )
    {
        if ( rocketnet_server_uring_arm( uring, queue_index ) )
            uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS]++;
        else
            uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARM_FAILED]++;
    }
}

// RNUringDriver Reap(c): one CQE, by the kind in its tag

static void rocketnet_server_uring_reap_cqe( struct rocketnet_server_uring_t * uring, const struct io_uring_cqe * cqe )
{
    const uint64_t tag = cqe->user_data;
    const int kind = rocketnet_uring_tag_kind( tag );
    const int queue_index = rocketnet_uring_tag_queue( tag );

    if ( kind == ROCKETNET_URING_TAG_SEND )
    {
        // the "send" case: the slot is reaped; its frame and msghdr are the
        // staging's again (RNUringDriver KernelComplete posted this CQE)

        uring->sends_pending--;
        if ( cqe->res < 0 )
            uring->counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED]++;
        else
            uring->counters[ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT]++;
        return;
    }

    if ( queue_index < 0 || queue_index >= uring->num_queues )
        return;

    struct rocketnet_server_uring_queue_t * queue = &uring->queues[queue_index];

    if ( kind == ROCKETNET_URING_TAG_RECV )
    {
#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
        struct io_uring_cqe injected;
        if ( uring->test_receive_error[queue_index] != 0 && ( cqe->flags & IORING_CQE_F_MORE ) == 0 && cqe->res == -ECANCELED )
        {
            injected = *cqe;
            injected.res = uring->test_receive_error[queue_index];
            uring->test_receive_error[queue_index] = 0;
            cqe = &injected;
        }
#endif
        if ( cqe->res >= 0 )
            rocketnet_server_uring_receive_cqe( uring, queue_index, cqe );

        if ( ( cqe->flags & IORING_CQE_F_MORE ) == 0 )
        {
            // the "term" case: the multishot ended (RNUringDriver KernelStop
            // posted this CQE). re-arm while running, never during destroy.

            if ( cqe->res == -ENOBUFS )
                uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS]++;

            rocketnet_server_uring_multishot_ended( uring, queue_index );
        }
        return;
    }

    if ( kind == ROCKETNET_URING_TAG_CANCEL )
    {
        // the "cancel" case. a cancel that found nothing (the multishot was
        // re-armed after the cancel was queued) is sent again by destroy
        queue->cancel_pending = false;
        return;
    }
}

// reap the CQEs available now, oldest first, without blocking and at most
// budget of them (RNUringDriver Reap within a call's Budget); returns how
// many. CQEs past the budget stay in the completion queue for a later call.

static int rocketnet_server_uring_reap_available( struct rocketnet_server_uring_t * uring, int budget )
{
    int total = 0;
    while ( total < budget )
    {
        struct io_uring_cqe * cqes[ROCKETNET_URING_CQE_BATCH];
        unsigned want = (unsigned) ( budget - total );
        if ( want > ROCKETNET_URING_CQE_BATCH )
            want = ROCKETNET_URING_CQE_BATCH;
        const unsigned count = rocketnet_server_uring_ring_peek( uring, cqes, want );
        if ( count == 0 )
            break;
        for ( unsigned i = 0; i < count; i++ )
            rocketnet_server_uring_reap_cqe( uring, cqes[i] );
        rocketnet_server_uring_ring_advance( uring, count );
        total += (int) count;
    }
    return total;
}

static void rocketnet_server_uring_submit( struct rocketnet_server_uring_t * uring )
{
    if ( rocketnet_server_uring_ring_sq_ready( uring ) == 0 )
        return;
    const int result = rocketnet_server_uring_ring_submit( uring );
    uring->counters[ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS]++;
    if ( result < 0 && result != -EINTR && result != -EAGAIN && result != -EBUSY )
        rocketnet_error( "io_uring transport: submit failed: %s", strerror( -result ) );
}

// wait for at least one CQE, submitting anything queued on the way

static void rocketnet_server_uring_wait( struct rocketnet_server_uring_t * uring, double seconds )
{
#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    uring->test_wait_calls++;
#endif
    if ( ROCKETNET_URING_FAKE( uring ) ) return;
    struct __kernel_timespec timeout;
    timeout.tv_sec = (long long) seconds;
    timeout.tv_nsec = (long long) ( ( seconds - (double) timeout.tv_sec ) * 1000000000.0 );
    struct io_uring_cqe * cqe = NULL;
    io_uring_submit_and_wait_timeout( &uring->ring, &cqe, 1, &timeout, NULL );
    uring->counters[ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS]++;
}

// ---------------------------------------------------------------------------

// bytes left allocated by destroys that gave up waiting, and how many did

static std::atomic<uint64_t> rocketnet_server_uring_leaked_bytes_total{0};
static std::atomic<uint64_t> rocketnet_server_uring_leaked_transports_total{0};

uint64_t rocketnet_server_uring_leaked_bytes()
{
    return rocketnet_server_uring_leaked_bytes_total.load();
}

uint64_t rocketnet_server_uring_leaked_transports()
{
    return rocketnet_server_uring_leaked_transports_total.load();
}

static void rocketnet_server_uring_abort_create( rocketnet_server_uring_t * uring )
{
    rocketnet_server_uring_stop( uring );
    while ( !rocketnet_server_uring_stopped( uring ) )
        rocketnet_server_uring_wait( uring, ROCKETNET_URING_ABORT_CREATE_WAIT_SECONDS );
    rocketnet_server_uring_destroy( uring );
}

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)

void rocketnet_server_uring_test_fail_receive( struct rocketnet_server_uring_t * uring, int queue_index, int error )
{
    rocketnet_assert( uring );
    rocketnet_assert( queue_index >= 0 && queue_index < uring->num_queues );
    rocketnet_assert( error < 0 );
    struct io_uring_sqe * sqe = rocketnet_server_uring_get_sqe( uring );
    if ( !sqe )
        return;
    uring->test_receive_error[queue_index] = error;
    uring->queues[queue_index].cancel_pending = true;
    io_uring_prep_cancel64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, queue_index, 0 ), 0 );
    io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, queue_index, 0 ) );
    rocketnet_server_uring_submit( uring );
}

void rocketnet_server_uring_test_fail_sends( struct rocketnet_server_uring_t * uring, int count )
{
    rocketnet_assert( uring );
    uring->test_fail_sends = count > 0 ? count : 0;
}

#endif // #if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)

#if ROCKETNET_TEST_SEAMS
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
#endif // #if ROCKETNET_TEST_SEAMS

#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)

// the no-kernel-ring seam: a fake transport with queues but no ring. the test
// owns it through these calls only.

struct rocketnet_server_uring_t * rocketnet_server_uring_test_create_fake( int num_queues )
{
    if ( num_queues <= 0 )
        return NULL;
    struct rocketnet_server_uring_t * uring = new (std::nothrow) rocketnet_server_uring_t();
    if ( !uring )
        return NULL;
    uring->queues = new (std::nothrow) rocketnet_server_uring_queue_t[(size_t) num_queues]();
    if ( !uring->queues )
    {
        delete uring;
        return NULL;
    }
    uring->num_queues = num_queues;
    uring->test_no_ring = true;
    return uring;
}

void rocketnet_server_uring_test_destroy_fake( struct rocketnet_server_uring_t * uring )
{
    delete[] uring->queues;
    delete uring;
}

void rocketnet_server_uring_test_set_arm_failures( struct rocketnet_server_uring_t * uring, int count )
{
    uring->test_arm_find_no_sqe = count > 0 ? count : 0;
}

void rocketnet_server_uring_test_multishot_ends( struct rocketnet_server_uring_t * uring, int queue_index, bool arm_finds_no_sqe )
{
    uring->test_arm_find_no_sqe = arm_finds_no_sqe ? 1 : 0;
    rocketnet_server_uring_multishot_ended( uring, queue_index );
}

bool rocketnet_server_uring_test_queue_armed( struct rocketnet_server_uring_t * uring, int queue_index )
{
    return uring->queues[queue_index].armed;
}

uint64_t rocketnet_server_uring_test_counter( struct rocketnet_server_uring_t * uring, int counter )
{
    return uring->counters[counter];
}

int rocketnet_server_uring_test_wait_calls( rocketnet_server_uring_t * uring )
{
    return uring->test_wait_calls;
}

// the fake transport: create's allocations and arms, with the source in place
// of the ring and the sockets. each queue's buffer ring is plain memory the
// transport returns buffers to as it does to a kernel's, and every buffer holds
// the multishot recvmsg layout of one datagram from 127.0.0.1:1000

struct rocketnet_server_uring_t * rocketnet_server_uring_test_create( void * context, int num_queues, const struct rocketnet_server_uring_test_source_t * source )
{
    rocketnet_assert( source );

    if ( num_queues < 1 || num_queues > ROCKETNET_URING_MAX_QUEUES )
        return NULL;

    struct rocketnet_server_uring_t * uring = (struct rocketnet_server_uring_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_uring_t) );
    if ( !uring )
        return NULL;

    new ( uring ) rocketnet_server_uring_t();

    uring->context = context;
    uring->num_queues = num_queues;
    uring->ipv6 = false;
    uring->packet_id = 1;
    uring->test_source = source;

    uring->queues = (struct rocketnet_server_uring_queue_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_uring_queue_t) * (size_t) num_queues );
    if ( !uring->queues )
    {
        uring->~rocketnet_server_uring_t();
        rocketnet_free( context, uring );
        return NULL;
    }

    for ( int i = 0; i < num_queues; i++ )
    {
        new ( &uring->queues[i] ) rocketnet_server_uring_queue_t();
        uring->queues[i].fd = -1;
    }

    uring->ring_ready = true;

    struct io_uring_recvmsg_out out;
    memset( &out, 0, sizeof(out) );
    out.namelen = sizeof(struct sockaddr_in);
    out.payloadlen = ROCKETNET_URING_TEST_PAYLOAD_BYTES;

    struct sockaddr_in source_address;
    memset( &source_address, 0, sizeof(source_address) );
    source_address.sin_family = AF_INET;
    source_address.sin_port = htons( 1000 );
    source_address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];

        queue->buf_ring = (struct io_uring_buf_ring*) rocketnet_malloc( context, ROCKETNET_URING_RECV_BUFFERS * sizeof(struct io_uring_buf) );
        if ( !queue->buf_ring )
        {
            rocketnet_server_uring_abort_create( uring );
            return NULL;
        }
        io_uring_buf_ring_init( queue->buf_ring );

        const int mask = io_uring_buf_ring_mask( ROCKETNET_URING_RECV_BUFFERS );
        for ( int b = 0; b < ROCKETNET_URING_RECV_BUFFERS; b++ )
        {
            uint8_t * buffer = queue->buffers[b];
            memset( buffer, 0, ROCKETNET_URING_RECV_BUFFER_BYTES );
            memcpy( buffer, &out, sizeof(out) );
            memcpy( buffer + sizeof(out), &source_address, sizeof(source_address) );
            memset( buffer + sizeof(out) + sizeof(struct sockaddr_storage), (uint8_t) b, ROCKETNET_URING_TEST_PAYLOAD_BYTES );
            io_uring_buf_ring_add( queue->buf_ring, buffer, ROCKETNET_URING_RECV_BUFFER_BYTES, (unsigned short) b, mask, b );
        }
        io_uring_buf_ring_advance( queue->buf_ring, ROCKETNET_URING_RECV_BUFFERS );

        memset( &queue->receive_message, 0, sizeof(queue->receive_message) );
        queue->receive_message.msg_namelen = sizeof(struct sockaddr_storage);

        if ( !rocketnet_server_uring_arm( uring, i ) )
        {
            rocketnet_server_uring_abort_create( uring );
            return NULL;
        }
    }

    rocketnet_server_uring_submit( uring );

    return uring;
}

#endif // #if defined(ROCKETNET_URING_REAP_TEST_HOOKS)

struct rocketnet_server_uring_t * rocketnet_server_uring_create( void * context, struct rocketnet_address_t * bind_address, int num_queues )
{
    rocketnet_assert( bind_address );
    rocketnet_assert( bind_address->type == ROCKETNET_ADDRESS_IPV4 || bind_address->type == ROCKETNET_ADDRESS_IPV6 );

    if ( num_queues <= 0 )
        num_queues = ROCKETNET_URING_DEFAULT_QUEUES;

    // refuse, never clamp: a queue count past this transport's geometry is a
    // configuration error named here, not a silently different server

    if ( num_queues > ROCKETNET_URING_MAX_QUEUES )
    {
        rocketnet_error( "io_uring transport num_queues %d is out of range (1 to %d)", num_queues, ROCKETNET_URING_MAX_QUEUES );
        return NULL;
    }

    if ( !rocketnet_server_uring_available() )
    {
        rocketnet_error( "io_uring transport refused: this kernel does not accept the io_uring ring, opcodes or provided buffer ring the transport needs" );
        return NULL;
    }

    struct rocketnet_server_uring_t * uring = (struct rocketnet_server_uring_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_uring_t) );
    if ( !uring )
        return NULL;

    new ( uring ) rocketnet_server_uring_t();

    uring->context = context;
    uring->num_queues = num_queues;
    uring->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    uring->packet_id = 1;

    uring->queues = (struct rocketnet_server_uring_queue_t*) rocketnet_malloc( context, sizeof(struct rocketnet_server_uring_queue_t) * (size_t) num_queues );
    if ( !uring->queues )
    {
        uring->~rocketnet_server_uring_t();
        rocketnet_free( context, uring );
        return NULL;
    }

    for ( int i = 0; i < num_queues; i++ )
    {
        new ( &uring->queues[i] ) rocketnet_server_uring_queue_t();
        uring->queues[i].fd = -1;
    }

    // the reuseport group: the first socket may discover an ephemeral port,
    // the rest join it

    uint16_t bound_port = bind_address->port;

    for ( int i = 0; i < num_queues; i++ )
    {
        uring->queues[i].fd = rocketnet_server_uring_open_socket( bind_address, i, &bound_port );
        if ( uring->queues[i].fd < 0 )
        {
            rocketnet_server_uring_abort_create( uring );
            return NULL;
        }
    }

    bind_address->port = bound_port;
    uring->bind_address = *bind_address;

    // the ring: ROCKETNET_URING_ENTRIES submission entries, a completion
    // queue twice that

    struct io_uring_params params;
    memset( &params, 0, sizeof(params) );
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = ROCKETNET_URING_ENTRIES * 2;

    const int init_result = io_uring_queue_init_params( ROCKETNET_URING_ENTRIES, &uring->ring, &params );
    if ( init_result < 0 )
    {
        rocketnet_error( "io_uring transport: io_uring_queue_init_params failed: %s", strerror( -init_result ) );
        rocketnet_server_uring_abort_create( uring );
        return NULL;
    }
    uring->ring_ready = true;

    if ( ( params.features & IORING_FEAT_NODROP ) == 0 )
    {
        rocketnet_error( "io_uring transport: this kernel drops completions on overflow (no IORING_FEAT_NODROP)" );
        rocketnet_server_uring_abort_create( uring );
        return NULL;
    }

    // per queue: the provided buffer ring (bgid = queue), filled, and the
    // multishot RECVMSG armed over it

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];

        int error = 0;
        queue->buf_ring = rocketnet_server_uring_setup_buf_ring( &uring->ring, ROCKETNET_URING_RECV_BUFFERS, i, &queue->buf_ring_inverted_resv, &error );
        if ( !queue->buf_ring )
        {
            rocketnet_error( "io_uring transport: io_uring_setup_buf_ring failed for queue %d: %s", i, strerror( -error ) );
            rocketnet_server_uring_abort_create( uring );
            return NULL;
        }

        const int mask = io_uring_buf_ring_mask( ROCKETNET_URING_RECV_BUFFERS );
        for ( int b = 0; b < ROCKETNET_URING_RECV_BUFFERS; b++ )
            io_uring_buf_ring_add( queue->buf_ring, queue->buffers[b], ROCKETNET_URING_RECV_BUFFER_BYTES, (unsigned short) b, mask, b );
        io_uring_buf_ring_advance( queue->buf_ring, ROCKETNET_URING_RECV_BUFFERS );

        memset( &queue->receive_message, 0, sizeof(queue->receive_message) );
        queue->receive_message.msg_namelen = sizeof(struct sockaddr_storage);
        queue->receive_message.msg_controllen = 0;

        if ( !rocketnet_server_uring_arm( uring, i ) )
        {
            rocketnet_server_uring_abort_create( uring );
            return NULL;
        }
    }

    rocketnet_server_uring_submit( uring );

    rocketnet_info( "io_uring transport: %d queue reuseport group on port %d, %d entry ring, %d receive buffers per queue",
                    num_queues, bound_port, ROCKETNET_URING_ENTRIES, ROCKETNET_URING_RECV_BUFFERS );

    return uring;
}

// Stop retains every buffer the kernel may own. A check does one bounded
// cancellation/reap pass; a stalled completion keeps the object pending.
void rocketnet_server_uring_stop( rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    uring->destroying = true;
}

bool rocketnet_server_uring_stopped( rocketnet_server_uring_t * uring )
{
    if ( !uring || !uring->destroying ) return false;
    if ( !uring->ring_ready ) return true;
    for ( int i = 0; i < uring->num_queues; ++i )
    {
        rocketnet_server_uring_queue_t * queue = &uring->queues[i];
        if ( queue->armed && !queue->cancel_pending )
        {
            io_uring_sqe * sqe = rocketnet_server_uring_ring_get_sqe( uring );
            if ( !sqe ) break;
            io_uring_prep_cancel64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_RECV, i, 0 ), 0 );
            io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_CANCEL, i, 0 ) );
            queue->cancel_pending = true;
        }
    }
    rocketnet_server_uring_submit( uring );
    rocketnet_server_uring_reap_available( uring, ROCKETNET_URING_RECEIVE_BUDGET );
    if ( uring->test_stall_destroy || uring->sends_pending ||
         rocketnet_server_uring_ring_sq_ready( uring ) || rocketnet_server_uring_ring_cq_ready( uring ) ) return false;
    for ( int i = 0; i < uring->num_queues; ++i )
        if ( uring->queues[i].armed || uring->queues[i].cancel_pending ) return false;
    return true;
}

void rocketnet_server_uring_destroy( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    const bool stopped = rocketnet_server_uring_stopped( uring );
    rocketnet_assert( stopped );
    if ( !stopped ) return;
    if ( uring->ring_ready )
    {
        for ( int i = 0; i < uring->num_queues; i++ )
        {
            if ( uring->queues[i].buf_ring )
            {
                if ( ROCKETNET_URING_FAKE( uring ) )
                    rocketnet_free( uring->context, uring->queues[i].buf_ring );
                else
                    rocketnet_server_uring_free_buf_ring( &uring->ring, uring->queues[i].buf_ring, ROCKETNET_URING_RECV_BUFFERS, i,
                                                           uring->queues[i].buf_ring_inverted_resv );
                uring->queues[i].buf_ring = NULL;
            }
        }

        if ( !ROCKETNET_URING_FAKE( uring ) )
            io_uring_queue_exit( &uring->ring );
        uring->ring_ready = false;
    }

    if ( uring->queues )
    {
        for ( int i = 0; i < uring->num_queues; i++ )
        {
            if ( uring->queues[i].fd >= 0 )
            {
                close( uring->queues[i].fd );
                uring->queues[i].fd = -1;
            }
            uring->queues[i].~rocketnet_server_uring_queue_t();
        }
        rocketnet_free( uring->context, uring->queues );
        uring->queues = NULL;
    }

    void * allocation_context = uring->context;
    uring->~rocketnet_server_uring_t();
    rocketnet_clear_and_free( allocation_context, uring, sizeof(rocketnet_server_uring_t) );
}

int rocketnet_server_uring_num_queues( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    return uring->num_queues;
}

void rocketnet_server_uring_counters( struct rocketnet_server_uring_t * uring, uint64_t * counters )
{
    rocketnet_assert( uring );
    rocketnet_assert( counters );
    memcpy( counters, uring->counters, sizeof(uring->counters) );
}

// the builtin transport's receive_drops hook: the size
// drops (:636-638, :653-655), split at the receive ceiling; main thread, where the
// CQEs are reaped

void rocketnet_server_uring_receive_drops( struct rocketnet_server_uring_t * uring, uint64_t * too_short, uint64_t * too_large )
{
    rocketnet_assert( uring );
    rocketnet_assert( too_short );
    rocketnet_assert( too_large );
    *too_large = uring->receive_dropped_too_large;
    *too_short = uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE] - uring->receive_dropped_too_large;
}

// ---------------------------------------------------------------------------
// send
// ---------------------------------------------------------------------------

// RNUringDriver Reserve: one fetch_add takes the next slot; NULL when full

uint8_t * rocketnet_server_uring_start_packet( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to, uint64_t * packet_id )
{
    rocketnet_assert( uring );
    rocketnet_assert( to );
    rocketnet_assert( packet_id );

    *packet_id = uring->packet_id.fetch_add( 1, std::memory_order_relaxed );

    const int slot = uring->send.num_packets.fetch_add( 1, std::memory_order_relaxed );

    // an app that stages forever without flushing wraps the counter
    // negative, and a negative index is an out-of-bounds write, not a
    // dropped packet

    if ( slot >= ROCKETNET_URING_SEND_QUEUE_SIZE || slot < 0 )
    {
        rocketnet_spam( "io_uring transport send staging full. packet dropped" );
        return NULL;
    }

    uring->send.info[slot].to = *to;
    uring->send.info[slot].packet_bytes = 0;

    return uring->send.data[slot];
}

// the slot from pointer arithmetic against the staging: a pointer that is not
// a live slot root lands out of range and fails closed as a lost packet

static int rocketnet_server_uring_slot( struct rocketnet_server_uring_t * uring, const uint8_t * packet_data )
{
    const ptrdiff_t offset = packet_data - uring->send.data[0];
    const int slot = (int) ( offset / ROCKETNET_FRAME_STRIDE );
    if ( offset < 0 || slot < 0 || slot >= ROCKETNET_URING_SEND_QUEUE_SIZE || uring->send.data[slot] != packet_data )
        return -1;
    return slot;
}

// RNUringDriver Finish(s): the bytes are recorded

void rocketnet_server_uring_finish_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data, int packet_bytes )
{
    rocketnet_assert( uring );
    rocketnet_assert( packet_data );
    rocketnet_assert( packet_bytes > 0 );
    rocketnet_assert( packet_bytes <= ROCKETNET_RECEIVE_CEILING );

    (void) packet_id;

    const int slot = rocketnet_server_uring_slot( uring, packet_data );
    if ( slot < 0 )
        return;

    uring->send.info[slot].packet_bytes = packet_bytes;
}

// RNUringDriver Abort(s): the bytes are zeroed, so send_packets skips the slot

void rocketnet_server_uring_abort_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data )
{
    rocketnet_assert( uring );

    (void) packet_id;

    if ( !packet_data )
        return;

    const int slot = rocketnet_server_uring_slot( uring, packet_data );
    if ( slot < 0 )
        return;

    uring->send.info[slot].packet_bytes = 0;
}

// RNUringDriver Submit, Reap, SendReturn and ResetIndex: one SENDMSG per
// slot with bytes not submitted by an earlier call, in chunks no larger than
// the free submission queue space, rotated across the group's sockets; then
// reap, at most ROCKETNET_URING_SEND_BUDGET CQEs in the whole call and after
// no waits (receive CQEs met on the way go to the fill bank). The reservation index goes back to 0 only
// when no send is without its CQE and every staged slot is submitted
// (ResetIndex); otherwise the call returns with the index unchanged, counted
// SEND_DEFERRED, and a later call submits and retires the rest (SendReturn).
// The kernel owns a slot from its submit to its CQE, and no return frees or
// reuses one.

void rocketnet_server_uring_send_packets( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    int num_slots = uring->send.num_packets.load( std::memory_order_relaxed );
    if ( num_slots > ROCKETNET_URING_SEND_QUEUE_SIZE )
        num_slots = ROCKETNET_URING_SEND_QUEUE_SIZE;

    int slot = uring->send_submitted;
    int budget = ROCKETNET_URING_SEND_BUDGET;

    while ( slot < num_slots )
    {
        // bound the sends in flight by the ring size, so the completion
        // queue (twice the ring) keeps room for receive completions

        if ( uring->sends_pending >= ROCKETNET_URING_ENTRIES )
        {
            const int reaped = rocketnet_server_uring_reap_available( uring, budget );
            budget -= reaped;
            if ( reaped == 0 || budget == 0 ) break;
            continue;
        }

        unsigned space = rocketnet_server_uring_ring_sq_space( uring );
        if ( space == 0 )
        {
            rocketnet_server_uring_submit( uring );
            budget -= rocketnet_server_uring_reap_available( uring, budget );
            if ( rocketnet_server_uring_ring_sq_space( uring ) == 0 )
                break;                                  // the submit took nothing: a later call submits the rest
            continue;
        }

        const unsigned in_flight_room = (unsigned) ( ROCKETNET_URING_ENTRIES - uring->sends_pending );
        if ( space > in_flight_room )
            space = in_flight_room;

        const int queue_index = (int) ( uring->send_rotation++ % (uint64_t) uring->num_queues );
        const int fd = uring->queues[queue_index].fd;

        unsigned prepared = 0;

        while ( slot < num_slots && prepared < space )
        {
            struct rocketnet_server_uring_send_info_t * info = &uring->send.info[slot];

            if ( info->packet_bytes <= 0 )
            {
                slot++;                                 // aborted slot
                continue;
            }

            const socklen_t namelen = rocketnet_server_uring_address_to_sockaddr( &info->to, uring->ipv6, &info->name );
            if ( namelen == 0 )
            {
                // unroutable on this socket family: dropped counted
                uring->counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED]++;
                slot++;
                continue;
            }

            info->iovec.iov_base = uring->send.data[slot] + ROCKETNET_FRAME_WIRE_OFFSET;
            info->iovec.iov_len = (size_t) info->packet_bytes;
            memset( &info->message, 0, sizeof(info->message) );
            info->message.msg_name = &info->name;
            info->message.msg_namelen = namelen;
#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
            if ( uring->test_fail_sends > 0 )
            {
                uring->test_fail_sends--;
                info->message.msg_name = NULL;
                info->message.msg_namelen = 0;
            }
#endif
            info->message.msg_iov = &info->iovec;
            info->message.msg_iovlen = 1;

            struct io_uring_sqe * sqe = rocketnet_server_uring_ring_get_sqe( uring );
            rocketnet_assert( sqe );
            io_uring_prep_sendmsg( sqe, fd, &info->message, MSG_DONTWAIT );
            io_uring_sqe_set_data64( sqe, rocketnet_uring_tag( ROCKETNET_URING_TAG_SEND, queue_index, (uint32_t) slot ) );

            uring->sends_pending++;
            prepared++;
            slot++;
        }

        rocketnet_server_uring_submit( uring );
        budget -= rocketnet_server_uring_reap_available( uring, budget );
    }

    uring->send_submitted = slot;

    // Reap: the sends still without a CQE, this call's and earlier calls',
    // within what is left of the budget, without waiting for another CQE

    if ( uring->sends_pending > 0 && budget > 0 )
    {
        budget -= rocketnet_server_uring_reap_available( uring, budget );
    }

    if ( budget == 0 && rocketnet_server_uring_ring_cq_ready( uring ) > 0 )
        uring->counters[ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED]++;

    // re-arms queued while reaping

    rocketnet_server_uring_submit( uring );

    if ( uring->sends_pending == 0 && slot >= num_slots )
    {
        // ResetIndex: no slot is kernel owned now

        uring->send_submitted = 0;
        uring->send.num_packets.store( 0, std::memory_order_relaxed );
    }
    else
    {
        // SendReturn: the kernel still owns some slots, or some staged slots
        // wait to be submitted; the index stays, so no start_packet returns
        // a slot the kernel owns

        uring->counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DEFERRED]++;
    }
}

// ---------------------------------------------------------------------------
// receive
// ---------------------------------------------------------------------------

// reap at most ROCKETNET_URING_RECEIVE_BUDGET CQEs without blocking into the
// fill bank (Reap within the Budget; CQEs past it stay in the completion
// queue, counted REAP_DEFERRED), submit any re-arm, then RNUringDriver Swap:
// the fill bank becomes the exposed bank the transport reads until the next
// call, and the other bank is emptied as the new fill bank

// re-arm every queue whose multishot is disarmed and not destroying, so a
// re-arm that found no submission entry at the end of a multishot does not
// silence the queue for good; each failed arm is counted

static void rocketnet_server_uring_rearm_unarmed( struct rocketnet_server_uring_t * uring )
{
    for ( int i = 0; i < uring->num_queues; i++ )
    {
        struct rocketnet_server_uring_queue_t * queue = &uring->queues[i];
        if ( !queue->armed && !uring->destroying )
        {
            if ( rocketnet_server_uring_arm( uring, i ) )
                uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS]++;
            else
                uring->counters[ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARM_FAILED]++;
        }
    }
}

void rocketnet_server_uring_receive_packets( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
    if ( uring->test_no_ring )
    {
        rocketnet_server_uring_rearm_unarmed( uring );
        return;
    }
#endif

    if ( rocketnet_server_uring_reap_available( uring, ROCKETNET_URING_RECEIVE_BUDGET ) == ROCKETNET_URING_RECEIVE_BUDGET &&
         rocketnet_server_uring_ring_cq_ready( uring ) > 0 )
    {
        uring->counters[ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED]++;
    }

    rocketnet_server_uring_rearm_unarmed( uring );

    rocketnet_server_uring_submit( uring );

    const int exposed_bank = uring->fill_bank;
    uring->fill_bank = 1 - exposed_bank;
    uring->banks[uring->fill_bank].num_packets = 0;

    struct rocketnet_server_uring_bank_t * exposed = &uring->banks[exposed_bank];

    uring->process_packets.num_packets = exposed->num_packets;
    for ( int i = 0; i < exposed->num_packets; i++ )
    {
        uring->process_packets.from[i] = exposed->from[i];
        uring->process_packets.packet_data[i] = exposed->packet_data[i];
        uring->process_packets.packet_bytes[i] = exposed->packet_bytes[i];
    }
}

// RNUringDriver TransportReads: the transport reads the exposed bank, which
// stays put until the next receive_packets swaps

struct rocketnet_server_socket_process_packets_t * rocketnet_server_uring_process_packets( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    return &uring->process_packets;
}

#else // #if ROCKETNET_URING_BUILT

// built without liburing (or on a platform without io_uring): the module
// keeps its entry points so a caller links, and create refuses with a log
// line naming why

struct rocketnet_server_uring_t
{
    int unused;
};

bool rocketnet_server_uring_available()
{
    return false;
}

uint64_t rocketnet_server_uring_allocation_bytes( int queues )
{
    (void) queues;
    return 0;
}

struct rocketnet_server_uring_t * rocketnet_server_uring_create( void * context, struct rocketnet_address_t * bind_address, int num_queues )
{
    (void) context;
    (void) bind_address;
    (void) num_queues;
    rocketnet_error( "io_uring transport refused: built without liburing" );
    return NULL;
}

void rocketnet_server_uring_stop( struct rocketnet_server_uring_t * ) {}
bool rocketnet_server_uring_stopped( struct rocketnet_server_uring_t * ) { return true; }

void rocketnet_server_uring_destroy( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
}

uint64_t rocketnet_server_uring_leaked_bytes()
{
    return 0;
}

uint64_t rocketnet_server_uring_leaked_transports()
{
    return 0;
}

#if ROCKETNET_TEST_SEAMS
void rocketnet_server_uring_test_resume_stop( struct rocketnet_server_uring_t * ) {}

void rocketnet_server_uring_test_stall_destroy( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
}
#endif // #if ROCKETNET_TEST_SEAMS

int rocketnet_server_uring_num_queues( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return 0;
}

void rocketnet_server_uring_counters( struct rocketnet_server_uring_t * uring, uint64_t * counters )
{
    (void) uring;
    if ( counters )
        memset( counters, 0, ROCKETNET_SERVER_URING_NUM_COUNTERS * sizeof(uint64_t) );
}

void rocketnet_server_uring_receive_drops( struct rocketnet_server_uring_t * uring, uint64_t * too_short, uint64_t * too_large )
{
    (void) uring;
    *too_short = 0;
    *too_large = 0;
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

void rocketnet_server_uring_receive_packets( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
}

struct rocketnet_server_socket_process_packets_t * rocketnet_server_uring_process_packets( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return NULL;
}

#if defined(ROCKETNET_SEND_ERROR_TEST_HOOKS)
void rocketnet_server_uring_test_fail_receive( struct rocketnet_server_uring_t *, int, int ) {}
void rocketnet_server_uring_test_fail_sends( struct rocketnet_server_uring_t *, int ) {}
#endif

#if defined(ROCKETNET_URING_REAP_TEST_HOOKS)
struct rocketnet_server_uring_t * rocketnet_server_uring_test_create( void *, int, const struct rocketnet_server_uring_test_source_t * )
{
    return NULL;
}
#endif

#endif // #if ROCKETNET_URING_BUILT

int rocketnet_server_uring_linux_cpp_dummy = 0;
