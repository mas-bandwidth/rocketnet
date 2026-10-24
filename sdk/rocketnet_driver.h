/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#include "rocketnet_internal.h"
#include "rocketnet_packet_batch.h"

// Native implementation interface. None of these symbols is a public C export.
// Receive byte counts describe the original datagram, not bytes copied. A
// transport writes only min(reported bytes, supplied capacity/stride) bytes.
// Truncated UDP datagrams retain their length for prefilter size accounting;
// consumers must reject sizes beyond the wire ceiling before parsing bytes.
// Negative lengths and lengths beyond the non-jumbogram UDP envelope are faults.
#define ROCKETNET_TRANSPORT_MAX_DATAGRAM_BYTES 65535
struct rocketnet_platform_send_t;

// A preference rank this large keeps a transport out of every auto order; a pin
// still selects it. The loopback transport is pin-only: it is chosen only by an
// explicit name, never by auto.
#define ROCKETNET_TRANSPORT_PIN_ONLY 1000

enum
{
#define ROCKETNET_TRANSPORT(symbol, name, discovers, operations, platforms, server, client) ROCKETNET_TRANSPORT_##symbol,
#include "rocketnet_drivers.inc"
#undef ROCKETNET_TRANSPORT
    ROCKETNET_NUM_TRANSPORTS
};

enum rocketnet_server_transport_io_flags_t
{
    // The transport owns frame storage and receive views. Without this flag the
    // transport supplies its existing buffers to send_batch/receive_batch.
    ROCKETNET_TRANSPORT_STAGED = 1 << 0,
    // Applies to staged transports. Buffered sends always complete their borrow
    // in the call, including the transport's control-traffic batch at update.
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE = 1 << 1,
    // A finished frame cannot be withdrawn; unfinished reservations can.
    ROCKETNET_TRANSPORT_FINISH_FINAL = 1 << 2,
    ROCKETNET_TRANSPORT_BINDS_PUBLIC_ADDRESS = 1 << 3
};

// status: where a failed open reports why, for server and client alike
// (NULL: not asked). rocketnet_server_transport_io_open and
// rocketnet_client_transport_io_open write ROCKETNET_STATUS_OUT_OF_MEMORY there when
// create returned NULL after an allocation on the calling thread was refused
// (sdk/rocketnet_driver.cpp:126-130, :148 and :182): a rocketnet_malloc
// refusal, or a transport's own page buffer counted by
// rocketnet_allocation_refused (sdk/rocketnet_server_socket_linux.cpp:1720,
// sdk/rocketnet_server_rio_windows.cpp:508). Any other NULL leaves it OK and
// is a refusal, and the transport ladder falls to the next candidate;
// OUT_OF_MEMORY ends the ladder and the create
// (sdk/rocketnet_server_transport.cpp:1299, sdk/rocketnet_client_transport.cpp:882).
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
    void * (*create)( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind );
    void (*destroy)( void * transport );
    // Main-thread request and completion check are idempotent and never wait
    // for workers. stopped may service a bounded batch of ready completions.
    // destroy requires stopped=true, then synchronously joins and frees.
    void (*stop)( void * transport );
    bool (*stopped)( void * transport );

    // Staged path: start/finish/abort may run on concurrent fill threads.
    // Join them before receive, flush, stop or destroy. Each frame has one
    // producer. start returns a FRAME_STRIDE root, wire bytes at WIRE_OFFSET.
    // finish only seals the frame; flush does not promise reusable storage.
    // start/finish/abort/flush also stage the stop-time disconnect burst:
    // custom backends must not wait on workers, queue capacity or blocking I/O
    // in these callbacks. Refuse/drop or retain work for a later worker pass.
    // start returns NULL with the id ROCKETNET_TRANSPORT_PACKET_UNREACHABLE when
    // the transport has no link-layer destination for the address (the
    // transport counts SEND_NO_LINK_ADDRESS); any other NULL is a full ring.
    uint8_t * (*start_packet)( void *, const rocketnet_address_t *, uint64_t * );
    void (*finish_packet)( void *, uint64_t, uint8_t *, int );
    void (*abort_packet)( void *, uint64_t, uint8_t * );
    void (*flush)( void * );
    // One complete, bounded drain. Views stay valid until the next receive or
    // destroy, including while background workers accept more packets.
    rocketnet_server_socket_process_packets_t * (*receive)( void * );

    // Buffered path: borrow caller storage only for this synchronous call.
    // A transport that retains buffers must use the staged path instead.
    // Ordinary batches may serialize with a receive worker. Shutdown uses
    // send_batch_shutdown when provided; that callback must return without
    // waiting for a worker or mutex, dropping on contention if necessary.
    // Without it, send_batch must also meet the shutdown requirement.
    void (*send_batch)( void *, const rocketnet_platform_send_t *, int );
    int (*receive_batch)( void *, rocketnet_address_t *, uint8_t *, int, int *, int );
    // optional; a transport with a kernel prefilter writes
    // ROCKETNET_SERVER_XDP_NUM_COUNTERS values in
    // ROCKETNET_SERVER_XDP_COUNTER_* order (sdk/rocketnet_wire.h), cumulative
    // since its filter was loaded; called on the main thread only.
    void (*prefilter_counters)( void * transport, uint64_t * values );
    // optional, all three or none; main thread only. A transport with a kernel
    // replay map writes it from these and nothing else does.
    void (*replay_forget)( void * transport, const rocketnet_address_t * address );
    void (*replay_mark)( void * transport, const rocketnet_address_t * address, uint32_t high_water );
    void (*replay_commit)( void * transport );
    // optional; main thread only, while the receive view is current. The
    // transport accepted the view's packet view_index for a connection (it
    // opened under the connection's keys); a transport that addresses frames
    // itself learns the packet's link-layer source for its address.
    void (*client_accepted)( void * transport, int view_index );
    // optional; any thread. The datagrams the transport's sends dropped on a
    // failed send call, cumulative for the transport's life; the transport
    // surfaces it as SEND_FAILED when its counters are read.
    uint64_t (*send_failures)( void * transport );
    void (*send_batch_shutdown)( void *, const rocketnet_platform_send_t *, int );
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

