/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The slots machine (docs/CHANNELS.md 10.2; tla/RNSlots.tla Observe and
// Admit). It holds no time, allocates nothing and calls no transport: the
// object hands it the transport's id for each slot and the id tagged on each
// batch entry.

#include <string.h>

#include "rocketnet_slots.h"

void rn_slots_init( struct rn_slots_t * slots, int max_clients )
{
    if ( !slots )
        return;
    // An out-of-range count is refused as zero slots, never clamped to a working machine of the wrong size.
    if ( max_clients < 0 || max_clients > ROCKETNET_MAX_CLIENTS )
    {
        rocketnet_assert( max_clients >= 0 && max_clients <= ROCKETNET_MAX_CLIENTS );
        rocketnet_error( "slots init refused: max_clients %d is outside 0..%d; using zero slots", max_clients, ROCKETNET_MAX_CLIENTS );
        max_clients = 0;
    }
    memset( slots, 0, sizeof( *slots ) );
    slots->max_clients = max_clients;
}

// RNSlots Observe (CHANNELS.md 10.2): the id changing to a nonzero value is
// the CONNECTED event and resets the slot; the id going to 0 is DISCONNECTED
// and drops it; an unchanged id leaves the binding alone. A slot whose
// binding changes is the update's changed slot, which the server withholds
// until each batch entry carries its connection's id (CHANNELS.md 11.5).
// tla/RNSlots.tla Observe
int rn_slots_observe( struct rn_slots_t * slots, int slot, uint64_t connection_id )
{
    if ( !slots || slot < 0 || slot >= slots->max_clients )
        return RN_SLOTS_NONE;

    if ( slots->connection_id[slot] == connection_id )
    {
        slots->changed[slot] = 0;
        return RN_SLOTS_NONE;
    }

    slots->connection_id[slot] = connection_id;
    slots->changed[slot] = 1;
    return connection_id != 0 ? RN_SLOTS_RESET : RN_SLOTS_DROP;
}

// RNSlots Admit (CHANNELS.md 10.2): the slot index alone never decides; an
// entry is applied only to the connection it was received for. A free slot
// binds no connection and no entry carries the id 0, so it discards every
// entry.
// tla/RNSlots.tla Admit
int rn_slots_admit( const struct rn_slots_t * slots, int slot, uint64_t entry_connection_id )
{
    if ( !slots || slot < 0 || slot >= slots->max_clients )
        return RN_SLOTS_DISCARD;

    const uint64_t bound = slots->connection_id[slot];
    if ( bound == 0 || entry_connection_id != bound )
        return RN_SLOTS_DISCARD;
    return RN_SLOTS_APPLY;
}
