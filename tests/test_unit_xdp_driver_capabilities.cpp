/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The XDP transport backend reports its kernel prefilter counters through the
// transport counter surface (rocketnet_server_transport_io_operations_t
// prefilter_counters). Only a transport with a kernel prefilter fills it; the
// other backends' stay NULL. This checks the registry, not a live map: no
// transport is created, no XDP program is loaded or attached.
//
// The AF_XDP bind config asks for zero copy in the bind flags, never in the
// attach flags, and its fallback asks for copy. The config is built and read;
// no socket is opened.
//
// The umem config sizes the fill and completion rings: non-zero powers of
// two (libxdp 1.4.2 hands a zero size to the kernel, which refuses it), the
// fill ring at least the fill entries the server reserves at start.
//
// The staging slot a finish or abort names comes from a byte offset that is
// refused when negative, unaligned or past the end before it is divided
// (rocketnet_server_socket_send_slot), and the TX queue comes from the
// destination, so one client's packets use one queue
// (rocketnet_server_socket_send_queue). Both are pure functions of their
// arguments: no socket and no kernel object.
//
// Each queue's UMEM is the send half and only the receive frames put on the
// fill ring at create (ROCKETNET_XDP_UMEM_FRAMES): rocketnet_xdp_umem_bytes,
// which the memory plan and the XDP preflight count, plans no frame the
// transport never uses. The size is computed, never allocated.

#include "test_unit_registry.h"

#include "rocketnet_driver.h"

#define check rocketnet_unit_check

#if ROCKETNET_XDP

#include "rocketnet_constants.h"
#include "rocketnet_memory.h"
#include "rocketnet_xdp_limits.h"
#include "rocketnet_xdp_socket_config.h"

#include <stddef.h>

// sdk/rocketnet_server_socket_linux.cpp:2158 and :2173
int rocketnet_server_socket_send_slot( ptrdiff_t offset );
int rocketnet_server_socket_send_queue( const struct rocketnet_address_t * to, int num_queues );

static void test_xdp_transport_prefilter_counters_transport_reports_them()
{
    const rocketnet_transport_definition_t * xdp = rocketnet_transport_definition( ROCKETNET_TRANSPORT_XDP );
    check( xdp->backend->server->prefilter_counters != NULL );

    const rocketnet_transport_definition_t * portable = rocketnet_transport_definition( ROCKETNET_TRANSPORT_PORTABLE );
    check( portable->backend->server->prefilter_counters == NULL );

    const rocketnet_transport_definition_t * gso = rocketnet_transport_definition( ROCKETNET_TRANSPORT_GSO );
    check( gso->backend->server->prefilter_counters == NULL );
}

static void test_xdp_transport_socket_config_requests_zero_copy_at_bind()
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

static void test_xdp_transport_umem_config_sizes_fill_and_completion_rings()
{
    struct xsk_umem_config config;
    memset( &config, 0xFF, sizeof( config ) );

    rocketnet_xdp_umem_config( &config );

    check( power_of_two( config.fill_size ) );
    check( power_of_two( config.comp_size ) );
    check( config.fill_size >= 2 * ROCKETNET_XDP_FILL_FRAMES );
    check( config.frame_size == ROCKETNET_XDP_FRAME_SIZE );
    check( config.frame_headroom == 0 );
    check( config.flags == 0 );
}

