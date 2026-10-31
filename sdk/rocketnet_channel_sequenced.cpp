/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_channel.h"

#include <string.h>

#include "rocketnet_platform.h"
#include "rocketnet_test_seams.h"

#if ROCKETNET_DEV
// Unit-library instrumentation only: hold entries visited by the hold take
// and by the end-of-update pass, per thread; no shipped library carries it.
thread_local unsigned long long g_sequenced_hold_visit_count = 0;
#endif // #if ROCKETNET_DEV

// Codes 1 and 4 follow RNChannelTypes!Dispatch and !Drain. A code-4 hold
// uses its receive descriptor and ring span; the high bit distinguishes that
// descriptor index from a code-1 Ru hold without changing channel state.
static const uint32_t RN_SEQUENCED_RELIABLE_HOLD = 0x80000000u;

// proofs/tla/RNChannelTypes.tla OpenPacket

static bool rn_sequenced_reliable_hold( uint32_t hold )
{
    return hold != RN_DESCRIPTOR_NONE && ( hold & RN_SEQUENCED_RELIABLE_HOLD ) != 0;
}

static rn_receive_descriptor_t * rn_sequenced_descriptor( rn_channels_t * channels, const rocketnet_message_record_t * record, uint32_t * slot )
{
    rn_region_t * region = channels->region;
    *slot = record->number & ( region->window - 1 );

    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[*slot];
    if ( !descriptor->live || ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) != record->number ||
         descriptor->code != ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        return NULL;

    return descriptor;
}

// proofs/tla/RNChannel.tla BuffersBounded: a returned hold rejoins the free stack in O(1).

static void rn_sequenced_hold_return( rn_region_t * region, uint32_t slot )
{
    region->sequenced_messages_held[slot].used = 0;
    region->sequenced_hold_free_stack[region->sequenced_hold_free_count++] = slot;
}

static void rn_sequenced_release_number( rn_channels_t * channels, uint64_t number )
{
    rn_region_t * region = channels->region;
    rn_receive_descriptor_t * descriptor = &region->receive_descriptors[number & ( region->window - 1 )];
    if ( descriptor->live && ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) == ( number & ROCKETNET_MESSAGE_U24_MASK ) )
        descriptor->live = 0;
}

// A descriptor a code-4 block owns: one of its fragments, so a code-4 record
// on the block's channel with the anchor's ordering and sequencing (every
// fragment that joins the block is checked against the anchor for both).

static bool rn_sequenced_block_owns( const rn_receive_descriptor_t * anchor, const rn_receive_descriptor_t * descriptor )
{
    return descriptor->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED && descriptor->channel == anchor->channel &&
           ( ( descriptor->ordering ^ anchor->ordering ) & ROCKETNET_MESSAGE_U24_MASK ) == 0 &&
           ( ( descriptor->sequencing ^ anchor->sequencing ) & ROCKETNET_MESSAGE_U24_MASK ) == 0;
}

// A code-4 hold's descriptor is live and is code 4 on the channel holding it;
// anything else is a broken invariant (proofs/tla/RNHeldOwnership.tla HeldNamesLive).

static bool rn_sequenced_hold_valid( const rn_region_t * region, uint32_t hold, uint32_t channel )
{
    if ( !rn_sequenced_reliable_hold( hold ) )
        return true;

    const uint32_t slot = hold & ~RN_SEQUENCED_RELIABLE_HOLD;
    if ( slot >= region->window )
        return false;

    const rn_receive_descriptor_t * descriptor = &region->receive_descriptors[slot];
    return descriptor->live && descriptor->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED && descriptor->channel == channel;
}

// A hold is a slot (held_sequenced or block_in_turn) and the first number of
// its code-4 span (held_sequenced_first or block_in_turn_first).
// proofs/tla/RNHeldOwnership.tla Supersede: a block's release frees only the
// descriptors it owns. A live descriptor of a number in the claimed span that
// the block does not own means the sender gave one number to two records: it
// is not released, the hold is dropped and the connection ends
// (MESSAGE_BAD_BLOCK). Returns false then; the caller returns
// ROCKETNET_CHANNEL_DISPATCH_NONE. A release by number alone is the reversed
// witness MCRNHeldOwnershipBrokenNumberOnlyRelease.cfg.

