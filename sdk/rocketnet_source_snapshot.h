/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SOURCE_SNAPSHOT_H
#define ROCKETNET_SDK_ROCKETNET_SOURCE_SNAPSHOT_H

// The connected-source snapshot: the set of connected client addresses that
// main publishes once per update and the receive workers look a datagram's
// source up in, so a known source may use the whole receive bank and an
// unknown one only a capped share. This layer is the snapshot alone: the
// share, the admission and the counting are the transport's.
//
// Two slots, each an open-addressing hash set of addresses (linear probe,
// capacity a power of two at least 2 * max_entries, so a slot is at most
// half full and every probe meets an empty cell). One writer, main; up to
// ROCKETNET_SOURCE_SNAPSHOT_MAX_READERS readers, the receive workers.
//
// The published word is ( generation << 1 ) | slot. Generations only grow,
// so a word is never published twice.
//
// The writer (rocketnet_source_snapshot_writer_begin, _add, _publish):
//   - its target is the slot that is not published;
//   - begin scans every reader's announced slot; when a reader announces
//     the target, the update's publish is skipped (begin returns
//     ROCKETNET_SOURCE_SNAPSHOT_NONE, which the caller counts) and the
//     readers keep the published snapshot, one update stale;
//   - otherwise it clears the target and adds the update's addresses to it;
//   - publish stores the new word once. The target becomes the other slot.
//
// A reader (rocketnet_source_snapshot_reader_acquire, _contains, _release),
// once per batch:
//   - loads the published word;
//   - announces the word's slot in its own word;
//   - re-checks that the published word is still the one it loaded; if not,
//     it withdraws the announcement and starts again;
//   - looks sources up in that slot for the batch, then releases it.
//
// Why the writer never writes a slot a reader reads: the reader's announce
// (a store) is followed by its re-check (a load), and the writer's publish
// (a store) is followed by its next scan (a load), all four sequentially
// consistent, so they fall in one total order. If the writer's scan of a
// reader misses the reader's announcement of the target, that announcement
// comes after the scan, so after the writer's last publish (whose slot is
// not the target), and the re-check sees that publish or a later one: never
// the word the reader loaded, whose slot was the target. The re-check fails
// and the reader never reads the slot. If the scan sees the announcement,
// the writer skips. A reader that passed its re-check holds a slot every
// later scan sees until its release.
//
// Model: proofs/tla/RNSourceSnapshotLayout.tla (README-RNSourceSnapshotLayout.md),
// at this layout, with reversed witnesses for a publish before the last add,
// an announce with no re-check and a lookup that does not wrap; the trace
// check (layer source-snapshot, tests/tla_trace_source_snapshot.cpp) binds
// these functions to it.
//
// No allocation: the caller allocates rocketnet_source_snapshot_bytes()
// bytes, any alignment, and passes them to rocketnet_source_snapshot_init.
// The step functions (writer_scan, writer_fill, reader_load,
// reader_announce, reader_recheck) are the pieces begin and acquire are made
// of; the trace walk drives them one at a time.

#include "rocketnet_internal.h"
#include "rocketnet_address.h"

#include <atomic>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ROCKETNET_SOURCE_SNAPSHOT_MAX_READERS       32
#define ROCKETNET_SOURCE_SNAPSHOT_MAX_ENTRIES       ( 1 << 24 )
#define ROCKETNET_SOURCE_SNAPSHOT_NONE              -1
#define ROCKETNET_SOURCE_SNAPSHOT_LINE              64

// one reader's announced slot, 0, 1 or ROCKETNET_SOURCE_SNAPSHOT_NONE, alone
// on its cache line: each receive worker writes its own
struct rocketnet_source_snapshot_reader_t
{
    std::atomic<int32_t> slot;
    uint8_t pad[ROCKETNET_SOURCE_SNAPSHOT_LINE - sizeof( std::atomic<int32_t> )];
};

// the published word, alone on its cache line: every reader loads it
struct rocketnet_source_snapshot_published_t
{
    std::atomic<uint64_t> word;
    uint8_t pad[ROCKETNET_SOURCE_SNAPSHOT_LINE - sizeof( std::atomic<uint64_t> )];
};

// the writer's own state, main's alone, on its own cache line
struct rocketnet_source_snapshot_writer_t
{
    uint64_t generation;        // the generation last published
    int32_t target;             // the slot the writer fills: the one not published
    int32_t filling;            // between a begin that returned the target and its publish
    int32_t count;              // addresses in the target this update
    uint8_t pad[ROCKETNET_SOURCE_SNAPSHOT_LINE - sizeof( uint64_t ) - 3 * sizeof( int32_t )];
};

