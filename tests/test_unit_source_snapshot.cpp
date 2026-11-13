/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The connected-source snapshot (sdk/rocketnet_source_snapshot.h), at the
// rules proofs/tla/RNSourceSnapshotLayout.tla checks: the writer fills the
// slot not published and publishes with one store after its last add; it
// skips an update while a reader announces its target; a reader's re-check
// refuses a word that is no longer published; the probe wraps at a slot's
// end; an add past max_entries is refused; a fill clears its slot; the hash
// covers only the bytes equality compares. The interleavings are driven one
// call at a time on one thread. Pure memory: no socket, no clock, no thread.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_source_snapshot.h"

#include <string.h>
#include <vector>

#define check rocketnet_unit_check

struct snapshot_test_t
{
    std::vector<uint8_t> memory;
    struct rocketnet_source_snapshot_t snapshot;

    snapshot_test_t( int max_entries, int readers ) : memory( rocketnet_source_snapshot_bytes( max_entries, readers ) )
    {
        check( !memory.empty() );
        check( rocketnet_source_snapshot_init( &snapshot, memory.data(), max_entries, readers ) );
    }

    // publishes exactly the addresses given; false when the update was skipped
    bool update( const struct rocketnet_address_t * addresses, int count )
    {
        if ( rocketnet_source_snapshot_writer_begin( &snapshot ) == ROCKETNET_SOURCE_SNAPSHOT_NONE )
        {
            return false;
        }
        for ( int i = 0; i < count; i++ )
        {
            check( rocketnet_source_snapshot_writer_add( &snapshot, &addresses[i] ) );
        }
        check( rocketnet_source_snapshot_writer_publish( &snapshot ) );
        return true;
    }

    int announced( int reader ) const
    {
        return snapshot.reader[reader].slot.load( std::memory_order_relaxed );
    }
};

static struct rocketnet_address_t snapshot_test_address( int n )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ROCKETNET_ADDRESS_IPV4;
    a.data.ipv4[0] = 10;
    a.data.ipv4[2] = (uint8_t) ( n >> 8 );
    a.data.ipv4[3] = (uint8_t) n;
    a.port = 40000;
    return a;
}

// the first address at or after n whose home is the cell given
static struct rocketnet_address_t snapshot_test_homed( const struct rocketnet_source_snapshot_t * snapshot, int home, int n )
{
    for ( ;; n++ )
    {
        const struct rocketnet_address_t a = snapshot_test_address( n );
        if ( rocketnet_source_snapshot_home( snapshot, &a ) == home )
        {
            return a;
        }
    }
}

// sizes and refusals; init publishes slot 0 empty, so every source is
// unknown until main's first publish

static void test_source_snapshot_init_publishes_empty_slot_zero()
{
    check( rocketnet_source_snapshot_bytes( 0, 1 ) == 0 );
    check( rocketnet_source_snapshot_bytes( 1, 0 ) == 0 );
    check( rocketnet_source_snapshot_bytes( 1, ROCKETNET_SOURCE_SNAPSHOT_MAX_READERS + 1 ) == 0 );
    check( rocketnet_source_snapshot_bytes( ROCKETNET_SOURCE_SNAPSHOT_MAX_ENTRIES + 1, 1 ) == 0 );
    check( rocketnet_source_snapshot_bytes( 1, ROCKETNET_SOURCE_SNAPSHOT_MAX_READERS ) > 0 );
    check( rocketnet_source_snapshot_capacity( 1 ) == 2 );
    check( rocketnet_source_snapshot_capacity( 2 ) == 4 );
    check( rocketnet_source_snapshot_capacity( 3 ) == 8 );
    check( rocketnet_source_snapshot_capacity( 1000 ) == 2048 );

    struct rocketnet_source_snapshot_t snapshot;
    uint8_t memory[16];
    check( !rocketnet_source_snapshot_init( &snapshot, memory, 0, 1 ) );
    check( !rocketnet_source_snapshot_init( &snapshot, memory, 1, 0 ) );

    snapshot_test_t t( 4, 2 );
    check( t.snapshot.capacity == 8 );
    check( ( (uintptr_t) t.snapshot.published & ( ROCKETNET_SOURCE_SNAPSHOT_LINE - 1 ) ) == 0 );
    check( t.announced( 0 ) == ROCKETNET_SOURCE_SNAPSHOT_NONE && t.announced( 1 ) == ROCKETNET_SOURCE_SNAPSHOT_NONE );

    const uint64_t word = rocketnet_source_snapshot_reader_load( &t.snapshot );
    check( rocketnet_source_snapshot_word_slot( word ) == 0 && rocketnet_source_snapshot_word_generation( word ) == 0 );
    const int slot = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( slot == 0 );
    const struct rocketnet_address_t a = snapshot_test_address( 1 );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &a ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );
    check( t.announced( 0 ) == ROCKETNET_SOURCE_SNAPSHOT_NONE );
}

