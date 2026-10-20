/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_transport.h"
#include "rocketnet_platform.h"
#include "rocketnet_memory.h"

// Adapters preserve the existing native backend entry points. New backends
// can define their operations directly in their own implementation file.
static void * portable_socket_create( void * context, const char * name, rocketnet_address_t * bind )
{
    return rocketnet_platform_socket_create_with_pump( context, bind,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, ROCKETNET_SOCKET_SEND_BUFFER_SIZE,
        ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, name );
}
static void * portable_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{ return portable_socket_create( context, config->name, bind ); }
static void portable_destroy( void * driver ) { rocketnet_platform_socket_destroy( (rocketnet_platform_socket_t*) driver ); }
static bool synchronous_stop( void * ) { return true; }
static void portable_send( void * driver, const rocketnet_platform_send_t * packets, int count )
{
    rocketnet_platform_socket_send_packets( (rocketnet_platform_socket_t*) driver, packets, count );
}
static int portable_receive( void * driver, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{
    return rocketnet_platform_socket_receive_packets( (rocketnet_platform_socket_t*) driver, from, data, stride, bytes, count );
}
static uint64_t portable_send_failures( void * driver )
{ return rocketnet_platform_socket_send_failures( (rocketnet_platform_socket_t*) driver ); }
static const rocketnet_server_transport_io_operations_t portable_server_operations =
{
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, portable_create, portable_destroy, synchronous_stop,
    NULL, NULL, NULL, NULL, NULL, portable_send, portable_receive,
    NULL, NULL, NULL, NULL, NULL, portable_send_failures
};
#if defined( ROCKETNET_PLATFORM_HAS_WAKE )
// The portable client on a platform with the client I/O wake: an eight-callback
// table over the socket and its wake, so the transport runs the I/O thread of
// docs/CLIENT-IO.md (RNClientIo) instead of the BASE receive thread.
struct portable_io_client_t
{
    rocketnet_platform_socket_t * socket;
    rocketnet_platform_wake_t * wake;
    void * context;
};
// RNClientIo initial / Stop and destroy order (docs/CLIENT-IO.md): create owns
// the socket and the wake beside it; any failure releases what it opened.
static void * portable_io_client_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) rocketnet_malloc( context, sizeof( *client ) );
    if ( !client )
        return NULL;
    client->socket = (rocketnet_platform_socket_t*) portable_socket_create( context, config->name, bind );
    if ( !client->socket )
    {
        rocketnet_free( context, client );
        return NULL;
    }
    client->wake = rocketnet_platform_wake_create( context, client->socket );
    if ( !client->wake )
    {
        rocketnet_platform_socket_destroy( client->socket );
        rocketnet_free( context, client );
        return NULL;
    }
    client->context = context;
    return client;
}
// RNClientIo Stop and destroy order (docs/CLIENT-IO.md): the transport joins
// the I/O thread before closing the driver, so the wake goes, then the socket.
static void portable_io_client_destroy( void * driver )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) driver;
    if ( !client )
        return;
    rocketnet_platform_wake_destroy( client->wake );
    rocketnet_platform_socket_destroy( client->socket );
    rocketnet_free( client->context, client );
}
// RNClientIo IoSend / IoReceive (docs/CLIENT-IO.md The I/O step): the
// per-packet platform functions on the instance's socket.
static void portable_io_client_send( void * driver, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) driver; rocketnet_platform_socket_send_packet( client->socket, to, data, bytes ); }
static int portable_io_client_receive( void * driver, rocketnet_address_t * from, uint8_t * data, int capacity )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) driver; return rocketnet_platform_socket_receive_packet( client->socket, from, data, capacity ); }
// RNClientIo IoSend / IoReceive batches (docs/CLIENT-IO.md The I/O step): the
// batch pump seam; on Linux it is sendmmsg/recvmmsg (#client-io-linux-mmsg),
// otherwise the seam's portable loop. Still off the main thread.
static void portable_io_client_send_batch( void * driver, const rocketnet_platform_send_t * sends, int num_packets )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) driver;
#if defined(__linux__)
    rocketnet_platform_socket_send_packets_mmsg( client->socket, sends, num_packets );
#else
    rocketnet_platform_socket_send_packets( client->socket, sends, num_packets );
