/* RocketNet. Copyright 2026 Más Bandwidth LLC. AGPL-3.0. */
#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_PACKET_LEASE_H
#define ROCKETNET_SDK_ROCKETNET_PACKET_LEASE_H

#include "rocketnet_internal.h"
#include <atomic>

// Process-wide, non-repeating packet tickets. Zero means exhausted.
uint64_t rocketnet_next_packet_lease_id();

// Contexts remain convenient caller-owned views. This table owns the metadata
// used by finish/abort. Slots are immutable during a fill; consuming a copied
// context succeeds once. Only the joined main thread advances the epoch.
template <size_t Capacity> struct rocketnet_packet_leases_t
{
    static_assert( ( Capacity & ( Capacity - 1 ) ) == 0, "lease table must be a power of two" );

    struct alignas( 64 ) entry_t
    {
        std::atomic<uint64_t> state{ 0 };
        rocketnet_packet_context_t context;
        uint64_t backend_id;
        // the rest of the cache line, explicit so the compiler adds no tail padding
        uint8_t padding[64 - sizeof( std::atomic<uint64_t> ) - sizeof( rocketnet_packet_context_t ) - sizeof( uint64_t )];
    };

    static_assert( sizeof( entry_t ) == 64 && alignof( entry_t ) == 64, "one lease entry per cache line" );

    entry_t entries[Capacity];
    std::atomic<uint64_t> used{ 0 };
    uint64_t epoch = 1;

    static size_t hash( const uint8_t * frame )
    {
        uint64_t value = (uintptr_t) frame;
        value ^= value >> 33;
        value *= UINT64_C( 0xff51afd7ed558ccd );
        value ^= value >> 33;

        return (size_t) value & ( Capacity - 1 );
    }

    bool insert( rocketnet_packet_context_t & context )
    {
        if ( !epoch || used.fetch_add( 1, std::memory_order_relaxed ) >= Capacity / 2 )
            return false;

        const uint64_t ticket = rocketnet_next_packet_lease_id();
        if ( !ticket )
            return false;

        const uint64_t writing = ( epoch << 2 ) | 1;
        size_t slot = hash( context.frame );
        for ( size_t probe = 0; probe < Capacity; ++probe, slot = ( slot + 1 ) & ( Capacity - 1 ) )
        {
            auto & entry = entries[slot];
            uint64_t state = entry.state.load( std::memory_order_acquire );
            if ( ( state >> 2 ) == epoch )
                continue;

            if ( !entry.state.compare_exchange_strong( state, writing, std::memory_order_acq_rel ) )
            {
                continue;
            }

            entry.backend_id = context.packet_id;
            context.packet_id = ticket;
            entry.context = context;
            entry.state.store( ( epoch << 2 ) | 2, std::memory_order_release );

            return true;
        }

        return false;
    }

    bool resolve( const rocketnet_packet_context_t & supplied, rocketnet_packet_context_t & trusted, uint64_t & backend_id, bool consume )
    {
        if ( !epoch || !supplied.frame )
            return false;

        size_t slot = hash( supplied.frame );
        for ( size_t probe = 0; probe < Capacity; ++probe, slot = ( slot + 1 ) & ( Capacity - 1 ) )
        {
            auto & entry = entries[slot];
            uint64_t state = entry.state.load( std::memory_order_acquire );
            if ( ( state >> 2 ) != epoch )
                return false;

            while ( ( state & 3 ) == 1 )
                state = entry.state.load( std::memory_order_acquire );

            const auto & owned = entry.context;
            if ( owned.frame != supplied.frame || owned.packet_id != supplied.packet_id )
                continue;

            if ( owned.sequence != supplied.sequence ||
                 owned.client_index != supplied.client_index || owned.payload_index != supplied.payload_index ||
                 owned.payload_capacity != supplied.payload_capacity )
                return false;

            uint64_t active = ( epoch << 2 ) | 2;
            if ( consume ? !entry.state.compare_exchange_strong( active, ( epoch << 2 ) | 3, std::memory_order_acq_rel ) : state != active )
                return false;

            trusted = owned;
            backend_id = entry.backend_id;

            return true;
        }

        return false;
    }

    bool take( const rocketnet_packet_context_t & supplied, rocketnet_packet_context_t & trusted, uint64_t & backend_id )
    {
        return resolve( supplied, trusted, backend_id, true );
    }

    // A lease insert placed and a later reset retired, still unmodified.
    // reset bumps the epoch and leaves the entry in place, so finish can tell
    // that miss from a context that was never leased, already consumed, or
    // modified. Does not consume the entry.
    bool retired( const rocketnet_packet_context_t & supplied ) const
    {
        if ( !epoch || !supplied.frame || !supplied.packet_id )
            return false;

        size_t slot = hash( supplied.frame );
        for ( size_t probe = 0; probe < Capacity; ++probe, slot = ( slot + 1 ) & ( Capacity - 1 ) )
        {
            const auto & entry = entries[slot];
            const uint64_t state = entry.state.load( std::memory_order_acquire );
            const uint64_t entry_epoch = state >> 2;
            if ( entry_epoch == 0 )
                return false;

            if ( entry_epoch == epoch || ( state & 3 ) != 2 )
                continue;

            const auto & owned = entry.context;
            if ( owned.frame == supplied.frame && owned.packet_id == supplied.packet_id &&
                 owned.sequence == supplied.sequence && owned.client_index == supplied.client_index &&
                 owned.payload_index == supplied.payload_index && owned.payload_capacity == supplied.payload_capacity )
                return true;
        }

        return false;
    }

    void reset()
    {
        epoch = epoch && epoch < ( UINT64_MAX >> 2 ) ? epoch + 1 : 0;
        used.store( 0, std::memory_order_relaxed );
    }
};

#endif // #ifndef ROCKETNET_SDK_ROCKETNET_PACKET_LEASE_H
