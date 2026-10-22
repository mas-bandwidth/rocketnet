/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The reassembly stale rule (STANDARD.md _Fragmentation_, tla/FragmentsAndAcks.tla
// StaleOnlyIfOlder): an honest sender's payloads are never refused as stale after
// any loss pattern, one hostile fragment with an id far ahead neither evicts
// an occupant nor holds a slot, and one bad fragment nearer costs at most the
// stale window's payloads of its slot. Each test drives the reassembly engine directly
// with an honest sender's id sequence -- one id per fragmented payload, two
// fragments each -- and a scripted loss or delay. The engine takes no time and
// no sockets: the scripted order is the arrival order.

#include "test_unit_registry.h"

#include "rocketnet_fragment.h"
#include "rocketnet_counters.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct rocketnet_reassembly_counters_t stale_rule_counters = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_FRAGMENT_STALE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL };

// every payload is two fragments: a 100-byte stride and a 60-byte final

#define STALE_RULE_STRIDE   100
#define STALE_RULE_FINAL    60
#define STALE_RULE_BYTES    ( STALE_RULE_STRIDE + STALE_RULE_FINAL )

struct stale_rule_harness_t
{
    struct rocketnet_counters_t counters_storage;
    uint64_t * counters;
    struct rocketnet_reassembly_set_t * set;
    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];
    int delivered;
};

static void stale_rule_begin( struct stale_rule_harness_t * h, int num_slots )
{
    memset( h, 0, sizeof(*h) );
    h->counters = h->counters_storage.values;
    h->set = rocketnet_reassembly_set_create( NULL, 1, num_slots, 8192, 4 );
    rocketnet_unit_check( h->set != NULL );
}

static void stale_rule_end( struct stale_rule_harness_t * h )
{
    rocketnet_reassembly_set_destroy( h->set );
}

static uint8_t stale_rule_pattern( uint32_t payload_id )
{
    return (uint8_t) ( payload_id * 7u + 3u );
}

// offers one fragment of a two-fragment payload; returns the process result.
// a completion is checked byte-exact against the payload id's pattern and
// counted delivered.

static int stale_rule_offer( struct stale_rule_harness_t * h, uint32_t payload_id, int fragment_index )
{
    const uint8_t pattern = stale_rule_pattern( payload_id );
    const int fragment_bytes = fragment_index == 0 ? STALE_RULE_STRIDE : STALE_RULE_FINAL;

    rocketnet_write_fragment_header( h->fragment, payload_id, fragment_index, 2 );
    for ( int i = 0; i < fragment_bytes; i++ )
        h->fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + i] = (uint8_t) ( pattern + STALE_RULE_STRIDE * fragment_index + i );

    rocketnet_reassembly_set_begin_update( h->set );
    const int result = rocketnet_reassembly_process_fragment( h->set, 0, h->fragment, ROCKETNET_FRAGMENT_HEADER_BYTES + fragment_bytes,
                                                              h->counters, &stale_rule_counters );
    if ( h->set->completed_bytes != 0 )
    {
        rocketnet_unit_check( h->set->completed_bytes == STALE_RULE_BYTES );
        for ( int i = 0; i < STALE_RULE_BYTES; i++ )
            rocketnet_unit_check( h->set->completed_payload[i] == (uint8_t) ( pattern + i ) );
        h->delivered++;
    }
    return result;
}

static void stale_rule_report( const char * name, struct stale_rule_harness_t * h, int sent )
{
    printf( "%s: %d of %d payloads delivered, FRAGMENT_STALE %llu, abandoned %llu\n", name, h->delivered, sent,
            (unsigned long long) h->counters[ROCKETNET_COUNTER_FRAGMENT_STALE],
            (unsigned long long) h->counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] );
    fflush( stdout );
}

// Route a: an incomplete occupant X, then the next 32 payloads of its slot's
// series lost whole (X+8 .. X+256 at eight slots) while the other seven slots'
// payloads arrive and complete. X+264 is the first of the series to arrive,
// and every later payload must deliver: the loss is X's abandonment, counted
// once, never a stale refusal.

