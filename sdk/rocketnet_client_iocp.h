/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The iocp transport's client side on Windows (sdk/rocketnet_client_iocp_windows.cpp).
//
// One socket, connected to its one server (TRANSPORT.md t11), associated with
// a completion port of its own: the client's port is never the server's
// (sdk/rocketnet_server_iocp_windows.cpp keeps its own, drained by its one
// completion worker). A fixed set of ROCKETNET_CLIENT_IOCP_RECEIVES overlapped
// WSARecvFrom receives is posted at create, into buffers allocated at create.
// The client I/O thread of docs/CLIENT-IO.md (tla/RNClientIo.tla,
// tla/RNBufferHandoff.tla) owns the socket and the port:
//
//   receive_batch  dequeues the completions on the port without waiting,
//                  copies each completed receive into the caller's slots and
//                  re-posts its buffer after the copy
//   wait           GetQueuedCompletionStatusEx with the timeout: returns when
//                  a receive completes (readable true) or the wake arrives
//   wake           PostQueuedCompletionStatus with the wake key, any thread;
//                  one wake completion is outstanding at most
//   send           synchronous WSASendTo on the connected socket
//   destroy        closes the socket, then dequeues until every posted
//                  receive has completed (closesocket aborts them), and only
//                  then frees the buffers
//
// The posted-receive ownership (the kernel owns a buffer from its post until
// its completion is dequeued) is the state tla/RNClientIocp.tla adds to
// RNClientIo: a buffer is never copied while posted, and destroy frees only
// when every posted receive is drained (FreedOnlyWhenDrained).
//
// The core below reaches the port only through rocketnet_client_iocp_port_t,
// so the unit cases (tests/test_unit_client_iocp.cpp) run it on any OS with
// a fake completion port: no socket, no thread, no time.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CLIENT_IOCP_H
#define ROCKETNET_SDK_ROCKETNET_CLIENT_IOCP_H

#include "rocketnet_internal.h"
#include "rocketnet_address.h"
#include "rocketnet_wire.h"

#include <stdint.h>
#include <atomic>

struct rocketnet_client_transport_io_operations_t;

// overlapped receives posted at create: one client I/O receive batch
// (ROCKETNET_CLIENT_IO_BATCH)
#define ROCKETNET_CLIENT_IOCP_RECEIVES                                      64
// bytes in one receive buffer: the largest datagram the client accepts
#define ROCKETNET_CLIENT_IOCP_RECEIVE_BYTES                ROCKETNET_RECEIVE_CEILING
// a destroy that has not drained after this many dequeues of
// ROCKETNET_CLIENT_IOCP_DRAIN_SECONDS leaks its buffers rather than free
// memory the kernel may still write
#define ROCKETNET_CLIENT_IOCP_DRAIN_WAITS                                   10
#define ROCKETNET_CLIENT_IOCP_DRAIN_SECONDS                                1.0

enum rocketnet_client_iocp_completion_kind_t
{
    ROCKETNET_CLIENT_IOCP_COMPLETION_RECEIVE,
    ROCKETNET_CLIENT_IOCP_COMPLETION_WAKE,
};

// One dequeued completion. For a receive: which receive, whether it carried a
// datagram (false: an error, an abort or an unreadable source), its bytes and
// its source.
struct rocketnet_client_iocp_completion_t
{
    int kind;
    int receive;
    bool ok;
    int packet_bytes;
    rocketnet_address_t from;
};

// The completion-port seam.
//   post_receive  starts an overlapped receive into buffer; false: refused,
//                 no I/O started and no completion will come
//   dequeue       dequeues up to max_completions, waiting up to
//                 timeout_seconds for the first (zero: does not wait)
//   post_wake     posts one wake completion; any thread
//   close         closes the socket: every posted receive then completes
struct rocketnet_client_iocp_port_t
{
    bool ( *post_receive )( void * context, int receive, uint8_t * buffer, int buffer_bytes );
    int ( *dequeue )( void * context, rocketnet_client_iocp_completion_t * completions, int max_completions, double timeout_seconds );
    void ( *post_wake )( void * context );
    void ( *close )( void * context );
};

// a receive's owner: idle (the I/O thread, not posted), posted (the kernel)
// or ready (completed and dequeued, the I/O thread may copy it)
enum rocketnet_client_iocp_receive_state_t
{
    ROCKETNET_CLIENT_IOCP_RECEIVE_IDLE,
    ROCKETNET_CLIENT_IOCP_RECEIVE_POSTED,
    ROCKETNET_CLIENT_IOCP_RECEIVE_READY,
};

// The posted receives' state, all sized here; the buffers are the caller's,
// allocated at create. I/O thread only, but for wake_posted.
struct rocketnet_client_iocp_t
{
    const rocketnet_client_iocp_port_t * port;
    void * port_context;
    uint8_t * buffers;                                  // num_receives * buffer_bytes
    int buffer_bytes;
    int num_receives;
    int num_posted;                                     // receives the kernel owns
    int num_idle;                                       // receives a refused post left idle
    bool closed;
    bool woken;                                         // a wake dequeued outside wait, owed to the next wait
    std::atomic<bool> wake_posted;                      // a wake completion is on the port

    uint64_t receives_completed;                        // datagrams copied to the caller
    uint64_t receives_failed;                           // completions that carried no datagram
    uint64_t post_refusals;                             // posts refused, retried later

    int ready_head;                                     // ring of ready receives in completion order
    int ready_count;
    int ready[ROCKETNET_CLIENT_IOCP_RECEIVES];
    uint8_t state[ROCKETNET_CLIENT_IOCP_RECEIVES];
    int packet_bytes[ROCKETNET_CLIENT_IOCP_RECEIVES];
    rocketnet_address_t from[ROCKETNET_CLIENT_IOCP_RECEIVES];
    rocketnet_client_iocp_completion_t completions[ROCKETNET_CLIENT_IOCP_RECEIVES + 1];
};

// Posts every receive (tla/RNClientIocp.tla Post). Returns the receives
// posted; a refused one stays idle and is posted again by receive_batch.
int rocketnet_client_iocp_init( rocketnet_client_iocp_t * iocp, const rocketnet_client_iocp_port_t * port, void * port_context,
                                uint8_t * buffers, int buffer_bytes, int num_receives );

// Drains completed receives into packet_data + i * packet_stride, from[i] and
// packet_bytes[i] (a datagram larger than packet_stride reports its true size
// and stores its prefix), re-posting each buffer after its copy. Returns the
// count, zero when drained. Never waits.
int rocketnet_client_iocp_receive_batch( rocketnet_client_iocp_t * iocp, rocketnet_address_t * from, uint8_t * packet_data,
                                         int packet_stride, int * packet_bytes, int max_packets );

// Waits up to timeout_seconds for the wake, or (readable true) a completed
// receive. Consumes the wake. Returns true when woken or readable.
bool rocketnet_client_iocp_wait( rocketnet_client_iocp_t * iocp, bool readable, double timeout_seconds );

// Sets the level-triggered wake; any thread.
void rocketnet_client_iocp_wake( rocketnet_client_iocp_t * iocp );

// Closes the socket and dequeues until no receive is posted, at most
// max_waits dequeues of wait_seconds each. Returns true when drained: only
// then may the buffers be freed.
bool rocketnet_client_iocp_close( rocketnet_client_iocp_t * iocp, int max_waits, double wait_seconds );

// The iocp client transport operations, the client column of the iocp row.
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
extern const rocketnet_client_transport_io_operations_t rocketnet_client_iocp_operations;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

#endif // ROCKETNET_SDK_ROCKETNET_CLIENT_IOCP_H
