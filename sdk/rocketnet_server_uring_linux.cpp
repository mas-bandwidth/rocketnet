/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_memory.h"
#include "rocketnet_test_seams.h"
#include "rocketnet_internal.h"
#include "rocketnet_server_uring.h"
#include "rocketnet_uring_pin.h"
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
#include "rocketnet_server_udp_linux.h"
#include "rocketnet_send_queue.h"
#include "rocketnet_socket_buffer.h"
#include "rocketnet_uring_queue_send.h"
#include "rocketnet_uring_queue_worker.h"
#include "rocketnet_uring_receive_queue.h"
#include "rocketnet_uring_counters.h"

#include <pthread.h>
#include <sched.h>
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
// bounds (64 segments, the library's own cap: the kernel's UDP_MAX_SEGMENTS
// is larger, 128 in Linux 6.12; and the 16-bit UDP payload)

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

    // the cpu the worker pinned itself to (rocketnet_uring_pin.h): PENDING
    // until its thread has run the pin, then the cpu or NONE

    std::atomic<int> pin_cpu{ ROCKETNET_URING_PIN_PENDING };

    // the lowest cpu of the worker's mask as the system reports it after the
    // pin or the confine (rocketnet_server_uring_worker_lowest_cpu); stored
    // before pin_cpu, so a reader that saw pin_cpu leave PENDING sees it

    std::atomic<int> lowest_cpu{ ROCKETNET_URING_PIN_PENDING };

    // its pin refused and the confine off cpu 0 failed too: the worker runs
    // on a mask that holds cpu 0 and asked for no real-time scheduling
    // (rocketnet_server_uring_pin_worker); stored before pin_cpu

    std::atomic<bool> left_on_core_zero{ false };

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
    int num_os_cpus = 1;                        // the server's num_os_cpus, never below 1: the workers pin to num_os_cpus + queue
    bool reserve_cpu_0 = true;                  // false (ROCKETNET_SERVER_URING_CPU_0_NOT_RESERVED): worker q pins to cpu q, num_os_cpus not applied
    std::atomic<int> pin_refusals{ 0 };         // pins and priorities the workers asked for and the system refused
    std::atomic<uint64_t> send_finish_refused{ 0 }; // finish_packet calls whose pointer named no slot of the producer bank: each one packet lost, counted into SEND_DROPPED
    bool ipv6 = false;
    int features = 0;                           // ROCKETNET_SERVER_URING_FEATURE_*: what the create probe found
    int max_clients = 0;
    int bank_size = 0;                          // each queue's receive bank and the process view
    int send_slots = 0;                         // each queue's send bank (rocketnet_server_uring_send_slots)
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

// ---------------------------------------------------------------------------

// the probe's answer: -1 not yet probed, 0 no, 1 yes
static std::atomic<int> rocketnet_server_uring_probed{ -1 };

#if ROCKETNET_DEV
// rocketnet_server_uring_test_probe_multishot: the terminating result the
// probe's multishot receive is given in place of the kernel's, 0 for the kernel's
static std::atomic<int> rocketnet_server_uring_test_multishot_res{ 0 };
#endif // #if ROCKETNET_DEV

#define ROCKETNET_URING_PROBE_RECV                                           1
#define ROCKETNET_URING_PROBE_CANCEL                                         2