static bool rn_sequenced_release_slot( rn_channels_t * channels, uint32_t * slot, uint32_t first_number )
{
    const uint32_t hold = *slot;
    if ( hold == RN_DESCRIPTOR_NONE )
        return true;

    rn_region_t * region = channels->region;
    bool owned = true;

    if ( rn_sequenced_reliable_hold( hold ) )
    {
        const rn_receive_descriptor_t * anchor = &region->receive_descriptors[hold & ~RN_SEQUENCED_RELIABLE_HOLD];
        if ( anchor->live && anchor->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
        {
            const rn_receive_descriptor_t copy = *anchor;     // the walk may free the anchor itself
            const uint32_t count = copy.block_count ? copy.block_count : 1;
            for ( uint32_t i = 0; i < count && i < region->window; i++ )
            {
                const uint32_t number = ( first_number + i ) & ROCKETNET_MESSAGE_U24_MASK;
                rn_receive_descriptor_t * descriptor = &region->receive_descriptors[number & ( region->window - 1 )];
                if ( !descriptor->live || ( descriptor->number & ROCKETNET_MESSAGE_U24_MASK ) != number )
                    continue;                                   // never arrived, or the slot holds another number

                if ( rn_sequenced_block_owns( &copy, descriptor ) )
                    descriptor->live = 0;
                else
                    owned = false;
            }
        }
        else
            owned = false;
    }
    else
        rn_sequenced_hold_return( region, hold );

    *slot = RN_DESCRIPTOR_NONE;

    if ( !owned )
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );

    return owned;
}

static bool rn_sequenced_release_hold( rn_channels_t * channels, rn_channel_receive_t * rx )
{
    return rn_sequenced_release_slot( channels, &rx->held_sequenced, rx->held_sequenced_first );
}

static bool rn_sequenced_release_in_turn( rn_channels_t * channels, rn_channel_receive_t * rx )
{
    return rn_sequenced_release_slot( channels, &rx->block_in_turn, rx->block_in_turn_first );
}

static void rn_sequenced_count( rn_channels_t * channels, int counter )
{
    if ( channels->region->counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &channels->region->counters->values[counter], 1 );
}

static bool rn_sequenced_room( rn_channels_t * channels, bool reliable )
{
    // the Du portion is shared with code 0; both are unreliable deliveries

    if ( ( !reliable && channels->unreliable_delivered >= channels->region->delivery_entries - channels->region->window ) ||
         channels->deliveries >= channels->delivery_capacity )
    {
        if ( reliable )
            rn_region_exhaust( channels->region );
        else
            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_DELIVERY_FULL );

        return false;
    }

    return true;
}

static rn_delivery_t * rn_sequenced_delivery( rn_channels_t * channels, uint32_t channel, const uint8_t * data, int bytes,
                                              int code, uint32_t descriptor )
{
    rn_delivery_t * delivery = &channels->region->deliveries[channels->deliveries++];
    memset( delivery, 0, sizeof( *delivery ) );
    delivery->data = data;
    delivery->bytes = bytes;
    delivery->channel = (uint16_t) channel;
    delivery->code = (uint8_t) code;
    delivery->descriptor = descriptor;

    if ( code == ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED )
        channels->unreliable_delivered++;

    return delivery;
}

// proofs/tla/RNChannelTypes.tla DispatchFragment
// proofs/tla/RNHeldOwnership.tla Fragment and Join: the first fragment claims the span, a later one joins it
// RNChannelTypes!DispatchFragment for a code-4 block. The first fragment to
// arrive is the anchor; *hold names it and *first holds its first number. An
// in-turn block collects in rx->block_in_turn, a future one in the one
// future-state slot (rx->held_sequenced, rx->held_sequenced_first), so each channel has at most
// one of each. Each fragment is copied once to its own offset because its
// receive batch expires before the block can complete.

