/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet.h"
#include "rocketnet_server_socket.h"
#include <stdio.h>

bool rocketnet_server_socket_test_inactive_generation();

int main()
{
    rocketnet_log_level(ROCKETNET_LOG_LEVEL_NONE);
    if (!rocketnet_init()) return 1;
    rocketnet_server_socket_forbid_commit();
    const bool passed = rocketnet_server_socket_test_inactive_generation();
    rocketnet_term();
    if (!passed)
    {
        fprintf(stderr, "native XDP inactive-generation check failed\n");
        return 1;
    }
    printf("test_xdp_inactive_generation\n");
    return 0;
}
