/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_window_receive.h"
#include "rocketnet_platform.h"

// standard/MESSAGES.md m5.2 (R8), RNWindow Accept: one duplicate record, one owner's
// counter. Regions can share that owner across connection threads (16.1).
static int rn_window_duplicate( rn_window_receive_t * receive )
{
    if ( receive->region->counters )
        rocketnet_platform_atomic_add_u64(
            (volatile uint64_t *) &receive->region->counters->values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE], 1 );

    return RN_WINDOW_RECEIVE_DUPLICATE;
}

// standard/MESSAGES.md m5, RNWindow Init: the region is carved by layer 1 at
// connect; this module owns only R and its pointer into those fixed tables.
// proofs/tla/RNWindow.tla Init
void rn_window_receive_init( rn_window_receive_t * receive, rn_region_t * region, uint64_t first_number )
{
    rocketnet_assert( receive && region && region->received_bits && region->receive_descriptors );
    rocketnet_assert( region->window && ( region->window & ( region->window - 1 ) ) == 0 );

    receive->region = region;
    receive->R = first_number;
    receive->violation = ROCKETNET_MESSAGE_VIOLATION_NONE;
}

// standard/MESSAGES.md m5.2 and m4; RNWindow Recon and Accept. The
// caller applies records in wire order and stops a payload on VIOLATION (R6).
// proofs/tla/RNWindow.tla OpenPacket
// proofs/tla/RNWindowLayout.tla Accept: the reconstruction, the bit word and index, the collision test, the claim and the advance at the code's layout
int rn_window_receive_accept( rn_window_receive_t * receive, const rocketnet_message_record_t * record, uint32_t * slot )
{
    rocketnet_assert( receive && receive->region && record && slot );

    if ( receive->violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return RN_WINDOW_RECEIVE_VIOLATION;

    *slot = RN_DESCRIPTOR_NONE;

    const uint32_t W = receive->region->window;
    const uint32_t wire = record->number & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t reference = (uint32_t) receive->R & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t unsigned_delta = ( wire - reference ) & ROCKETNET_MESSAGE_U24_MASK;
    const int32_t delta = unsigned_delta >= ROCKETNET_MESSAGE_HALF_RANGE ? (int32_t) unsigned_delta - (int32_t) ROCKETNET_MESSAGE_NUMBER_RANGE : (int32_t) unsigned_delta;

    // A negative signed half-range difference is behind R, including when
    // R is zero. No unsigned subtraction can turn it into a huge fresh n.

    if ( delta < 0 )
        return rn_window_duplicate( receive );

    if ( (uint32_t) delta >= W || receive->R > UINT64_MAX - (uint32_t) delta )
    {
        receive->violation = ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW;
        return RN_WINDOW_RECEIVE_VIOLATION;
    }

    const uint64_t number = receive->R + (uint32_t) delta;
    const uint32_t index = (uint32_t) ( number & ( W - 1 ) );
    const uint64_t bit = 1ULL << ( index & 63 );
    uint64_t * word = &receive->region->received_bits[index >> 6];
    if ( *word & bit )
        return rn_window_duplicate( receive );

    rn_receive_descriptor_t * descriptor = &receive->region->receive_descriptors[index];
    if ( descriptor->live && descriptor->number != number )
    {
        receive->violation = ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION;
        return RN_WINDOW_RECEIVE_VIOLATION;
    }

    // RNWindow Accept: claim the descriptor before advancing R. Layer 4
    // releases it when delivery/update lifetime or a hold ends. The block
    // bookkeeping fields belong to layer 5 and begin empty here.

    descriptor->number = number;
    descriptor->ordering = record->ordering;
    descriptor->sequencing = record->sequencing;
    descriptor->ring = record->ring;
    descriptor->bytes = (uint32_t) record->bytes;
    descriptor->channel = (uint16_t) record->channel;
    descriptor->code = (uint8_t) record->code;
    descriptor->block_have = 0;
    descriptor->block_count = 0;
    descriptor->live = 1;
    *word |= bit;
    *slot = index;

    // RNWindow Advance: inspect only bits R actually passes.
    // Clearing one frees that ring position for the number W above it, while
    // the live descriptor remains protected until release(number).

    for ( ;; )
    {
        const uint32_t floor_index = (uint32_t) ( receive->R & ( W - 1 ) );
        const uint64_t floor_bit = 1ULL << ( floor_index & 63 );
        uint64_t * floor_word = &receive->region->received_bits[floor_index >> 6];
        if ( !( *floor_word & floor_bit ) )
            break;

        *floor_word &= ~floor_bit;
        receive->R++;
    }

    return RN_WINDOW_RECEIVE_ACCEPT;
}

#if ROCKETNET_DEV
// standard/MESSAGES.md m5, m5.4, RNWindow EndUpdate and held release: only the exact live
// number may free its slot; R and received history never move backwards.
// proofs/tla/RNWindow.tla EndUpdate
// proofs/tla/RNWindowLayout.tla Release: live = 0, as every release of a receive descriptor does
void rn_window_receive_release( rn_window_receive_t * receive, uint64_t number )
{
    rocketnet_assert( receive && receive->region );

    const uint32_t index = (uint32_t) ( number & ( receive->region->window - 1 ) );
    rn_receive_descriptor_t * descriptor = &receive->region->receive_descriptors[index];
    if ( descriptor->live && descriptor->number == number )
        descriptor->live = 0;
}
#endif // #if ROCKETNET_DEV
