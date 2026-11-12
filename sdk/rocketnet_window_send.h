/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_WINDOW_SEND_H
#define ROCKETNET_SDK_ROCKETNET_WINDOW_SEND_H

#include "rocketnet_allocator.h"
#include "rocketnet_message.h"

// The pure sender numbering half of standard/MESSAGES.md m5.1. The per-number
// table belongs to the connection's region; this state reads no clock, owns
// no socket, and allocates nothing (R4). A caller invokes first_write only
// when it is about to put the record in a packet, never at send_message.
struct rn_window_send_t
{
    rn_region_t * region;
    uint64_t next;
    uint64_t U;
    uint64_t U_prime;
    uint64_t ring_next;
    uint8_t stalled;
};

enum
{
    RN_WINDOW_SEND_STALL = 0,
    RN_WINDOW_SEND_NUMBERED = 1,
    RN_WINDOW_SEND_UNRELIABLE = 2
};

void rn_window_send_init( rn_window_send_t * send, rn_region_t * region, uint64_t first_number );

// Number a single record at first packet write. A code 0/1 record returns
// UNRELIABLE without changing the window. On NUMBERED, outputs are valid;
// ring_start is meaningful only for code 3/4.
int rn_window_send_first_write( rn_window_send_t * send, int code, uint32_t bytes,
                                uint32_t descriptor, uint64_t * number, uint64_t * ring_start );

// Reserve a block's consecutive numbers and contiguous ring span together.
// Returns its fragment count, or zero while a bound stops reliable numbering.
uint32_t rn_window_send_first_write_block( rn_window_send_t * send, int code, uint32_t bytes,
                                           uint32_t descriptor, uint64_t * first_number, uint64_t * ring_start );

// Whether first_write (one fragment) or first_write_block (fragments > 1)
// would number this message now. Changes nothing: the endpoint asks before it
// starts a frame, so a frame is never started for a message the window stalls.
int rn_window_send_would_number( const rn_window_send_t * send, int code, uint32_t bytes, int fragments );

// As would_number, and a refusal marks and counts the stall as first_write's
// does, so a stall found before a frame is started reads as one found by it.
int rn_window_send_stall_unless_numbered( rn_window_send_t * send, int code, uint32_t bytes, int fragments );

// A sent-packet map calls this for each acknowledged number. Stale or repeat
// acks return zero; a newly marked number returns one.
int rn_window_send_mark_acked( rn_window_send_t * send, uint64_t number );

#endif // ROCKETNET_SDK_ROCKETNET_WINDOW_SEND_H
