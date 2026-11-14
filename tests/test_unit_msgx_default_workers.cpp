/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The macOS batched server transport's default number of send threads
// (ROCKETNET_MSGX_DEFAULT_WORKERS, sdk/rocketnet_server_msgx.h) is 3: space
// game's four load rows pass at 3 send threads and one receive thread, and
// the peak row with a game load fails at 2. The constant is msgx's own; no
// other transport reads it. No socket, no clock, no thread.
//
// The expected 3 is copied from the code on purpose: it is a change detector
// for a measured setting, not a derivation. Moving the default means moving
// this line, with the load rows that justify the new value. The bounds below
// it (at least one, within the limit, room for the receive worker) are the
// checks that hold for any value.

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
