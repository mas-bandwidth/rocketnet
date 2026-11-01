/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_DRIVER_H
#define ROCKETNET_SDK_ROCKETNET_DRIVER_H

#include "rocketnet_internal.h"
#include "rocketnet_packet_batch.h"

// native implementation interface. None of these symbols is a public C export.
// Receive byte counts describe the original datagram, not bytes copied. A
// transport writes only min(reported bytes, supplied capacity/stride) bytes.
// Truncated UDP datagrams retain their length for prefilter size accounting;
// consumers must reject sizes beyond the wire ceiling before parsing bytes.
// Negative lengths and lengths beyond the non-jumbogram UDP envelope are faults.
#define ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES 65535

struct rocketnet_platform_send_t;

// a preference rank this large keeps a transport out of every auto order; a pin
// still selects it. The loopback transport is pin-only: it is chosen only by an
// explicit name, never by auto.
#define ROCKETNET_TRANSPORT_PIN_ONLY 1000

enum
{
#define ROCKETNET_TRANSPORT( symbol, name, discovers, operations, platforms, server, client ) ROCKETNET_TRANSPORT_##symbol,
#include "rocketnet_drivers.inc"
#undef ROCKETNET_TRANSPORT
    ROCKETNET_NUM_TRANSPORTS
};

enum rocketnet_server_transport_io_flags_t
{
    // the transport owns frame storage and receive views. Without this flag the
    // transport supplies its existing buffers to send_batch/receive_batch.
    ROCKETNET_TRANSPORT_STAGED = 1 << 0,

    // applies to staged transports. Buffered sends always complete their borrow
    // in the call, including the transport's control-traffic batch at update.
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE = 1 << 1,

    // a finished frame cannot be withdrawn; unfinished reservations can.
    ROCKETNET_TRANSPORT_FINISH_FINAL = 1 << 2,

    ROCKETNET_TRANSPORT_BINDS_PUBLIC_ADDRESS = 1 << 3,

    // applies to staged transports. A flush can leave work behind for a later
    // call (a retained producer, sends waiting for completions), so every send
    // boundary flushes again, with or without a new commit.
    ROCKETNET_TRANSPORT_REFLUSHES = 1 << 4
};

// status: where a failed create reports why, for server and client alike
// (NULL: not asked). A create that is refused an allocation writes
// ROCKETNET_STATUS_OUT_OF_MEMORY there, at the allocation, through
// rocketnet_create_status (sdk/rocketnet_internal.h), which never replaces an
// OUT_OF_MEMORY already written; the platform socket, wake and thread creates
// take the status for this. A NULL that leaves it OK is a refusal, and the
// transport ladder falls to the next candidate; OUT_OF_MEMORY ends the ladder
// and the create (sdk/rocketnet_server_transport.cpp,
// sdk/rocketnet_client_transport.cpp).
struct rocketnet_server_transport_io_config_t
{
    const char * name;
    const rocketnet_server_transport_config_t * transport;
    const char * public_address;
    int * status;
};

struct rocketnet_server_transport_io_operations_t
{
    unsigned int flags;

    // create borrows config only for the call. Failure releases every partial
    // resource. A successful discovering transport writes its actual bind address.
    void * ( *create )( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind );
    void ( *destroy )( void * transport );

    // main-thread request and completion check are idempotent and never wait
    // for workers. stopped may service a bounded batch of ready completions.
    // destroy requires stopped=true, then synchronously joins and frees.
    void ( *stop )( void * transport );
    bool ( *stopped )( void * transport );

    // staged path: start/finish/abort may run on concurrent fill threads.
    // Join them before receive, flush, stop or destroy. Each frame has one
    // producer. start returns a FRAME_STRIDE root, wire bytes at WIRE_OFFSET.
    // finish only seals the frame; flush does not promise reusable storage.
    // start/finish/abort/flush also stage the stop-time disconnect burst:
    // custom backends must not wait on workers, queue capacity or blocking I/O
    // in these callbacks. Refuse/drop or retain work for a later worker pass.
    // start returns NULL with the id ROCKETNET_TRANSPORT_PACKET_UNREACHABLE when
    // the transport has no link-layer destination for the address (the
    // transport counts SEND_NO_LINK_ADDRESS). Any other NULL is a full staging
    // when send_room reports no room, else a refusal of that one frame (an
    // address the socket cannot carry), which the transport counts in its
    // send_failures.
    uint8_t * ( *start_packet )( void *, const rocketnet_address_t *, uint64_t * );
    void ( *finish_packet )( void *, uint64_t, uint8_t *, int );
    void ( *abort_packet )( void *, uint64_t, uint8_t * );
    void ( *flush )( void * );