// a multishot RECVMSG over buffer group bgid on an unbound UDP socket, armed
// as rocketnet_uring_receive_queue_arm arms it, then cancelled: a kernel that
// takes it ends it with -ENOBUFS (the probe's buffer ring holds no buffer;
// Linux 7.0 answers so) or -ECANCELED (the cancel first). One without
// multishot receive (Linux 5.19 has provided buffer rings, multishot receive
// came in 6.0) refuses it at prep: submission stops at that entry, so the
// submit does not take both, and were it to, the receive would end -EINVAL;
// either is a no. Nothing can arrive on the unbound socket, and the cancel
// ends the receive either way, so the wait is for two completions; a wait
// for either that passes a second is a refusal too (each completion has its
// own second, so the probe can take up to two)
static bool rocketnet_server_uring_probe_multishot( struct io_uring * ring, int bgid )
{
    const int fd = socket( AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0 );
    if ( fd < 0 )
    {
        return false;
    }

    struct msghdr message;
    memset( &message, 0, sizeof( message ) );
    bool ok = false;
    int receive_res = 0;
    bool receive_ended = false;
    int completions = 0;

    struct io_uring_sqe * sqe = io_uring_get_sqe( ring );
    if ( sqe )
    {
        memset( sqe, 0, sizeof( *sqe ) );
        sqe->opcode = IORING_OP_RECVMSG;
        sqe->fd = fd;
        sqe->addr = (uint64_t) (uintptr_t) &message;
        sqe->len = 1;
        sqe->ioprio = IORING_RECV_MULTISHOT;
        sqe->flags = IOSQE_BUFFER_SELECT;
        sqe->buf_group = (uint16_t) bgid;
        sqe->user_data = ROCKETNET_URING_PROBE_RECV;
        struct io_uring_sqe * cancel = io_uring_get_sqe( ring );
        if ( cancel )
        {
            io_uring_prep_cancel64( cancel, ROCKETNET_URING_PROBE_RECV, 0 );
            cancel->user_data = ROCKETNET_URING_PROBE_CANCEL;
            ok = io_uring_submit( ring ) == 2;
        }
    }

    // the receive's last completion (no IORING_CQE_F_MORE) and the cancel's;
    // a datagram's completion before them cannot come, and is passed over if it does
    while ( ok && completions < 2 )
    {
        struct __kernel_timespec timeout;
        timeout.tv_sec = 1;
        timeout.tv_nsec = 0;
        struct io_uring_cqe * cqe = NULL;
        if ( io_uring_wait_cqe_timeout( ring, &cqe, &timeout ) != 0 || !cqe )
        {
            ok = false;
            break;
        }
        if ( cqe->user_data == ROCKETNET_URING_PROBE_CANCEL )
        {
            completions++;
        }
        else if ( cqe->user_data == ROCKETNET_URING_PROBE_RECV && !( cqe->flags & IORING_CQE_F_MORE ) )
        {
            receive_res = cqe->res;
            receive_ended = true;
            completions++;
        }
        io_uring_cqe_seen( ring, cqe );
    }

#if ROCKETNET_DEV
    const int test_res = rocketnet_server_uring_test_multishot_res.load( std::memory_order_relaxed );
    if ( test_res != 0 )
    {
        receive_res = test_res;
    }
#endif // #if ROCKETNET_DEV

    close( fd );
    return ok && receive_ended && ( receive_res == -ENOBUFS || receive_res == -ECANCELED );
}

