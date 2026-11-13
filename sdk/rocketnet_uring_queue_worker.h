/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_URING_QUEUE_WORKER_H
#define ROCKETNET_SDK_ROCKETNET_URING_QUEUE_WORKER_H

// One queue's worker of the io_uring server (layer L3 of the per-queue send),
// the XDP transport's shape: the worker owns its queue's ring and does all of
// the queue's kernel-facing work. It submits the runs of the banks main
// publishes (rocketnet_uring_queue_send.h: one SENDMSG per run, grouped by
// destination here, not on main), keeps the queue's multishot receive armed
// and hands its completions to the receive module (rocketnet_uring_receive_queue.h,
// which runs the prefilter and banks for main), and sleeps in one wait on
// its ring for any completion. Main wakes it through an eventfd the worker
// keeps a READ armed on; a write made while no read is armed stays in the
// eventfd's count and completes the next read at once, so a wake is never
// lost and the worker never spins.
//
// A run answered full (-EAGAIN, and -EINTR, -ENOBUFS, -ENOMEM: the socket or
// the device queue behind it had no room) goes back to be submitted again.
// No run is submitted until the worker's retry timer (a
// ROCKETNET_URING_SEND_RETRY_SECONDS timeout on its ring) fires or, before
// the stop, main wakes it: the gso worker's wait. Never a POLLOUT poll: a
// UDP socket reports room while the device queue behind it still refuses
// (-ENOBUFS), and the poll would fire at once, again and again.
//
// begin_shutdown (the server's stop, before its last disconnect batches)
// stores shutdown and wakes the worker. From then on each wait a full answer
// starts counts; a run done (sent or failed) or a new bank claimed starts
// the count again, and past ROCKETNET_URING_SEND_RETRIES_AFTER_QUIT waits
// what still waits of the bank is abandoned (counted), so a socket that
// stays full cannot keep send_idle false: the server asks the stop only once
// it is true. Stop stores shutdown and quit and wakes the worker: it cancels
// its receive and its wake read, keeps sending what is published with the
// same bounded waits, and exits only when nothing of its is in the kernel:
// no run, no receive, no wake read, no timer (a cancel itself holds nothing:
// its completion may come after, and the ring is torn down after the join).
// Main joins it after.
//
// SENDMSG goes with MSG_DONTWAIT: without it io_uring waits inside the
// kernel for room and never answers -EAGAIN. The wake eventfd must be
// blocking (no EFD_NONBLOCK: a READ on a non-blocking eventfd with a zero
// count completes at once with -EAGAIN, and the worker would spin) and not
// EFD_SEMAPHORE (a read takes the whole count).
//
// A step that needs a submission entry and finds the submission queue full
// marks the pass starved: the pass then says continue, so the submit that
// follows does not wait and frees the room (RNUringQueueWorker Sleep is never
// taken with a worker action still to do).
//
// proofs/tla/RNUringQueueWorker.tla models the loop: each part of pass names
// the action it implements. The kernel is behind a small interface
// (rocketnet_uring_queue_worker_kernel_t): the transport's is its ring, the
// unit suite's a fake completion source with no ring, no socket and no time.
//
// Linux with kernel uapi headers that have multishot receive
// (IORING_RECV_MULTISHOT) and liburing (ROCKETNET_IO_URING); elsewhere
// create refuses.

#include "rocketnet_address.h"
#include "rocketnet_server_gso.h"

#include <atomic>
#include <stdint.h>

struct io_uring_sqe;
struct rocketnet_uring_queue_send_t;

// the kinds of the worker's own submissions, beside ROCKETNET_URING_TAG_SEND,
// _RECV and _CANCEL (rocketnet_uring_codec.h)
#define ROCKETNET_URING_TAG_WAKE                                             4      // the eventfd READ
#define ROCKETNET_URING_TAG_TIMER                                            5      // the retry timer after a full answer

// what a cancel names, its tag's index
#define ROCKETNET_URING_CANCEL_RECV                                          0
#define ROCKETNET_URING_CANCEL_WAKE                                          1

// waits after shutdown with no run done before the worker abandons what
// waits: the gso worker's bound (unmeasured there too: chosen)
#define ROCKETNET_URING_SEND_RETRIES_AFTER_QUIT          ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT

// the retry timer: how long a run answered full waits before it goes again,
// unless main's wake comes first (before the stop): the gso worker's wait
#define ROCKETNET_URING_SEND_RETRY_SECONDS                     ROCKETNET_GSO_SEND_RETRY_SECONDS

// completions taken per pass
#define ROCKETNET_URING_WORKER_CQE_BATCH                                   256

