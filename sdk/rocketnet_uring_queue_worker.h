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
// A run answered -EAGAIN (the socket full) goes back to be submitted again,
// and no run is submitted until a POLLOUT poll on the socket fires. Stop sets
// quit and wakes the worker: it cancels its receive and its wake read, keeps
// sending what is published, and once -EAGAIN has answered
// ROCKETNET_URING_SEND_RETRIES_AFTER_QUIT times since it saw the stop it
// abandons what still waits (counted) and cancels its poll. It exits only
// when nothing of its is in the kernel: no run, no receive, no wake read, no
// poll, no cancel. Main joins it after.
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
#define ROCKETNET_URING_TAG_POLL                                             5      // the POLLOUT poll after -EAGAIN

// what a cancel names, its tag's index
#define ROCKETNET_URING_CANCEL_RECV                                          0
#define ROCKETNET_URING_CANCEL_WAKE                                          1
#define ROCKETNET_URING_CANCEL_POLL                                          2

// -EAGAIN answers after the stop is seen before the worker abandons what
// waits: the gso worker's bound (unmeasured there too: chosen)
#define ROCKETNET_URING_SEND_RETRIES_AFTER_QUIT          ROCKETNET_GSO_SEND_RETRIES_AFTER_QUIT

// completions taken per pass
#define ROCKETNET_URING_WORKER_CQE_BATCH                                   256

// what pass says to do next
#define ROCKETNET_URING_WORKER_CONTINUE                                      0      // more to do now: submit without waiting and pass again
#define ROCKETNET_URING_WORKER_SLEEP                                         1      // nothing to do: submit and wait for a completion
#define ROCKETNET_URING_WORKER_EXIT                                          2      // the stop seen and nothing in the kernel

// the worker's counters, one writer, the worker
#define ROCKETNET_URING_WORKER_COUNTER_SUBMITS                               0      // io_uring_enter calls: submits and waits
#define ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED                         1      // passes that left completions for the next pass (a full batch taken)
#define ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS                             2      // segmented SENDMSGs submitted
#define ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS                         3      // segmentation turned off for the queue (rocketnet_server_gso.h's refusal threshold)
#define ROCKETNET_URING_WORKER_COUNTER_SEND_DROPPED                          4      // runs that could not be sent: a destination the socket family cannot carry (packets)
#define ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL                           5      // -EAGAIN answers: the socket was full
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
    int max_retry;                                  // ROCKETNET_URING_SEND_RETRIES_AFTER_QUIT, smaller in the unit suite
    bool segment;                                   // UDP_SEGMENT is on: runs may carry more than one packet
    struct rocketnet_uring_queue_send_t * send;     // the banks' handoff; not owned
    struct rocketnet_uring_queue_worker_receive_t receive;  // context NULL: no receive
    struct rocketnet_uring_queue_worker_kernel_t kernel;
};

struct rocketnet_uring_queue_worker_t
{
    // create's, read-only after it

    struct rocketnet_uring_queue_worker_config_t config;

    // main's: stop stores quit (release), the worker loads it (acquire)

    alignas( 64 ) std::atomic<bool> quit;

    // the worker's (RNUringQueueWorker's variables)

    alignas( 64 ) bool seen;                        // seen
    bool blocked;                                   // blocked: a run came back -EAGAIN, no submit until the poll fires
    bool poll_armed;                                // poll # "none"
    bool poll_wanted;                               // blocked with no poll armed yet: the next pass arms it
    bool wake_armed;                                // wakeRead # "none"
    bool cancelled_recv;
    bool cancelled_wake;
    bool cancelled_poll;
    int in_kernel;                                  // k
    int cancels;                                    // cancels in the kernel
    int agains;                                     // agains, up to max_retry
    uint64_t wake_value;                            // the READ's destination
    struct rocketnet_segment_refused_t refused;     // the destinations that refused a segmented send
    std::atomic<uint64_t> counters[ROCKETNET_URING_WORKER_NUM_COUNTERS];
};

// initializes worker in place (it may be part of a larger allocation). false
// when a config value is out of range or the platform has no io_uring.
bool rocketnet_uring_queue_worker_init( struct rocketnet_uring_queue_worker_t * worker, const struct rocketnet_uring_queue_worker_config_t * config );

// the worker's: one pass of the loop, without waiting. Observes the stop,
// reaps a batch of completions, submits what it may, arms what it needs, and
// says what to do next (ROCKETNET_URING_WORKER_*).
int rocketnet_uring_queue_worker_pass( struct rocketnet_uring_queue_worker_t * worker );

// the worker thread's body: pass, then submit (waiting when pass says
// sleep), until pass says exit. A refused submit is logged once and retried
// on the next pass.
void rocketnet_uring_queue_worker_run( struct rocketnet_uring_queue_worker_t * worker );

// main's: wakes the worker (a write to its eventfd). RNUringQueueWorker Signal.
void rocketnet_uring_queue_worker_wake( struct rocketnet_uring_queue_worker_t * worker );

// main's: RNUringQueueWorker Stop. quit, then the wake.
void rocketnet_uring_queue_worker_stop( struct rocketnet_uring_queue_worker_t * worker );

// any thread: one counter, relaxed
uint64_t rocketnet_uring_queue_worker_counter( const struct rocketnet_uring_queue_worker_t * worker, int counter );

#endif // ROCKETNET_SDK_ROCKETNET_URING_QUEUE_WORKER_H