static void test_reassembly_lost_run_does_not_stale_the_slot()
{
    struct stale_rule_harness_t h;
    stale_rule_begin( &h, 8 );

    const uint32_t occupant = 8;
    const uint32_t last = 1000;
    int lost = 0;

    for ( uint32_t id = 1; id <= last; id++ )
    {
        if ( id == occupant )
        {
            rocketnet_unit_check( stale_rule_offer( &h, id, 0 ) == 1 );       // its final is lost
            lost++;
            continue;
        }
        if ( id % 8 == occupant % 8 && id > occupant && id <= occupant + 256 )
        {
            lost++;                                                             // lost whole: nothing arrives
            continue;
        }
        stale_rule_offer( &h, id, 0 );
        stale_rule_offer( &h, id, 1 );
    }

    stale_rule_report( "test_reassembly_lost_run_does_not_stale_the_slot", &h, (int) last );

    rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == 0 );
    rocketnet_unit_check( h.delivered == (int) last - lost );
    rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 1 );   // X, displaced by X+264

    stale_rule_end( &h );
}

// Route b: payload X loses its final to delay (X+8 displaces X, counted
// abandoned), and the final arrives just after X+256 completes and empties
// the slot -- 256 two-fragment payloads late, 512 sequences, well inside the
// replay window, so it reaches reassembly. It is no newer than anything live,
// so it may hold the slot only until the next live id of the series, X+264,
// displaces it; every later payload delivers. No loss beyond the one delay.

static void test_reassembly_late_fragment_after_completion_does_not_poison()
{
    struct stale_rule_harness_t h;
    stale_rule_begin( &h, 8 );

    const uint32_t late = 3;
    const uint32_t last = 736;

    for ( uint32_t id = 1; id <= last; id++ )
    {
        if ( id == late )
        {
            rocketnet_unit_check( stale_rule_offer( &h, id, 0 ) == 1 );       // its final is delayed
            continue;
        }
        stale_rule_offer( &h, id, 0 );
        stale_rule_offer( &h, id, 1 );
        if ( id == late + 256 )
        {
            rocketnet_unit_check( h.delivered == (int) id - 1 );              // X+256 completed, the slot is empty
            stale_rule_offer( &h, late, 1 );                                    // and X's delayed final lands in it
        }
    }

    stale_rule_report( "test_reassembly_late_fragment_after_completion_does_not_poison", &h, (int) last );

    rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == 0 );
    rocketnet_unit_check( h.delivered == (int) last - 1 );                      // every payload but X
    rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 2 );   // X by X+8, its ghost by X+264

    stale_rule_end( &h );
}

// The hostile case the stale rule defends: one sealed fragment with an id far
// ahead. Against an occupant it refuses stale and the occupant completes;
// into an empty slot it refuses stale too, so it never holds the slot; an id
// ahead within the bound may take an empty slot, but the next honest id of the
// series displaces it. Every honest payload delivers.

static void test_reassembly_hostile_far_ahead_id_cannot_poison()
{
    struct stale_rule_harness_t h;
    stale_rule_begin( &h, 8 );

    const uint32_t last = 600;
    uint64_t hostile_refused = 0;

    for ( uint32_t id = 1; id <= last; id++ )
    {
        rocketnet_unit_check( stale_rule_offer( &h, id, 0 ) == 1 );

        if ( id == 5 )
        {
            // NEGATIVE: just under half the id space ahead of the occupant, in its slot
            rocketnet_unit_check( stale_rule_offer( &h, id + 0x7FFFFFF8u, 0 ) == 0 );
            hostile_refused++;
            rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 0 );
        }

        rocketnet_unit_check( stale_rule_offer( &h, id, 1 ) == 1 );
        rocketnet_unit_check( h.delivered == (int) id );

        if ( id == 13 )
        {
            // NEGATIVE: the same far-ahead id into the slot the completion of 13 just emptied
            rocketnet_unit_check( stale_rule_offer( &h, id + 0x7FFFFFF8u, 0 ) == 0 );
            hostile_refused++;
            // NEGATIVE: half the id space away, the point where newer and older are undefined
            rocketnet_unit_check( stale_rule_offer( &h, id + 0x80000000u, 0 ) == 0 );
            hostile_refused++;
        }

        if ( id == 21 )
        {
            // an id ahead within the bound takes the empty slot, which the
            // next honest id of the series, 29, displaces (counted abandoned)
            rocketnet_unit_check( stale_rule_offer( &h, id + 0x20000000u, 0 ) == 1 );
        }
    }

    stale_rule_report( "test_reassembly_hostile_far_ahead_id_cannot_poison", &h, (int) last );

    rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == hostile_refused );
    rocketnet_unit_check( h.delivered == (int) last );
    rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 1 );

    stale_rule_end( &h );
}

