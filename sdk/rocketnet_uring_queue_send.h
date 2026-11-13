/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_URING_QUEUE_SEND_H
#define ROCKETNET_SDK_ROCKETNET_URING_QUEUE_SEND_H

// The io_uring server's per-queue send handoff (layer L1 of the per-queue
// send): each queue's banks (rocketnet_uring_send_queue.h, layer L0) move
// between the game threads, main and the queue's worker through the shared bank
// exchange (rocketnet_buffer_handoff.h) under the queue's handoff mutex.
//
//   main     publish: with the game threads joined, a producer that staged anything
//            goes to the back of the ready FIFO and the head of the free FIFO
//            becomes the producer, reset; with no free bank the producer is
//            kept with its slots for the next send_packets.
//   worker   claim: takes the oldest ready bank and cuts its staged slots into
//            runs with the gather (rocketnet_send_gather.h): each run is
//            staged slots to one destination in staged order, one send and
//            one completion. next hands out the next run to submit, oldest
//            position first; complete takes a run's completion: sent,
//            dropped, or handed back to be submitted again (the socket was
//            full, or the destination refused a segmented send, which comes
//            back as one run per packet). retire gives the bank back to the
//            free FIFO only when nothing in it is waiting to be submitted and
//            every run of it has its completion: the kernel reads a run's
//            frames and message until its completion, so a bank is never
//            reused while the kernel owns any of it.
//
// Portable and pure: no ring, no socket, no thread of its own. The worker
// thread, its ring and the messages are the transport's
// (rocketnet_server_uring_linux.cpp). proofs/tla/RNUringQueueSend.tla models
// one queue; each function cites the action it implements, and
// tests/tla_trace_queue_send.cpp binds the code to the model
// (TraceRNUringQueueSend.tla).

#include "rocketnet_address.h"
#include "rocketnet_buffer_handoff.h"
#include "rocketnet_platform.h"
#include "rocketnet_send_gather.h"
#include "rocketnet_uring_send_queue.h"

#include <atomic>
#include <stdint.h>

struct msghdr;
struct rocketnet_uring_queue_send_message_t;

// a slot of a bank, as the worker sees it (RNUringQueueSend st)

#define ROCKETNET_URING_QUEUE_SEND_SLOT_EMPTY                                0      // not claimed by a game thread since the bank was last free
#define ROCKETNET_URING_QUEUE_SEND_SLOT_STAGED                               1      // finished by a game thread, waiting to be submitted
#define ROCKETNET_URING_QUEUE_SEND_SLOT_ABORTED                              2      // claimed and aborted, or never finished: in no run
#define ROCKETNET_URING_QUEUE_SEND_SLOT_RUN                                  3      // in a run the kernel owns until its completion is taken
#define ROCKETNET_URING_QUEUE_SEND_SLOT_SENT                                 4
#define ROCKETNET_URING_QUEUE_SEND_SLOT_DROPPED                              5

// what a completion says about its run

#define ROCKETNET_URING_QUEUE_SEND_OK                                        0      // sent: res >= 0
#define ROCKETNET_URING_QUEUE_SEND_FAILED                                    1      // dropped: any other negative res
#define ROCKETNET_URING_QUEUE_SEND_AGAIN                                     2      // -EAGAIN: the socket was full; the run is submitted again
#define ROCKETNET_URING_QUEUE_SEND_REFUSED                                   3      // a segmented run its destination refused: resent one packet a run

// a queue's counters, cumulative; one writer, the queue's worker

#define ROCKETNET_URING_QUEUE_SEND_COUNTER_SENT                              0      // packets of runs completed ok
#define ROCKETNET_URING_QUEUE_SEND_COUNTER_DROPPED                           1      // packets of runs completed failed (a refused one-packet run included)
#define ROCKETNET_URING_QUEUE_SEND_COUNTER_AGAIN                             2      // runs handed back by -EAGAIN
#define ROCKETNET_URING_QUEUE_SEND_COUNTER_REFUSED                           3      // segmented runs handed back as one run per packet
#define ROCKETNET_URING_QUEUE_SEND_COUNTER_STRAY                             4      // completions that named no run in the kernel: ignored
#define ROCKETNET_URING_QUEUE_SEND_COUNTER_SEGMENTED                         5      // runs of more than one packet handed out by next
#define ROCKETNET_URING_QUEUE_SEND_COUNTER_ABANDONED                         6      // packets given up on by abandon, the stop's last resort
#define ROCKETNET_URING_QUEUE_SEND_NUM_COUNTERS                              7