    // one complete, bounded drain. Views stay valid until the next receive or
    // destroy, including while background workers accept more packets.
    rocketnet_server_socket_process_packets_t * ( *receive )( void * );

    // buffered path: borrow caller storage only for this synchronous call.
    // A transport that retains buffers must use the staged path instead.
    // Ordinary batches may serialize with a receive worker. Shutdown uses
    // send_batch_shutdown when provided; that callback must return without
    // waiting for a worker or mutex, dropping on contention if necessary.
    // Without it, send_batch must also meet the shutdown requirement.
    void ( *send_batch )( void *, const rocketnet_platform_send_t *, int );
    int ( *receive_batch )( void *, rocketnet_address_t *, uint8_t *, int, int *, int );

    // optional; a transport with a kernel prefilter writes
    // ROCKETNET_SERVER_XDP_NUM_COUNTERS values in
    // ROCKETNET_SERVER_XDP_COUNTER_* order (sdk/rocketnet_wire.h), cumulative
    // since its filter was loaded; called on the main thread only.
    void ( *prefilter_counters )( void * transport, uint64_t * values );

    // optional, all three or none; main thread only. A transport with a kernel
    // replay map writes it from these and nothing else does. replay_forget
    // returns false when it cannot queue the forget, so the walk keeps its
    // slot record and tries again; replay_mark returns false the same way,
    // and the walk owes that mark and takes it again.
    bool ( *replay_forget )( void * transport, const rocketnet_address_t * address );
    bool ( *replay_mark )( void * transport, const rocketnet_address_t * address, uint32_t high_water );
    void ( *replay_commit )( void * transport );

    // optional; main thread only, while the receive view is current. The
    // transport accepted the view's packet view_index for a connection (it
    // opened under the connection's keys); a transport that addresses frames
    // itself learns the packet's link-layer source for its address.
    void ( *client_accepted )( void * transport, int view_index );

    // optional; any thread. The datagrams the transport's sends dropped on a
    // failed send call, cumulative for the transport's life; the transport
    // surfaces it as SEND_FAILED when its counters are read.
    uint64_t ( *send_failures )( void * transport );

    void ( *send_batch_shutdown )( void *, const rocketnet_platform_send_t *, int );

    // optional; main thread only. The frames the transport accepts before
    // its next flush without a refusal for room: start_packet on the staged
    // path, or the frames send_batch takes without dropping one on the
    // buffered path. A frame the transport still holds from an earlier flush
    // is not room. Producers on other threads only lower it, so with none
    // running it is exact; a transport without it is treated as unbounded
    // (proofs/tla/ServerStopSpread.tla Room).
    int ( *send_room )( void * transport );

    // optional; main thread only, never waits. True once every frame the
    // transport was handed has finished: nothing staged, published, queued
    // for a worker or waiting for its send to complete. A lock it cannot take
    // at once reads as not idle; it may retry its own deferred publish, as
    // stopped may service completions. Stop tells the I/O to stop only once
    // this is true, so no flushed send is cut off. A transport without it is
    // idle as soon as its flush returns (proofs/tla/ServerStopSpread.tla
    // RequestIoStop).
    bool ( *send_idle )( void * transport );

    // optional; main thread, once when shutdown begins, before the last
    // disconnect batches are staged. Does not stop admission or wait. A
    // buffered backend may switch retained sends to its terminal attempt.
    // stop still follows the final flush and send_idle.
    void ( *begin_shutdown )( void * transport );
};

// the start_packet id of a refusal for want of a link-layer destination
#define ROCKETNET_TRANSPORT_PACKET_UNREACHABLE    0xFFFFFFFFFFFFFFFFULL

// status: as rocketnet_server_transport_io_config_t.status
struct rocketnet_client_transport_io_config_t
{
    const char * name;
    const rocketnet_client_transport_config_t * transport;
    int * status;
};

// a backend with wait and wake runs the client I/O thread design of
// docs/CLIENT-IO.md; a backend without them keeps the receive thread and
// per-packet main-thread sends.
struct rocketnet_client_transport_io_operations_t
{
    void * ( *create )( void *, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * );
    void ( *destroy )( void * );
    void ( *send )( void *, const rocketnet_address_t *, const uint8_t *, int );
    int ( *receive )( void *, rocketnet_address_t *, uint8_t *, int );

    // optional; I/O thread only. Sends num_packets datagrams in one batch,
    // borrowing sends and the bytes they point at for the call only.
    void ( *send_batch )( void * transport, const rocketnet_platform_send_t * sends, int num_packets );

    // optional; I/O thread only. Drains up to max_packets into caller
    // storage at packet_data + i * packet_stride, writing from[i] and
    // packet_bytes[i]; returns the count, zero when drained.
    int ( *receive_batch )( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );

    // optional, with wake; I/O thread only. Blocks until a wake is set,
    // the socket is readable (only when readable is true), or
    // timeout_seconds pass; consumes the wake. With readable false the
    // socket's readiness is ignored. Returns true when woken or readable.
    bool ( *wait )( void * transport, bool readable, double timeout_seconds );

    // optional, with wait; any thread. Sets the level-triggered wake, so a
    // wait in progress or the next wait returns at once. Called by shutdown
    // requests, so custom wake callbacks must never wait for a worker/lock.
    void ( *wake )( void * transport );

    // optional; any thread. The datagrams the transport's sends dropped on a
    // failed send call, cumulative for the transport's life; the transport
    // surfaces it as SEND_FAILED when its counters are read.
    uint64_t ( *send_failures )( void * transport );

    // optional; main thread, once, at create, before the first send or
    // receive. Connects the transport's socket to the one server the connect
    // token selected, so the kernel drops datagrams from any other source
    // (the client trusts only the server, TRANSPORT.md t11); the transport
    // then sends only to that server. False refuses the transport. NULL for
    // a transport with no socket of its own to connect (loopback: the
    // caller's hub, shared by its endpoints).
    bool ( *connect )( void * transport, const rocketnet_address_t * server_address );
    // optional; the reading thread, once, after its last receive or wait on
    // the transport, with no other thread using it. Closes the transport's
    // operating-system handles (socket, wake, completion port) and frees
    // nothing: destroy, on the caller's thread, frees the memory later and
    // closes nothing more. The family race closes its losing family here
    // (sdk/rocketnet_client_transport.cpp, proofs/tla/RNClientRace.tla). NULL for a
    // transport with nothing to close before destroy.
    void ( *close )( void * transport );

    // optional, with send_batch; I/O thread only, instead of it while running.
    // Consumes an original input prefix (sent or terminally refused), in
    // [0,num_packets]. A transient refusal leaves the suffix caller-owned.
    // Borrows bytes only for this call; the core retains the ring for retry.
    // Shutdown uses send_batch for one final bounded attempt of the suffix.
    int ( *send_batch_progress )( void * transport, const rocketnet_platform_send_t * sends, int num_packets );
};

struct rocketnet_transport_backend_t
{
    const rocketnet_server_transport_io_operations_t * server;
    const rocketnet_client_transport_io_operations_t * client;
};

struct rocketnet_transport_definition_t
{
    const char * name;
    bool discovers_bound_port;
    unsigned int platforms;
    int server_preference;
    int client_preference;
    const rocketnet_transport_backend_t * backend;
};

struct rocketnet_server_transport_io_t
{
    const rocketnet_server_transport_io_operations_t * operations;
    void * instance;
};

struct rocketnet_client_transport_io_t
{
    const rocketnet_client_transport_io_operations_t * operations;
    void * instance;
};

const rocketnet_transport_definition_t * rocketnet_transport_definition( int transport );
const char * rocketnet_transport_name( int transport );
const char * rocketnet_transport_names();
const char * rocketnet_transport_discovering_names();
bool rocketnet_transport_discovers_bound_port( int transport );

// pin-only on the server side (a server's ladder, and the one-argument form)
bool rocketnet_transport_pin_only( int transport );

// pin-only for that endpoint: its own column of the registry row, so a
// transport can rank in one endpoint's auto order and be pin-only in the other
bool rocketnet_transport_pin_only( int transport, bool server );

const int * rocketnet_server_transport_io_availability( int * num_available );
const int * rocketnet_client_transport_io_availability( int * num_available );

// validate the contract once at creation, never in the packet path. Also used
// by native extension conformance tests before a backend enters the registry.
bool rocketnet_server_transport_io_open( const rocketnet_server_transport_io_operations_t * operations,
                                         void * context, const rocketnet_server_transport_io_config_t * config,
                                         rocketnet_address_t * bind, rocketnet_server_transport_io_t * transport );
void rocketnet_server_transport_io_close( rocketnet_server_transport_io_t * transport );
void rocketnet_server_transport_io_stop( rocketnet_server_transport_io_t * transport );
bool rocketnet_server_transport_io_stopped( const rocketnet_server_transport_io_t * transport );

bool rocketnet_client_transport_io_open( const rocketnet_client_transport_io_operations_t * operations,
                                         void * context, const rocketnet_client_transport_io_config_t * config,
                                         rocketnet_address_t * bind, rocketnet_client_transport_io_t * transport );
void rocketnet_client_transport_io_close( rocketnet_client_transport_io_t * transport );

inline bool rocketnet_server_transport_io_has( const rocketnet_server_transport_io_t & transport, unsigned int flag )
{
    return ( transport.operations->flags & flag ) != 0;
}

#endif // ROCKETNET_SDK_ROCKETNET_DRIVER_H
