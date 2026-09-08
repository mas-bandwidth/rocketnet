/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
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
static void * portable_create( void * context, const rocketnet_server_driver_config_t * config, rocketnet_address_t * bind )
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
static const rocketnet_server_driver_operations_t portable_server_operations =
{
    ROCKETNET_DRIVER_FLUSH_ON_UPDATE, portable_create, portable_destroy, synchronous_stop,
    NULL, NULL, NULL, NULL, NULL, portable_send, portable_receive
};
static void * portable_client_create( void * context, const rocketnet_client_driver_config_t * config, rocketnet_address_t * bind )
{ return portable_socket_create( context, config->name, bind ); }
static void portable_client_send( void * driver, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{ rocketnet_platform_socket_send_packet( (rocketnet_platform_socket_t*) driver, to, data, bytes ); }
static int portable_client_receive( void * driver, rocketnet_address_t * from, uint8_t * data, int capacity )
{ return rocketnet_platform_socket_receive_packet( (rocketnet_platform_socket_t*) driver, from, data, capacity ); }
static const rocketnet_client_driver_operations_t portable_client_operations =
{ portable_client_create, portable_destroy, portable_client_send, portable_client_receive };
extern const rocketnet_driver_backend_t rocketnet_portable_driver = { &portable_server_operations, &portable_client_operations };
extern const rocketnet_driver_backend_t rocketnet_msgx_driver = { &portable_server_operations, &portable_client_operations };

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

static void * gso_create( void * context, const rocketnet_server_driver_config_t * config, rocketnet_address_t * bind )
{
    if ( config->transport->num_xdp_queues > ROCKETNET_GSO_MAX_QUEUES )
    {
        rocketnet_warn( "gso driver refused: num_xdp_queues %d is past the gso driver's %d queue geometry",
            config->transport->num_xdp_queues, ROCKETNET_GSO_MAX_QUEUES );
        return NULL;
    }
    return rocketnet_server_gso_create( context, bind, config->transport->num_xdp_queues, 0 );
}
static void gso_destroy( void * driver ) { rocketnet_server_gso_destroy( (rocketnet_server_gso_t*) driver ); }
ROCKETNET_STAGED_ADAPTER(gso, rocketnet_server_gso_t, rocketnet_server_gso)
static const rocketnet_server_driver_operations_t gso_operations =
{
    ROCKETNET_DRIVER_STAGED | ROCKETNET_DRIVER_FLUSH_ON_UPDATE,
    gso_create, gso_destroy, synchronous_stop,
    gso_start, gso_finish, gso_abort, gso_flush, gso_receive, NULL, NULL
};
extern const rocketnet_driver_backend_t rocketnet_gso_driver = { &gso_operations, NULL };

static void * xdp_create( void * context, const rocketnet_server_driver_config_t * config, rocketnet_address_t * )
{
    const int queues = config->transport->num_xdp_queues > 0 ? config->transport->num_xdp_queues : 7;
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
static const rocketnet_server_driver_operations_t xdp_operations =
{
    ROCKETNET_DRIVER_STAGED | ROCKETNET_DRIVER_FINISH_FINAL | ROCKETNET_DRIVER_BINDS_PUBLIC_ADDRESS,
    xdp_create, xdp_destroy, xdp_stop,
    xdp_start, xdp_finish, xdp_abort, xdp_flush, xdp_receive, NULL, NULL
};
extern const rocketnet_driver_backend_t rocketnet_xdp_driver = { &xdp_operations, NULL };
#endif

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include "rocketnet_server_iocp.h"
static void * iocp_create( void * context, const rocketnet_server_driver_config_t * config, rocketnet_address_t * bind )
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
static const rocketnet_server_driver_operations_t iocp_operations =
{
    ROCKETNET_DRIVER_STAGED | ROCKETNET_DRIVER_FLUSH_ON_UPDATE,
    iocp_create, iocp_destroy, iocp_stop,
    iocp_start, iocp_finish, iocp_abort, iocp_flush, iocp_receive, NULL, NULL
};
extern const rocketnet_driver_backend_t rocketnet_iocp_driver = { &iocp_operations, NULL };
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
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC || ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_IOS
    else if (strcmp(driver, "msgx") == 0)
    {
        plan.driver_bytes = rocketnet_platform_socket_allocation_bytes(); plan.sockets = 1;
    }
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    else if (strcmp(driver, "gso") == 0)
    {
        const int queues = config->num_xdp_queues ? config->num_xdp_queues : ROCKETNET_GSO_DEFAULT_QUEUES;
        plan.driver_bytes = rocketnet_server_gso_allocation_bytes(queues);
        plan.worker_threads = queues; plan.sockets = queues;
    }
    else if (strcmp(driver, "xdp") == 0)
    {
        const int queues = config->num_xdp_queues ? config->num_xdp_queues : 7;
        plan.driver_bytes = rocketnet_server_xdp_allocation_bytes(queues);
        plan.umem_bytes = (uint64_t)queues * ROCKETNET_XDP_NUM_FRAMES * ROCKETNET_XDP_FRAME_SIZE;
        plan.worker_threads = queues; plan.sockets = queues;
    }
#endif
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    else if (strcmp(driver, "iocp") == 0)
    {
        const int workers = config->num_xdp_queues ? config->num_xdp_queues : ROCKETNET_IOCP_DEFAULT_WORKERS;
        plan.driver_bytes = rocketnet_server_iocp_allocation_bytes(workers);
        plan.worker_threads = workers; plan.sockets = 1;
    }
#endif
    else return false; // A custom driver's resource policy remains native to it.
    return plan.driver_bytes != 0;
}