#endif
}
static int portable_io_client_receive_batch( void * driver, rocketnet_address_t * from, uint8_t * packet_data, int packet_stride, int * packet_bytes, int max_packets )
{
    struct portable_io_client_t * client = (struct portable_io_client_t*) driver;
#if defined(__linux__)
    return rocketnet_platform_socket_receive_packets_mmsg( client->socket, from, packet_data, packet_stride, packet_bytes, max_packets );
#else
    return rocketnet_platform_socket_receive_packets( client->socket, from, packet_data, packet_stride, packet_bytes, max_packets );
#endif
}
// RNClientIo IoWait / waitReadable and the wake (docs/CLIENT-IO.md The wake):
// readable passes through, so with it false the socket is not polled and a
// full receive buffer waits on the eventfd alone.
static bool portable_io_client_wait( void * driver, bool readable, double timeout_seconds )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) driver; return rocketnet_platform_wake_wait( client->wake, readable, timeout_seconds ); }
static void portable_io_client_wake( void * driver )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) driver; rocketnet_platform_wake_set( client->wake ); }
static uint64_t portable_io_client_send_failures( void * driver )
{ struct portable_io_client_t * client = (struct portable_io_client_t*) driver; return rocketnet_platform_socket_send_failures( client->socket ); }
static const rocketnet_client_transport_io_operations_t portable_client_operations =
{
    portable_io_client_create, portable_io_client_destroy, portable_io_client_send, portable_io_client_receive,
    portable_io_client_send_batch, portable_io_client_receive_batch, portable_io_client_wait, portable_io_client_wake,
    portable_io_client_send_failures
};
#else // #if defined( ROCKETNET_PLATFORM_HAS_WAKE )
static void * portable_client_create( void * context, const rocketnet_client_transport_io_config_t * config, rocketnet_address_t * bind )
{ return portable_socket_create( context, config->name, bind ); }
static void portable_client_send( void * driver, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{ rocketnet_platform_socket_send_packet( (rocketnet_platform_socket_t*) driver, to, data, bytes ); }
static int portable_client_receive( void * driver, rocketnet_address_t * from, uint8_t * data, int capacity )
{ return rocketnet_platform_socket_receive_packet( (rocketnet_platform_socket_t*) driver, from, data, capacity ); }
#if defined(__linux__)
static void portable_client_send_batch( void * driver, const rocketnet_platform_send_t * sends, int count )
{ rocketnet_platform_socket_send_packets_mmsg( (rocketnet_platform_socket_t*) driver, sends, count ); }
static int portable_client_receive_batch( void * driver, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{ return rocketnet_platform_socket_receive_packets_mmsg( (rocketnet_platform_socket_t*) driver, from, data, stride, bytes, count ); }
#endif
static const rocketnet_client_transport_io_operations_t portable_client_operations =
{ portable_client_create, portable_destroy, portable_client_send, portable_client_receive,
#if defined(__linux__)
  portable_client_send_batch, portable_client_receive_batch,
#else
  NULL, NULL,
#endif
  NULL, NULL, portable_send_failures
};
#endif // #if defined( ROCKETNET_PLATFORM_HAS_WAKE )
extern const rocketnet_transport_backend_t rocketnet_portable_transport = { &portable_server_operations, &portable_client_operations };
extern const rocketnet_transport_backend_t rocketnet_msgx_transport = { &portable_server_operations, &portable_client_operations };

#define ROCKETNET_STAGED_ADAPTER(name, type, prefix) \
    static uint8_t * name##_start( void * driver, const rocketnet_address_t * to, uint64_t * id ) \
    { return prefix##_start_packet( (type*) driver, to, id ); } \
    static void name##_finish( void * driver, uint64_t id, uint8_t * frame, int bytes ) \
    { prefix##_finish_packet( (type*) driver, id, frame, bytes ); } \
    static void name##_abort( void * driver, uint64_t id, uint8_t * frame ) \
    { prefix##_abort_packet( (type*) driver, id, frame ); } \
    static void name##_flush( void * driver ) { prefix##_send_packets( (type*) driver ); } \
    static rocketnet_server_socket_process_packets_t * name##_receive( void * driver ) \
    { prefix##_receive_packets( (type*) driver ); return prefix##_process_packets( (type*) driver ); }

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
#include "rocketnet_server_gso.h"
#include "rocketnet_server_socket.h"

// the native drivers' drop counts behind send_failures and receive_drops,
// defined at sdk/rocketnet_server_gso_linux.cpp:869 and sdk/rocketnet_server_socket_linux.cpp:2598
void rocketnet_server_gso_receive_drops( struct rocketnet_server_gso_t * gso, uint64_t * too_short, uint64_t * too_large );
uint64_t rocketnet_server_socket_send_failures( struct rocketnet_server_socket_t * server_socket );

static void * gso_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    if ( config->transport->num_xdp_queues > ROCKETNET_GSO_MAX_QUEUES )
    {
        rocketnet_warn( "gso transport refused: num_xdp_queues %d is past the gso transport's %d queue geometry",
            config->transport->num_xdp_queues, ROCKETNET_GSO_MAX_QUEUES );
        return NULL;
    }
    return rocketnet_server_gso_create( context, bind, config->transport->num_xdp_queues, 0 );
}
static void gso_destroy( void * driver ) { rocketnet_server_gso_destroy( (rocketnet_server_gso_t*) driver ); }
ROCKETNET_STAGED_ADAPTER(gso, rocketnet_server_gso_t, rocketnet_server_gso)
// SEND_DROPPED: the datagrams a refused sendmmsg dropped
static uint64_t gso_send_failures( void * driver )
{
    uint64_t counters[ROCKETNET_SERVER_GSO_NUM_COUNTERS];
    rocketnet_server_gso_counters( (rocketnet_server_gso_t*) driver, counters );
    return counters[ROCKETNET_SERVER_GSO_COUNTER_SEND_DROPPED];
}
static void gso_receive_drops( void * driver, uint64_t * too_short, uint64_t * too_large )
{ rocketnet_server_gso_receive_drops( (rocketnet_server_gso_t*) driver, too_short, too_large ); }
static const rocketnet_server_transport_io_operations_t gso_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    gso_create, gso_destroy, synchronous_stop,
    gso_start, gso_finish, gso_abort, gso_flush, gso_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, gso_send_failures
};
extern const rocketnet_transport_backend_t rocketnet_gso_transport = { &gso_operations, NULL };

#include "rocketnet_server_uring.h"
void rocketnet_server_uring_receive_drops( struct rocketnet_server_uring_t * uring, uint64_t * too_short, uint64_t * too_large ); // sdk/rocketnet_server_uring_linux.cpp:1223
static void * io_uring_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    if ( config->transport->num_xdp_queues > ROCKETNET_URING_MAX_QUEUES )
    {
        rocketnet_warn( "io_uring transport refused: num_xdp_queues %d is past the io_uring transport's %d queue geometry",
            config->transport->num_xdp_queues, ROCKETNET_URING_MAX_QUEUES );
        return NULL;
    }
    return rocketnet_server_uring_create( context, bind, config->transport->num_xdp_queues );
}
static void io_uring_destroy( void * driver ) { rocketnet_server_uring_destroy( (rocketnet_server_uring_t*) driver ); }
ROCKETNET_STAGED_ADAPTER(io_uring, rocketnet_server_uring_t, rocketnet_server_uring)
// SEND_DROPPED: send CQEs with a negative result, and destinations the
// socket family cannot carry
static uint64_t io_uring_send_failures( void * driver )
{
    uint64_t counters[ROCKETNET_SERVER_URING_NUM_COUNTERS];
    rocketnet_server_uring_counters( (rocketnet_server_uring_t*) driver, counters );
    return counters[ROCKETNET_SERVER_URING_COUNTER_SEND_DROPPED];
}
static void io_uring_receive_drops( void * driver, uint64_t * too_short, uint64_t * too_large )
{ rocketnet_server_uring_receive_drops( (rocketnet_server_uring_t*) driver, too_short, too_large ); }
static const rocketnet_server_transport_io_operations_t io_uring_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    io_uring_create, io_uring_destroy, synchronous_stop,
    io_uring_start, io_uring_finish, io_uring_abort, io_uring_flush, io_uring_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, io_uring_send_failures
};
extern const rocketnet_transport_backend_t rocketnet_io_uring_transport = { &io_uring_operations, NULL };

static void * xdp_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * )
{
    const int queues = config->transport->num_xdp_queues > 0 ? config->transport->num_xdp_queues : ROCKETNET_XDP_DEFAULT_QUEUES;
    const int cpus = config->transport->num_os_cpus > 0 ? config->transport->num_os_cpus : 1;
    // The backend retains its existing verify-and-bind preflight and commit
    // barrier. Registration never bypasses either or provisions the machine.
    return rocketnet_server_socket_create( context, config->public_address, config->transport->filter_value, queues, cpus );
}
static bool xdp_stop( void * driver )
{
    rocketnet_server_socket_t * socket = (rocketnet_server_socket_t*) driver;
    if ( rocketnet_server_socket_state( socket ) != ROCKETNET_SERVER_SOCKET_STOPPED )
    {
        rocketnet_server_socket_stop( socket );
        rocketnet_server_socket_update( socket );
    }
    return rocketnet_server_socket_state( socket ) == ROCKETNET_SERVER_SOCKET_STOPPED;
}
static void xdp_destroy( void * driver )
{
    xdp_stop( driver );
    rocketnet_server_socket_destroy( (rocketnet_server_socket_t*) driver );
}
ROCKETNET_STAGED_ADAPTER(xdp, rocketnet_server_socket_t, rocketnet_server_socket)
static void xdp_prefilter_counters( void * driver, uint64_t * values )
{
    rocketnet_server_socket_xdp_counters( (rocketnet_server_socket_t*) driver, values );
}
static void xdp_replay_forget( void * driver, const rocketnet_address_t * address )
{
    rocketnet_server_socket_replay_forget( (rocketnet_server_socket_t*) driver, address );
}
static void xdp_replay_mark( void * driver, const rocketnet_address_t * address, uint32_t high_water )
{
    rocketnet_server_socket_replay_mark( (rocketnet_server_socket_t*) driver, address, high_water );
}
static void xdp_replay_commit( void * driver )
{
    rocketnet_server_socket_replay_commit( (rocketnet_server_socket_t*) driver );
}
static void xdp_peer_accepted( void * driver, int view_index )
{
    rocketnet_server_socket_peer_accepted( (rocketnet_server_socket_t*) driver, view_index );
}
static uint64_t xdp_send_failures( void * driver )
{
    return rocketnet_server_socket_send_failures( (rocketnet_server_socket_t*) driver );
}
static const rocketnet_server_transport_io_operations_t xdp_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FINISH_FINAL | ROCKETNET_TRANSPORT_BINDS_PUBLIC_ADDRESS,
    xdp_create, xdp_destroy, xdp_stop,
    xdp_start, xdp_finish, xdp_abort, xdp_flush, xdp_receive, NULL, NULL, xdp_prefilter_counters,
    xdp_replay_forget, xdp_replay_mark, xdp_replay_commit, xdp_peer_accepted, xdp_send_failures
};
extern const rocketnet_transport_backend_t rocketnet_xdp_transport = { &xdp_operations, NULL };
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include "rocketnet_server_iocp.h"
static void * iocp_create( void * context, const rocketnet_server_transport_io_config_t * config, rocketnet_address_t * bind )
{
    return rocketnet_server_iocp_create( context, bind, config->transport->num_xdp_queues );
}
static void iocp_destroy( void * driver ) { rocketnet_server_iocp_destroy( (rocketnet_server_iocp_t*) driver ); }
static bool iocp_stop( void * driver )
{
    rocketnet_server_iocp_stop( (rocketnet_server_iocp_t*) driver );
    return rocketnet_server_iocp_stopped( (rocketnet_server_iocp_t*) driver );
}
ROCKETNET_STAGED_ADAPTER(iocp, rocketnet_server_iocp_t, rocketnet_server_iocp)
// SEND_FAILED: the driver's own count of datagrams a refused post or a failed
// send completion dropped (ROCKETNET_IOCP_SEND_FAILED)
static uint64_t iocp_send_failures( void * driver )
{
    uint64_t counters[ROCKETNET_IOCP_NUM_COUNTERS];
    rocketnet_server_iocp_counters( (rocketnet_server_iocp_t*) driver, counters );
    return counters[ROCKETNET_IOCP_SEND_FAILED];
}
static const rocketnet_server_transport_io_operations_t iocp_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    iocp_create, iocp_destroy, iocp_stop,
    iocp_start, iocp_finish, iocp_abort, iocp_flush, iocp_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, iocp_send_failures
};
extern const rocketnet_transport_backend_t rocketnet_iocp_transport = { &iocp_operations, NULL };

