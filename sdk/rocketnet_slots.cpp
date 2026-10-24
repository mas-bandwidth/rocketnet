/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The slots machine (docs/CHANNELS.md 10.2; tla/RNSlots.tla Observe and
// Admit). It holds no time, allocates nothing and calls no transport: the
// object hands it the transport's id for each slot and the id tagged on each
// batch entry.
// The rest of RNSlots is modeled at the message layer and the transport the
// object drives; each action is cited here where its step runs:
//   tla/RNSlots.tla BeginUpdate: rn_message_object_update checks every view
//     released before the transport runs (sdk/rocketnet_message_server.cpp).
//   tla/RNSlots.tla EndUpdate: that update returns with phase application,
//     exposing this update's views.
//   tla/RNSlots.tla Request: rn_message_object_start submits the next record
//     against the slot's bound connection.
//   tla/RNSlots.tla StartPending: rn_message_object_start holds one start
//     pending per object on a connected slot.
//   tla/RNSlots.tla FinishPending: rn_message_object_finish (and abort) end
//     the pending start, accepted or refused.
//   tla/RNSlots.tla Send: the transport send puts an opened packet on the
//     wire (rocketnet_message_server_send_packets).
//   tla/RNSlots.tla TransportOpen: the transport opens a received packet and
//     stamps its connection at that moment (the receive path).
//   tla/RNSlots.tla TransportAck: the transport's acked-packet-id
//     notification (P2/P4) carries the original connection tag.
//   tla/RNSlots.tla Kick: rn_message_object_kick ends an exhausted slot with
//     reason MESSAGE_MEMORY_EXHAUSTED.
//   tla/RNSlots.tla PublicDisconnect: rocketnet_server_disconnect_client and
//     rocketnet_server_disconnect_all_clients end a slot between updates.
//   tla/RNSlots.tla ResetFree: the next update resets a disconnected slot to
//     Free, releasing its region and held views.
//   tla/RNSlots.tla Release: rn_message_object_release gives a held view back
//     so the region room is reusable.
//   tla/RNSlots.tla CarryOn: the "carryon" defect recovers an exhausted slot
//     to Connected (reversed witness MCRNSlotsBrokenKickCarriesOn.cfg only;
//     no production path).
//   tla/RNSlots.tla RejectUnreleasedUpdate: an update over unreleased views
//     is refused with ROCKETNET_UPDATE_MESSAGE_NOT_RELEASED.

#include <string.h>

#include "rocketnet_slots.h"

void rn_slots_init( struct rn_slots_t * slots, int max_clients )
{
    if ( !slots )
        return;
    if ( max_clients < 0 )
        max_clients = 0;
    if ( max_clients > ROCKETNET_MAX_CLIENTS )
        max_clients = ROCKETNET_MAX_CLIENTS;
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