bool rocketnet_server_uring_available()
{
    // the probe: a ring, the five opcodes the workers submit (SENDMSG,
    // RECVMSG, ASYNC_CANCEL, the wake's READ and the retry TIMEOUT), a
    // provided buffer ring and a multishot receive over it.

    std::atomic<int> & available = rocketnet_server_uring_probed;

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
                 io_uring_opcode_supported( probe, IORING_OP_TIMEOUT );

            io_uring_free_probe( probe );
        }

        if ( ok )
        {
            int error = 0;
            bool inverted_resv = false;
            struct io_uring_buf_ring * buf_ring = rocketnet_uring_receive_buf_ring_setup( &ring, 1, 0, &inverted_resv, &error );
            if ( buf_ring )
            {
                ok = rocketnet_server_uring_probe_multishot( &ring, 0 );
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

// the worker pins itself the way the XDP transport's thread does
// (rocketnet_server_socket_xdp_thread_function): affinity to cpu num_os_cpus +
// queue (cpu queue when cpu 0 is not reserved), then, when that pin was
// granted, SCHED_RR at the minimum unless the priority switch is off. A
// refusal is a warning naming the queue and the cpu, counted in pin_refusals
// (rocketnet_server_uring_pin_refusals; a refused priority is counted in the
// platform's priority refusals instead), and the queue's pin_cpu stays NONE: a worker is reported pinned
// only when pthread_setaffinity_np said so. num_os_cpus below 1 reads as 1 (the
// transport driven directly).

// a refused pin leaves the worker on the mask it inherited, which may include
// cpu 0, the OS's core: confine it to the cpus of its own mask past the OS's,
// or to all of them but cpu 0 (rocketnet_uring_pin_confine_words); the pin
// state stays NONE either way

#define ROCKETNET_URING_PIN_WORDS ( CPU_SETSIZE / 64 )

// the affinity and priority calls a worker makes on its own thread. The
// worker passes the real ones; the development seam
// (rocketnet_server_uring_test_place_worker) passes fakes, so the refusal
// paths run without changing any thread's affinity or priority

struct rocketnet_uring_affinity_calls_t
{
    int ( *get )( cpu_set_t * mask );
    int ( *set )( const cpu_set_t * mask );
    int ( *set_round_robin )( int priority );   // SCHED_RR on the calling thread
};

static int rocketnet_uring_affinity_get_real( cpu_set_t * mask )
{
    return pthread_getaffinity_np( pthread_self(), sizeof( cpu_set_t ), mask );
}

static int rocketnet_uring_affinity_set_real( const cpu_set_t * mask )
{
    return pthread_setaffinity_np( pthread_self(), sizeof( cpu_set_t ), mask );
}

static int rocketnet_uring_set_round_robin_real( int priority )
{
    struct sched_param param;
    param.sched_priority = priority;
    return pthread_setschedparam( pthread_self(), SCHED_RR, &param );
}

static const struct rocketnet_uring_affinity_calls_t rocketnet_uring_affinity_real = { rocketnet_uring_affinity_get_real, rocketnet_uring_affinity_set_real,
                                                                                       rocketnet_uring_set_round_robin_real };

// one warning for a worker whose confinement failed, naming the queue and the
// mask it stays on (NULL: the mask could not be read). true when that mask
// holds cpu 0 or cannot be read: the worker is left on cpu 0
static bool rocketnet_server_uring_warn_not_confined( int queue_index, const cpu_set_t * mask, const char * why )
{
    if ( !mask )
    {
        rocketnet_warn( "io_uring worker %d left on cpus unknown, cpu 0 may be among them: %s; it asks for no real-time scheduling", queue_index, why );
        return true;
    }
    char cpus[128] = { 0 };
    unsigned char in_mask[CPU_SETSIZE] = { 0 };
    for ( int cpu = 0; cpu < CPU_SETSIZE; cpu++ )
    {
        in_mask[cpu] = CPU_ISSET( (size_t) cpu, mask ) ? 1 : 0;
    }
    rocketnet_platform_cpu_list_text( in_mask, CPU_SETSIZE, cpus, (int) sizeof( cpus ) );
    if ( !in_mask[0] )
    {
        rocketnet_warn( "io_uring worker %d not confined: %s; it stays on cpus %s, which do not hold cpu 0", queue_index, why, cpus );
        return false;
    }
    rocketnet_warn( "io_uring worker %d left on cpus %s, cpu 0 among them: %s; it asks for no real-time scheduling", queue_index, cpus, why );
    return true;
}

// true when the worker is kept off cpu 0; a failed confinement is named in one
// warning with the mask the worker stays on, and is false when that mask holds
// cpu 0 or cannot be read (the refused pin is already counted: the row counts
// pins, so this is not counted again)

static bool rocketnet_server_uring_confine_worker( const struct rocketnet_server_uring_t * uring, int queue_index, int online,
                                                   const struct rocketnet_uring_affinity_calls_t * calls )
{
    // the mask the worker has now (only cpus the system lets it use); when it
    // cannot be read, every online cpu
    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    uint64_t current[ROCKETNET_URING_PIN_WORDS] = {};
    const bool mask_read = calls->get( &cpuset ) == 0;
    if ( mask_read )
    {
        for ( int cpu = 0; cpu < CPU_SETSIZE; cpu++ )
        {
            if ( CPU_ISSET( (size_t) cpu, &cpuset ) )
            {
                current[cpu / 64] |= (uint64_t) 1 << ( cpu % 64 );
            }
        }
    }
    else
    {
        for ( int cpu = 0; cpu < online && cpu < CPU_SETSIZE; cpu++ )
        {
            current[cpu / 64] |= (uint64_t) 1 << ( cpu % 64 );
        }
    }

    uint64_t confined[ROCKETNET_URING_PIN_WORDS] = {};
    if ( !rocketnet_uring_pin_confine_words( current, ROCKETNET_URING_PIN_WORDS, uring->num_os_cpus, confined ) )
    {
        return !rocketnet_server_uring_warn_not_confined( queue_index, mask_read ? &cpuset : NULL, "no cpu past the OS's to confine it to" );
    }

    const cpu_set_t kept = cpuset;
    CPU_ZERO( &cpuset );
    for ( int cpu = 0; cpu < CPU_SETSIZE; cpu++ )
    {
        if ( confined[cpu / 64] & ( (uint64_t) 1 << ( cpu % 64 ) ) )
        {
            CPU_SET( (size_t) cpu, &cpuset );
        }
    }

    // a subset of the cpus it may use: refused only when one went offline in
    // between; named, the refusal of the pin itself is already counted
    const int result = calls->set( &cpuset );
    if ( result != 0 )
    {
        char why[192];
        (void) snprintf( why, sizeof( why ), "confining it past the OS's cpus was refused (%s)", rocketnet_platform_socket_error_text( result ).text );
        return !rocketnet_server_uring_warn_not_confined( queue_index, mask_read ? &kept : NULL, why );
    }
    return true;
}

// what the worker may run on now, read back from the system: the lowest cpu
// of its mask, or NONE when the mask cannot be read
static void rocketnet_server_uring_note_mask( struct rocketnet_server_uring_queue_t * queue, const struct rocketnet_uring_affinity_calls_t * calls )
{
    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    int lowest = ROCKETNET_URING_PIN_NONE;
    if ( calls->get( &cpuset ) == 0 )
    {
        for ( int cpu = 0; cpu < CPU_SETSIZE; cpu++ )
        {
            if ( CPU_ISSET( (size_t) cpu, &cpuset ) )
            {
                lowest = cpu;
                break;
            }
        }
    }
    queue->lowest_cpu.store( lowest, std::memory_order_relaxed );
}

// after a refused pin (already counted and warned): with cpu 0 reserved the
// worker is confined past the OS's cpus (rocketnet_server_uring_confine_worker)
// and flagged when that leaves it on cpu 0; not reserved it stays on the mask
// it inherited, unflagged. Either way the pin state stays NONE

static void rocketnet_server_uring_pin_refused( struct rocketnet_server_uring_queue_t * queue, int online, const struct rocketnet_uring_affinity_calls_t * calls )
{
    struct rocketnet_server_uring_t * uring = queue->uring;
    bool left_on_core_zero = false;
    if ( uring->reserve_cpu_0 )
    {
        left_on_core_zero = !rocketnet_server_uring_confine_worker( uring, queue->index, online, calls );
    }
    queue->left_on_core_zero.store( left_on_core_zero, std::memory_order_relaxed );
    rocketnet_server_uring_note_mask( queue, calls );
    queue->pin_cpu.store( ROCKETNET_URING_PIN_NONE, std::memory_order_release );
}

// the pin (rocketnet_network_thread_cpu: cpu num_os_cpus + queue, or cpu
// queue when cpu 0 is not reserved), then SCHED_RR only when that pin was
// granted: a worker whose pin was refused shares its cpus with other threads
// (the game's, or the OS's on cpu 0) and asks for no real-time scheduling; it
// carries on (a one cpu host still runs)

static void rocketnet_server_uring_pin_worker( struct rocketnet_server_uring_queue_t * queue, int online, const struct rocketnet_uring_affinity_calls_t * calls )
{
    struct rocketnet_server_uring_t * uring = queue->uring;
    const int cpu = rocketnet_network_thread_cpu( queue->index, uring->num_os_cpus, uring->reserve_cpu_0 );
    const char * fallback = uring->reserve_cpu_0 ? "it falls back to the cpus past the OS's" : "it stays on the cpus it inherited";
    bool pinned = false;
    const int check = rocketnet_uring_pin_check( cpu, CPU_SETSIZE, online );
    if ( check != ROCKETNET_URING_PIN_OK )
    {
        uring->pin_refusals.fetch_add( 1, std::memory_order_relaxed );
        if ( check == ROCKETNET_URING_PIN_OUTSIDE_SET )
        {
            rocketnet_warn( "io_uring worker %d not pinned: cpu %d is outside 0 .. %d; %s and asks for no real-time scheduling", queue->index, cpu, CPU_SETSIZE - 1, fallback );
        }
        else
        {
            rocketnet_warn( "io_uring worker %d not pinned: cpu %d is not online (0 .. %d); %s and asks for no real-time scheduling", queue->index, cpu, online - 1, fallback );
        }
        rocketnet_server_uring_pin_refused( queue, online, calls );
    }
    else
    {
        cpu_set_t cpuset;
        CPU_ZERO( &cpuset );
        CPU_SET( (size_t) cpu, &cpuset );
        const int result = calls->set( &cpuset );
        if ( result != 0 )
        {
            uring->pin_refusals.fetch_add( 1, std::memory_order_relaxed );
            rocketnet_warn( "io_uring worker %d not pinned: pthread_setaffinity_np cpu %d refused (%s); %s and asks for no real-time scheduling", queue->index, cpu,
                            rocketnet_platform_socket_error_text( result ).text, fallback );
            rocketnet_server_uring_pin_refused( queue, online, calls );
        }
        else
        {
            rocketnet_info( "io_uring worker %d pinned to cpu %d", queue->index, cpu );
            queue->left_on_core_zero.store( false, std::memory_order_relaxed );
            rocketnet_server_uring_note_mask( queue, calls );
            queue->pin_cpu.store( cpu, std::memory_order_release );
            pinned = true;
        }
    }

    if ( pinned && rocketnet_platform_thread_priority_enabled() )
    {
        struct sched_param param;
        param.sched_priority = sched_get_priority_min( SCHED_RR );
        const int result = calls->set_round_robin( param.sched_priority );
        if ( result != 0 )
        {
            char call[64];
            (void) snprintf( call, sizeof( call ), "pthread_setschedparam SCHED_RR %d", param.sched_priority );
            rocketnet_warn( "io_uring worker %d priority not set: %s refused (%s); it runs at its default priority", queue->index, call, rocketnet_platform_socket_error_text( result ).text );
            rocketnet_platform_thread_priority_refused( ROCKETNET_PLATFORM_PRIORITY_CALL_SET_SCHED, "thread priority", call, rocketnet_platform_socket_error_text( result ).text, "the thread runs at its default priority" );
        }
    }
}

// the queue's worker thread: the loop until it has stopped and nothing of
// its is in the kernel, then the exited flag (its last access to the queue)

static void rocketnet_server_uring_worker_thread( struct rocketnet_platform_thread_t *, void * data )
{
    struct rocketnet_server_uring_queue_t * queue = (struct rocketnet_server_uring_queue_t *) data;
    rocketnet_assert( queue );
    rocketnet_server_uring_pin_worker( queue, (int) sysconf( _SC_NPROCESSORS_ONLN ), &rocketnet_uring_affinity_real );
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
    const uint64_t banks = rocketnet_uring_send_queues_allocation_bytes( queues, rocketnet_server_uring_send_slots( max_clients, queues ) );
    const uint64_t send = rocketnet_uring_queue_send_allocation_bytes( queues, rocketnet_server_uring_send_slots( max_clients, queues ) );
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

int rocketnet_server_uring_send_queue_size( const struct rocketnet_server_uring_t * uring )
{
    return uring ? uring->send_slots : 0;
}

// each queue's provided buffer ring, mapped outside the allocator, whole
// pages, and an upper bound of the queue's submission and completion ring
// mappings (rocketnet_server_uring_ring_mapped_bound)

uint64_t rocketnet_server_uring_mapped_bytes( int queues )
{
    if ( queues < 1 || queues > ROCKETNET_URING_MAX_QUEUES )
    {
        return 0;
    }
    return (uint64_t) queues * ( rocketnet_uring_receive_queue_mapped_bytes( ROCKETNET_URING_RECV_BUFFERS ) + rocketnet_server_uring_ring_mapped_bound( ROCKETNET_URING_ENTRIES ) );
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
        rocketnet_error( "io_uring transport: queue %d: io_uring_queue_init_params failed: %s", queue->index, rocketnet_platform_socket_error_text( -result ).text );
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
    return rocketnet_server_uring_create_pinned( context, bind_address, num_queues, max_clients, flags, 0, status, packet_filter );
}

struct rocketnet_server_uring_t * rocketnet_server_uring_create_pinned( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int num_os_cpus,
                                                                        int * status, const uint8_t * packet_filter )
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
        rocketnet_error( "io_uring transport refused: this kernel does not accept the io_uring ring, opcodes, provided buffer ring or multishot receive (Linux 6.0) the transport needs" );
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
    uring->num_os_cpus = num_os_cpus < 1 ? 1 : num_os_cpus; // cpu 0 is the OS's whatever the caller says, unless the flag below
    uring->reserve_cpu_0 = ( flags & ROCKETNET_SERVER_URING_CPU_0_NOT_RESERVED ) == 0;
    uring->ipv6 = bind_address->type == ROCKETNET_ADDRESS_IPV6;
    uring->max_clients = max_clients;
    uring->bank_size = rocketnet_server_uring_bank_packets( max_clients );
    uring->send_slots = rocketnet_server_uring_send_slots( max_clients, num_queues );

    // the send modules refuse only arguments outside their ranges (the queue
    // count is checked above against ROCKETNET_URING_MAX_QUEUES, which
    // ROCKETNET_URING_SEND_MAX_QUEUES holds, and send_slots is clamped to
    // 1 .. ROCKETNET_URING_SEND_MAX_BANK_SLOTS), so a refusal below is the
    // allocation or its layout overflowing: OUT_OF_MEMORY is the real status
    rocketnet_assert( uring->send_slots >= 1 && uring->send_slots <= ROCKETNET_URING_SEND_MAX_BANK_SLOTS );

    uring->queues = (struct rocketnet_server_uring_queue_t *) rocketnet_malloc( context, sizeof( struct rocketnet_server_uring_queue_t ) * (size_t) num_queues );
    uring->banks = rocketnet_uring_send_queues_create( context, num_queues, uring->send_slots );
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
        uring->queues[i].fd = rocketnet_server_udp_open( "io_uring", bind_address, i, max_clients, &bound_port, status );
        if ( uring->queues[i].fd < 0 )
        {
            rocketnet_server_uring_free( uring );
            return NULL;
        }
    }

    bind_address->port = bound_port;

    // the io_uring transport's own buffer step after the shared open: when the
    // open's grant is short, ask once more past the sysctl limit with the
    // force option (rocketnet_socket_buffer_request) and name a short grant
    // at start with the sysctl to set
    // (rocketnet_socket_buffer_log). a short grant is a fact about the host,
    // not an error

    const int send_buffer_size = ROCKETNET_SOCKET_SEND_BUFFER_SIZE;
    const int receive_buffer_size = rocketnet_server_transport_receive_buffer_request( "io_uring", max_clients );

    for ( int i = 0; i < num_queues; i++ )
    {
        struct rocketnet_socket_buffer_grant_t send_grant;
        struct rocketnet_socket_buffer_grant_t receive_grant;
        if ( !rocketnet_socket_buffer_request( uring->queues[i].fd, false, send_buffer_size, &send_grant ) ||
             !rocketnet_socket_buffer_request( uring->queues[i].fd, true, receive_buffer_size, &receive_grant ) )
        {
            rocketnet_error( "io_uring transport: failed to set socket buffer size on queue %d: %s", i, rocketnet_platform_socket_error_text( errno ).text );
            rocketnet_server_uring_free( uring );
            return NULL;
        }

        if ( i == 0 )
        {
            rocketnet_socket_buffer_log( "io_uring", false, send_buffer_size, &send_grant );
            rocketnet_socket_buffer_log( "io_uring", true, receive_buffer_size, &receive_grant );
        }
    }

    // the UDP_SEGMENT and UDP_GRO probes, the gso transport's, each all or
    // nothing over the group (rocketnet_server_udp_probe_offloads). with
    // UDP_GRO the multishot reserves a control area for the UDP_GRO cmsg and
    // one buffer may hold a coalesced receive of several datagrams. failure
    // is a tier, not an error

    int fds[ROCKETNET_URING_MAX_QUEUES] = {};
    for ( int i = 0; i < num_queues; i++ )
    {
        fds[i] = uring->queues[i].fd;
    }

    bool segment_tier = false;
    bool gro_tier = false;
    rocketnet_server_udp_probe_offloads( fds, num_queues, ( flags & ROCKETNET_SERVER_URING_DISABLE_SEGMENT ) == 0,
                                         ( flags & ROCKETNET_SERVER_URING_DISABLE_GRO ) == 0, setsockopt, &segment_tier, &gro_tier );
    if ( segment_tier )
    {
        uring->features |= ROCKETNET_SERVER_URING_FEATURE_SEGMENT;
    }
    if ( gro_tier )
    {
        uring->features |= ROCKETNET_SERVER_URING_FEATURE_GRO;
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
            rocketnet_error( "io_uring transport: queue %d: eventfd failed: %s", i, rocketnet_platform_socket_error_text( errno ).text );
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
                    uring->send_slots, segment ? "yes" : "no (one SENDMSG per packet)", gro ? "yes" : "no (one datagram per receive buffer)",
                    socket_drop_queues, num_queues, packet_filter ? "in the workers" : "off (raw datagrams)" );

    return uring;
}

// proofs/tla/RNUringQueueWorker.tla BeginStop, for every queue: shutdown,
// then the wake. The builtin transport's begin_shutdown hook: main thread,
// once, when the server starts stopping and before its last disconnect
// batches. The server asks the stop only once send_idle is true, so from here
// a socket that stays full gets its queue worker's bounded retries and the
// worker then abandons what waits (counted) instead of holding the bank
// forever (ShutdownDrains).

void rocketnet_server_uring_begin_shutdown( rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    for ( int i = 0; i < uring->num_queues; i++ )
    {
        if ( uring->queues[i].worker_ready )
        {
            rocketnet_uring_queue_worker_begin_shutdown( &uring->queues[i].worker );
        }
    }
}

// no bank is published or kept after the workers exit: the server stops this
// transport only after its final flush and send_idle (the driver's stop
// follows begin_shutdown, sdk/rocketnet_driver.h), starts no packet after the
// stop, and rocketnet_server_uring_send_packets runs on main only in an
// update, never after stop. What the workers still hold at the stop they
// send within bounded retries or count abandoned (SEND_DROPPED).
//
// proofs/tla/RNUringQueueWorker.tla Stop, for every queue: shutdown and quit,
// then the wake. Each worker keeps sending what was published, cancels its
// receive and exits once nothing of its is in the kernel.

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

    memset( counters + ROCKETNET_SERVER_URING_FIRST_COUNTER, 0, ROCKETNET_SERVER_URING_COUNTER_ROWS * sizeof( uint64_t ) );
    counters[ROCKETNET_COUNTER_URING_PIN_REFUSALS] = (uint64_t) uring->pin_refusals.load( std::memory_order_relaxed );
    counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SOCKET] = 0;

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

        rocketnet_uring_counters_add_queue( counters, receive, worker, sent, dropped, abandoned );

        // a run the socket family cannot name is dropped by the worker
        // without a send: rocketnet_uring_queue_send_complete( FAILED )
        // already counted its packets in the send module's DROPPED, so the
        // worker's own SEND_DROPPED is not added again
    }

    rocketnet_uring_counters_add_finish_refused( counters, uring->send_finish_refused.load( std::memory_order_relaxed ) );
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

