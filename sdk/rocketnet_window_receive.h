/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_WINDOW_RECEIVE_H
#define ROCKETNET_WINDOW_RECEIVE_H

#include <stdint.h>
#include <stddef.h>

#include "rocketnet.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_allocator.h"

// The receiving half of layer 3, the reliable window (docs/CHANNELS.md 6.1,
// 6.3, 6.5, 13.5; tla/RNWindow.tla and tla/README-RNWindow.md, case
// MCRNWindow.cfg). A pure module (R4): its state is one struct plus the
// connection's region (layer 1) it points into -- received_bits,
// receive_descriptors, window -- plus R, the lowest number not yet received
// (u64). It holds no socket, reads no clock and allocates nothing.
//
// Events: apply one reliable record's wire number (u24) with the fields layer
// 2's reader hands out (rocketnet_message_record_t), and release(number), which
// layer 4 calls when a live number stops being live (delivered and its update
// ended, or a hold released). Answers: ACCEPT with the descriptor slot,
// DUPLICATE, or a violation.
//
// The rules (6.5, RNWindow Accept and Recon): reconstruct the number against
// R by signed 24-bit half range (13.5); n < R, or its bit already set, is a
// duplicate, dropped and counted MESSAGE_DUPLICATE (not a violation);
// n >= R + W is the violation MESSAGE_NUMBER_OUTSIDE_WINDOW; a live descriptor
// at n mod W naming a different number is the violation MESSAGE_SLOT_COLLISION;
// otherwise set the bit, fill the descriptor (live), and while the bit at R is
// set clear it and advance R, at a cost of O(advance), never a scan of the
// window (R7). A violation stops processing and leaves the module in its
// error state (R6).

// The module's whole state. received_bits, receive_descriptors and window are
// the connection's region's (layer 1); the module points into them.

struct rn_window_receive_t
{
    rn_region_t * region;           // layer 1: received_bits, receive_descriptors, window, counters
    uint64_t R;                     // the lowest number not yet received (6.1)
    int error;                      // ROCKETNET_MESSAGE_VIOLATION_NONE, or the first violation (R6)
};

// The answer to applying one record (6.5).
#define ROCKETNET_WINDOW_RECEIVE_ACCEPT      0   // fresh; the descriptor slot is *slot
#define ROCKETNET_WINDOW_RECEIVE_DUPLICATE   1   // dropped and counted MESSAGE_DUPLICATE, not a violation
#define ROCKETNET_WINDOW_RECEIVE_VIOLATION   2   // a layer-3 violation; *violation names it

// Binds the module to a region. The region must already be carved (its bitset
// and descriptors zeroed). No bytes are written; R starts at 0.

void rn_window_receive_init( rn_window_receive_t * w, rn_region_t * region );

// Applies one reliable record (code 2, 3 or 4) against the window (6.5).
// Returns one of the three answers above; on ACCEPT *slot is the descriptor
// slot (n % W) and *violation is ROCKETNET_MESSAGE_VIOLATION_NONE, on
// DUPLICATE MESSAGE_DUPLICATE is counted, on VIOLATION *violation is
// MESSAGE_NUMBER_OUTSIDE_WINDOW or MESSAGE_SLOT_COLLISION, counted on the
// region's owner counters atomically, and the module is left in its error
// state: every later apply returns VIOLATION with that violation again (R6).

int rn_window_receive_apply( rn_window_receive_t * w, const struct rocketnet_message_record_t * record,
                             uint32_t * slot, int * violation );

// Layer 4 calls this when a live number stops being live (6.3 step 1, RNWindow
// EndUpdate): the descriptor at number % W, if it is live and names number, is
// marked not live. Nothing else changes.

void rn_window_receive_release( rn_window_receive_t * w, uint64_t number );

#endif // #ifndef ROCKETNET_WINDOW_RECEIVE_H