REGISTER_UNIT_TEST( test_source_snapshot_init_publishes_empty_slot_zero );

// the writer fills the slot not published, so a reader holding the
// published slot keeps its answers while the writer adds; the publish
// switches readers to the new set (TargetUnpublished, LookupMatchesPublished)

static void test_source_snapshot_writer_fills_the_slot_not_published()
{
    snapshot_test_t t( 4, 1 );
    const struct rocketnet_address_t a = snapshot_test_address( 1 );
    const struct rocketnet_address_t b = snapshot_test_address( 2 );
    const struct rocketnet_address_t c = snapshot_test_address( 3 );

    check( t.update( &a, 1 ) );                                     // {a} in slot 1

    const int held = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( held == 1 );
    check( rocketnet_source_snapshot_contains( &t.snapshot, held, &a ) );

    // the next update fills slot 0; slot 1 is untouched under the reader
    check( rocketnet_source_snapshot_writer_begin( &t.snapshot ) == 0 );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &b ) );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &c ) );
    check( rocketnet_source_snapshot_contains( &t.snapshot, held, &a ) );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, held, &b ) );
    check( rocketnet_source_snapshot_writer_publish( &t.snapshot ) );
    check( rocketnet_source_snapshot_contains( &t.snapshot, held, &a ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );

    const int next = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( next == 0 );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, next, &a ) );
    check( rocketnet_source_snapshot_contains( &t.snapshot, next, &b ) );
    check( rocketnet_source_snapshot_contains( &t.snapshot, next, &c ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );

    const uint64_t word = rocketnet_source_snapshot_reader_load( &t.snapshot );
    check( rocketnet_source_snapshot_word_generation( word ) == 2 );
}

REGISTER_UNIT_TEST( test_source_snapshot_writer_fills_the_slot_not_published );

// an add publishes nothing: the published word changes only at publish,
// after the last add (HalfPublished, the trace check's plant)

static void test_source_snapshot_publish_only_after_the_last_add()
{
    snapshot_test_t t( 4, 1 );
    const uint64_t before = rocketnet_source_snapshot_reader_load( &t.snapshot );
    check( rocketnet_source_snapshot_writer_begin( &t.snapshot ) == 1 );
    for ( int n = 1; n <= 4; n++ )
    {
        const struct rocketnet_address_t a = snapshot_test_address( n );
        check( rocketnet_source_snapshot_writer_add( &t.snapshot, &a ) );
        check( rocketnet_source_snapshot_reader_load( &t.snapshot ) == before );
    }
    check( rocketnet_source_snapshot_writer_publish( &t.snapshot ) );
    const uint64_t after = rocketnet_source_snapshot_reader_load( &t.snapshot );
    check( after != before );
    check( rocketnet_source_snapshot_word_slot( after ) == 1 && rocketnet_source_snapshot_word_generation( after ) == 1 );
}

REGISTER_UNIT_TEST( test_source_snapshot_publish_only_after_the_last_add );

// a reader still holding the writer's target makes the update skip its
// publish; the readers keep the published snapshot, and the next update
// after the release goes ahead (NoTornRead, WriterNeverWritesAnnounced)

static void test_source_snapshot_skips_while_a_reader_holds_the_target()
{
    snapshot_test_t t( 4, 3 );
    const struct rocketnet_address_t a = snapshot_test_address( 1 );
    const struct rocketnet_address_t b = snapshot_test_address( 2 );

    // reader 2 holds slot 0 (generation 0); slot 1 is published under it
    check( rocketnet_source_snapshot_reader_acquire( &t.snapshot, 2 ) == 0 );
    check( t.update( &a, 1 ) );

    // the target is slot 0 again: reader 2 still holds it
    check( rocketnet_source_snapshot_writer_begin( &t.snapshot ) == ROCKETNET_SOURCE_SNAPSHOT_NONE );
    check( !t.snapshot.writer->filling );
    check( !t.update( &b, 1 ) );

    // other readers still see {a}
    const int slot = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( slot == 1 );
    check( rocketnet_source_snapshot_contains( &t.snapshot, slot, &a ) );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &b ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );
    check( rocketnet_source_snapshot_word_generation( rocketnet_source_snapshot_reader_load( &t.snapshot ) ) == 1 );

    // released: the update goes ahead into slot 0
    rocketnet_source_snapshot_reader_release( &t.snapshot, 2 );
    check( t.update( &b, 1 ) );
    const int next = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 1 );
    check( next == 0 );
    check( rocketnet_source_snapshot_contains( &t.snapshot, next, &b ) );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, next, &a ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 1 );
}

