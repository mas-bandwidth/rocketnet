/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_REPLAY_H
#define ROCKETNET_REPLAY_H

#include <stdint.h>
#include "rocketnet_encryption_map.h"

// Replay protection over the struct the encryption mapping already carries:
// a high-water mark and a 256-entry window at the standard's floor. Within
// the window a sequence is accepted exactly once; below the window it is
// refused; above the high-water mark it always passes and advances. The
// check is separate from the advance so a packet is marked received only
// after it has actually opened -- marking on receipt would let a forgery
// with a guessed sequence block the real packet.

// returns 1 when this sequence has already been received, or is too old to
// tell -- either way the packet is refused

int rocketnet_replay_already_received( const struct rocketnet_replay_protection_t * replay, uint64_t sequence );

// marks a sequence received after a successful open, advancing the window.
// clears the entries the advance skips past, so a sequence far ahead does
// not inherit stale bits.

void rocketnet_replay_advance( struct rocketnet_replay_protection_t * replay, uint64_t sequence );

#endif // #ifndef ROCKETNET_REPLAY_H
