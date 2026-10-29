/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_URING_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_URING_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"
#include "rocketnet_packet_batch.h"

// The io_uring server transport: one io_uring per transport, used only on the main
// thread, and no worker thread. An SO_REUSEPORT socket group (bound the way
// the gso transport binds its group) shards receives across queues; each queue
// has a provided buffer ring and one multishot RECVMSG, so the kernel fills
// receive buffers with no syscall per datagram, and send_packets submits one
// SENDMSG per send run in chunks no larger than the free submission queue
// space: with UDP_SEGMENT (probed at create, the gso transport's tier) a run
// of consecutive staged slots to one destination, equal sizes but for one
// shorter tail, goes out as one segmented SENDMSG; without it a run is one
// slot; with UDP_GRO (probed at create, all or nothing) one receive buffer
// holds a coalesced receive that is split by its stride into the receive
// bank. each send_packets and receive_packets call reaps at most a fixed
// completion budget, so a call returns under sustained traffic.
// proofs/tla/RNUringDriver.tla models the send staging, the two receive banks, the
// per-call budget and destroy; the functions that implement a model action
// cite it.
//
// The transport is built only when the library is configured with liburing
// (ROCKETNET_IO_URING, Linux). Without it, create logs the refusal and
// returns NULL, and rocketnet_server_uring_available() is false.

struct rocketnet_server_uring_t;

// the transport's geometry, all compile time:

#ifndef ROCKETNET_URING_DEFAULT_QUEUES
#define ROCKETNET_URING_DEFAULT_QUEUES                                       4
#endif // #ifndef ROCKETNET_URING_DEFAULT_QUEUES

#define ROCKETNET_URING_MAX_QUEUES                                          32

// submission queue entries; the completion queue is twice this

#define ROCKETNET_URING_ENTRIES                                           4096

// send staging: one phase's worth of frame-stride slots, the gso transport's
// budget

#define ROCKETNET_URING_SEND_QUEUE_SIZE                                   2048

// receive: per queue a provided buffer ring of this many buffers of this many
// bytes. one buffer holds the multishot recvmsg header, the reserved name
// area, the UDP_GRO control area and the payload, which with UDP_GRO is one
// coalesced receive of up to 64 KiB (the 16-bit UDP payload), so a buffer is
// 66 KiB: 4096 * 66 KiB = 264 MiB per queue, all allocated at create (2 KiB
// buffers, 8 MiB per queue, before UDP_GRO). memory is spent so that one
// completion carries a whole coalesced run of datagrams.

#define ROCKETNET_URING_RECV_BUFFERS                                      4096
#define ROCKETNET_URING_RECV_BUFFER_BYTES                          ( 66 * 1024 )

// the per-call completion budgets: receive_packets takes at most
// ROCKETNET_URING_RECEIVE_BUDGET completions (one receive bank's worth) and
// send_packets at most ROCKETNET_URING_SEND_BUDGET (a whole staging of sends
// and a completion queue of receives besides), each from its own budget, so
// a call returns under sustained traffic and receive_packets spending its
// budget never stops send_packets reaping. completions past a budget stay
// in the completion queue for the next call, counted REAP_DEFERRED. a
// receive completion holds its ring buffer until it is reaped, so at most
// queues * ROCKETNET_URING_RECV_BUFFERS receive completions are ever ahead
// of a send's.

#define ROCKETNET_URING_RECEIVE_BUDGET          ROCKETNET_SERVER_MAX_RECEIVE_PACKETS_PER_UPDATE
#define ROCKETNET_URING_SEND_BUDGET    ( ROCKETNET_URING_SEND_QUEUE_SIZE + ROCKETNET_URING_ENTRIES * 2 )

ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_RECV_BUFFERS >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE,
                         "receive budget twice send: io_uring receive ring holds at least twice the send queue" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_RECEIVE_BUDGET >= 2 * ROCKETNET_URING_SEND_QUEUE_SIZE,
                         "receive budget twice send: io_uring receive budget holds at least twice the send queue" );

// the send staging is per queue and does not derive from ROCKETNET_MAX_CLIENTS:
// clients spread over the queues, and the most queues stage two packets for
// every client (BUILDING.md, "Maximum clients")

ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_MAX_QUEUES * ROCKETNET_URING_SEND_QUEUE_SIZE >= 2 * ROCKETNET_MAX_CLIENTS,
                         "the io_uring queues together can stage two packets for every client" );
ROCKETNET_STATIC_ASSERT( ROCKETNET_URING_RECEIVE_BUDGET >= ROCKETNET_URING_SEND_BUDGET,
                         "receive budget twice send: one send reap fits the receive bank" );

// counters, readable any time from the main thread; indexes into the array
// rocketnet_server_uring_counters fills