static void test_xdp_transport_send_slot_refuses_bad_offsets_before_dividing()
{
    // every slot root maps to its slot

    check( rocketnet_server_socket_send_slot( 0 ) == 0 );
    check( rocketnet_server_socket_send_slot( ROCKETNET_XDP_FRAME_SIZE ) == 1 );
    check( rocketnet_server_socket_send_slot( (ptrdiff_t) ( ROCKETNET_XDP_SEND_QUEUE_SIZE - 1 ) * ROCKETNET_XDP_FRAME_SIZE ) == ROCKETNET_XDP_SEND_QUEUE_SIZE - 1 );

    // a negative offset short of one frame truncates to slot 0 when divided
    // first: it is refused

    check( rocketnet_server_socket_send_slot( -1 ) == -1 );
    check( rocketnet_server_socket_send_slot( -( ROCKETNET_XDP_FRAME_SIZE - 1 ) ) == -1 );
    check( rocketnet_server_socket_send_slot( -ROCKETNET_XDP_FRAME_SIZE ) == -1 );

    // a pointer inside a slot, not its root, is refused

    check( rocketnet_server_socket_send_slot( 1 ) == -1 );
    check( rocketnet_server_socket_send_slot( ROCKETNET_XDP_FRAME_SIZE + 17 ) == -1 );

    // past the last slot is refused

    check( rocketnet_server_socket_send_slot( (ptrdiff_t) ROCKETNET_XDP_SEND_QUEUE_SIZE * ROCKETNET_XDP_FRAME_SIZE ) == -1 );
}

static void test_xdp_transport_send_queue_is_one_per_destination()
{
    struct rocketnet_address_t client;
    memset( &client, 0, sizeof( client ) );
    client.type = ROCKETNET_ADDRESS_IPV4;
    client.data.ipv4[0] = 198;
    client.data.ipv4[1] = 51;
    client.data.ipv4[2] = 100;
    client.data.ipv4[3] = 7;
    client.port = 50000;

    // one destination, one queue, every time

    const int queue = rocketnet_server_socket_send_queue( &client, 4 );
    check( queue >= 0 && queue < 4 );

    for ( int i = 0; i < 64; i++ )
    {
        check( rocketnet_server_socket_send_queue( &client, 4 ) == queue );
    }

    // one queue means queue 0

    check( rocketnet_server_socket_send_queue( &client, 1 ) == 0 );

    // destinations spread across the queues: 64 client ports reach every one
    // of 4 queues, and an IPv6 destination stays in range

    bool seen[4] = { false, false, false, false };

    for ( int i = 0; i < 64; i++ )
    {
        client.port = (uint16_t) ( 50000 + i );
        const int q = rocketnet_server_socket_send_queue( &client, 4 );
        check( q >= 0 && q < 4 );
        if ( q >= 0 && q < 4 )
        {
            seen[q] = true;
        }
    }

    check( seen[0] && seen[1] && seen[2] && seen[3] );

    struct rocketnet_address_t client_ipv6;
    memset( &client_ipv6, 0, sizeof( client_ipv6 ) );
    client_ipv6.type = ROCKETNET_ADDRESS_IPV6;
    client_ipv6.data.ipv6[0] = 0x2001;
    client_ipv6.data.ipv6[7] = 7;
    client_ipv6.port = 50000;
    const int queue_ipv6 = rocketnet_server_socket_send_queue( &client_ipv6, 3 );
    check( queue_ipv6 >= 0 && queue_ipv6 < 3 );
    check( rocketnet_server_socket_send_queue( &client_ipv6, 3 ) == queue_ipv6 );
}

static void test_xdp_transport_umem_holds_only_the_fill_frames()
{
    const uint64_t queue_bytes = (uint64_t) ( ROCKETNET_XDP_NUM_FRAMES / 2 + ROCKETNET_XDP_FILL_FRAMES ) * ROCKETNET_XDP_FRAME_SIZE;

    check( (uint64_t) ROCKETNET_XDP_UMEM_FRAMES * ROCKETNET_XDP_FRAME_SIZE == queue_bytes );
    check( rocketnet_xdp_umem_bytes( 1 ) == queue_bytes );
    check( rocketnet_xdp_umem_bytes( 7 ) == 7 * queue_bytes );

    // a queue count out of range plans nothing

    check( rocketnet_xdp_umem_bytes( 0 ) == 0 );
    check( rocketnet_xdp_umem_bytes( ROCKETNET_XDP_MAX_QUEUES + 1 ) == 0 );
}

