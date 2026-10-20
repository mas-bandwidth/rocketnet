/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP driver backend reports its kernel prefilter counters through the
// transport counter surface (rocketnet_server_driver_operations_t
// prefilter_counters). Only a driver with a kernel prefilter fills it; the
// other backends' stay NULL. This checks the registry, not a live map: no
// driver is created, no XDP program is loaded or attached.
//
// The AF_XDP bind config asks for zero copy in the bind flags, never in the
// attach flags, and its fallback asks for copy. The config is built and read;
// no socket is opened.
//
// The umem config sizes the fill and completion rings: non-zero powers of
// two (libxdp 1.4.2 hands a zero size to the kernel, which refuses it), the
// fill ring at least the fill entries the server reserves at start.

#include "test_unit_registry.h"

#include "rocketnet_driver.h"

#define check rocketnet_unit_check

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#include "rocketnet_constants.h"
#include "rocketnet_xdp_socket_config.h"

static void prefilter_counters_xdp_driver_reports_them()
{
    const rocketnet_driver_definition_t * xdp = rocketnet_driver_definition( ROCKETNET_DRIVER_XDP );
    check( xdp->backend->server->prefilter_counters != NULL );

    const rocketnet_driver_definition_t * portable = rocketnet_driver_definition( ROCKETNET_DRIVER_PORTABLE );
    check( portable->backend->server->prefilter_counters == NULL );

    const rocketnet_driver_definition_t * gso = rocketnet_driver_definition( ROCKETNET_DRIVER_GSO );
    check( gso->backend->server->prefilter_counters == NULL );
}

static void xdp_socket_config_requests_zero_copy_at_bind()
{
    struct xsk_socket_config config;

    // the first attempt: zero copy and need wakeup at bind, no attach flags

    rocketnet_xdp_socket_config( &config, ROCKETNET_XDP_RECV_QUEUE_SIZE, ROCKETNET_XDP_SEND_QUEUE_SIZE, true );
    check( config.xdp_flags == 0 );
    check( config.bind_flags == ( XDP_ZEROCOPY | XDP_USE_NEED_WAKEUP ) );
    check( ( config.bind_flags & XDP_COPY ) == 0 );
    check( config.libbpf_flags == XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD );
    check( config.rx_size == ROCKETNET_XDP_RECV_QUEUE_SIZE );
    check( config.tx_size == ROCKETNET_XDP_SEND_QUEUE_SIZE );

    // the fallback: copy and need wakeup at bind, never zero copy

    rocketnet_xdp_socket_config( &config, ROCKETNET_XDP_RECV_QUEUE_SIZE, ROCKETNET_XDP_SEND_QUEUE_SIZE, false );
    check( config.xdp_flags == 0 );
    check( config.bind_flags == ( XDP_COPY | XDP_USE_NEED_WAKEUP ) );
    check( ( config.bind_flags & XDP_ZEROCOPY ) == 0 );
    check( config.libbpf_flags == XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD );
    check( config.rx_size == ROCKETNET_XDP_RECV_QUEUE_SIZE );
    check( config.tx_size == ROCKETNET_XDP_SEND_QUEUE_SIZE );
}

static bool power_of_two( uint32_t value )
{
    return value != 0 && ( value & ( value - 1 ) ) == 0;
}

static void xdp_umem_config_sizes_fill_and_completion_rings()
{
    struct xsk_umem_config config;
    memset( &config, 0xFF, sizeof(config) );

    rocketnet_xdp_umem_config( &config );

    check( power_of_two( config.fill_size ) );
    check( power_of_two( config.comp_size ) );
    check( config.fill_size >= ROCKETNET_XDP_FILL_QUEUE_SIZE );
    check( config.frame_size == ROCKETNET_XDP_FRAME_SIZE );
    check( config.frame_headroom == 0 );
    check( config.flags == 0 );
}

#else

static void xdp_umem_config_sizes_fill_and_completion_rings()
{
    printf( "xdp_umem_config_sizes_fill_and_completion_rings [SKIPPED AF_XDP is Linux only]\n" );
    fflush( stdout );
}

static void xdp_socket_config_requests_zero_copy_at_bind()
{
    printf( "xdp_socket_config_requests_zero_copy_at_bind [SKIPPED AF_XDP is Linux only]\n" );
    fflush( stdout );
}

static void prefilter_counters_xdp_driver_reports_them()
{
    printf( "prefilter_counters_xdp_driver_reports_them [SKIPPED the XDP driver backend registers only on Linux]\n" );
    fflush( stdout );
}

#endif

REGISTER_UNIT_TEST( prefilter_counters_xdp_driver_reports_them );
REGISTER_UNIT_TEST( xdp_socket_config_requests_zero_copy_at_bind );
REGISTER_UNIT_TEST( xdp_umem_config_sizes_fill_and_completion_rings );