// any game thread: the destination's queue (the XDP hash), one claim of its
// producer bank (proofs/tla/RNUringQueueBankLayout.tla Start); NULL when that
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
    if ( !rocketnet_uring_send_queues_finish( uring->banks, packet_id, packet_data, packet_bytes ) )
    {
        uring->send_finish_refused.fetch_add( 1, std::memory_order_relaxed );
    }
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

// main, the game threads joined: each queue's producer published to its worker
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

int rocketnet_server_uring_worker_cpu( struct rocketnet_server_uring_t * uring, int queue )
{
    rocketnet_assert( uring );
    rocketnet_assert( queue >= 0 && queue < uring->num_queues );
    return uring->queues[queue].pin_cpu.load( std::memory_order_acquire );
}

int rocketnet_server_uring_worker_lowest_cpu( struct rocketnet_server_uring_t * uring, int queue )
{
    rocketnet_assert( uring );
    rocketnet_assert( queue >= 0 && queue < uring->num_queues );
    if ( uring->queues[queue].pin_cpu.load( std::memory_order_acquire ) == ROCKETNET_URING_PIN_PENDING )
    {
        return ROCKETNET_URING_PIN_PENDING;
    }
    return uring->queues[queue].lowest_cpu.load( std::memory_order_relaxed );
}