REGISTER_UNIT_TEST( test_source_snapshot_skips_while_a_reader_holds_the_target );

// a reader whose load is two publishes old announces after the writer's
// scan, while the writer fills that slot: the re-check refuses the stale
// word and withdraws the announcement, and the next acquire takes the
// published slot (NoRecheck)

static void test_source_snapshot_recheck_refuses_a_stale_word()
{
    snapshot_test_t t( 4, 1 );
    const struct rocketnet_address_t a = snapshot_test_address( 1 );
    const struct rocketnet_address_t b = snapshot_test_address( 2 );

    const uint64_t stale = rocketnet_source_snapshot_reader_load( &t.snapshot );   // slot 0, generation 0
    check( t.update( &a, 1 ) );                                                     // slot 1 published

    // the writer scans (nothing announced) and fills slot 0
    check( rocketnet_source_snapshot_writer_begin( &t.snapshot ) == 0 );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &b ) );

    // the reader announces slot 0 now, after the scan
    rocketnet_source_snapshot_reader_announce( &t.snapshot, 0, stale );
    check( t.announced( 0 ) == 0 );
    check( !rocketnet_source_snapshot_reader_recheck( &t.snapshot, 0, stale ) );
    check( t.announced( 0 ) == ROCKETNET_SOURCE_SNAPSHOT_NONE );

    // a fresh acquire takes slot 1
    const int slot = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( slot == 1 );
    check( rocketnet_source_snapshot_contains( &t.snapshot, slot, &a ) );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &b ) );
    check( rocketnet_source_snapshot_writer_publish( &t.snapshot ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );

    // a word republished on the same slot at a later generation is not the
    // word loaded: the re-check compares the generation too
    const uint64_t old = rocketnet_source_snapshot_reader_load( &t.snapshot );     // slot 0, generation 2
    check( t.update( &a, 1 ) );                                                     // slot 1, generation 3
    check( t.update( &b, 1 ) );                                                     // slot 0, generation 4
    rocketnet_source_snapshot_reader_announce( &t.snapshot, 0, old );
    check( !rocketnet_source_snapshot_reader_recheck( &t.snapshot, 0, old ) );
}

REGISTER_UNIT_TEST( test_source_snapshot_recheck_refuses_a_stale_word );

// two addresses homed on the last cell: the second's probe wraps to cell 0,
// and the lookup wraps to find it; an absent address homed on cell 0
// probes past it to an empty cell (LookupNoWrap, ChainIntact)

static void test_source_snapshot_probe_wraps_at_the_slot_end()
{
    snapshot_test_t t( 2, 1 );
    check( t.snapshot.capacity == 4 );
    const struct rocketnet_address_t a = snapshot_test_homed( &t.snapshot, 3, 1 );
    const struct rocketnet_address_t b = snapshot_test_homed( &t.snapshot, 3, a.data.ipv4[3] + 256 * a.data.ipv4[2] + 1 );
    const struct rocketnet_address_t c = snapshot_test_homed( &t.snapshot, 0, 1 );

    check( rocketnet_source_snapshot_writer_begin( &t.snapshot ) == 1 );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &a ) );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &b ) );
    check( rocketnet_source_snapshot_writer_publish( &t.snapshot ) );

    const struct rocketnet_address_t * cells = &t.snapshot.cells[1 * t.snapshot.capacity];
    check( rocketnet_address_equal( &cells[3], &a ) );
    check( rocketnet_address_equal( &cells[0], &b ) );

    const int slot = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( rocketnet_source_snapshot_contains( &t.snapshot, slot, &a ) );
    check( rocketnet_source_snapshot_contains( &t.snapshot, slot, &b ) );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &c ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );
}

REGISTER_UNIT_TEST( test_source_snapshot_probe_wraps_at_the_slot_end );

// a slot holds at most max_entries: a new address past it is refused (the
// caller counts it), an address already in it is not refused and not
// stored twice (AtMostHalf)

