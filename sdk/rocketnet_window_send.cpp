/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_window_send.h"
#include "rocketnet_platform.h"

// standard/MESSAGES.md m5.1 len16; RNRing Len16 and PlaceSingle/PlaceBlock.
static uint64_t rn_window_send_round16( uint32_t bytes )
{
    return ( (uint64_t) bytes + 15 ) & ~(uint64_t) 15;
}

// standard/MESSAGES.md m5.1 boundary padding; RNRing Pos and PlaceSingle/PlaceBlock.
static uint64_t rn_window_send_position( const rn_window_send_t * send, uint64_t span )
{
    const uint64_t B = send->region->ring_bytes;
    const uint64_t remaining = B - send->ring_next % B;

    return span > remaining ? send->ring_next + remaining : send->ring_next;
}

// standard/MESSAGES.md m5.1's count and byte bounds; RNWindow NumberRecord and RNRing
// Admits/PlaceSingle/PlaceBlock. Subtraction avoids overflowing U'+W or
// p+span. A failed first write counts once until U' moves or one succeeds.
// proofs/tla/RNWindow.tla NumberRecord
static int rn_window_send_admit( rn_window_send_t * send, uint32_t count,
                                 uint64_t span, int holdable, uint64_t * position )
{
    const uint64_t W = send->region->window;
    const uint64_t B = send->region->ring_bytes;

    rocketnet_assert( send->next >= send->U_prime && count > 0 && count <= W );

    // proofs/tla/RNWindow.tla NumberRecord and ASSUME W \in Nat \ {0}: a block's
    // numbers fit in W. The assert compiles out in Release (NDEBUG), so a
    // count of 0 or above W, or a window of 0, stalls here before W - count
    // can wrap or any % W is taken.

    int fits = count > 0 && count <= W && send->next - send->U_prime <= W - count;
    uint64_t p = send->ring_next;

    // proofs/tla/RNRing.tla ASSUME B > 0 /\ B % 16 = 0: the init assert compiles out
    // in Release, so a zero-byte ring stalls here rather than reaching the
    // modulo in rn_window_send_position.

    if ( holdable && B == 0 )
        fits = 0;
    else if ( fits && holdable )
    {
        p = rn_window_send_position( send, span );
        const uint64_t floor = send->U_prime < send->next ? send->region->send_numbers[send->U_prime % W].ring_start : p;

        rocketnet_assert( p >= floor );
        fits = fits && span <= B && p >= floor && p - floor <= B - span;
    }

    if ( !fits )
    {
        if ( !send->stalled )
        {
            if ( send->region->counters )
                rocketnet_platform_atomic_add_u64(
                    (volatile uint64_t *) &send->region->counters->values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL], 1 );

            send->stalled = 1;
        }

        return 0;
    }

    *position = p;
    return 1;
}

// standard/MESSAGES.md m5, m5.1; RNWindow NumberRecord and RNRing PlaceSingle/PlaceBlock.
// Code 2 gets a zero-byte ring anchor at p so it can safely be U'.
// proofs/tla/RNWindow.tla NumberRecord
static void rn_window_send_set_number( rn_window_send_t * send, uint64_t number,
                                       uint32_t descriptor, uint16_t fragment,
                                       uint64_t ring_start )
{

    rn_send_number_t * entry = &send->region->send_numbers[number % send->region->window];
    entry->number = number;
    entry->ring_start = ring_start;

    // A new number is never sent. A carve's memset leaves unnumbered slots at
    // 0.0, but only numbered slots are walked, so that is fine.

    entry->last_send = RN_SEND_NUMBER_NEVER_SENT;
    entry->descriptor = descriptor;
    entry->fragment = fragment;
    entry->acked = 0;
}

// standard/MESSAGES.md m5, m5.1; RNWindow Init and RNRing Init. Layer 1 already carved
// the fixed region; this state has no allocation, clock or socket (R4).
// proofs/tla/RNWindow.tla Init
void rn_window_send_init( rn_window_send_t * send, rn_region_t * region, uint64_t first_number )
{
    rocketnet_assert( send && region && region->send_numbers );
    rocketnet_assert( region->window && ( region->window & ( region->window - 1 ) ) == 0 );
    rocketnet_assert( region->ring_bytes && region->ring_bytes % 16 == 0 );

    send->region = region;
    send->next = first_number;
    send->U = first_number;
    send->U_prime = first_number;
    send->ring_next = 0;
    send->stalled = 0;
}

