/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_REPLAY_MARKS_H
#define ROCKETNET_SERVER_REPLAY_MARKS_H

#include <stdint.h>

#include "rocketnet_address.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"

// The userspace walk that decides which kernel replay marks to write and
// forget. Once per server update, after packet processing and before the
// transport flush, it forgets the address of every slot a walk saw connected
// that is no longer connected or whose mapping changed address, whether or not
// it held a mark (the forget also clears the reply MAC learned on accept), then
// marks every connected slot whose session replay window's highest_opened is
// nonzero and differs from what it last marked. All forgets come before any
// mark. The kernel map lags the truth by at most one update and is never ahead
// of it.

struct rocketnet_server_replay_sink_t
{
    void * context;
    void ( *forget )( void *, const struct rocketnet_address_t * );
    void ( *mark )( void *, const struct rocketnet_address_t *, uint32_t high_water );
};

struct rocketnet_server_replay_marks_t;

// One record per client slot: the address a walk last saw the slot connected
// at (type ROCKETNET_ADDRESS_NONE when unseen), and whether a mark is held there
// and its value.

struct rocketnet_server_replay_marks_t * rocketnet_server_replay_marks_create( void * context, int max_clients );

void rocketnet_server_replay_marks_destroy( struct rocketnet_server_replay_marks_t * marks );

// Unit-test poison/forget witness for the private per-slot mark cache.
bool rocketnet_server_replay_marks_reset_witness_for_test();

// tla/RNXdpReplay.tla Walk: PASS 1 forgets the seen address of every slot a
// walk saw connected that is no longer connected or whose mapping address
// differs, mark or not; PASS 2 records every connected slot with a mapping as
// seen at its address and marks it when its highest_opened is nonzero and
// differs from the value it last marked. The sink sees all forgets first.

void rocketnet_server_replay_marks_walk( struct rocketnet_server_replay_marks_t * marks,
                                         const struct rocketnet_client_slots_t * slots,
                                         struct rocketnet_encryption_map_t * map,
                                         const struct rocketnet_server_replay_sink_t * sink );

#endif // #ifndef ROCKETNET_SERVER_REPLAY_MARKS_H
