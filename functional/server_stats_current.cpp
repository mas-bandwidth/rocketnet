/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every one of 1000 connected slots reads statistics that already reflect the
// update that just ran (tests/server_stats_world.h). A thousand clients and a
// thousand-slot server in one process are too heavy for the unit tier; the
// unit suite runs the same case at 64 slots.

#include "../tests/unit_shutdown.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_driver_loopback.h"
#include "../tests/suite_runner.h"

#define check rocketnet_unit_check

#include "../tests/server_stats_world.h"

// The case holds a 1000-slot server and 1000 clients in one process, so its
// RSS bound comes from the SDK's memory plans, as the backlog case does: the
// server's plan plus 1000 client plans for the case's configs, plus one
// loopback queue per endpoint, plus 32 MiB for the executable and runtime.
static long server_stats_current_rss_bound_mb()
{
    rocketnet_server_config_t server;
    rocketnet_client_config_t client;
    if ( rocketnet_server_default_config( &server ) != ROCKETNET_STATUS_OK ||
         rocketnet_client_default_config( &client ) != ROCKETNET_STATUS_OK )
        return 0; // a missing plan falls back to the strict default bound
    server.protocol_id = NO_WORKER_PROTOCOL;
    server.max_clients = NO_WORKER_CLIENTS;
    server.transport = "loopback";
    client.transport = "loopback";
    no_worker_message_config( &server, &client );
    const uint64_t server_bytes = (uint64_t) rocketnet_server_required_memory( &server );
    const uint64_t client_bytes = (uint64_t) rocketnet_client_required_memory( &client );
    if ( !server_bytes || !client_bytes )
        return 0;
    const uint64_t queues = uint64_t( NO_WORKER_CLIENTS + 1 ) * rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS );
    return suite_rss_bound_mb( server_bytes + uint64_t( NO_WORKER_CLIENTS ) * client_bytes + queues, 32 );
}

SUITE_CASE_RSS_BOUND( test_server_stats_current_every_update_1000_slots, server_stats_current_rss_bound_mb );

void rocketnet_test_server_stats_current_every_update_1000_slots()
{
    no_worker_stats_current_every_update( NO_WORKER_CLIENTS );
}