int rocketnet_server_uring_pin_refusals( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );
    return uring->pin_refusals.load( std::memory_order_relaxed );
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
    return staged >= uring->send_slots ? 0 : uring->send_slots - staged;
}

// proofs/tla/ServerStopSpread.tla Room: the fullest queue's producer room

int rocketnet_server_uring_send_room( struct rocketnet_server_uring_t * uring )
{
    rocketnet_assert( uring );

    int room = uring->send_slots;
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

void rocketnet_server_uring_test_probe_multishot( int res )
{
    rocketnet_server_uring_test_multishot_res.store( res, std::memory_order_relaxed );
    rocketnet_server_uring_probed.store( -1, std::memory_order_relaxed );
}

// the fakes of rocketnet_server_uring_test_place_worker
static int uring_fake_cpus = 4;
static bool uring_fake_without_cpu_zero = false;
static bool uring_fake_refuse_pin = false;
static bool uring_fake_refuse_all = false;
static int uring_fake_round_robin = 0;

static int uring_fake_get( cpu_set_t * mask )
{
    CPU_ZERO( mask );
    for ( int cpu = uring_fake_without_cpu_zero ? 1 : 0; cpu < uring_fake_cpus && cpu < CPU_SETSIZE; cpu++ )
    {
        CPU_SET( (size_t) cpu, mask );
    }
    return 0;
}

static int uring_fake_set( const cpu_set_t * mask )
{
    if ( uring_fake_refuse_all || ( uring_fake_refuse_pin && CPU_COUNT( mask ) == 1 ) )
    {
        return EPERM;
    }
    return 0;
}

static int uring_fake_set_round_robin( int priority )
{
    (void) priority;
    uring_fake_round_robin++;
    return 0;
}

bool rocketnet_server_uring_test_place_worker( rocketnet_server_uring_test_transport_counters_t * dispatch, const struct rocketnet_server_transport_io_operations_t * operations,
                                               int num_os_cpus, bool reserve_cpu_0, int online, int cpus, bool without_cpu_zero, bool refuse_pin, bool refuse_all,
                                               uint64_t * row, bool * left_on_core_zero, int * round_robin_requests, int * pinned_cpu )
{
    void * uring_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_uring_t ) );
    void * queue_storage = rocketnet_malloc( NULL, sizeof( rocketnet_server_uring_queue_t ) );
    if ( !uring_storage || !queue_storage )
    {
        rocketnet_free( NULL, queue_storage );
        rocketnet_free( NULL, uring_storage );
        return false;
    }
    auto * uring = new ( uring_storage ) rocketnet_server_uring_t;
    auto * queue = new ( queue_storage ) rocketnet_server_uring_queue_t;
    uring->num_os_cpus = num_os_cpus > 1 ? num_os_cpus : 1;
    uring->reserve_cpu_0 = reserve_cpu_0;
    uring->num_queues = 1;
    uring->queues = queue;
    queue->uring = uring;
    queue->index = 0;

    uring_fake_cpus = cpus;
    uring_fake_without_cpu_zero = without_cpu_zero;
    uring_fake_refuse_pin = refuse_pin;
    uring_fake_refuse_all = refuse_all;
    uring_fake_round_robin = 0;
    const struct rocketnet_uring_affinity_calls_t fake = { uring_fake_get, uring_fake_set, uring_fake_set_round_robin };

    const bool priority_enabled = rocketnet_platform_thread_priority_enabled();
    rocketnet_platform_set_thread_priority_enabled( true );
    rocketnet_server_uring_pin_worker( queue, online, &fake );
    rocketnet_platform_set_thread_priority_enabled( priority_enabled );

    // the public row through dispatch: the queue has no receive or send
    // module, so the read sees no queues and answers the transport's own rows
    uring->num_queues = 0;
    uint64_t values[ROCKETNET_NUM_COUNTERS] = {};
    values[ROCKETNET_COUNTER_URING_PIN_REFUSALS] = UINT64_MAX;
    const bool answered = dispatch( operations, uring, values );
    *row = values[ROCKETNET_COUNTER_URING_PIN_REFUSALS];
    *left_on_core_zero = queue->left_on_core_zero.load( std::memory_order_relaxed );
    *round_robin_requests = uring_fake_round_robin;
    *pinned_cpu = queue->pin_cpu.load( std::memory_order_acquire );

    uring->queues = NULL;
    queue->~rocketnet_server_uring_queue_t();
    uring->~rocketnet_server_uring_t();
    rocketnet_free( NULL, queue_storage );
    rocketnet_free( NULL, uring_storage );
    return answered;
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

