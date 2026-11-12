/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_MSGX_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_MSGX_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_packet_batch.h"

// The macOS batched server transport (msgx), staged. macOS spends about 12 us
// of kernel time per datagram sent on an unconnected socket to many
// destinations, and about 1.9 us on a connected socket with sendmsg_x
// (final-fable MAC-LINE-RATE.md 3b), so this transport keeps one socket per
// connected client, bound to the server's port with SO_REUSEADDR and
// SO_REUSEPORT and connected to the client (the table:
// sdk/rocketnet_msgx_socket_table.h, proofs/tla/RNMsgxSocketTable.tla).
//
// Send: the game's fill threads stage into per-worker banks, the worker
// chosen by the destination hash every per-queue transport uses, so a
// client's packets leave through one worker in the order staged. Main's
// flush only hands each worker its bank and wakes it. Each transmit worker
// groups a bank by client socket, keeping staged order, and sends each
// client's run with one sendmsg_x on its connected socket. A destination
// with no socket (a handshake, a client whose socket is not open yet or was
// retired) leaves through the shared unconnected socket.
//
// Receive: once a client has its socket the kernel delivers its datagrams to
// that socket only, so the one receive worker reads every socket through a
// kqueue, runs the stateless prefilter before banking (every drop counted by
// name, sdk/rocketnet_receive_prefilter.h) and hands banks to main.
//
// The sockets open and close on the server's replay walk: mark (a connected
// client seen at an address) opens, forget (it left or moved) retires, and a
// retired socket is closed once no worker can still hold it.

struct rocketnet_server_msgx_t;

#ifndef ROCKETNET_MSGX_DEFAULT_WORKERS
#define ROCKETNET_MSGX_DEFAULT_WORKERS                                   4
#endif // #ifndef ROCKETNET_MSGX_DEFAULT_WORKERS

#define ROCKETNET_MSGX_MAX_WORKERS                                      16

// per worker per flush, as the XDP transport's per-queue staging: 10 Gb/s of
// 1200 B at 60 Hz is about 17,400 starts a frame across the workers
#define ROCKETNET_MSGX_SEND_QUEUE_SIZE                                4096
#define ROCKETNET_MSGX_TX_BANKS                                          3

// one sendmsg_x carries at most this many datagrams of one client's run
#define ROCKETNET_MSGX_SEND_BATCH                                       64

// the receive worker's bank and its recvmsg_x batch
#define ROCKETNET_MSGX_RECV_QUEUE_SIZE                                8192
#define ROCKETNET_MSGX_RECV_BATCH                                       32
#define ROCKETNET_MSGX_RECV_EVENTS                                     256
#define ROCKETNET_MSGX_RECV_SOCKET_BUDGET                              256

// sockets opened in one update at most: a connect storm of 1000 clients
// opens over 16 updates instead of one long frame. Chosen, not measured.
#define ROCKETNET_MSGX_OPENS_PER_UPDATE                                 64

// descriptors the process keeps beyond one per client
#define ROCKETNET_MSGX_DESCRIPTOR_SLACK                                256

// each connected socket's buffers: a client's run of a frame (about 16
// datagrams of 1200 B) fits the send buffer with room
#define ROCKETNET_MSGX_CLIENT_SOCKET_BUFFER                          65536

// a full socket keeps the worker's bank; this is how long the worker waits
// for main's wake before it tries again. After shutdown begins it retries
// this many more times, then drops the tail counted.
#define ROCKETNET_MSGX_SEND_RETRY_SECONDS                            0.001
#define ROCKETNET_MSGX_SEND_RETRIES_AFTER_QUIT                         100