// standard/MESSAGES.md m5.1; RNWindow NumberRecord and RNRing PlaceSingle. This
// is called only at first packet write, never when send_message queues it.
// proofs/tla/RNWindow.tla NumberRecord
int rn_window_send_first_write( rn_window_send_t * send, int code, uint32_t bytes,
                                uint32_t descriptor, uint64_t * number, uint64_t * ring_start )
{
    rocketnet_assert( send && send->region && number && ring_start );

    if ( code == ROCKETNET_MESSAGE_CODE_UNRELIABLE || code == ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED )
        return RN_WINDOW_SEND_UNRELIABLE;

    rocketnet_assert( code >= ROCKETNET_MESSAGE_CODE_RELIABLE && code <= ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED );
    rocketnet_assert( bytes > 0 && bytes <= ROCKETNET_MESSAGE_SINGLE_BYTES );
    rocketnet_assert( bytes <= send->region->max_message_bytes );

    const int holdable = code != ROCKETNET_MESSAGE_CODE_RELIABLE;
    const uint64_t span = holdable ? rn_window_send_round16( bytes ) : 0;

    uint64_t p;
    if ( !rn_window_send_admit( send, 1, span, holdable, &p ) )
        return RN_WINDOW_SEND_STALL;

    const uint64_t n = send->next;
    rn_window_send_set_number( send, n, descriptor, 0, p );
    send->next++;

    if ( holdable )
        send->ring_next = p + span;
    send->stalled = 0;

    *number = n;
    *ring_start = p;

    return RN_WINDOW_SEND_NUMBERED;
}

// standard/MESSAGES.md m5.1, m8; RNWindow NumberRecord and RNRing PlaceBlock. Reserve
// all k consecutive numbers and one span at first fragment write, checking
// both bounds once. The concrete final fragment uses rounded actual bytes
// (standard/MESSAGES.md m4); RNRing abstracts it to a full F stride.
// proofs/tla/RNWindow.tla NumberRecord
uint32_t rn_window_send_first_write_block( rn_window_send_t * send, int code, uint32_t bytes,
                                           uint32_t descriptor, uint64_t * first_number, uint64_t * ring_start )
{
    rocketnet_assert( send && send->region && first_number && ring_start );
    rocketnet_assert( code >= ROCKETNET_MESSAGE_CODE_RELIABLE && code <= ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED );
    (void) code; // read only by the assert above

    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    rocketnet_assert( bytes > F && bytes <= send->region->max_message_bytes );

    const uint32_t count = ( bytes + F - 1 ) / F;
    rocketnet_assert( count >= 2 && count <= send->region->window );

    const uint32_t last = bytes - ( count - 1 ) * F;
    const uint64_t last16 = rn_window_send_round16( last );
    const uint64_t span = (uint64_t) ( count - 1 ) * F + last16;
    rocketnet_assert( span <= send->region->ring_bytes );

    uint64_t p;
    if ( !rn_window_send_admit( send, count, span, 1, &p ) )
        return 0;

    const uint64_t first = send->next;
    for ( uint32_t i = 0; i < count; i++ )
        rn_window_send_set_number( send, first + i, descriptor, (uint16_t) i, p + (uint64_t) i * F );

    send->next += count;
    send->ring_next = p + span;
    send->stalled = 0;
    *first_number = first;
    *ring_start = p;

    return count;
}

// standard/MESSAGES.md m5.1, m9 (R7); RNWindow ReceiveAck and RNRing Ack. U advances
// only over newly acknowledged contiguous entries; U' stays at a partial
// block's first fragment until the whole block has been acknowledged.
// proofs/tla/RNWindow.tla ReceiveAck
int rn_window_send_mark_acked( rn_window_send_t * send, uint64_t number )
{
    rocketnet_assert( send && send->region );

    if ( number < send->U || number >= send->next )
        return 0;

    rn_send_number_t * entry = &send->region->send_numbers[number % send->region->window];
    if ( entry->number != number || entry->acked )
        return 0;

    entry->acked = 1;

    while ( send->U < send->next )
    {
        const rn_send_number_t * oldest = &send->region->send_numbers[send->U % send->region->window];
        if ( oldest->number != send->U || !oldest->acked )
            break;

        send->U++;
    }

    const uint64_t old_prime = send->U_prime;

    if ( send->U == send->next )
        send->U_prime = send->U;
    else
    {
        const rn_send_number_t * oldest = &send->region->send_numbers[send->U % send->region->window];
        rocketnet_assert( oldest->number == send->U && oldest->fragment <= send->U );
        send->U_prime = send->U - oldest->fragment;
    }

    if ( send->U_prime != old_prime )
        send->stalled = 0;

    return 1;
}