// what pass says to do next
#define ROCKETNET_URING_WORKER_CONTINUE                                      0      // more to do now: submit without waiting and pass again
#define ROCKETNET_URING_WORKER_SLEEP                                         1      // nothing to do: submit and wait for a completion
#define ROCKETNET_URING_WORKER_EXIT                                          2      // the stop seen and nothing in the kernel

// what one step did
#define ROCKETNET_URING_WORKER_STEP_NONE                                     0      // nothing: Sleep
#define ROCKETNET_URING_WORKER_STEP_EXIT                                     1      // Exit
#define ROCKETNET_URING_WORKER_STEP_OBSERVE                                  2
#define ROCKETNET_URING_WORKER_STEP_REAP                                     3
#define ROCKETNET_URING_WORKER_STEP_RETIRE                                   4
#define ROCKETNET_URING_WORKER_STEP_CLAIM                                    5
#define ROCKETNET_URING_WORKER_STEP_ABANDON                                  6
#define ROCKETNET_URING_WORKER_STEP_SUBMIT                                   7
#define ROCKETNET_URING_WORKER_STEP_SPLIT                                    8
#define ROCKETNET_URING_WORKER_STEP_DROP                                     9
#define ROCKETNET_URING_WORKER_STEP_ARM_TIMER                               10
#define ROCKETNET_URING_WORKER_STEP_ARM_RECV                                11
#define ROCKETNET_URING_WORKER_STEP_ARM_WAKE                                12
#define ROCKETNET_URING_WORKER_STEP_CANCEL_RECV                             13
#define ROCKETNET_URING_WORKER_STEP_CANCEL_WAKE                             14

// the worker's counters, one writer, the worker
#define ROCKETNET_URING_WORKER_COUNTER_SUBMITS                               0      // io_uring_enter calls: submits and waits
#define ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED                         1      // passes that left completions for the next pass (a full batch taken)
#define ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS                             2      // segmented SENDMSGs submitted
#define ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS                         3      // segmentation turned off for the queue (rocketnet_server_gso.h's refusal threshold)
#define ROCKETNET_URING_WORKER_COUNTER_SEND_DROPPED                          4      // runs that could not be sent: a destination the socket family cannot carry (packets)
#define ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL                           5      // full answers: -EAGAIN, -EINTR, -ENOBUFS, -ENOMEM
#define ROCKETNET_URING_WORKER_COUNTER_STRAY                                 6      // completions that named nothing of the worker's
#define ROCKETNET_URING_WORKER_NUM_COUNTERS                                  7

struct rocketnet_uring_queue_worker_completion_t
{
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

// the kernel, as the worker sees it. space is the submission entries free;
// sqe the next one, called only while space > 0, whose fields the worker
// prepares; submit hands the prepared entries to the kernel and, with wait,
// returns once at least one completion is ready (a negative errno when
// refused); peek copies at most max ready completions, oldest first, and
// consumes none; advance consumes count of them.
struct rocketnet_uring_queue_worker_kernel_t
{
    void * context;
    int ( *space )( void * context );
    struct io_uring_sqe * ( *sqe )( void * context );
    int ( *submit )( void * context, bool wait );
    int ( *peek )( void * context, struct rocketnet_uring_queue_worker_completion_t * completions, int max );
    void ( *advance )( void * context, int count );
};

// the queue's receive, as the worker sees it: the transport's is the receive
// module (rocketnet_uring_receive_queue.h), whose functions these are; the
// unit suite's a fake. needs_arm: the multishot ended and the receive is not
// stopping; arm prepares sqe as the multishot, tagged
// (ROCKETNET_URING_TAG_RECV, queue_index, 0); cqe takes one completion with
// that tag; stop: never arm again; armed: its last completion is still to
// come.
struct rocketnet_uring_queue_worker_receive_t
{
    void * context;
    bool ( *needs_arm )( void * context );
    bool ( *arm )( void * context, struct io_uring_sqe * sqe );
    void ( *cqe )( void * context, int32_t res, uint32_t flags );
    void ( *stop )( void * context );
    bool ( *armed )( void * context );
};

struct rocketnet_uring_queue_worker_config_t
{
    int queue_index;
    int fd;                                         // the queue's socket
    bool ipv6;                                      // the socket is ipv6 (dual stack)
    int wake_fd;                                    // the eventfd main writes, -1 for none (the unit suite posts wake completions itself)
    int max_in_kernel;                              // runs in the kernel at once: the ring's room for them
    int max_retry;                                  // ROCKETNET_URING_SEND_RETRIES_AFTER_QUIT, smaller in the unit suite: waits after shutdown
    bool segment;                                   // UDP_SEGMENT is on: runs may carry more than one packet
    struct rocketnet_uring_queue_send_t * send;     // the banks' handoff; not owned
    struct rocketnet_uring_queue_worker_receive_t receive;  // context NULL: no receive
    struct rocketnet_uring_queue_worker_kernel_t kernel;
};

// Cache-line alignment pads this on MSVC (C4324); the padding is the point.
#if defined( _MSC_VER )
#pragma warning( push )
#pragma warning( disable : 4324 )
#endif // #if defined( _MSC_VER )
struct rocketnet_uring_queue_worker_t
{
    // create's, read-only after it

