/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SEQUENCE_BUFFER_H
#define ROCKETNET_SDK_ROCKETNET_SEQUENCE_BUFFER_H

#include <stddef.h>
#include <stdint.h>

// A fixed-capacity, allocation-free modulo table for internal identities.
// Identity is deliberately uint64_t: packet sequences are already wide,
// message numbers are reconstructed to wide values, and payload ids must not
// acquire a narrower storage contract merely because their wire form is small.

enum rn_sequence_buffer_policy_t
{
    RN_SEQUENCE_BUFFER_HISTORY,
    RN_SEQUENCE_BUFFER_OWNED
};

enum rn_sequence_buffer_result_t
{
    RN_SEQUENCE_BUFFER_ADMITTED,
    RN_SEQUENCE_BUFFER_DUPLICATE,
    RN_SEQUENCE_BUFFER_TOO_OLD,
    RN_SEQUENCE_BUFFER_SLOT_LIVE,
    RN_SEQUENCE_BUFFER_TICKETS_EXHAUSTED,
    RN_SEQUENCE_BUFFER_GENERATION_EXHAUSTED
};

struct rn_sequence_buffer_handle_t
{
    uint64_t identity;
    uint64_t generation;
    uint64_t owner_epoch;
    uint64_t ticket;
};

template <typename T, size_t Capacity, rn_sequence_buffer_policy_t Policy>
class rn_sequence_buffer_t
{
    static_assert( Capacity > 0, "a sequence buffer needs at least one slot" );

    struct slot_t
    {
        T value;
        uint64_t identity;
        uint64_t generation;
        uint64_t owner_epoch;
        uint64_t ticket;
        bool occupied;
    };

    slot_t slots_[Capacity];
    uint64_t owner_epoch_;
    uint64_t next_ticket_;              // zero is the exhausted state
    uint64_t newest_history_identity_;
    size_t count_;
    bool has_history_identity_;

    static size_t index_of( uint64_t identity )
    {
        return (size_t) ( identity % (uint64_t) Capacity );
    }

    static uint64_t history_floor( uint64_t newest )
    {
        const uint64_t width = (uint64_t) Capacity - 1;
        return newest > width ? newest - width : 0;
    }

    void clear_occupied()
    {
        for ( size_t i = 0; i < Capacity; ++i )
            slots_[i].occupied = false;
        count_ = 0;
    }

public:
    static const size_t capacity = Capacity;
    static const size_t slot_bytes = sizeof( slot_t );
    static const size_t storage_bytes = sizeof( slot_t ) * Capacity;

    rn_sequence_buffer_t()
    {
        initialize();
    }

    // A nonzero starting ticket is normally one. Supplying another value is
    // useful when restoring a persisted owner and for deterministic boundary
    // tests. Ticket UINT64_MAX is issued once; the next admission refuses.
    void initialize( uint64_t owner_epoch = 1, uint64_t next_ticket = 1,
                     uint64_t initial_generation = 0 )
    {
        owner_epoch_ = owner_epoch != 0 ? owner_epoch : 1;
        next_ticket_ = next_ticket;
        newest_history_identity_ = 0;
        has_history_identity_ = false;
        for ( size_t i = 0; i < Capacity; ++i )
        {
            slots_[i].generation = initial_generation;
            slots_[i].occupied = false;
        }
        count_ = 0;
    }

    // Reset is O(Capacity), makes every old handle stale through owner_epoch,
    // restores tickets at one and permits numeric identities to be reused.
    // At owner epoch UINT64_MAX it refuses without mutation: reconstruct the
    // containing connection/object rather than wrapping an ownership tag.
    bool reset()
    {
        if ( owner_epoch_ == UINT64_MAX )
            return false;
        ++owner_epoch_;
        next_ticket_ = 1;
        newest_history_identity_ = 0;
        has_history_identity_ = false;
        for ( size_t i = 0; i < Capacity; ++i )
            slots_[i].generation = 0;
        clear_occupied();
        return true;
    }

    size_t size() const { return count_; }
    uint64_t owner_epoch() const { return owner_epoch_; }
    bool tickets_exhausted() const { return next_ticket_ == 0; }