// R3 of the pre-flight refuses an interface with a program attached in any
// mode. libbpf reports prog_id 0 when programs are attached in more than one
// mode (XDP_ATTACHED_MULTI), so the decision reads every per-mode id. The
// decision is a pure function of the query's result: nothing is queried here.

static void test_xdp_transport_preflight_refuses_any_attached_mode()
{
    rocketnet_xdp_attached_t attached;

    memset( &attached, 0, sizeof( attached ) );
    check( !rocketnet_xdp_attached_any( &attached ) );

    memset( &attached, 0, sizeof( attached ) );
    attached.prog_id = 11;
    check( rocketnet_xdp_attached_any( &attached ) );

    memset( &attached, 0, sizeof( attached ) );
    attached.drv_prog_id = 12;
    check( rocketnet_xdp_attached_any( &attached ) );

    memset( &attached, 0, sizeof( attached ) );
    attached.skb_prog_id = 13;
    check( rocketnet_xdp_attached_any( &attached ) );

    memset( &attached, 0, sizeof( attached ) );
    attached.hw_prog_id = 14;
    check( rocketnet_xdp_attached_any( &attached ) );

    // the multi-mode shape: no single id, a native and a generic program

    memset( &attached, 0, sizeof( attached ) );
    attached.drv_prog_id = 12;
    attached.skb_prog_id = 13;
    check( rocketnet_xdp_attached_any( &attached ) );
}

#if ROCKETNET_DEV
// The frames each queue's userspace receive dropped, by reason, reach the
// transport's XDP_RECEIVE_FRAME_DROPPED through the builtin dispatch the
// counter read calls, once each queue's worker publishes them: two synthetic
// queues, no socket, no worker (sdk/rocketnet_server_socket_linux.cpp)
bool rocketnet_server_transport_io_receive_frames_dropped( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                           uint64_t * dropped );
typedef bool rocketnet_server_socket_test_frames_dropped_t( const struct rocketnet_server_transport_io_operations_t * operations, void * transport,
                                                            uint64_t * dropped );
bool rocketnet_server_socket_test_receive_dropped( rocketnet_server_socket_test_frames_dropped_t * dispatch,
                                                   const struct rocketnet_server_transport_io_operations_t * xdp );

static void test_xdp_transport_receive_frames_dropped_counted()
{
    const rocketnet_transport_definition_t * xdp = rocketnet_transport_definition( ROCKETNET_TRANSPORT_XDP );
    check( rocketnet_server_socket_test_receive_dropped( rocketnet_server_transport_io_receive_frames_dropped, xdp->backend->server ) );

    // no other transport answers the dispatch

    const rocketnet_transport_definition_t * gso = rocketnet_transport_definition( ROCKETNET_TRANSPORT_GSO );
    uint64_t dropped = 0;
    check( !rocketnet_server_transport_io_receive_frames_dropped( gso->backend->server, NULL, &dropped ) );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_frames_dropped_counted );
#endif // #if ROCKETNET_DEV

REGISTER_UNIT_TEST( test_xdp_transport_prefilter_counters_transport_reports_them );
REGISTER_UNIT_TEST( test_xdp_transport_socket_config_requests_zero_copy_at_bind );
REGISTER_UNIT_TEST( test_xdp_transport_umem_config_sizes_fill_and_completion_rings );
REGISTER_UNIT_TEST( test_xdp_transport_send_slot_refuses_bad_offsets_before_dividing );
REGISTER_UNIT_TEST( test_xdp_transport_send_queue_is_one_per_destination );
REGISTER_UNIT_TEST( test_xdp_transport_umem_holds_only_the_fill_frames );
REGISTER_UNIT_TEST( test_xdp_transport_preflight_refuses_any_attached_mode );

#endif // #if ROCKETNET_XDP