static int rn_sequenced_block( rn_channels_t * channels, const rocketnet_message_record_t * record,
                               uint32_t * hold, uint32_t * first_hold, uint32_t delta, uint32_t slot, rn_delivery_t * out )
{
    rn_region_t * region = channels->region;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = record->fragments + 1;
    const uint32_t index = record->fragment;

    int64_t base = 0;
    if ( !rn_block_shape_valid( channels, record, &base ) || region->receive_descriptors[slot].number < index )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    // the fragment's own ring span lies inside the ring (code 2's check)

    if ( !rn_region_ring_span_valid( region, (uint64_t) record->ring * 16,
                                     ( ( (uint64_t) record->bytes + 15 ) & ~(uint64_t) 15 ) ) )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const bool last = index + 1 == count;
    const uint64_t end = base < 0 ? UINT64_MAX : (uint64_t) base * 16 + (uint64_t) ( count - 1 ) * F + ( last ? ( ( (uint64_t) record->bytes + 15 ) & ~(uint64_t) 15 ) : 16 );
    if ( base < 0 || end > region->ring_bytes )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t first_number = ( record->number - index ) & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t current = *hold;
    rn_receive_descriptor_t * anchor;

    if ( current == RN_DESCRIPTOR_NONE )
    {
        *hold = slot | RN_SEQUENCED_RELIABLE_HOLD;
        *first_hold = first_number;
        anchor = &region->receive_descriptors[slot];
        anchor->block_count = (uint16_t) count;
        anchor->block_have = 0;
        anchor->ring = (uint32_t) base;
    }
    else
    {
        if ( !rn_sequenced_reliable_hold( current ) )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }

        anchor = &region->receive_descriptors[current & ~RN_SEQUENCED_RELIABLE_HOLD];
        if ( !anchor->live || anchor->block_count != count || anchor->block_have >= count ||
             *first_hold != first_number || anchor->ring != (uint32_t) base ||
             anchor->ordering != record->ordering || anchor->sequencing != record->sequencing )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }
    }

    memcpy( region->receive_ring + (uint64_t) record->ring * 16, record->data, (size_t) record->bytes );

    anchor->block_have++;
    if ( anchor->block_have < count )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;

    // every chunk is present; the first descriptor releases the entire block when its delivered view expires, just as for code 3

    const uint32_t first_slot = first_number & ( region->window - 1 );
    const uint32_t final_number = ( first_number + count - 1 ) & ROCKETNET_MESSAGE_U24_MASK;
    rn_receive_descriptor_t * first = &region->receive_descriptors[first_slot];
    rn_receive_descriptor_t * final = &region->receive_descriptors[final_number & ( region->window - 1 )];
    const uint32_t total = ( count - 1 ) * F + final->bytes;
    if ( !first->live || ( first->number & ROCKETNET_MESSAGE_U24_MASK ) != first_number ||
         !final->live || ( final->number & ROCKETNET_MESSAGE_U24_MASK ) != final_number ||
         total > region->max_message_bytes )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_BLOCK );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( anchor != first )
        anchor->block_count = 0;

    first->block_count = (uint16_t) count;
    first->block_have = (uint16_t) count;
    first->ring = (uint32_t) base;
    first->bytes = total;
    *hold = first_slot | RN_SEQUENCED_RELIABLE_HOLD;

    if ( delta != 0 )
        return ROCKETNET_CHANNEL_DISPATCH_HELD;
    if ( !rn_sequenced_room( channels, true ) ||
         !rn_channels_deliver_sequenced( channels, record->channel, record->ordering, record->sequencing ) )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    *out = *rn_sequenced_delivery( channels, record->channel, region->receive_ring + (uint64_t) base * 16,
                                   (int) total, ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED, first_slot );
    *hold = RN_DESCRIPTOR_NONE;

    return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
}

// proofs/tla/RNChannelTypes.tla InTurnOneRule
// RNChannelTypes!Dispatch and !DispatchFragment in turn (delta == 0), one rule
// for a record and a block: the message is checked against the in-turn block
// and the watermark only, never against the held future state, and delivered
// as soon as it is complete. A newer in-turn message releases an in-turn block
// still collecting; one no newer is superseded by it. An in-turn block held in
// the future-state slot is the reversed witness MCRNChannelBrokenBlockInTurnHeld.cfg.