// counters, rocketnet_server_msgx_counters
#define ROCKETNET_SERVER_MSGX_COUNTER_PACKETS_SENT                       0  // datagrams the kernel took
#define ROCKETNET_SERVER_MSGX_COUNTER_SENT_CONNECTED                     1  // of those, on a client's connected socket
#define ROCKETNET_SERVER_MSGX_COUNTER_SENT_SHARED                        2  // of those, on the shared socket
#define ROCKETNET_SERVER_MSGX_COUNTER_SEND_CALLS                         3  // sendmsg_x and sendto calls
#define ROCKETNET_SERVER_MSGX_COUNTER_SEND_DROPPED                       4  // refused by the kernel for good, or a full socket's tail after shutdown
#define ROCKETNET_SERVER_MSGX_COUNTER_SEND_REFUSED                       5  // ECONNREFUSED on a connected socket (an ICMP from the client): the run is sent again once
#define ROCKETNET_SERVER_MSGX_COUNTER_SEND_RETIRED                       6  // stamped for a socket retired before the send: sent on the shared socket
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKET_DEAD                        7  // a connected socket answered ENOTCONN or EPIPE and was reported dead
#define ROCKETNET_SERVER_MSGX_COUNTER_STAGING_FULL                       8  // starts refused: the worker's producer bank was full
#define ROCKETNET_SERVER_MSGX_COUNTER_PACKETS_RECEIVED                   9  // datagrams banked for main
#define ROCKETNET_SERVER_MSGX_COUNTER_RECEIVE_CALLS                     10
#define ROCKETNET_SERVER_MSGX_COUNTER_RECEIVE_REFUSED                   11  // ECONNREFUSED on a read of a connected socket
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKETS_OPENED                    12
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKETS_CLOSED                    13
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKETS_OPEN                      14  // now
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKET_OPEN_FAILED                15
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKET_OPEN_DEFERRED              16
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKET_TABLE_FULL                 17
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKETS_RETIRED_FORGET            18
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKETS_RETIRED_DEAD              19
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKET_COOLDOWN                   20
#define ROCKETNET_SERVER_MSGX_COUNTER_SOCKETS_IN_QUARANTINE             21  // now
#define ROCKETNET_SERVER_MSGX_NUM_COUNTERS                              22

// bind_address as rocketnet_platform_socket_create's: written back with the
// bound port. num_workers <= 0 selects the default. NULL with status left OK
// is a refusal (sendmsg_x/recvmsg_x missing, or the descriptor limit cannot
// hold a socket per client): the server falls to its next transport.
struct rocketnet_server_msgx_t * rocketnet_server_msgx_create( void * context, struct rocketnet_address_t * bind_address, int num_workers,
                                                             int max_clients, const uint8_t * packet_filter, int * status = NULL );

void rocketnet_server_msgx_stop( struct rocketnet_server_msgx_t * msgx );
bool rocketnet_server_msgx_stopped( struct rocketnet_server_msgx_t * msgx );
void rocketnet_server_msgx_begin_shutdown( struct rocketnet_server_msgx_t * msgx );

// every allocator byte create requests, overhead included; 0 out of range
uint64_t rocketnet_server_msgx_allocation_bytes( int num_workers, int max_clients );

// requires stopped; joins and frees
void rocketnet_server_msgx_destroy( struct rocketnet_server_msgx_t * msgx );

void rocketnet_server_msgx_counters( struct rocketnet_server_msgx_t * msgx, uint64_t * counters );

// the staged send path: start, finish and abort on any number of fill
// threads strictly between flushes, one client's packets from one thread
uint8_t * rocketnet_server_msgx_start_packet( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * to, uint64_t * packet_id );
void rocketnet_server_msgx_finish_packet( struct rocketnet_server_msgx_t * msgx, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );
void rocketnet_server_msgx_abort_packet( struct rocketnet_server_msgx_t * msgx, uint64_t packet_id, uint8_t * packet_data );
void rocketnet_server_msgx_send_packets( struct rocketnet_server_msgx_t * msgx );

int rocketnet_server_msgx_send_room( struct rocketnet_server_msgx_t * msgx );
bool rocketnet_server_msgx_send_idle( struct rocketnet_server_msgx_t * msgx );
uint64_t rocketnet_server_msgx_send_failures( struct rocketnet_server_msgx_t * msgx );

int rocketnet_server_msgx_send_queues( struct rocketnet_server_msgx_t * msgx );
int rocketnet_server_msgx_send_queue_of( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * to );
int rocketnet_server_msgx_send_room_queue( struct rocketnet_server_msgx_t * msgx, int queue );

void rocketnet_server_msgx_receive_packets( struct rocketnet_server_msgx_t * msgx );
struct rocketnet_server_socket_process_packets_t * rocketnet_server_msgx_process_packets( struct rocketnet_server_msgx_t * msgx );
void rocketnet_server_msgx_receive_drop_totals( struct rocketnet_server_msgx_t * msgx, uint64_t * values );

// the socket table's feed from the server's replay walk (main thread):
// mark opens a connected client's socket, forget retires it, commit sweeps
// the quarantine and readies the next update's open budget
bool rocketnet_server_msgx_mark( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * address );
bool rocketnet_server_msgx_forget( struct rocketnet_server_msgx_t * msgx, const struct rocketnet_address_t * address );
void rocketnet_server_msgx_commit( struct rocketnet_server_msgx_t * msgx );

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_MSGX_H