int rocketnet_server_uring_send_queue_size( const struct rocketnet_server_uring_t * )
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

struct rocketnet_server_uring_t * rocketnet_server_uring_create_pinned( void * context, struct rocketnet_address_t * bind_address, int num_queues, int max_clients, int flags, int num_os_cpus,
                                                                        int * status, const uint8_t * packet_filter )
{
    (void) num_os_cpus;
    return rocketnet_server_uring_create_flags( context, bind_address, num_queues, max_clients, flags, status, packet_filter );
}

int rocketnet_server_uring_worker_cpu( struct rocketnet_server_uring_t * uring, int queue )
{
    (void) uring;
    (void) queue;
    return ROCKETNET_URING_PIN_NONE;
}

int rocketnet_server_uring_worker_lowest_cpu( struct rocketnet_server_uring_t * uring, int queue )
{
    (void) uring;
    (void) queue;
    return ROCKETNET_URING_PIN_NONE;
}

int rocketnet_server_uring_pin_refusals( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return 0;
}

int rocketnet_server_uring_features( struct rocketnet_server_uring_t * uring )
{
    (void) uring;
    return 0;
}

void rocketnet_server_uring_begin_shutdown( struct rocketnet_server_uring_t * ) {}
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
        memset( counters + ROCKETNET_SERVER_URING_FIRST_COUNTER, 0, ROCKETNET_SERVER_URING_COUNTER_ROWS * sizeof( uint64_t ) );
        counters[ROCKETNET_COUNTER_URING_PIN_REFUSALS] = 0;
        counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SOCKET] = 0;
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
void rocketnet_server_uring_test_probe_multishot( int ) {}
bool rocketnet_server_uring_test_place_worker( rocketnet_server_uring_test_transport_counters_t *, const struct rocketnet_server_transport_io_operations_t *, int, bool, int,
                                               int, bool, bool, bool, uint64_t *, bool *, int *, int * )
{
    return false;
}
#endif // #if ROCKETNET_DEV

#endif // #if defined( ROCKETNET_URING_BUILT ) && ROCKETNET_URING_BUILT

int rocketnet_server_uring_linux_cpp_dummy = 0;