// one run to submit: the frames of slots order[first .. first + count) of
// the bank, all to one destination, each segment_bytes long but the last
// (last_bytes). tag is the index its completion carries back.

struct rocketnet_uring_queue_send_run_t
{
    int bank;
    int slot;                               // the first packet's slot
    int first;                              // its position in the bank's order
    int count;
    int segment_bytes;
    int last_bytes;
    bool plain;                             // handed back by a refusal: send it with no segmentation
    uint32_t tag;                           // bank * slots + slot
    const struct rocketnet_address_t * to;  // the destination: the first slot's, in the bank
};

// a run in the bank, kept at its first slot

struct rocketnet_uring_queue_send_entry_t
{
    int count;                              // packets; 0: no run starts at this slot
    int first;
    int segment_bytes;
    int last_bytes;
    bool kernel;                            // submitted, its completion not yet taken
    bool plain;
};

struct rocketnet_uring_queue_send_queue_t
{
    // the handoff: the mutex guards handoff and held

    struct rocketnet_platform_mutex_t mutex;
    rocketnet_buffer_handoff_t<ROCKETNET_URING_SEND_BANKS> handoff;
    int held;                               // the bank the worker holds, -1 for none

    // the worker's

    int pending[ROCKETNET_URING_SEND_BANKS];     // runs of each bank in the kernel
    int unsubmitted;                        // packets of the held bank waiting to be submitted
    int ordered;                            // the held bank's order length: its staged slots
    int cursor;                             // no run before this order position waits to be submitted
    uint8_t * state;                        // [banks * slots] ROCKETNET_URING_QUEUE_SEND_SLOT_*
    int * order;                            // [banks * slots] each bank's staged slots, grouped by destination
    struct rocketnet_uring_queue_send_entry_t * entry;  // [banks * slots] runs at their first slot
    struct rocketnet_send_gather_run_t * runs;          // [slots] the gather's runs
    struct rocketnet_send_gather_scratch_t scratch;
    int max_segments;                       // the packets a run may carry: 1 once the transport cannot segment
    struct rocketnet_uring_queue_send_message_t * message;  // [banks * slots] each run's message at its tag, Linux only (NULL elsewhere)
    void * iovecs;                          // [banks * slots] struct iovec at each order position, Linux only
    std::atomic<uint64_t> counters[ROCKETNET_URING_QUEUE_SEND_NUM_COUNTERS];
};

struct rocketnet_uring_queue_send_t
{
    void * context;
    struct rocketnet_uring_send_queues_t * queues;     // the banks; not owned
    int num_queues;
    int slots;
    int max_segments;
    int max_bytes;
    struct rocketnet_uring_queue_send_queue_t * queue;
    void * memory;
};

// NULL when max_segments or max_bytes is under 1, or a mutex or the
// allocation is refused. queues outlives it.

struct rocketnet_uring_queue_send_t * rocketnet_uring_queue_send_create( void * context, struct rocketnet_uring_send_queues_t * queues, int max_segments, int max_bytes );

// the bytes create allocates for queues of this count and bank size, 0 out
// of range
uint64_t rocketnet_uring_queue_send_allocation_bytes( int num_queues, int slots );

// no worker running
void rocketnet_uring_queue_send_destroy( struct rocketnet_uring_queue_send_t * send );

// main, the game threads joined. RNUringQueueSend Publish: true when the queue staged
// anything and a free bank replaced its producer; false keeps the producer.
bool rocketnet_uring_queue_send_publish( struct rocketnet_uring_queue_send_t * send, int queue );

// main: no bank published and not yet retired, and the producer empty
bool rocketnet_uring_queue_send_idle( struct rocketnet_uring_queue_send_t * send, int queue );

// the worker's

// RNUringQueueSend WorkerClaim: when the worker holds no bank, takes the
// oldest ready one and cuts it into runs. The bank taken, or -1.
int rocketnet_uring_queue_send_claim( struct rocketnet_uring_queue_send_t * send, int queue );

// the bank the worker holds, -1 for none
int rocketnet_uring_queue_send_held( const struct rocketnet_uring_queue_send_t * send, int queue );

