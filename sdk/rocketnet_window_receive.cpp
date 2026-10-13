/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_window_receive.h"
#include "rocketnet_platform.h"

#include <string.h>

// The receiving half of layer 3, the reliable window (docs/CHANNELS.md 6.1,
// 6.3, 6.5, 13.5). Rules kept here: R1 (no live allocation), R4 (pure module),
// R6 (a violation stops parsing and leaves the module in error), R7 (advance
// is O(advance), never a scan of the window), R8 (a duplicate is its own
// counter). The TLA+ twin is tla/RNWindow.tla; its receiver's operations and
// the checks they state are cited on each function below.

static void rn_window_receive_count( rocketnet_counters_t * counters, int counter )
{
    // the owner's counters are shared by all of an object's regions, and
    // different connections may receive from different threads (16.1), so a
    // drop or a violation adds atomically, as rn_region_send_alloc counts its
    // refusals (docs/CHANNELS.md 4, "As built").
    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &counters->values[counter], 1 );
}

// The descriptor slot of a number: n mod W. W is a power of two (validated at
// create, docs/CHANNELS.md 14), so the mod is a mask.
static inline uint32_t rn_window_receive_slot( const rn_window_receive_t * w, uint64_t n )
{
    return (uint32_t) ( n & (uint64_t) ( w->region->window - 1 ) );
}

static inline int rn_window_receive_bit_get( const rn_window_receive_t * w, uint64_t n )
{
    const uint32_t s = rn_window_receive_slot( w, n );
    return (int) ( ( w->region->received_bits[s >> 6] >> ( s & 63 ) ) & 1 );
}

static inline void rn_window_receive_bit_set( rn_window_receive_t * w, uint64_t n )
{
    const uint32_t s = rn_window_receive_slot( w, n );
    w->region->received_bits[s >> 6] |= 1ULL << ( s & 63 );
}

static inline void rn_window_receive_bit_clear( rn_window_receive_t * w, uint64_t n )
{
    const uint32_t s = rn_window_receive_slot( w, n );
    w->region->received_bits[s >> 6] &= ~( 1ULL << ( s & 63 ) );
}

// 13.5, RNWindow Recon: the 24-bit wire value, read against R by signed half
// range. n = R + d, where d is the signed difference of the wire value from
// R's own wire value, in -2^23 .. 2^23-1. Every live value lies within W (at
// most 2^20) of R, and a stale resend is bounded by the transport's replay
// window (13.5), so the reading never aliases.
static uint64_t rn_window_receive_reconstruct( uint64_t R, uint32_t wire )
{
    const uint64_t mod  = (uint64_t) ROCKETNET_MESSAGE_U24_MASK + 1u;   // 2^24
    const uint64_t half = mod / 2;                                      // 2^23
    const uint64_t r    = R & ROCKETNET_MESSAGE_U24_MASK;               // R's wire value
    const uint64_t d    = ( (uint64_t) wire + mod - r ) % mod;          // the signed difference, biased
    return d >= half ? R + d - mod : R + d;
}

// 6.5, RNWindow Advance: while the bit at R is set, clear it and advance R.
// Only bits in [R, R + W) are ever set (the window check admits n < R + W and
// drops n < R), so this loop runs once per set bit it clears and never scans
// the window (R7): each bit is cleared exactly once, ever.
static void rn_window_receive_advance( rn_window_receive_t * w )
{
    while ( rn_window_receive_bit_get( w, w->R ) )
    {
        rn_window_receive_bit_clear( w, w->R );
        w->R++;
    }
}

void rn_window_receive_init( rn_window_receive_t * w, rn_region_t * region )
{
    w->region = region;
    w->R = 0;
    w->error = ROCKETNET_MESSAGE_VIOLATION_NONE;
}

int rn_window_receive_apply( rn_window_receive_t * w, const struct rocketnet_message_record_t * record,
                             uint32_t * slot, int * violation )
{
    if ( violation )
        *violation = ROCKETNET_MESSAGE_VIOLATION_NONE;

    // a violation stops processing and leaves the module in its error state (R6)
    if ( w->error != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        if ( violation )
            *violation = w->error;
        return ROCKETNET_WINDOW_RECEIVE_VIOLATION;
    }

    // 6.5, RNWindow Recon: read the wire number against R
    const uint64_t n = rn_window_receive_reconstruct( w->R, record->number );

    // 6.5, RNWindow Accept (DropDuplicates): n < R, or the bit already set, is
    // a duplicate after a resend: dropped and counted, not a violation
    if ( n < w->R || rn_window_receive_bit_get( w, n ) )
    {
        rn_window_receive_count( w->region->counters, ROCKETNET_COUNTER_MESSAGE_DUPLICATE );
        return ROCKETNET_WINDOW_RECEIVE_DUPLICATE;
    }

    // 6.5, RNWindow Accept (outside): n >= R + W is MESSAGE_NUMBER_OUTSIDE_WINDOW.
    // A violation is named in the module's error state and returned; it is not a
    // counter here (the endpoint maps it to its per-violation counter, R6).
    if ( n >= w->R + w->region->window )
    {
        w->error = ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW;
        if ( violation )
            *violation = w->error;
        return ROCKETNET_WINDOW_RECEIVE_VIOLATION;
    }

    const uint32_t s = rn_window_receive_slot( w, n );
    rn_receive_descriptor_t * descriptor = &w->region->receive_descriptors[s];

    // 6.5, RNWindow Accept (clash): a live descriptor naming a different number
    // is MESSAGE_SLOT_COLLISION
    if ( descriptor->live && descriptor->number != n )
    {
        w->error = ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION;
        if ( violation )
            *violation = w->error;
        return ROCKETNET_WINDOW_RECEIVE_VIOLATION;
    }

    // 6.5: set the bit, fill the descriptor (live), then advance R past the
    // set bits. The descriptor carries the fields layer 4 dispatches on; the
    // block fields (block_have, block_count) are layer 5's.
    rn_window_receive_bit_set( w, n );
    memset( descriptor, 0, sizeof( *descriptor ) );
    descriptor->number = n;
    descriptor->ordering = record->ordering;
    descriptor->sequencing = record->sequencing;
    descriptor->ring = record->ring;
    descriptor->bytes = (uint32_t) record->bytes;
    descriptor->channel = (uint16_t) record->channel;
    descriptor->code = (uint8_t) record->code;
    descriptor->live = 1;

    rn_window_receive_advance( w );

    if ( slot )
        *slot = s;
    return ROCKETNET_WINDOW_RECEIVE_ACCEPT;
}

void rn_window_receive_release( rn_window_receive_t * w, uint64_t number )
{
    // 6.3 step 1, RNWindow EndUpdate: a live number stops being live (its
    // update ended, or a hold was released). The descriptor at number % W, if
    // it is live and names number, is marked not live; the received bit stays,
    // so a late resend of number is still a duplicate.
    const uint32_t s = rn_window_receive_slot( w, number );
    rn_receive_descriptor_t * descriptor = &w->region->receive_descriptors[s];
    if ( descriptor->live && descriptor->number == number )
        descriptor->live = 0;
}
