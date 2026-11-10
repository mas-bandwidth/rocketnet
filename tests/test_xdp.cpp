/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_internal.h"
#include "rocketnet_server_socket.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"
#include <stdio.h>

bool rocketnet_server_socket_test_buffer_exchange();
bool rocketnet_server_socket_test_receive_handoff_threads( uint64_t * counts );
bool rocketnet_server_socket_test_inactive_flow();
bool rocketnet_server_socket_test_replay_marks();
bool rocketnet_server_socket_test_send_failures_without_mutex();
bool rocketnet_server_socket_test_send_idle_hard_error();
bool rocketnet_server_socket_test_reply_mac_refreshed_at_finish();
bool rocketnet_server_socket_test_gateway_refresh();
bool rocketnet_server_socket_test_worker_stack();

int main()
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;
    rocketnet_server_socket_forbid_commit();

    const bool passed = rocketnet_server_socket_test_buffer_exchange();
    uint64_t handoff_counts[6] = {};
    const bool handoff_threads_passed = rocketnet_server_socket_test_receive_handoff_threads( handoff_counts );
    const bool inactive_passed = rocketnet_server_socket_test_inactive_flow();
    const bool replay_passed = rocketnet_server_socket_test_replay_marks();
    const bool send_failures_passed = rocketnet_server_socket_test_send_failures_without_mutex();
    const bool send_idle_hard_error_passed = rocketnet_server_socket_test_send_idle_hard_error();
    const bool reply_mac_passed = rocketnet_server_socket_test_reply_mac_refreshed_at_finish();
    const bool gateway_refresh_passed = rocketnet_server_socket_test_gateway_refresh();
    const bool worker_stack_passed = rocketnet_server_socket_test_worker_stack();
    rocketnet_term();

    if ( !passed )
    {
        fprintf( stderr, "native XDP buffer-exchange check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_buffer_exchange\n" );

    // the receive handoff against a real worker thread: every packet once, in
    // order. The counts are this run's scheduling, printed for whoever reads
    // the log; none of them is a pass condition.

    printf( "receive handoff: %llu calls, %llu owed a swap, %llu swaps, %llu missed, longest run of misses %llu, %llu worker yields\n",
            (unsigned long long) handoff_counts[0], (unsigned long long) handoff_counts[1], (unsigned long long) handoff_counts[2],
            (unsigned long long) handoff_counts[3], (unsigned long long) handoff_counts[4], (unsigned long long) handoff_counts[5] );

    if ( !handoff_threads_passed )
    {
        fprintf( stderr, "native XDP receive handoff with a worker thread check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_receive_handoff_with_a_worker_thread\n" );

    if ( !inactive_passed )
    {
        fprintf( stderr, "native XDP inactive-flow finish/abort check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_inactive_flow_finish_abort\n" );

    if ( !replay_passed )
    {
        fprintf( stderr, "native XDP replay-marks buffer/commit check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_replay_marks_buffered_and_committed\n" );

    if ( !send_failures_passed )
    {
        fprintf( stderr, "native XDP send-failure query check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_send_failures_without_mutex\n" );

    if ( !send_idle_hard_error_passed )
    {
        fprintf( stderr, "native XDP send-idle hard-error check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_send_idle_hard_error\n" );

    if ( !reply_mac_passed )
    {
        fprintf( stderr, "native XDP reply MAC refresh at finish check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_reply_mac_refreshed_at_finish\n" );

    if ( !gateway_refresh_passed )
    {
        fprintf( stderr, "native XDP gateway refresh check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_gateway_refresh\n" );

    if ( !worker_stack_passed )
    {
        fprintf( stderr, "native XDP worker 128 KiB stack check failed\n" );
        return 1;
    }

    printf( "test_xdp_transport_worker_fits_128k_stack\n" );

    // a umem frame holds the largest datagram a receiver accepts, with the
    // ethernet header and the larger (IPv6) IP header in front of it

    const int frame_need = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES + ROCKETNET_RECEIVE_CEILING;
    if ( frame_need > ROCKETNET_XDP_FRAME_SIZE )
    {
        fprintf( stderr, "xdp frame %d does not hold %d + %d + %d + %d = %d\n", ROCKETNET_XDP_FRAME_SIZE,
                 ROCKETNET_ETHERNET_HEADER_BYTES, ROCKETNET_IPV6_HEADER_BYTES, ROCKETNET_UDP_HEADER_BYTES, ROCKETNET_RECEIVE_CEILING, frame_need );
        return 1;
    }

    printf( "test_xdp_transport_frame_holds_receive_ceiling\n" );

    return 0;
}
