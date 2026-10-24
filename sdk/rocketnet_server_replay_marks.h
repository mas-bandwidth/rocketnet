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
// transport flush, it forgets the address of every slot that held a mark and is
// no longer connected or whose mapping changed address, then marks every
// connected slot whose session replay window's highest_opened is nonzero and
// differs from what it last marked. All forgets come before any mark. The
// kernel map lags the truth by at most one update and is never ahead of it.

struct rocketnet_server_replay_sink_t
{
    void * context;
    bool ( *forget )( void *, const struct rocketnet_address_t * );
    void ( *mark )( void *, const struct rocketnet_address_t *, uint32_t high_water );
};

struct rocketnet_server_replay_marks_t;

// One record per client slot: whether a mark is held and, when it is, the
// value and the address it was taken from.

struct rocketnet_server_replay_marks_t * rocketnet_server_replay_marks_create( void * context, int max_clients );

void rocketnet_server_replay_marks_destroy( struct rocketnet_server_replay_marks_t * marks );

// Unit-test poison/forget witness for the private per-slot mark cache.
bool rocketnet_server_replay_marks_reset_witness_for_test();

// tla/RNXdpReplay.tla Walk: PASS 1 forgets the marked address of every slot
// with a mark that is no longer connected or whose mapping address differs;
// PASS 2 marks every connected slot whose highest_opened is nonzero and
// differs from the value it last marked. The sink sees all forgets first.

void rocketnet_server_replay_marks_walk( struct rocketnet_server_replay_marks_t * marks,
                                         const struct rocketnet_client_slots_t * slots,
                                         struct rocketnet_encryption_map_t * map,
                                         const struct rocketnet_server_replay_sink_t * sink );

#endif // #ifndef ROCKETNET_SERVER_REPLAY_MARKS_H