// read only after init, by any thread
struct rocketnet_source_snapshot_t
{
    int max_entries;
    int readers;
    int capacity;                                           // cells per slot, a power of two
    struct rocketnet_source_snapshot_published_t * published;
    struct rocketnet_source_snapshot_writer_t * writer;
    struct rocketnet_source_snapshot_reader_t * reader;     // [readers]
    struct rocketnet_address_t * cells;                     // [2 * capacity]: slot s is cells[s * capacity ..]
};

// cells per slot for max_entries: the smallest power of two at least
// 2 * max_entries. 0 when max_entries is out of range.
static inline int rocketnet_source_snapshot_capacity( int max_entries )
{
    if ( max_entries < 1 || max_entries > ROCKETNET_SOURCE_SNAPSHOT_MAX_ENTRIES )
    {
        return 0;
    }
    int capacity = 1;
    while ( capacity < 2 * max_entries )
    {
        capacity *= 2;
    }
    return capacity;
}

// the memory a snapshot of max_entries addresses and readers readers needs,
// with room to align it to a cache line. 0 when either is out of range.
static inline size_t rocketnet_source_snapshot_bytes( int max_entries, int readers )
{
    const int capacity = rocketnet_source_snapshot_capacity( max_entries );
    if ( capacity == 0 || readers < 1 || readers > ROCKETNET_SOURCE_SNAPSHOT_MAX_READERS )
    {
        return 0;
    }
    return ROCKETNET_SOURCE_SNAPSHOT_LINE                                       // alignment slack
           + sizeof( struct rocketnet_source_snapshot_published_t )
           + sizeof( struct rocketnet_source_snapshot_writer_t )
           + (size_t) readers * sizeof( struct rocketnet_source_snapshot_reader_t )
           + 2 * (size_t) capacity * sizeof( struct rocketnet_address_t );
}

// lays the snapshot out in memory of rocketnet_source_snapshot_bytes(
// max_entries, readers ) bytes and publishes slot 0, empty, at generation 0:
// until main's first publish every source is unknown. false when either
// count is out of range. Not concurrent with anything.
static inline bool rocketnet_source_snapshot_init( struct rocketnet_source_snapshot_t * snapshot, void * memory, int max_entries, int readers )
{
    rocketnet_assert( snapshot );
    rocketnet_assert( memory );

    const size_t bytes = rocketnet_source_snapshot_bytes( max_entries, readers );
    if ( bytes == 0 )
    {
        return false;
    }

    memset( memory, 0, bytes );

    uintptr_t base = (uintptr_t) memory;
    base = ( base + ( ROCKETNET_SOURCE_SNAPSHOT_LINE - 1 ) ) & ~(uintptr_t) ( ROCKETNET_SOURCE_SNAPSHOT_LINE - 1 );
    uint8_t * p = (uint8_t *) base;

    snapshot->max_entries = max_entries;
    snapshot->readers = readers;
    snapshot->capacity = rocketnet_source_snapshot_capacity( max_entries );

    snapshot->published = new ( p ) rocketnet_source_snapshot_published_t();
    p += sizeof( struct rocketnet_source_snapshot_published_t );
    snapshot->writer = new ( p ) rocketnet_source_snapshot_writer_t();
    p += sizeof( struct rocketnet_source_snapshot_writer_t );
    snapshot->reader = (struct rocketnet_source_snapshot_reader_t *) p;
    for ( int r = 0; r < readers; ++r )
    {
        new ( &snapshot->reader[r] ) rocketnet_source_snapshot_reader_t();
        snapshot->reader[r].slot.store( ROCKETNET_SOURCE_SNAPSHOT_NONE, std::memory_order_relaxed );
    }
    p += (size_t) readers * sizeof( struct rocketnet_source_snapshot_reader_t );
    snapshot->cells = (struct rocketnet_address_t *) p;     // zeroed: every cell ROCKETNET_ADDRESS_NONE

    snapshot->writer->generation = 0;
    snapshot->writer->target = 1;
    snapshot->writer->filling = 0;
    snapshot->writer->count = 0;

    // no reader runs yet: the thread that starts the workers publishes this
    snapshot->published->word.store( 0, std::memory_order_relaxed );

    return true;
}

static inline int rocketnet_source_snapshot_word_slot( uint64_t word )
{
    return (int) ( word & 1 );
}

static inline uint64_t rocketnet_source_snapshot_word_generation( uint64_t word )
{
    return word >> 1;
}