    rn_sequence_buffer_result_t insert( uint64_t identity, const T & value,
                                        rn_sequence_buffer_handle_t * handle )
    {
        const size_t index = index_of( identity );
        slot_t & target = slots_[index];

        if ( Policy == RN_SEQUENCE_BUFFER_HISTORY && has_history_identity_ &&
             identity < history_floor( newest_history_identity_ ) )
            return RN_SEQUENCE_BUFFER_TOO_OLD;
        if ( target.occupied && target.identity == identity )
            return RN_SEQUENCE_BUFFER_DUPLICATE;
        if ( Policy == RN_SEQUENCE_BUFFER_OWNED && target.occupied )
            return RN_SEQUENCE_BUFFER_SLOT_LIVE;
        if ( next_ticket_ == 0 )
            return RN_SEQUENCE_BUFFER_TICKETS_EXHAUSTED;
        if ( target.generation == UINT64_MAX )
            return RN_SEQUENCE_BUFFER_GENERATION_EXHAUSTED;

        // Advancing history examines at most Capacity cells, even across an
        // arbitrarily large numeric gap. The age, duplicate, ticket, and
        // generation checks above precede this retirement, so a rejected
        // insert cannot mutate history; the post-retirement target.occupied
        // check below is a defensive guard.
        if ( Policy == RN_SEQUENCE_BUFFER_HISTORY &&
             ( !has_history_identity_ || identity > newest_history_identity_ ) )
        {
            const uint64_t floor = history_floor( identity );
            for ( size_t i = 0; i < Capacity; ++i )
            {
                if ( slots_[i].occupied && slots_[i].identity < floor )
                {
                    slots_[i].occupied = false;
                    --count_;
                }
            }
            newest_history_identity_ = identity;
            has_history_identity_ = true;
        }

        if ( target.occupied )
        {
            // Only history reaches this branch; its aliased resident was not
            // outside the retained window, so overwriting it is never legal.
            return RN_SEQUENCE_BUFFER_SLOT_LIVE;
        }

        ++target.generation;
        target.value = value;
        target.identity = identity;
        target.owner_epoch = owner_epoch_;
        target.ticket = next_ticket_;
        target.occupied = true;
        ++count_;

        if ( handle != NULL )
        {
            handle->identity = identity;
            handle->generation = target.generation;
            handle->owner_epoch = owner_epoch_;
            handle->ticket = next_ticket_;
        }
        next_ticket_ = next_ticket_ == UINT64_MAX ? 0 : next_ticket_ + 1;
        return RN_SEQUENCE_BUFFER_ADMITTED;
    }

    T * find( const rn_sequence_buffer_handle_t & handle )
    {
        slot_t & slot = slots_[index_of( handle.identity )];
        return slot.occupied && slot.identity == handle.identity &&
                       slot.generation == handle.generation &&
                       slot.owner_epoch == handle.owner_epoch &&
                       slot.ticket == handle.ticket
                   ? &slot.value
                   : NULL;
    }

    const T * find( const rn_sequence_buffer_handle_t & handle ) const
    {
        const slot_t & slot = slots_[index_of( handle.identity )];
        return slot.occupied && slot.identity == handle.identity &&
                       slot.generation == handle.generation &&
                       slot.owner_epoch == handle.owner_epoch &&
                       slot.ticket == handle.ticket
                   ? &slot.value
                   : NULL;
    }

    bool handle_for( uint64_t identity, rn_sequence_buffer_handle_t * handle ) const
    {
        const slot_t & slot = slots_[index_of( identity )];
        if ( handle == NULL || !slot.occupied || slot.identity != identity ||
             slot.owner_epoch != owner_epoch_ )
            return false;
        handle->identity = identity;
        handle->generation = slot.generation;
        handle->owner_epoch = slot.owner_epoch;
        handle->ticket = slot.ticket;
        return true;
    }

    bool remove( const rn_sequence_buffer_handle_t & handle )
    {
        slot_t & slot = slots_[index_of( handle.identity )];
        if ( !slot.occupied || slot.identity != handle.identity ||
             slot.generation != handle.generation ||
             slot.owner_epoch != handle.owner_epoch ||
             slot.ticket != handle.ticket )
            return false;
        slot.occupied = false;
        --count_;
        return true;
    }
};

#endif // ROCKETNET_SDK_ROCKETNET_SEQUENCE_BUFFER_H