// A backend with wait and wake runs the client I/O thread design of
// docs/CLIENT-IO.md; a backend without them keeps the receive thread and
// per-packet main-thread sends.
struct rocketnet_client_transport_io_operations_t
{
    void * (*create)( void *, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * );
    void (*destroy)( void * );
    void (*send)( void *, const rocketnet_address_t *, const uint8_t *, int );
    int (*receive)( void *, rocketnet_address_t *, uint8_t *, int );
    // optional; I/O thread only. Sends num_packets datagrams in one batch,
    // borrowing sends and the bytes they point at for the call only.
    void (*send_batch)( void * transport, const rocketnet_platform_send_t * sends, int num_packets );
    // optional; I/O thread only. Drains up to max_packets into caller
    // storage at packet_data + i * packet_stride, writing from[i] and
    // packet_bytes[i]; returns the count, zero when drained.
    int (*receive_batch)( void * transport, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );
    // optional, with wake; I/O thread only. Blocks until a wake is set,
    // the socket is readable (only when readable is true), or
    // timeout_seconds pass; consumes the wake. With readable false the
    // socket's readiness is ignored. Returns true when woken or readable.
    bool (*wait)( void * transport, bool readable, double timeout_seconds );
    // optional, with wait; any thread. Sets the level-triggered wake, so a
    // wait in progress or the next wait returns at once. Called by shutdown
    // requests, so custom wake callbacks must never wait for a worker/lock.
    void (*wake)( void * transport );
    // optional; any thread. The datagrams the transport's sends dropped on a
    // failed send call, cumulative for the transport's life; the transport
    // surfaces it as SEND_FAILED when its counters are read.
    uint64_t (*send_failures)( void * transport );
    // optional; main thread, once, at create, before the first send or
    // receive. Connects the transport's socket to the one server the connect
    // token selected, so the kernel drops datagrams from any other source
    // (the client trusts only the server, TRANSPORT.md t11); the transport
    // then sends only to that server. False refuses the transport. NULL for
    // a transport with no socket of its own to connect (loopback: one
    // in-process router shared by every endpoint).
    bool (*connect)( void * transport, const rocketnet_address_t * server_address );
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

// The allocations rocketnet_malloc has refused on the calling thread, ever
// (defined in sdk/rocketnet.cpp beside rocketnet_malloc).
uint64_t rocketnet_allocation_refusals();

// Counts one failed allocation made outside rocketnet_malloc on the calling
// thread, so a transport open reports OUT_OF_MEMORY for it (sdk/rocketnet.cpp).
void rocketnet_allocation_refused();

// Validate the contract once at creation, never in the packet path. Also used
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
