/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_URING_H
#define ROCKETNET_SERVER_URING_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"
#include "rocketnet_packet_batch.h"

// The io_uring server driver: one io_uring per driver, used only on the main
// thread, and no worker thread. An SO_REUSEPORT socket group (bound the way
// the gso driver binds its group) shards receives across queues; each queue
// has a provided buffer ring and one multishot RECVMSG, so the kernel fills
// receive buffers with no syscall per datagram, and send_packets submits one
// SENDMSG per staged slot in chunks no larger than the free submission queue
// space. tla/RNUringDriver.tla models the send staging, the two receive banks
// and destroy; the functions that implement a model action cite it.
//
// The driver is built only when the library is configured with liburing
// (ROCKETNET_IO_URING, Linux). Without it, create logs the refusal and
// returns NULL, and rocketnet_server_uring_available() is false.

struct rocketnet_server_uring_t;

// the driver's geometry, all compile time:

#ifndef ROCKETNET_URING_DEFAULT_QUEUES
#define ROCKETNET_URING_DEFAULT_QUEUES                                       4
#endif

#define ROCKETNET_URING_MAX_QUEUES                                          32

// submission queue entries; the completion queue is twice this

#define ROCKETNET_URING_ENTRIES                                           4096

// send staging: one phase's worth of frame-stride slots, the gso driver's
// budget

#define ROCKETNET_URING_SEND_QUEUE_SIZE                           ( 4096 * 4 )

// receive: per queue a provided buffer ring of this many buffers of this many
// bytes. one buffer holds the multishot recvmsg header, the reserved name
// area and the payload, so a buffer is larger than the receive ceiling.

#define ROCKETNET_URING_RECV_BUFFERS                                      4096
#define ROCKETNET_URING_RECV_BUFFER_BYTES                                 2048

// counters, readable any time from the main thread; indexes into the array
// rocketnet_server_uring_counters fills

#define ROCKETNET_SERVER_URING_COUNTER_PACKETS_SENT                          0
#define ROCKETNET_SERVER_URING_COUNTER_PACKETS_RECEIVED                      1
#define ROCKETNET_SERVER_URING_COUNTER_SUBMIT_SYSCALLS                       2      // io_uring_enter calls: submits and waits
#define ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED                          3      // a send CQE with a negative result, or a destination the socket family cannot carry
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SIZE                  4      // datagram outside [server min, receive ceiling], or truncated in its buffer
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_OVERFLOW              5      // the fill bank was full
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_DROPPED_SOURCE                6      // source address the boundary could not convert
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_NO_BUFFERS                    7      // the multishot ended because the buffer ring was empty (ENOBUFS)
#define ROCKETNET_SERVER_URING_COUNTER_RECEIVE_REARMS                        8      // multishot receives armed again after a CQE without F_MORE
#define ROCKETNET_SERVER_URING_NUM_COUNTERS                                  9

// true when the library was built with liburing and this kernel accepts the
// ring, the opcodes and the provided buffer ring the driver needs. the probe
// runs once and its answer is kept.

bool rocketnet_server_uring_available();

// the bytes create allocates for a queue count, 0 for a count out of range

uint64_t rocketnet_server_uring_allocation_bytes( int queues );

// bind_address is read for family and port and written back when the port
// was zero, the contract of rocketnet_platform_socket_create. an ipv6 bind is
// dual stack: ipv4 senders arrive as native ipv4 addresses. num_queues <= 0
// selects the default; a count past ROCKETNET_URING_MAX_QUEUES refuses.

struct rocketnet_server_uring_t * rocketnet_server_uring_create( void * context, struct rocketnet_address_t * bind_address, int num_queues );

// destroy cancels every multishot receive and waits, bounded, for every
// outstanding completion before freeing. a destroy that gives up waiting
// closes the ring and the sockets but leaks the memory the kernel may still
// write, and counts it here: bytes, and drivers so leaked, since the start.

void rocketnet_server_uring_destroy( struct rocketnet_server_uring_t * uring );

uint64_t rocketnet_server_uring_leaked_bytes();

uint64_t rocketnet_server_uring_leaked_drivers();

// for tests: one completion that never arrives, so the next destroy counts
// its rounds (without waiting) and takes the give-up path

void rocketnet_server_uring_test_stall_destroy( struct rocketnet_server_uring_t * uring );

#if ROCKETNET_DEVELOPMENT
// for tests: end queue 0's multishot receive now with a terminating CQE that
// reports -error, as the kernel does on an errno; datagrams sent after the
// call queue behind the error and the next receive_packets must deliver them.
// development library only.

void rocketnet_server_uring_inject_recv_error_for_testing( struct rocketnet_server_uring_t * uring, int error );
#endif // #if ROCKETNET_DEVELOPMENT

int rocketnet_server_uring_num_queues( struct rocketnet_server_uring_t * uring );

void rocketnet_server_uring_counters( struct rocketnet_server_uring_t * uring, uint64_t * counters );

// send packets. the phase contract is the transport's: start, finish and
// abort may run on any number of game threads strictly between one
// send_packets call and the next; send_packets runs on the main thread,
// submits every staged slot and returns once every one of its sends has
// completed. the frame handed back is a ROCKETNET_FRAME_STRIDE slot: wire
// bytes begin at ROCKETNET_FRAME_WIRE_OFFSET, exactly as the other drivers.

uint8_t * rocketnet_server_uring_start_packet( struct rocketnet_server_uring_t * uring, const struct rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_uring_finish_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_uring_abort_packet( struct rocketnet_server_uring_t * uring, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_uring_send_packets( struct rocketnet_server_uring_t * uring );

// receive packets: receive_packets drains the completion queue without
// blocking into the fill bank, then swaps the banks; process_packets exposes
// the swapped bank, valid until the next receive_packets call.

void rocketnet_server_uring_receive_packets( struct rocketnet_server_uring_t * uring );

struct rocketnet_server_socket_process_packets_t * rocketnet_server_uring_process_packets( struct rocketnet_server_uring_t * uring );

#endif // #ifndef ROCKETNET_SERVER_URING_H
