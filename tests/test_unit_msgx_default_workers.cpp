/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS batched server transport's default number of send threads
// (ROCKETNET_MSGX_DEFAULT_WORKERS, sdk/rocketnet_server_msgx.h) is 3: space
// game's four load rows pass at 3 send threads and one receive thread, and
// the peak row with a game load fails at 2. The constant is msgx's own; no
// other transport reads it. No socket, no clock, no thread.

#include "test_unit_registry.h"

#include "rocketnet_server_msgx.h"
#include "rocketnet_msgx_socket_table.h"

#include <stdint.h>

static void test_rocketnet_msgx_default_workers()
{
    rocketnet_unit_check( ROCKETNET_MSGX_DEFAULT_WORKERS == 3 );
    rocketnet_unit_check( ROCKETNET_MSGX_DEFAULT_WORKERS >= 1 );
    rocketnet_unit_check( ROCKETNET_MSGX_DEFAULT_WORKERS <= ROCKETNET_MSGX_MAX_WORKERS );

    // the receive worker holds the slot after the send workers
    rocketnet_unit_check( ROCKETNET_MSGX_DEFAULT_WORKERS + 1 <= ROCKETNET_MSGX_TABLE_MAX_HOLDERS );
}

REGISTER_UNIT_TEST( test_rocketnet_msgx_default_workers );