// The bound on one bad fragment: an id no honest payload yet carries, inside
// the reach, landing in an occupied slot. Its worst case is an id exactly
// ROCKETNET_REASSEMBLY_BEHIND (256) ahead of the occupant, in the occupant's
// slot: it displaces occupant 5 (abandoned), and the slot's series behind it,
// 13 .. 253, refuses stale until the series reaches 261 -- 32 payloads lost,
// 5 and 31 more, the limit of the stale test before this rule. Payload 261
// itself delivers, its early copy a duplicate. The same fragment further
// ahead (4005) holds the slot only until 5's own final displaces it: one
// payload lost, nothing stale.

static void test_reassembly_one_bad_fragment_costs_at_most_the_window()
{
    const uint32_t last = 2000;

    {
        struct stale_rule_harness_t h;
        stale_rule_begin( &h, 8 );

        const uint32_t bad = 5 + ROCKETNET_REASSEMBLY_BEHIND;

        for ( uint32_t id = 1; id <= last; id++ )
        {
            stale_rule_offer( &h, id, 0 );
            if ( id == 5 )
                rocketnet_unit_check( stale_rule_offer( &h, bad, 0 ) == 1 );   // displaces the occupant
            stale_rule_offer( &h, id, 1 );
        }

        stale_rule_report( "test_reassembly_one_bad_fragment_costs_at_most_the_window (256 ahead)", &h, (int) last );

        const int lost = (int) ( ROCKETNET_REASSEMBLY_BEHIND / 8 );                    // 32: 5, 13 .. 253
        rocketnet_unit_check( lost == 32 );
        rocketnet_unit_check( h.delivered == (int) last - lost );
        rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == 1 + 2 * 31 );   // 5's final, both of 13 .. 253
        rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED] == 1 );
        rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX] == 1 );   // 261's own first

        stale_rule_end( &h );
    }

    {
        struct stale_rule_harness_t h;
        stale_rule_begin( &h, 8 );

        const uint32_t bad = 4005;

        for ( uint32_t id = 1; id <= last; id++ )
        {
            stale_rule_offer( &h, id, 0 );
            if ( id == 5 )
                rocketnet_unit_check( stale_rule_offer( &h, bad, 0 ) == 1 );   // displaces the occupant
            stale_rule_offer( &h, id, 1 );
        }

        stale_rule_report( "test_reassembly_one_bad_fragment_costs_at_most_the_window (4000 ahead)", &h, (int) last );

        rocketnet_unit_check( h.delivered == (int) last - 1 );                      // every payload but 5
        rocketnet_unit_check( h.counters[ROCKETNET_COUNTER_FRAGMENT_STALE] == 0 );

        stale_rule_end( &h );
    }
}

REGISTER_UNIT_TEST( test_reassembly_lost_run_does_not_stale_the_slot );
REGISTER_UNIT_TEST( test_reassembly_one_bad_fragment_costs_at_most_the_window );
REGISTER_UNIT_TEST( test_reassembly_late_fragment_after_completion_does_not_poison );
REGISTER_UNIT_TEST( test_reassembly_hostile_far_ahead_id_cannot_poison );
