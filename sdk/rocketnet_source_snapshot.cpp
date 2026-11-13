/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_source_snapshot.h"

// The connected-source snapshot's add, out of line: main calls it once per
// connected client per update, and the trace check's plant
// (ROCKETNET_SOURCE_SNAPSHOT_PLANT) must be library code the plant binary
// compiles in place of the library's (proofs/tla/README-TraceCheck.md): only
// tests/tla_trace_source_snapshot.cpp in the plant build defines it, and its
// traces must fail TraceConsistent. The rest of the snapshot stays inline in
// its header.

// proofs/tla/RNSourceSnapshotLayout.tla Add and Refuse: adds a connected
// address to the target. true when it is in the target (added now or
// before); false, a refusal the caller counts, when the target already holds
// max_entries addresses and this one is not among them. An address that is
// not ipv4 or ipv6, or an add outside begin and publish, is a write the
// caller must not make.
bool rocketnet_source_snapshot_writer_add( struct rocketnet_source_snapshot_t * snapshot, const struct rocketnet_address_t * address )
{
    rocketnet_assert( snapshot );
    rocketnet_assert( address );
    struct rocketnet_source_snapshot_writer_t * writer = snapshot->writer;
    rocketnet_assert( writer->filling );
    rocketnet_assert( address->type == ROCKETNET_ADDRESS_IPV4 || address->type == ROCKETNET_ADDRESS_IPV6 );
    if ( !writer->filling || ( address->type != ROCKETNET_ADDRESS_IPV4 && address->type != ROCKETNET_ADDRESS_IPV6 ) )
    {
        return false;
    }

#if defined( ROCKETNET_SOURCE_SNAPSHOT_PLANT )
    // the trace check's plant (README-TraceCheck.md): the publish store
    // before the add, so the target is published half built
    // (RNSourceSnapshotLayout HalfPublished)
    snapshot->published->word.store( ( ( writer->generation + 1 ) << 1 ) | (uint64_t) writer->target, std::memory_order_seq_cst );
#endif // #if defined( ROCKETNET_SOURCE_SNAPSHOT_PLANT )

    struct rocketnet_address_t * cells = &snapshot->cells[(size_t) writer->target * (size_t) snapshot->capacity];
    const uint32_t mask = (uint32_t) ( snapshot->capacity - 1 );
    const uint32_t home = rocketnet_source_snapshot_hash( address ) & mask;
    for ( uint32_t i = 0; i < (uint32_t) snapshot->capacity; ++i )
    {
        struct rocketnet_address_t * cell = &cells[( home + i ) & mask];
        if ( cell->type == ROCKETNET_ADDRESS_NONE )
        {
            if ( writer->count >= snapshot->max_entries )
            {
                return false;
            }
            *cell = *address;
            writer->count++;
            return true;
        }
        if ( rocketnet_address_equal( cell, address ) )
        {
            return true;
        }
    }

    // a slot is at most half full, so a probe always meets an empty cell
    rocketnet_assert( false );
    return false;
}
