/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#pragma once

#include "rocketnet_internal.h"
#include "rocketnet_packet_batch.h"

// Native implementation interface. None of these symbols is a public C export.
struct rocketnet_platform_send_t;

// A preference rank this large keeps a driver out of every auto order; a pin
// still selects it. The loopback driver is pin-only: it is chosen only by an
// explicit name, never by auto.
#define ROCKETNET_DRIVER_PIN_ONLY 1000

enum
{
#define ROCKETNET_DRIVER(symbol, name, discovers, operations, platforms, server, client) ROCKETNET_DRIVER_##symbol,
#include "rocketnet_drivers.inc"
#undef ROCKETNET_DRIVER
    ROCKETNET_NUM_DRIVERS
};

enum rocketnet_server_driver_flags_t
{
    // The driver owns frame storage and receive views. Without this flag the
    // transport supplies its existing buffers to send_batch/receive_batch.
    ROCKETNET_DRIVER_STAGED = 1 << 0,
    // Applies to staged drivers. Buffered sends always complete their borrow
    // in the call, including the transport's control-traffic batch at update.
    ROCKETNET_DRIVER_FLUSH_ON_UPDATE = 1 << 1,
    // A finished frame cannot be withdrawn; unfinished reservations can.
    ROCKETNET_DRIVER_FINISH_FINAL = 1 << 2,
    ROCKETNET_DRIVER_BINDS_PUBLIC_ADDRESS = 1 << 3
};

struct rocketnet_server_driver_config_t
{
    const char * name;
    const rocketnet_server_transport_config_t * transport;
    const char * public_address;
};

struct rocketnet_server_driver_operations_t
{
    unsigned int flags;
    // create borrows config only for the call. Failure releases every partial
    // resource. A successful discovering driver writes its actual bind address.
    void * (*create)( void * context, const rocketnet_server_driver_config_t * config, rocketnet_address_t * bind );
    void (*destroy)( void * driver );
    // Main-thread stop/poll is idempotent. False requires another poll, without
    // requiring any particular update cadence. destroy also handles a driver
    // that was created but never updated/stopped and waits for owned I/O.
    bool (*stop)( void * driver );

    // Staged path: start/finish/abort may run on concurrent fill threads.
    // Join them before receive, flush, stop or destroy. Each frame has one
    // producer. start returns a FRAME_STRIDE root, wire bytes at WIRE_OFFSET.
    // finish only seals the frame; flush does not promise reusable storage.
    // start returns NULL with the id ROCKETNET_DRIVER_PACKET_UNREACHABLE when
    // the driver has no link-layer destination for the address (the
    // transport counts SEND_NO_LINK_ADDRESS); any other NULL is a full ring.
    uint8_t * (*start_packet)( void *, const rocketnet_address_t *, uint64_t * );
    void (*finish_packet)( void *, uint64_t, uint8_t *, int );
    void (*abort_packet)( void *, uint64_t, uint8_t * );
    void (*flush)( void * );
    // One complete, bounded drain. Views stay valid until the next receive or
    // destroy, including while background workers accept more packets.
    rocketnet_server_socket_process_packets_t * (*receive)( void * );

    // Buffered path: borrow caller storage only for this synchronous call.
    // A driver that retains buffers must use the staged path instead.
    void (*send_batch)( void *, const rocketnet_platform_send_t *, int );
    int (*receive_batch)( void *, rocketnet_address_t *, uint8_t *, int, int *, int );
    // optional; a driver with a kernel prefilter writes
    // ROCKETNET_SERVER_XDP_NUM_COUNTERS values in
    // ROCKETNET_SERVER_XDP_COUNTER_* order (sdk/rocketnet_wire.h), cumulative
    // since its filter was loaded; called on the main thread only.
    void (*prefilter_counters)( void * driver, uint64_t * values );
    // optional, all three or none; main thread only. A driver with a kernel
    // replay map writes it from these and nothing else does.
    void (*replay_forget)( void * driver, const rocketnet_address_t * address );
    void (*replay_mark)( void * driver, const rocketnet_address_t * address, uint32_t high_water );
    void (*replay_commit)( void * driver );
    // optional; main thread only, while the receive view is current. The
    // transport accepted the view's packet view_index for a connection (it
    // opened under the connection's keys); a driver that addresses frames
    // itself learns the packet's link-layer source for its address.
    void (*peer_accepted)( void * driver, int view_index );
};

