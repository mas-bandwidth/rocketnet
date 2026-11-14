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
#include "rocketnet_network_threads.h"

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
// retired) leaves through the shared unconnected socket. One exception to
// the staged order: a bank whose publish was deferred across the client's
// socket opening or retiring holds that client under two stamps, and the
// shared socket's packets leave first (proofs/tla/MCRNMsgxSendRunsRestampOrder.cfg).
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

// the send threads (one transmit worker each; one more thread receives): the
// server's num_msgx_transmit_workers, its default and limit written in
// sdk/rocketnet_network_threads.h (space game's peak row holds at 3 and not
// at 2: peak with a game load failed at 2). Read by msgx only
// (tests/test_unit_msgx_default_workers.cpp)
#define ROCKETNET_MSGX_DEFAULT_WORKERS                                   ROCKETNET_MSGX_DEFAULT_TRANSMIT_WORKERS
#define ROCKETNET_MSGX_MAX_WORKERS                                       ROCKETNET_MSGX_MAX_TRANSMIT_WORKERS

// per worker per flush: 10 Gb/s of 1200 B at 60 Hz is about 17,400 starts a
// frame across the workers, so one worker's bank holds the frame's whole
// peak share (at the default 3 workers, or at 2) with room for the hash
// (space game's peak, 15 starts a client at 1000 clients, is 15,000). At 4096
// two workers refused 7,000 starts a frame (the XDP transport's per-queue
// staging needed the same 16384 for 2 queues)
#define ROCKETNET_MSGX_SEND_QUEUE_SIZE                               16384
#define ROCKETNET_MSGX_TX_BANKS                                          3

// one sendmsg_x carries at most this many datagrams of one client's run
#define ROCKETNET_MSGX_SEND_BATCH                                       64

// the receive worker's bank and its recvmsg_x batch. One bank holds one
// update's whole receive budget at the server's configured max_clients
// (rocketnet_server_receive_packets_per_update, 16000 at 1000 clients, the
// budget the Linux and Windows servers size theirs by), sized at create, so
// main takes an update's budget in one claim. The worker reads only into room
// (sdk/rocketnet_msgx_receive_fill.h): a read asks for the batch or the room
// left, whichever is less, so a bank smaller than the batch is read in
// smaller calls. With its bank full and the last one not yet taken by main it
// stops reading, the kernel's socket buffers hold the rest, and it sleeps
// HOLD_SECONDS before it looks again
#define ROCKETNET_MSGX_RECV_QUEUE_SIZE( max_clients )                 rocketnet_server_receive_packets_per_update( max_clients )
#define ROCKETNET_MSGX_RECV_BATCH                                       32
#define ROCKETNET_MSGX_RECV_EVENTS                                     256
#define ROCKETNET_MSGX_RECV_SOCKET_BUDGET                              256
#define ROCKETNET_MSGX_RECV_HOLD_SECONDS                            0.0005

// sockets opened in one update at most: a connect storm of 1000 clients
// opens over 16 updates instead of one long frame. Chosen, not measured.
// the most datagrams read off one retired socket when it is closed
#define ROCKETNET_MSGX_RETIRED_DRAIN_LIMIT                           65536

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

// what the transport did, rocketnet_server_msgx_stats: plain fields the log at
// destroy and the tests read; none of them is a public counter row (the
// transport's public rows come from its send failures and receive drop totals)
struct rocketnet_server_msgx_stats_t
{
    uint64_t packets_sent;                    // datagrams the kernel took
    uint64_t sent_connected;                  // of those, on a client's connected socket
    uint64_t sent_shared;                     // of those, on the shared socket
    uint64_t send_calls;                      // sendmsg_x and sendto calls
    uint64_t send_dropped;                    // refused by the kernel for good, or a full socket's tail after shutdown
    uint64_t send_refused;                    // ECONNREFUSED on a connected socket (an ICMP from the client): the run is sent again once
    uint64_t send_retired;                    // stamped for a socket retired before the send: sent on the shared socket
    uint64_t socket_dead;                     // a connected socket answered ENOTCONN or EPIPE and was reported dead
    uint64_t staging_full;                    // starts refused: the worker's producer bank was full
    uint64_t packets_received;                // datagrams banked for main
    uint64_t receive_calls;
    uint64_t receive_refused;                 // an ICMP error (ECONNREFUSED, host or network unreachable or down) on a read of a client's or the shared socket, read past
    uint64_t sockets_opened;
    uint64_t sockets_closed;
    uint64_t sockets_open;                    // now
    uint64_t socket_open_failed;
    uint64_t socket_open_deferred;
    uint64_t socket_table_full;
    uint64_t sockets_retired_forget;
    uint64_t sockets_retired_dead;
    uint64_t socket_cooldown;
    uint64_t sockets_in_quarantine;           // now
    uint64_t shared_receive_buffer;           // SO_RCVBUF bytes the kernel granted the shared socket (after any halving)
};

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

void rocketnet_server_msgx_stats( struct rocketnet_server_msgx_t * msgx, struct rocketnet_server_msgx_stats_t * stats );

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

#if ROCKETNET_DEV
// development library only. fail_sends makes the next count send calls
// (sendmsg_x on a client socket or sendto on the shared one, any worker)
// answer -1 with error without reaching the kernel. sockets_open is the
// table's open client sockets (main thread).
void rocketnet_server_msgx_test_fail_sends( struct rocketnet_server_msgx_t * msgx, int count, int error );
int rocketnet_server_msgx_test_sockets_open( struct rocketnet_server_msgx_t * msgx );
// receive_source drives one valid datagram from 127.0.0.1:port through the
// production drain of an IPv4 transport with a fake recvmsg_x and no
// socket: banked is what reached the worker bank, failed its RECEIVE_FAILED
bool rocketnet_server_msgx_test_receive_source( uint16_t port, int * banked, uint64_t * failed );
// test_allocate makes every allocation create makes before its sockets and
// threads (the transport, the process view, the socket table, the receiver
// and its banks, the send workers and their buckets) through the installed
// allocator and frees them: false when out of range or out of memory
bool rocketnet_server_msgx_test_allocate( int num_workers, int max_clients );
#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_MSGX_H
