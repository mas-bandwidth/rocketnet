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

#define check rocketnet_unit_check

#include "../tests/server_stats_world.h"

void rocketnet_test_server_stats_current_every_update_1000_slots()
{
    no_worker_stats_current_every_update( NO_WORKER_CLIENTS );
}