static int rn_sequenced_in_turn( rn_channels_t * channels, const rocketnet_message_record_t * record,
                                 rn_channel_receive_t * rx, uint32_t number_slot, rn_delivery_t * out )
{
    rn_region_t * region = channels->region;
    const bool reliable = record->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
    const bool block = reliable && record->block;
    const uint32_t sequencing = record->sequencing & ROCKETNET_MESSAGE_U24_MASK;

    const uint32_t current = rx->block_in_turn;
    if ( current != RN_DESCRIPTOR_NONE && !rn_sequenced_hold_valid( region, current, record->channel ) )
    {
        rx->block_in_turn = RN_DESCRIPTOR_NONE;
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( current != RN_DESCRIPTOR_NONE )
    {
        const rn_receive_descriptor_t * held = &region->receive_descriptors[current & ~RN_SEQUENCED_RELIABLE_HOLD];
        const uint32_t held_gap = ( sequencing - held->sequencing ) & ROCKETNET_MESSAGE_U24_MASK;
        if ( !( block && held_gap == 0 ) )
        {
            if ( held_gap == 0 || held_gap >= ROCKETNET_MESSAGE_HALF_RANGE )
            {
                if ( reliable )
                    rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );

                rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
                return ROCKETNET_CHANNEL_DISPATCH_STALE;
            }

            if ( !rn_sequenced_release_in_turn( channels, rx ) )
                return ROCKETNET_CHANNEL_DISPATCH_NONE;

            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
        }
    }

    const uint32_t sequence_gap = ( sequencing - rx->sequenced_watermark ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( sequence_gap >= ROCKETNET_MESSAGE_HALF_RANGE )
    {
        if ( reliable )
            rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );

        if ( !rn_sequenced_release_in_turn( channels, rx ) )  // only a continuation still names it here
            return ROCKETNET_CHANNEL_DISPATCH_NONE;

        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        return ROCKETNET_CHANNEL_DISPATCH_STALE;
    }

    if ( block )
        return rn_sequenced_block( channels, record, &rx->block_in_turn, &rx->block_in_turn_first, 0, number_slot, out );

    if ( !rn_sequenced_room( channels, reliable ) )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    if ( !rn_channels_deliver_sequenced( channels, record->channel, record->ordering & ROCKETNET_MESSAGE_U24_MASK, sequencing ) )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    *out = *rn_sequenced_delivery( channels, record->channel, record->data, record->bytes,
                                   record->code, reliable ? number_slot : RN_DESCRIPTOR_NONE );

    return ROCKETNET_CHANNEL_DISPATCH_DELIVERED;
}

// proofs/tla/RNChannelTypes.tla Dispatch
// RNChannelTypes!Dispatch: code 1 does not consult the reliable receive window.

