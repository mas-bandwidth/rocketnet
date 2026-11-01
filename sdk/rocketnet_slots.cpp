/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include <string.h>

#include "rocketnet_slots.h"

void rn_slots_init( struct rn_slots_t * slots, int max_clients, uint64_t * connection_id, uint8_t * changed )
{
    if ( !slots )
        return;

    // an out-of-range count, or no storage for it, is refused as zero slots,
    // never clamped to a working machine of the wrong size

    memset( slots, 0, sizeof( *slots ) );
    if ( max_clients < 0 || max_clients > ROCKETNET_MAX_CLIENTS || ( max_clients > 0 && ( !connection_id || !changed ) ) )
    {
        rocketnet_assert( max_clients >= 0 && max_clients <= ROCKETNET_MAX_CLIENTS );
        rocketnet_error( "slots init refused: max_clients %d is outside 0..%d or has no storage; using zero slots", max_clients, ROCKETNET_MAX_CLIENTS );
        return;
    }

    slots->max_clients = max_clients;
    slots->connection_id = connection_id;
    slots->changed = changed;
    if ( max_clients > 0 )
    {
        memset( connection_id, 0, (size_t) max_clients * sizeof( *connection_id ) );
        memset( changed, 0, (size_t) max_clients * sizeof( *changed ) );
    }
}

// proofs/tla/RNSlots.tla Observe
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

// proofs/tla/RNSlots.tla Admit
int rn_slots_admit( const struct rn_slots_t * slots, int slot, uint64_t entry_connection_id )
{
    if ( !slots || slot < 0 || slot >= slots->max_clients )
        return RN_SLOTS_DISCARD;

    const uint64_t bound = slots->connection_id[slot];
    if ( bound == 0 || entry_connection_id != bound )
        return RN_SLOTS_DISCARD;

    return RN_SLOTS_APPLY;
}
