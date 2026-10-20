/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Translation unit that includes the public C SDK header as C++17 so the
// documentation compilation database exposes the public surface.

#include "rocketnet.h"

// The counters of sdk/rocketnet_counters.h, documented: this resolves to the
// documentation-only expansion of ROCKETNET_COUNTER_ROWS that
// docs/mrdocs/CMakeLists.txt writes with tools/docs/counters_doc.py into a
// directory searched before sdk/.
#include "rocketnet_counters.h"