// RNUringQueueSend WorkerSubmit: the next run of the held bank waiting to be
// submitted, oldest order position first, now owned by the kernel. false when
// none waits; the caller submits the run, and its completion goes to
// complete. A run handed out is the kernel's until then: the caller never
// takes it back.
bool rocketnet_uring_queue_send_next( struct rocketnet_uring_queue_send_t * send, int queue, struct rocketnet_uring_queue_send_run_t * run );

// RNUringQueueSend WorkerReap: the completion of the run tag names, as one of
// ROCKETNET_URING_QUEUE_SEND_OK, _FAILED, _AGAIN or _REFUSED. Returns the
// run's packets, or -1 when tag names no run in the kernel (counted STRAY,
// nothing changed). The frames and message of the run are the worker's
// again.
int rocketnet_uring_queue_send_complete( struct rocketnet_uring_queue_send_t * send, int queue, uint32_t tag, int result );

// what a completion's res says: >= 0 OK, -EAGAIN AGAIN, -EINVAL, -EIO and
// -EOPNOTSUPP on a segmented run REFUSED (rocketnet_server_gso.h), any other
// FAILED
int rocketnet_uring_queue_send_result( int res, bool segmented );

// the model's name for a send completion (RNUringQueueWorker.tla ReapOk,
// ReapAgain, ReapRefused): res as the kernel answered and packets the run
// held. A segmented run its destination refused (more than one packet) is
// split; a refused one-packet run is a plain failure and reads as OK to the
// model (the packet is done, counted dropped by complete)

#define ROCKETNET_URING_QUEUE_SEND_CLASS_OK                                  1
#define ROCKETNET_URING_QUEUE_SEND_CLASS_AGAIN                               2
#define ROCKETNET_URING_QUEUE_SEND_CLASS_SPLIT                               3

int rocketnet_uring_queue_send_class( int res, int packets );

// a segmented run next handed out, to a destination the worker knows
// refuses segmentation: taken back before any send, as one plain run per
// packet waiting to be submitted (complete's REFUSED hand-back, counted
// nowhere). false, nothing changed, when run is not a segmented run in the
// kernel.
bool rocketnet_uring_queue_send_split( struct rocketnet_uring_queue_send_t * send, int queue, const struct rocketnet_uring_queue_send_run_t * run );

// RNUringQueueSend WorkerRetire: the held bank back to the free FIFO, when
// nothing in it waits to be submitted and every run of it has its
// completion. True when retired.
bool rocketnet_uring_queue_send_retire( struct rocketnet_uring_queue_send_t * send, int queue );

// the stop's last resort, when the socket stays full past the stop's bounded
// retries: every packet of the held bank still waiting to be submitted is
// counted ABANDONED and its slot dropped, so the bank can retire once its
// runs in the kernel complete. Returns the packets abandoned.
int rocketnet_uring_queue_send_abandon( struct rocketnet_uring_queue_send_t * send, int queue );

// worker: runs cut by later claims carry one packet each (the transport's
// segmentation turned off, rocketnet_server_gso.h's refusal threshold)
void rocketnet_uring_queue_send_plain( struct rocketnet_uring_queue_send_t * send, int queue );

// Layer L2, the worker's: the one SENDMSG of a run handed out by next. Its
// iovecs are the run's frames where they lie, scattered across the bank
// (the iovec at each of the run's order positions: the wire bytes from
// ROCKETNET_FRAME_WIRE_OFFSET, the slot's packet_bytes), its name the
// destination of the run's first slot, and a run of more than one packet
// not sent plain carries a UDP_SEGMENT cmsg of segment_bytes. The message,
// name, control and iovecs are the run's, at its tag and its order
// positions, which no other run in the kernel uses, and stay untouched until
// its completion. NULL when the socket's family cannot name the destination
// (the caller completes the run failed without a send), and on a platform
// without the io_uring uapi.
struct msghdr * rocketnet_uring_queue_send_message( struct rocketnet_uring_queue_send_t * send, int queue, const struct rocketnet_uring_queue_send_run_t * run, bool socket_ipv6 );

// runs of the held bank in the kernel
int rocketnet_uring_queue_send_pending( const struct rocketnet_uring_queue_send_t * send, int queue );

// any thread: one counter of a queue, relaxed
uint64_t rocketnet_uring_queue_send_counter( const struct rocketnet_uring_queue_send_t * send, int queue, int counter );

#endif // ROCKETNET_SDK_ROCKETNET_URING_QUEUE_SEND_H