#define ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT                          0
#define ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED                      1
#define ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS                       2      // io_uring_enter calls: submits and waits
#define ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED                          3      // a send CQE with a negative result, or a destination the socket family cannot carry
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE                  4      // datagram outside [server min, receive ceiling], or truncated in its buffer
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW              5      // the fill bank was full during destroy's drain (a running transport leaves the CQE in the queue instead)
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SOURCE                6      // source address the boundary could not convert
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS                    7      // the multishot ended because the buffer ring was empty (ENOBUFS)
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS                        8      // multishot receives armed again after a CQE without F_MORE
#define ROCKETNET_SERVER_URING_COUNTER_REAP_DEFERRED                         9      // a send_packets or receive_packets call that left completions in the queue for the next call: its budget spent, or the fill bank without room for a receive (deferred, not dropped)
#define ROCKETNET_SERVER_URING_COUNTER_SEND_DEFERRED                        10      // a send_packets call that returned with sends not yet completed or staged slots not yet submitted, retired by a later call
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_NO_BUFFER            11      // a receive CQE with res >= 0 that names no provided buffer (no IORING_CQE_F_BUFFER, or an id outside the ring)
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARM_FAILED                 12      // a re-arm found no submission entry (queue left disarmed)
#define ROCKETNET_SERVER_URING_COUNTER_GSO_SENDS                            13      // segmented SENDMSGs submitted: a run of more than one packet with a UDP_SEGMENT cmsg
#define ROCKETNET_SERVER_URING_COUNTER_GSO_FALLBACKS                        14      // a segmented send's CQE refused with EINVAL, EIO or EOPNOTSUPP: segmentation is off for the transport from then on
#define ROCKETNET_SERVER_URING_COUNTER_GRO_BATCHES                          15      // receive buffers that held more than one segment (a UDP_GRO coalesced receive split by its stride)
#define ROCKETNET_SERVER_URING_COUNTER_GRO_CMSG_FAULTS                      16      // a receive whose UDP_GRO control was truncated or malformed: taken as one datagram, the gso transport's fault
#define ROCKETNET_SERVER_URING_NUM_COUNTERS                                 17

// feature bits reported by rocketnet_server_uring_features: what the create
// probe found and the transport is using now

#define ROCKETNET_SERVER_URING_FEATURE_SEGMENT                          (1<<0)      // UDP_SEGMENT accepted: send runs go out as segmented SENDMSGs
#define ROCKETNET_SERVER_URING_FEATURE_GRO                              (1<<1)      // UDP_GRO enabled on every socket: a receive buffer may hold a coalesced run, split by its stride

// create flags: force a probe result off, so the plain tier is testable on
// kernels where the real probe would say yes (ROCKETNET_SERVER_GSO_DISABLE_SEGMENT
// and ROCKETNET_SERVER_GSO_DISABLE_GRO for the gso transport)

#define ROCKETNET_SERVER_URING_DISABLE_SEGMENT                          (1<<0)
#define ROCKETNET_SERVER_URING_DISABLE_GRO                              (1<<1)

// true when the library was built with liburing and this kernel accepts the
// ring, the opcodes and the provided buffer ring the transport needs. the probe
// runs once and its answer is kept.

bool rocketnet_server_uring_available();

// the bytes create allocates for a queue count, 0 for a count out of range

uint64_t rocketnet_server_uring_allocation_bytes( int queues );

// bind_address is read for family and port and written back when the port
// was zero, the contract of rocketnet_platform_socket_create. an ipv6 bind is
// dual stack: ipv4 senders arrive as native ipv4 addresses. num_queues <= 0
// selects the default; a count past ROCKETNET_URING_MAX_QUEUES refuses.

struct rocketnet_server_uring_t * rocketnet_server_uring_create( void * context, struct rocketnet_address_t * bind_address, int num_queues );

// create with flags, the DISABLE set above; create is create_flags with 0

struct rocketnet_server_uring_t * rocketnet_server_uring_create_flags( void * context, struct rocketnet_address_t * bind_address, int num_queues, int flags );

// the FEATURE bits in use now: a live refusal of a segmented send clears
// ROCKETNET_SERVER_URING_FEATURE_SEGMENT

int rocketnet_server_uring_features( struct rocketnet_server_uring_t * uring );

// Stop requests cancellation; stopped services a bounded batch of currently
// ready completions and returns false while the kernel still owns any buffer.
// A pending object remains owned by the caller. Destroy never abandons it.

void rocketnet_server_uring_stop( struct rocketnet_server_uring_t * uring );
bool rocketnet_server_uring_stopped( struct rocketnet_server_uring_t * uring );

// Requires stopped completion. Frees synchronously.
void rocketnet_server_uring_destroy( struct rocketnet_server_uring_t * uring );

// Test seam: hold completion pending until test_resume_stop releases it.