#include "rocketnet_server_rio.h"
static void * rio_create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * bind )
{ return rocketnet_server_rio_create( context, bind, 0 ); }
static void rio_destroy( void * driver ) { rocketnet_server_rio_destroy( (rocketnet_server_rio_t*) driver ); }
static bool rio_stop( void * driver )
{
    rocketnet_server_rio_stop( (rocketnet_server_rio_t*) driver );
    return rocketnet_server_rio_stopped( (rocketnet_server_rio_t*) driver );
}
ROCKETNET_STAGED_ADAPTER(rio, rocketnet_server_rio_t, rocketnet_server_rio)
// SEND_FAILED: the driver's own count of datagrams a refused post or a failed
// send completion dropped (ROCKETNET_RIO_COUNTER_SEND_FAILED)
static uint64_t rio_send_failures( void * driver )
{
    uint64_t counters[ROCKETNET_RIO_NUM_COUNTERS];
    rocketnet_server_rio_counters( (rocketnet_server_rio_t*) driver, counters );
    return counters[ROCKETNET_RIO_COUNTER_SEND_FAILED];
}
static const rocketnet_server_transport_io_operations_t rio_operations =
{
    ROCKETNET_TRANSPORT_STAGED | ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE,
    rio_create, rio_destroy, rio_stop,
    rio_start, rio_finish, rio_abort, rio_flush, rio_receive, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, rio_send_failures
};
extern const rocketnet_transport_backend_t rocketnet_rio_transport = { &rio_operations, NULL };
#endif
#undef ROCKETNET_STAGED_ADAPTER

