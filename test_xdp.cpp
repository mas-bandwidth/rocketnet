/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_internal.h"
#include "test/plan_rss.h"
#include "rocketnet_server_socket.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"
#include <stdio.h>

bool rocketnet_server_socket_test_buffer_exchange();
bool rocketnet_server_socket_test_replay_marks();
bool rocketnet_server_socket_test_send_failures_without_mutex();

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    if (rocketnet_init( NULL, plan_rss_malloc, plan_rss_free ) != ROCKETNET_STATUS_OK) return 1;
    rocketnet_server_socket_forbid_commit();
    const bool passed = rocketnet_server_socket_test_buffer_exchange();
    const bool replay_passed = rocketnet_server_socket_test_replay_marks();
    const bool send_failures_passed = rocketnet_server_socket_test_send_failures_without_mutex();
    rocketnet_term();
    if (!passed)
    {
        fprintf(stderr, "native XDP buffer-exchange check failed\n");
        return 1;
    }
    printf("test_xdp_transport_buffer_exchange\n");

    if (!replay_passed)
    {
        fprintf(stderr, "native XDP replay-marks buffer/commit check failed\n");
        return 1;
    }
    printf("test_xdp_transport_replay_marks_buffered_and_committed\n");

    if (!send_failures_passed)
    {
        fprintf(stderr, "native XDP send-failure query check failed\n");
        return 1;
    }
    printf("test_xdp_transport_send_failures_without_mutex\n");

    // a umem frame holds the largest datagram a receiver accepts, with the
    // ethernet header and the larger (IPv6) IP header in front of it
    const int frame_need = ROCKETNET_ETHERNET_HEADER_BYTES + ROCKETNET_IPV6_HEADER_BYTES + ROCKETNET_UDP_HEADER_BYTES + ROCKETNET_RECEIVE_CEILING;
    if (frame_need > ROCKETNET_XDP_FRAME_SIZE)
    {
        fprintf(stderr, "xdp frame %d does not hold %d + %d + %d + %d = %d\n", ROCKETNET_XDP_FRAME_SIZE,
                ROCKETNET_ETHERNET_HEADER_BYTES, ROCKETNET_IPV6_HEADER_BYTES, ROCKETNET_UDP_HEADER_BYTES, ROCKETNET_RECEIVE_CEILING, frame_need);
        return 1;
    }
    printf("test_xdp_transport_frame_holds_receive_ceiling\n");
    return plan_rss_check("test_xdp");
}