#if ROCKETNET_DEV
void rocketnet_server_uring_test_stall_destroy( struct rocketnet_server_uring_t * uring );
void rocketnet_server_uring_test_resume_stop( struct rocketnet_server_uring_t * uring );

// development library only. fail_receive cancels the queue's multishot
// receive and hands its terminating CQE to the reap with error (negative
// errno) in place of -ECANCELED, the path a kernel-side receive failure
// takes. fail_sends prepares the next count send SQEs with no destination,
// so the kernel completes each with a failed CQE.
void rocketnet_server_uring_test_fail_receive( struct rocketnet_server_uring_t * uring, int queue_index, int error );
void rocketnet_server_uring_test_fail_sends( struct rocketnet_server_uring_t * uring, int count );
// main thread: no staged slot waiting and no send without its CQE.
bool rocketnet_server_uring_test_send_idle( struct rocketnet_server_uring_t * uring );

// unit suite only (the test hook library): a transport over a fake completion
// source, with no kernel ring and no socket. the source stands in for the
// completion queue: peek copies at most max completions, oldest first, and
// consumes none; advance consumes the count peek returned; ready is how many
// it holds. submitted is called with the user_data of every queued
// submission entry when the transport submits (each send, each multishot arm,
// each cancel). a receive completion names its buffer in flags the way the
// kernel does (IORING_CQE_F_BUFFER, the id above IORING_CQE_BUFFER_SHIFT);
// every buffer of a fake transport holds one datagram of
// ROCKETNET_URING_TEST_PAYLOAD_BYTES from 127.0.0.1, which a completion with
// res ROCKETNET_URING_RECV_BUFFER_BYTES delivers. a wait returns at once.
// rocketnet_server_uring_destroy destroys a fake transport. NULL when the
// transport is not built.

#define ROCKETNET_URING_TEST_PAYLOAD_BYTES                                 100

struct rocketnet_server_uring_test_completion_t
{
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

struct rocketnet_server_uring_test_source_t
{
    void * context;
    int ( *peek )( void * context, struct rocketnet_server_uring_test_completion_t * completions, int max );
    void ( *advance )( void * context, int count );
    int ( *ready )( void * context );
    void ( *submitted )( void * context, uint64_t user_data );
};

struct rocketnet_server_uring_t * rocketnet_server_uring_test_create( void * context, int num_queues, const struct rocketnet_server_uring_test_source_t * source );

// the no-kernel-ring seam of the re-arm cases: a fake transport with queues and
// no ring, whose receive_packets runs only the re-arm loop and whose next
// count arm calls find no submission entry. wait_calls records attempted
// blocking waits for deterministic nonwaiting-shutdown regressions.

struct rocketnet_server_uring_t * rocketnet_server_uring_test_create_fake( int num_queues );
void rocketnet_server_uring_test_destroy_fake( struct rocketnet_server_uring_t * uring );
void rocketnet_server_uring_test_set_arm_failures( struct rocketnet_server_uring_t * uring, int count );
void rocketnet_server_uring_test_multishot_ends( struct rocketnet_server_uring_t * uring, int queue_index, bool arm_finds_no_sqe );
bool rocketnet_server_uring_test_queue_armed( struct rocketnet_server_uring_t * uring, int queue_index );
uint64_t rocketnet_server_uring_test_counter( struct rocketnet_server_uring_t * uring, int counter );
int rocketnet_server_uring_test_wait_calls( struct rocketnet_server_uring_t * uring );
#endif // #if ROCKETNET_DEV

int rocketnet_server_uring_num_queues( struct rocketnet_server_uring_t * uring );

void rocketnet_server_uring_counters( struct rocketnet_server_uring_t * uring, uint64_t * counters );

// send packets. the phase contract is the transport's: start, finish and
// abort may run on any number of game threads strictly between one
// send_packets call and the next; send_packets runs on the main thread,
// submits the staged slots, reaps at most ROCKETNET_URING_SEND_BUDGET
// currently available completions without waiting, and returns. a send
// without its completion keeps its slot, and the reservation index, until a
// later call reaps it (SEND_DEFERRED counts each call that returns so).
// the frame handed back is a ROCKETNET_FRAME_STRIDE slot: wire
// bytes begin at ROCKETNET_FRAME_WIRE_OFFSET, exactly as the other transports.

uint8_t * rocketnet_server_uring_start_packet( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_uring_finish_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_uring_abort_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_uring_send_packets( struct rocketnet_server_uring_t * uring );

// receive packets: receive_packets reaps at most
// ROCKETNET_URING_RECEIVE_BUDGET completions without blocking into the fill
// bank, then swaps the banks; process_packets exposes
// the swapped bank, valid until the next receive_packets call.

void rocketnet_server_uring_receive_packets( struct rocketnet_server_uring_t * uring );

struct rocketnet_server_socket_process_packets_t * rocketnet_server_uring_process_packets( struct rocketnet_server_uring_t * uring );

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_URING_H