// FNV-1a over exactly the bytes rocketnet_address_equal compares: the type,
// the ipv4 or ipv6 bytes and the port, as rocketnet_iocp_lane_index hashes
// (sdk/rocketnet_server_iocp_windows.cpp). Equal addresses hash equal.
static inline uint32_t rocketnet_source_snapshot_hash( const struct rocketnet_address_t * address )
{
    uint32_t hash = 2166136261u;
    const uint8_t type = address->type;
    hash ^= (uint32_t) type;
    hash *= 16777619u;
    if ( type == ROCKETNET_ADDRESS_IPV4 )
    {
        for ( int i = 0; i < 4; ++i )
        {
            hash ^= (uint32_t) address->data.ipv4[i];
            hash *= 16777619u;
        }
    }
    else if ( type == ROCKETNET_ADDRESS_IPV6 )
    {
        for ( int i = 0; i < 8; ++i )
        {
            hash ^= (uint32_t) (uint8_t) ( address->data.ipv6[i] >> 8 );
            hash *= 16777619u;
            hash ^= (uint32_t) (uint8_t) address->data.ipv6[i];
            hash *= 16777619u;
        }
    }
    hash ^= (uint32_t) (uint8_t) ( address->port >> 8 );
    hash *= 16777619u;
    hash ^= (uint32_t) (uint8_t) address->port;
    hash *= 16777619u;
    return hash;
}

// an address's home cell in a slot (RNSourceSnapshotLayout Home): the trace
// walk reads it to give the model the same table
static inline int rocketnet_source_snapshot_home( const struct rocketnet_source_snapshot_t * snapshot, const struct rocketnet_address_t * address )
{
    return (int) ( rocketnet_source_snapshot_hash( address ) & (uint32_t) ( snapshot->capacity - 1 ) );
}

// ---- the writer: main, one thread ----

// RNSourceSnapshotLayout Scan: true when the reader announces the writer's
// target. Sequentially consistent: this load follows the writer's last
// publish in the total order, which the argument at the top needs; it is
// also an acquire, so a reader's release (a store of NONE after its last
// lookup) that it reads orders that reader's lookups before the writer's
// writes to the slot.
static inline bool rocketnet_source_snapshot_writer_scan( const struct rocketnet_source_snapshot_t * snapshot, int reader )
{
    rocketnet_assert( reader >= 0 && reader < snapshot->readers );
    return snapshot->reader[reader].slot.load( std::memory_order_seq_cst ) == snapshot->writer->target;
}

// RNSourceSnapshotLayout Fill: after a scan that found no reader on the
// target, clears the target and returns it. Plain writes: no reader reads
// the target until it is published.
static inline int rocketnet_source_snapshot_writer_fill( struct rocketnet_source_snapshot_t * snapshot )
{
    struct rocketnet_source_snapshot_writer_t * writer = snapshot->writer;
    rocketnet_assert( !writer->filling );
    memset( &snapshot->cells[(size_t) writer->target * (size_t) snapshot->capacity], 0,
            (size_t) snapshot->capacity * sizeof( struct rocketnet_address_t ) );
    writer->filling = 1;
    writer->count = 0;
    return writer->target;
}

// Starts an update's snapshot: returns the slot the writer now fills, or
// ROCKETNET_SOURCE_SNAPSHOT_NONE when a reader still announces it. On NONE
// the update publishes nothing (no add, no publish); the caller counts the
// skipped publish, and the readers keep the snapshot already published.
static inline int rocketnet_source_snapshot_writer_begin( struct rocketnet_source_snapshot_t * snapshot )
{
    rocketnet_assert( snapshot );
    rocketnet_assert( !snapshot->writer->filling );
    for ( int r = 0; r < snapshot->readers; ++r )
    {
        if ( rocketnet_source_snapshot_writer_scan( snapshot, r ) )
        {
            return ROCKETNET_SOURCE_SNAPSHOT_NONE;
        }
    }
    return rocketnet_source_snapshot_writer_fill( snapshot );
}

// RNSourceSnapshotLayout Add: adds a connected address to the target. true
// when it is in the target (added now or before); false, a refusal the
// caller counts, when the target already holds max_entries addresses and
// this one is not among them. An address that is not ipv4 or ipv6, or an
// add outside begin and publish, is a write the caller must not make. Out
// of line in sdk/rocketnet_source_snapshot.cpp, where the trace check's
// plant is.
bool rocketnet_source_snapshot_writer_add( struct rocketnet_source_snapshot_t * snapshot, const struct rocketnet_address_t * address );

// RNSourceSnapshotLayout Publish: publishes the target with one store, the
// next generation. Release orders every add before it for a reader whose
// re-check reads it; sequentially consistent so it precedes the next scan
// in the total order (the argument at the top). The other slot becomes the
// target. false, with nothing published, outside begin and publish.
static inline bool rocketnet_source_snapshot_writer_publish( struct rocketnet_source_snapshot_t * snapshot )
{
    rocketnet_assert( snapshot );
    struct rocketnet_source_snapshot_writer_t * writer = snapshot->writer;
    rocketnet_assert( writer->filling );
    if ( !writer->filling )
    {
        return false;
    }
    writer->generation++;
    snapshot->published->word.store( ( writer->generation << 1 ) | (uint64_t) writer->target, std::memory_order_seq_cst );
    writer->target = 1 - writer->target;
    writer->filling = 0;
    writer->count = 0;
    return true;
}

