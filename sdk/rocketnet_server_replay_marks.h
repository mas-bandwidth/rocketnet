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
// transport flush, it forgets the address of every slot that was seen
// connected and is no longer connected or whose mapping changed address,
// then marks every connected slot whose session replay window's highest_opened
// is nonzero and differs from what it last marked. All forgets come before
// any mark. A forget the sink cannot queue leaves the seen and marked records
// in place, so the next walk retries the old address before a new mapping may
// replace it. Once queued, the transport owns the forget until the kernel
// accepts the delete (tla/RNXdpReplay.tla, ForgetOwedUntilApplied).

struct rocketnet_server_replay_sink_t
{
    void * context;
    bool ( *forget )( void *, const struct rocketnet_address_t * );
    void ( *mark )( void *, const struct rocketnet_address_t *, uint32_t high_water );
};

struct rocketnet_server_replay_marks_t;

// One record per client slot: whether a mark is held and, when it is, the
// value and the address it was taken from, and whether the slot was seen
// connected and, when it was, the address it was seen at. The seen record
// covers a slot that left before it ever marked, so that the transport
// forgets the reply MAC of that address as well.

struct rocketnet_server_replay_marks_t * rocketnet_server_replay_marks_create( void * context, int max_clients );

void rocketnet_server_replay_marks_destroy( struct rocketnet_server_replay_marks_t * marks );

// Unit-test poison/forget witness for the private per-slot mark cache.
bool rocketnet_server_replay_marks_reset_witness_for_test();

// tla/RNXdpReplay.tla Walk: PASS 1 forgets the address of every slot that
// was seen connected and is no longer connected or whose mapping address
// differs, whether or not it held a mark; PASS 2 marks every connected slot
// whose highest_opened is nonzero and differs from the value it last marked,
// and records every connected slot with a mapping as seen at its address. The
// sink sees all forgets first. A refused forget keeps both records, and PASS
// 2 skips the slot while its old seen address is still pending.

void rocketnet_server_replay_marks_walk( struct rocketnet_server_replay_marks_t * marks,
                                         const struct rocketnet_client_slots_t * slots,
                                         struct rocketnet_encryption_map_t * map,
                                         const struct rocketnet_server_replay_sink_t * sink );

#endif // #ifndef ROCKETNET_SERVER_REPLAY_MARKS_H
