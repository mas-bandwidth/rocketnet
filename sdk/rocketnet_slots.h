/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SLOTS_H
#define ROCKETNET_SDK_ROCKETNET_SLOTS_H

// The slots machine (CHANNELS.md 10.2, tla/RNSlots.tla Observe and Admit).
// It holds the connection each slot is bound to and the per-update changed
// bit; observe reports the slot's CONNECTED (RESET) and DISCONNECTED (DROP)
// events, and admit says whether a batch entry belongs to the slot's current
// connection. No time, no allocation and no transport call: the caller feeds
// observe from the transport's slot accessor and admit from the entry's
// connection tag; every batch entry carries its connection's id.
//
// Header only state, so the object embeds it in its one allocation.

#include <stdint.h>

#include "rocketnet_internal.h"                  // ROCKETNET_MAX_CLIENTS

// observe's answers, and the RNSlots Observe actions they name
#define RN_SLOTS_NONE    0              // the binding did not change
#define RN_SLOTS_RESET   1              // the id changed to a nonzero value (CONNECTED)
#define RN_SLOTS_DROP    2              // the id went to 0 (DISCONNECTED)

// admit's answers, and the RNSlots Admit actions they name
#define RN_SLOTS_APPLY   0              // the entry belongs to the bound connection
#define RN_SLOTS_DISCARD 1              // the entry does not, or the slot is free

struct rn_slots_t
{
    int max_clients;
    uint64_t connection_id[ROCKETNET_MAX_CLIENTS];   // 0 when the slot is free
    uint8_t changed[ROCKETNET_MAX_CLIENTS];          // bound or dropped in this update
};

// A machine with every slot free.
void rn_slots_init( struct rn_slots_t * slots, int max_clients );

// RNSlots Observe: reads one slot's connection id after the transport's
// update. Every call refreshes the slot's per-update changed bit, which the
// server uses to withhold that slot until each batch entry carries its
// connection's id (CHANNELS.md 11.5).
int rn_slots_observe( struct rn_slots_t * slots, int slot, uint64_t connection_id );

// RNSlots Admit: the slot index alone never decides. APPLY only when the slot
// is bound and the entry's connection id is that binding; DISCARD otherwise,
// a free slot included (no entry carries the id 0).
int rn_slots_admit( const struct rn_slots_t * slots, int slot, uint64_t entry_connection_id );

#endif // ROCKETNET_SDK_ROCKETNET_SLOTS_H