static void test_source_snapshot_add_refuses_past_max_entries()
{
    snapshot_test_t t( 2, 1 );
    const struct rocketnet_address_t a = snapshot_test_address( 1 );
    const struct rocketnet_address_t b = snapshot_test_address( 2 );
    const struct rocketnet_address_t c = snapshot_test_address( 3 );

    check( rocketnet_source_snapshot_writer_begin( &t.snapshot ) == 1 );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &a ) );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &a ) );
    check( t.snapshot.writer->count == 1 );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &b ) );
    check( !rocketnet_source_snapshot_writer_add( &t.snapshot, &c ) );
    check( rocketnet_source_snapshot_writer_add( &t.snapshot, &b ) );
    check( t.snapshot.writer->count == 2 );
    check( rocketnet_source_snapshot_writer_publish( &t.snapshot ) );

    int stored = 0;
    for ( int i = 0; i < t.snapshot.capacity; i++ )
    {
        stored += t.snapshot.cells[1 * t.snapshot.capacity + i].type != ROCKETNET_ADDRESS_NONE ? 1 : 0;
    }
    check( stored == 2 );

    const int slot = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &c ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );
}

REGISTER_UNIT_TEST( test_source_snapshot_add_refuses_past_max_entries );

// a fill clears its slot: the set published there two updates ago does not
// survive into the next (PublishedExact)

static void test_source_snapshot_fill_clears_the_slot()
{
    snapshot_test_t t( 4, 1 );
    const struct rocketnet_address_t a = snapshot_test_address( 1 );
    const struct rocketnet_address_t b = snapshot_test_address( 2 );
    const struct rocketnet_address_t c = snapshot_test_address( 3 );

    check( t.update( &a, 1 ) );         // slot 1: {a}
    check( t.update( &b, 1 ) );         // slot 0: {b}
    check( t.update( &c, 1 ) );         // slot 1: {c}, a gone

    const int slot = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( slot == 1 );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &a ) );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &b ) );
    check( rocketnet_source_snapshot_contains( &t.snapshot, slot, &c ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );

    // an empty update publishes the empty set
    check( t.update( NULL, 0 ) );
    const int empty = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( empty == 0 );
    check( !rocketnet_source_snapshot_contains( &t.snapshot, empty, &b ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );
}

REGISTER_UNIT_TEST( test_source_snapshot_fill_clears_the_slot );

// equal addresses are found whatever the bytes equality does not compare
// hold: an ipv4 address with other bytes in the rest of the union, and the
// port and type are part of the key

static void test_source_snapshot_hash_covers_only_compared_bytes()
{
    snapshot_test_t t( 64, 1 );
    std::vector<struct rocketnet_address_t> stored;
    for ( int n = 1; n <= 64; n++ )
    {
        stored.push_back( snapshot_test_address( n ) );
    }
    check( t.update( stored.data(), (int) stored.size() ) );

    const int slot = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    int found = 0;
    for ( int n = 1; n <= 64; n++ )
    {
        struct rocketnet_address_t probe = snapshot_test_address( n );
        uint8_t * bytes = (uint8_t *) &probe.data;
        for ( size_t i = 4; i < sizeof( probe.data ); i++ )
        {
            bytes[i] = (uint8_t) ( 0xA5 ^ ( n * 31 + (int) i ) );
        }
        check( rocketnet_address_equal( &probe, &stored[(size_t) ( n - 1 )] ) );
        found += rocketnet_source_snapshot_contains( &t.snapshot, slot, &probe ) ? 1 : 0;

        struct rocketnet_address_t other_port = snapshot_test_address( n );
        other_port.port = 40001;
        check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &other_port ) );
    }
    check( found == 64 );

    struct rocketnet_address_t v6;
    memset( &v6, 0, sizeof( v6 ) );
    v6.type = ROCKETNET_ADDRESS_IPV6;
    v6.data.ipv6[0] = 0x2001;
    v6.data.ipv6[7] = 1;
    v6.port = 40000;
    check( !rocketnet_source_snapshot_contains( &t.snapshot, slot, &v6 ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );

    check( t.update( &v6, 1 ) );
    const int next = rocketnet_source_snapshot_reader_acquire( &t.snapshot, 0 );
    check( rocketnet_source_snapshot_contains( &t.snapshot, next, &v6 ) );
    rocketnet_source_snapshot_reader_release( &t.snapshot, 0 );
}

REGISTER_UNIT_TEST( test_source_snapshot_hash_covers_only_compared_bytes );