// proofs/tla/RNSequencedHoldPool.tla Hold: a newer future code-1 record returns the old hold and takes the free stack's top
int rn_channels_dispatch_sequenced( struct rn_channels_t * channels, const struct rocketnet_message_record_t * record, struct rn_delivery_t * out )
{
    if ( !channels || !record || !out )
        return ROCKETNET_CHANNEL_DISPATCH_NONE;

    const bool reliable = record->code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;

    // the mask is checked once, by wire code, in rn_channels_dispatch

    rn_region_t * region = channels->region;

    if ( record->channel >= channels->num_channels )
    {
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_CHANNEL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    uint32_t number_slot = RN_DESCRIPTOR_NONE;
    if ( reliable && !rn_sequenced_descriptor( channels, record, &number_slot ) )
    {
        // the slot is live for another number (check 10, SLOT_COLLISION)
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_SLOT_COLLISION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    rn_channel_receive_t * rx = &region->channel_receive[record->channel];
    const uint32_t ordering = record->ordering & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t sequencing = record->sequencing & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t delta = ( ordering - rx->ordered_read ) & ROCKETNET_MESSAGE_U24_MASK;

    if ( delta >= ROCKETNET_MESSAGE_HALF_RANGE )
    {
        // RNChannelTypes!Dispatch: the transport already accepted and ACKs a stale code-4 packet. Only its application view is discarded (I12).
        if ( reliable )
            rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );

        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        return ROCKETNET_CHANNEL_DISPATCH_STALE;
    }

    // an ordering any distance ahead is held, never refused: the sender
    // stamps the channel's ordered write index, which runs as far ahead of
    // the receiver's read index as the sender's ordered backlog, a bound the
    // receiver does not know (standard m6.1 rules 7 and 9). The one hold slot
    // keeps the newest, so a far-ahead record costs nothing more than a near one.

    if ( delta == 0 )
        return rn_sequenced_in_turn( channels, record, rx, number_slot, out );

    // RNChannelTypes!HoldNewest: only one future state per channel is retained.
    // Code 1 uses Ru; code 4 uses its numbered ring span, including blocks.

    if ( !reliable && record->bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
    {
        // A fragmented unreliable state is legal. It cannot fit the fixed future-state slot, so drop it without treating the sender as invalid.
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t current = rx->held_sequenced;
    if ( current != RN_DESCRIPTOR_NONE && !rn_sequenced_hold_valid( region, current, record->channel ) )
    {
        rx->held_sequenced = RN_DESCRIPTOR_NONE;
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    if ( current != RN_DESCRIPTOR_NONE )
    {
        const bool held_reliable = rn_sequenced_reliable_hold( current );
        const uint32_t held_slot = current & ~RN_SEQUENCED_RELIABLE_HOLD;
        const uint32_t held_ordering = held_reliable ? region->receive_descriptors[held_slot].ordering : region->sequenced_messages_held[current].ordering;
        const uint32_t held_sequencing = held_reliable ? region->receive_descriptors[held_slot].sequencing : region->sequenced_messages_held[current].sequencing;
        const uint32_t gap = ( ordering - held_ordering ) & ROCKETNET_MESSAGE_U24_MASK;
        const uint32_t sequence_gap = ( sequencing - held_sequencing ) & ROCKETNET_MESSAGE_U24_MASK;
        const bool continuation = reliable && record->block && held_reliable && gap == 0 && sequence_gap == 0 &&
                                  region->receive_descriptors[held_slot].block_count > 0;
        if ( gap >= ROCKETNET_MESSAGE_HALF_RANGE || ( gap == 0 && ( sequence_gap >= ROCKETNET_MESSAGE_HALF_RANGE || ( sequence_gap == 0 && !continuation ) ) ) )
        {
            if ( reliable )
                rn_sequenced_release_number( channels, region->receive_descriptors[number_slot].number );

            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
            return ROCKETNET_CHANNEL_DISPATCH_STALE;
        }

        if ( !continuation )
        {
            if ( !rn_sequenced_release_hold( channels, rx ) )
                return ROCKETNET_CHANNEL_DISPATCH_NONE;

            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_SUPERSEDED );
        }
    }

    if ( reliable )
    {
        if ( record->block )
            return rn_sequenced_block( channels, record, &rx->held_sequenced, &rx->held_sequenced_first, delta, number_slot, out );

        const uint64_t offset = (uint64_t) record->ring * 16;
        const uint64_t span = ( (uint64_t) record->bytes + 15 ) & ~(uint64_t) 15;
        if ( !rn_region_ring_span_valid( region, offset, span ) )
        {
            rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_BAD_RING );
            return ROCKETNET_CHANNEL_DISPATCH_NONE;
        }

        // THE COPIES (b): this future state outlives the receive batch

        memcpy( region->receive_ring + offset, record->data, (size_t) record->bytes );

        rx->held_sequenced = number_slot | RN_SEQUENCED_RELIABLE_HOLD;
        rx->held_sequenced_first = (uint32_t) region->receive_descriptors[number_slot].number;
        return ROCKETNET_CHANNEL_DISPATCH_HELD;
    }

    // proofs/tla/RNChannel.tla BuffersBounded: a free hold is the free stack's top, in O(1)

    if ( region->sequenced_hold_free_count == 0 )
    {
        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_HOLD_FULL );
        return ROCKETNET_CHANNEL_DISPATCH_NONE;
    }

    const uint32_t slot = region->sequenced_hold_free_stack[--region->sequenced_hold_free_count];
    rx->held_sequenced = slot;

    rn_sequenced_hold_t * hold = &region->sequenced_messages_held[slot];
    hold->bytes = (uint32_t) record->bytes;
    hold->ordering = ordering;
    hold->sequencing = sequencing;
    hold->channel = (uint16_t) record->channel;
    hold->code = ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED;
    hold->used = 1;

    memcpy( region->sequenced_message_payloads[slot].data, record->data, (size_t) record->bytes );

    return ROCKETNET_CHANNEL_DISPATCH_HELD;
}

// proofs/tla/RNChannelTypes.tla Drain
// RNChannelTypes!Drain: the ordered transition has already opened the epoch.

// proofs/tla/RNSequencedHoldPool.tla DrainDeliver and DrainRelease: a delivered hold joins the delivered list, any other returns to the free stack
bool rn_channels_drain_sequenced( struct rn_channels_t * channels, uint32_t channel )
{
    if ( !channels || channel >= channels->num_channels )
        return false;

    rn_channel_receive_t * rx = &channels->region->channel_receive[channel];

    // a hold whose descriptor is not this channel's live code-4 record is a broken invariant: dropped, unread, and the connection ends

    if ( !rn_sequenced_hold_valid( channels->region, rx->block_in_turn, channel ) ||
         !rn_sequenced_hold_valid( channels->region, rx->held_sequenced, channel ) )
    {
        rx->block_in_turn = RN_DESCRIPTOR_NONE;
        rx->held_sequenced = RN_DESCRIPTOR_NONE;
        rn_channels_count_violation( channels, ROCKETNET_COUNTER_MESSAGE_PROTOCOL_VIOLATION );
        return false;
    }

    if ( rx->block_in_turn != RN_DESCRIPTOR_NONE )
    {
        // RNChannelTypes!Drain: an in-turn block of an epoch now closed is stale
        const rn_receive_descriptor_t * anchor = &channels->region->receive_descriptors[rx->block_in_turn & ~RN_SEQUENCED_RELIABLE_HOLD];
        if ( ( ( anchor->ordering - rx->ordered_read ) & ROCKETNET_MESSAGE_U24_MASK ) != 0 )
        {
            if ( !rn_sequenced_release_in_turn( channels, rx ) )
                return false;

            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        }
    }

    const uint32_t slot = rx->held_sequenced;
    if ( slot == RN_DESCRIPTOR_NONE )
        return false;

    const bool reliable = rn_sequenced_reliable_hold( slot );
    rn_receive_descriptor_t * descriptor = reliable ? &channels->region->receive_descriptors[slot & ~RN_SEQUENCED_RELIABLE_HOLD] : NULL;
    rn_sequenced_hold_t * hold = reliable ? NULL : &channels->region->sequenced_messages_held[slot];
    const uint32_t ordering = reliable ? descriptor->ordering : hold->ordering;
    const uint32_t sequencing = reliable ? descriptor->sequencing : hold->sequencing;
    const uint32_t gap = ( ordering - rx->ordered_read ) & ROCKETNET_MESSAGE_U24_MASK;
    if ( gap != 0 )
    {
        if ( gap >= ROCKETNET_MESSAGE_HALF_RANGE )
        {
            if ( !rn_sequenced_release_hold( channels, rx ) )
                return false;

            rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        }

        return false;
    }

    if ( reliable && descriptor->block_count > 0 && descriptor->block_have < descriptor->block_count )
    {
        // the ordered epoch opened while this block is incomplete: it is in turn now and leaves the future-state slot (RNChannelTypes!Drain)
        if ( rx->block_in_turn == RN_DESCRIPTOR_NONE )
        {
            rx->block_in_turn = slot;
            rx->block_in_turn_first = rx->held_sequenced_first;
            rx->held_sequenced = RN_DESCRIPTOR_NONE;
        }

        return false;
    }

    rx->held_sequenced = RN_DESCRIPTOR_NONE;

    if ( !rn_sequenced_room( channels, reliable ) )
    {
        rx->held_sequenced = slot;
        rn_sequenced_release_hold( channels, rx );
        return false;
    }

    if ( !rn_channels_deliver_sequenced( channels, channel, ordering, sequencing ) )
    {
        // standard/MESSAGES.md m7.4 (nothing is silent): the refused record is stale, so release it and count it (proofs/tla/RNChannelTypes.tla DrainStale)
        rx->held_sequenced = slot;
        if ( !rn_sequenced_release_hold( channels, rx ) )
            return false;

        rn_sequenced_count( channels, ROCKETNET_COUNTER_MESSAGE_SEQUENCED_STALE );
        return false;
    }

    if ( reliable )
    {
        const uint32_t number_slot = slot & ~RN_SEQUENCED_RELIABLE_HOLD;
        rn_sequenced_delivery( channels, channel, channels->region->receive_ring + (uint64_t) descriptor->ring * 16,
                               (int) descriptor->bytes, ROCKETNET_CHANNEL_TYPE_RELIABLE_SEQUENCED, number_slot );
    }
    else
    {
        rn_sequenced_delivery( channels, channel, channels->region->sequenced_message_payloads[slot].data, (int) hold->bytes,
                               ROCKETNET_CHANNEL_TYPE_UNRELIABLE_SEQUENCED, RN_DESCRIPTOR_NONE );

        hold->used = 2; // delivered view stays live until RNChannel!EndUpdate
        channels->region->sequenced_holds_delivered[channels->region->sequenced_holds_delivered_count++] = slot;
    }

    return true;
}

// proofs/tla/RNSequencedHoldPool.tla EndUpdate: the delivered list returns to the free stack
void rn_channels_end_update_sequenced( struct rn_channels_t * channels )
{
    if ( !channels )
        return;

    // RNChannel!EndUpdate: only slots whose views expired return to the pool.
    // proofs/tla/RNChannel.tla EndUpdate: the pass visits the delivered list only, and does nothing when it is empty.

    rn_region_t * region = channels->region;
    for ( uint32_t i = 0; i < region->sequenced_holds_delivered_count; i++ )
    {
#if ROCKETNET_DEV
        g_sequenced_hold_visit_count++;
#endif // #if ROCKETNET_DEV
        rn_sequenced_hold_return( region, region->sequenced_holds_delivered[i] );
    }

    region->sequenced_holds_delivered_count = 0;
}