// the start_packet id of a refusal for want of a link-layer destination
#define ROCKETNET_DRIVER_PACKET_UNREACHABLE    0xFFFFFFFFFFFFFFFFULL

struct rocketnet_client_driver_config_t
{
    const char * name;
    const rocketnet_client_transport_config_t * transport;
};

// A backend with wait and wake runs the client I/O thread design of
// docs/CLIENT-IO.md; a backend without them keeps the receive thread and
// per-packet main-thread sends.
struct rocketnet_client_driver_operations_t
{
    void * (*create)( void *, const rocketnet_client_driver_config_t *, rocketnet_address_t * );
    void (*destroy)( void * );
    void (*send)( void *, const rocketnet_address_t *, const uint8_t *, int );
    int (*receive)( void *, rocketnet_address_t *, uint8_t *, int );
    // optional; I/O thread only. Sends num_packets datagrams in one batch,
    // borrowing sends and the bytes they point at for the call only.
    void (*send_batch)( void * driver, const rocketnet_platform_send_t * sends, int num_packets );
    // optional; I/O thread only. Drains up to max_packets into caller
    // storage at packet_data + i * packet_stride, writing from[i] and
    // packet_bytes[i]; returns the count, zero when drained.
    int (*receive_batch)( void * driver, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets );
    // optional, with wake; I/O thread only. Blocks until a wake is set,
    // the socket is readable (only when readable is true), or
    // timeout_seconds pass; consumes the wake. With readable false the
    // socket's readiness is ignored. Returns true when woken or readable.
    bool (*wait)( void * driver, bool readable, double timeout_seconds );
    // optional, with wait; any thread. Sets the level-triggered wake, so a
    // wait in progress or the next wait returns at once.
    void (*wake)( void * driver );
};

struct rocketnet_driver_backend_t
{
    const rocketnet_server_driver_operations_t * server;
    const rocketnet_client_driver_operations_t * client;
};

struct rocketnet_driver_definition_t
{
    const char * name;
    bool discovers_bound_port;
    unsigned int platforms;
    int server_preference;
    int client_preference;
    const rocketnet_driver_backend_t * backend;
};

struct rocketnet_server_driver_t
{
    const rocketnet_server_driver_operations_t * operations;
    void * instance;
};

struct rocketnet_client_driver_t
{
    const rocketnet_client_driver_operations_t * operations;
    void * instance;
};

const rocketnet_driver_definition_t * rocketnet_driver_definition( int driver );
const char * rocketnet_driver_name( int driver );
const char * rocketnet_driver_names();
const char * rocketnet_driver_discovering_names();
bool rocketnet_driver_discovers_bound_port( int driver );
bool rocketnet_driver_pin_only( int driver );
const int * rocketnet_server_driver_availability( int * num_available );
const int * rocketnet_client_driver_availability( int * num_available );

// Validate the contract once at creation, never in the packet path. Also used
// by native extension conformance tests before a backend enters the registry.
bool rocketnet_server_driver_open( const rocketnet_server_driver_operations_t * operations,
    void * context, const rocketnet_server_driver_config_t * config,
    rocketnet_address_t * bind, rocketnet_server_driver_t * driver );
void rocketnet_server_driver_close( rocketnet_server_driver_t * driver );
bool rocketnet_server_driver_stop( rocketnet_server_driver_t * driver );
bool rocketnet_client_driver_open( const rocketnet_client_driver_operations_t * operations,
    void * context, const rocketnet_client_driver_config_t * config,
    rocketnet_address_t * bind, rocketnet_client_driver_t * driver );
void rocketnet_client_driver_close( rocketnet_client_driver_t * driver );

inline bool rocketnet_server_driver_has( const rocketnet_server_driver_t & driver, unsigned int flag )
{
    return ( driver.operations->flags & flag ) != 0;
}