    struct rocketnet_uring_queue_worker_config_t config;

    // main's: begin_shutdown stores shutdown, stop stores shutdown then quit
    // (release); the worker loads them (acquire)

    alignas( 64 ) std::atomic<bool> shutdown;       // shutdown
    std::atomic<bool> quit;                         // quit

    // the worker's (RNUringQueueWorker's variables)

    alignas( 64 ) bool seen;                        // seen
    bool blocked;                                   // blocked: a run came back full, no submit until the timer fires (or main's wake, before the stop)
    bool starved;                                   // this pass found the submission queue full (not a model variable)
    bool timer_armed;                               // timer # "none"
    bool wake_armed;                                // wakeRead # "none"
    bool cancelled_recv;
    bool cancelled_wake;
    int in_kernel;                                  // k
    int last_send_class;                            // the class (ROCKETNET_URING_QUEUE_SEND_CLASS_*) of the last send completion reaped, 0 before one: what the trace check reads, never its own rule
    int cancels;                                    // cancels in the kernel
    int agains;                                     // agains: waits after shutdown since a run was done or a bank claimed, up to max_retry
    uint64_t wake_value;                            // the READ's destination
    alignas( 8 ) int64_t timer_spec[2];             // the timer's __kernel_timespec (seconds, nanoseconds), read by the kernel at submit
    struct rocketnet_segment_refused_t refused;     // the destinations that refused a segmented send
    std::atomic<uint64_t> counters[ROCKETNET_URING_WORKER_NUM_COUNTERS];
};
#if defined( _MSC_VER )
#pragma warning( pop )
#endif // #if defined( _MSC_VER )

// initializes worker in place (it may be part of a larger allocation). false
// when a config value is out of range or the platform has no io_uring.
bool rocketnet_uring_queue_worker_init( struct rocketnet_uring_queue_worker_t * worker, const struct rocketnet_uring_queue_worker_config_t * config );

// the worker's: one pass of the loop, without waiting: steps until nothing
// is left to do without the kernel, reaping at most
// ROCKETNET_URING_WORKER_CQE_BATCH completions, and says what to do next
// (ROCKETNET_URING_WORKER_*): sleep (submit what it prepared and wait),
// continue (completions left past the batch: submit and pass again), exit.
int rocketnet_uring_queue_worker_pass( struct rocketnet_uring_queue_worker_t * worker );

// one step of the loop: one of RNUringQueueWorker's worker actions (a
// ROCKETNET_URING_WORKER_STEP_* naming it), taken in a fixed order: observe
// the stop, reap the oldest ready completion (when reap), retire, claim,
// abandon, send one run, arm the timer, arm the receive and the wake read
// before the stop or cancel them after it, exit. NONE: nothing to do
// without the kernel (RNUringQueueWorker Sleep). The trace walk drives it
// one step a record.
int rocketnet_uring_queue_worker_step( struct rocketnet_uring_queue_worker_t * worker, bool reap );

// the worker thread's body: pass, then submit (waiting when pass says
// sleep), until pass says exit. A refused submit is logged once and retried
// on the next pass.
void rocketnet_uring_queue_worker_run( struct rocketnet_uring_queue_worker_t * worker );

// main's: wakes the worker (a write to its eventfd). RNUringQueueWorker Signal.
void rocketnet_uring_queue_worker_wake( struct rocketnet_uring_queue_worker_t * worker );

// main's: RNUringQueueWorker BeginStop (the driver's begin_shutdown).
// shutdown, then the wake: from here a full socket's waits are counted.
void rocketnet_uring_queue_worker_begin_shutdown( struct rocketnet_uring_queue_worker_t * worker );

// main's: RNUringQueueWorker Stop. shutdown and quit, then the wake.
void rocketnet_uring_queue_worker_stop( struct rocketnet_uring_queue_worker_t * worker );

// any thread: one counter, relaxed
uint64_t rocketnet_uring_queue_worker_counter( const struct rocketnet_uring_queue_worker_t * worker, int counter );

#endif // ROCKETNET_SDK_ROCKETNET_URING_QUEUE_WORKER_H