// Existing public compatibility hook; the backend policy stays in this layer.
void rocketnet_forbid_xdp_commit()
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    rocketnet_server_socket_forbid_commit();
#endif
}

// Built-in resource policy belongs beside the native adapters, never in the
// transport. Custom drivers may report their own plan without changing create.
bool rocketnet_builtin_server_memory_plan(const rocketnet_server_transport_config_t * config,
    const char * driver, rocketnet_server_memory_plan_t & plan)
{
    if (strcmp(driver, "portable") == 0)
    {
        plan.driver_bytes = rocketnet_platform_socket_allocation_bytes(); plan.sockets = 1;
    }
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    else if (strcmp(driver, "msgx") == 0)
    {
        plan.driver_bytes = rocketnet_platform_socket_allocation_bytes(); plan.sockets = 1;
    }
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    else if (strcmp(driver, "gso") == 0)
    {
        const int queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : ROCKETNET_GSO_DEFAULT_QUEUES;
        plan.driver_bytes = rocketnet_server_gso_allocation_bytes(queues);
        plan.worker_threads = queues; plan.sockets = queues;
    }
    else if (strcmp(driver, "io_uring") == 0)
    {
        const int queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : ROCKETNET_URING_DEFAULT_QUEUES;
        plan.driver_bytes = rocketnet_server_uring_allocation_bytes(queues);
        plan.worker_threads = 0; plan.sockets = queues;
    }
    else if (strcmp(driver, "xdp") == 0)
    {
        const int queues = config->num_xdp_queues > 0 ? config->num_xdp_queues : ROCKETNET_XDP_DEFAULT_QUEUES;
        plan.driver_bytes = rocketnet_server_xdp_allocation_bytes(queues);
        plan.umem_bytes = rocketnet_xdp_umem_bytes(queues);
        plan.worker_threads = queues; plan.sockets = queues;
    }
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    else if (strcmp(driver, "iocp") == 0)
    {
        const int workers = config->num_xdp_queues > 0 ? config->num_xdp_queues : ROCKETNET_IOCP_DEFAULT_WORKERS;
        plan.driver_bytes = rocketnet_server_iocp_allocation_bytes(workers);
        plan.worker_threads = workers; plan.sockets = 1;
    }
    else if (strcmp(driver, "rio") == 0)
    {
        plan.driver_bytes = rocketnet_server_rio_allocation_bytes();
        plan.worker_threads = 0; plan.sockets = 1;
    }
#endif
    else return false; // A custom driver's resource policy remains native to it.
    return plan.driver_bytes != 0;
}

// Builtin-only receive-drop hook. Match the operations table, not a configured
// name, so custom drivers retain their own counter policy without a cast.
bool rocketnet_server_transport_io_receive_drops( const rocketnet_server_transport_io_operations_t * operations,
                                           void * driver, uint64_t * too_short, uint64_t * too_large )
{
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    if ( operations == &gso_operations )
        gso_receive_drops( driver, too_short, too_large );
    else if ( operations == &io_uring_operations )
        io_uring_receive_drops( driver, too_short, too_large );
    else
        return false;
    return true;
#else
    (void) operations;
    (void) driver;
    (void) too_short;
    (void) too_large;
    return false;
#endif
}