// ---- a reader: one receive worker, its own reader index ----

// RNSourceSnapshotLayout Load. Acquire, though only the re-check's load
// decides what the reader reads.
static inline uint64_t rocketnet_source_snapshot_reader_load( const struct rocketnet_source_snapshot_t * snapshot )
{
    return snapshot->published->word.load( std::memory_order_acquire );
}

// RNSourceSnapshotLayout Announce: the loaded word's slot in the reader's
// own word. Sequentially consistent: it precedes the re-check's load in the
// total order (the argument at the top).
static inline void rocketnet_source_snapshot_reader_announce( struct rocketnet_source_snapshot_t * snapshot, int reader, uint64_t word )
{
    rocketnet_assert( reader >= 0 && reader < snapshot->readers );
    snapshot->reader[reader].slot.store( (int32_t) rocketnet_source_snapshot_word_slot( word ), std::memory_order_seq_cst );
}

// RNSourceSnapshotLayout Recheck: true when the published word is still the
// one loaded, and the reader holds its slot. Sequentially consistent (the
// argument at the top); its acquire reads the writer's publish store, so
// every add to the slot is visible to the reader's lookups. On false the
// reader withdraws its announcement (relaxed: it read nothing of the slot)
// and must load again.
static inline bool rocketnet_source_snapshot_reader_recheck( struct rocketnet_source_snapshot_t * snapshot, int reader, uint64_t word )
{
    rocketnet_assert( reader >= 0 && reader < snapshot->readers );
    if ( snapshot->published->word.load( std::memory_order_seq_cst ) == word )
    {
        return true;
    }
    snapshot->reader[reader].slot.store( ROCKETNET_SOURCE_SNAPSHOT_NONE, std::memory_order_relaxed );
    return false;
}

// Acquires the published slot for one batch and returns it. Lock free: a
// retry means the writer published between the load and the re-check, at
// most once per update. The reader must not already hold a slot.
static inline int rocketnet_source_snapshot_reader_acquire( struct rocketnet_source_snapshot_t * snapshot, int reader )
{
    rocketnet_assert( snapshot );
    rocketnet_assert( reader >= 0 && reader < snapshot->readers );
    rocketnet_assert( snapshot->reader[reader].slot.load( std::memory_order_relaxed ) == ROCKETNET_SOURCE_SNAPSHOT_NONE );
    for ( ;; )
    {
        const uint64_t word = rocketnet_source_snapshot_reader_load( snapshot );
        rocketnet_source_snapshot_reader_announce( snapshot, reader, word );
        if ( rocketnet_source_snapshot_reader_recheck( snapshot, reader, word ) )
        {
            return rocketnet_source_snapshot_word_slot( word );
        }
    }
}

// RNSourceSnapshotLayout Lookup: true when address is in the slot. The slot
// must be one the caller holds (rocketnet_source_snapshot_reader_acquire).
// Linear probe from the home cell, wrapping at the slot's end, stopping at
// the address or at an empty cell.
static inline bool rocketnet_source_snapshot_contains( const struct rocketnet_source_snapshot_t * snapshot, int slot, const struct rocketnet_address_t * address )
{
    rocketnet_assert( snapshot );
    rocketnet_assert( address );
    rocketnet_assert( slot == 0 || slot == 1 );
    const struct rocketnet_address_t * cells = &snapshot->cells[(size_t) slot * (size_t) snapshot->capacity];
    const uint32_t mask = (uint32_t) ( snapshot->capacity - 1 );
    const uint32_t home = rocketnet_source_snapshot_hash( address ) & mask;
    for ( uint32_t i = 0; i < (uint32_t) snapshot->capacity; ++i )
    {
        const struct rocketnet_address_t * cell = &cells[( home + i ) & mask];
        if ( cell->type == ROCKETNET_ADDRESS_NONE )
        {
            return false;
        }
        if ( rocketnet_address_equal( cell, address ) )
        {
            return true;
        }
    }
    return false;
}

// RNSourceSnapshotLayout Release: the batch is over. Release orders every
// lookup before the writer's scan that reads NONE, and so before any write
// to the slot after it.
static inline void rocketnet_source_snapshot_reader_release( struct rocketnet_source_snapshot_t * snapshot, int reader )
{
    rocketnet_assert( snapshot );
    rocketnet_assert( reader >= 0 && reader < snapshot->readers );
    rocketnet_assert( snapshot->reader[reader].slot.load( std::memory_order_relaxed ) != ROCKETNET_SOURCE_SNAPSHOT_NONE );
    snapshot->reader[reader].slot.store( ROCKETNET_SOURCE_SNAPSHOT_NONE, std::memory_order_release );
}

#endif // ROCKETNET_SDK_ROCKETNET_SOURCE_SNAPSHOT_H
