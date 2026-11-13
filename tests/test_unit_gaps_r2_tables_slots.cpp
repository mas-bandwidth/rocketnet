/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The slot machines at the refusals and the edges no other case reached
// (mutation gaps, round 2, r2-tables): rn_slots init at zero slots, the
// client slot table's fresh generations, its bounds, its corrupt-index
// refusals with asserts quieted, its DEV assign helper and seams, the reset
// witnesses under a failing allocator, and the reset inventory's success
// path. No sockets, no real time and no threads. Each failing check prints the
// fault first.
//
// The client slot table is separate rocketnet_malloc blocks, so a read or a
// write one past an array lands in the allocator's own slack, where no
// sanitizer looks. These cases install an allocator that fills the bytes past
// every block with a guard byte: a write there is found when the block is
// freed, and a read there sees the guard, chosen so that the stale read takes
// a path the bound would have refused.

#include "test_unit_registry.h"
#include "loopback_hub.h"

#include "rocketnet_internal.h"
#include "rocketnet_slots.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_server_replay_marks.h"
#include "rocketnet_connection_reset.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_memory.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// rn_slots: zero slots is a valid machine

// mutation gap: hunt 2/3619
// mutation gap: hunt 2/3626
//
// Zero slots is inside 0..ROCKETNET_MAX_CLIENTS: init takes it, logs nothing
// and keeps the caller's storage, a machine with no slot to observe.

static void test_gaps_r2_tables_slots_init_takes_zero_slots()
{
    struct rn_slots_t slots;
    uint64_t connection_ids[1] = { 0 };
    uint8_t changed[1] = { 0 };

    memset( &slots, 0xA5, sizeof( slots ) );
    rn_slots_init( &slots, 0, connection_ids, changed );
    const bool taken = slots.max_clients == 0 && slots.connection_id == connection_ids && slots.changed == changed;
    if ( !taken )
    {
        printf( "fault: rn_slots_init refused zero slots: the caller's storage was not kept\n" );
    }
    check( taken );
    check( rn_slots_observe( &slots, 0, 7 ) == RN_SLOTS_NONE );
    check( rn_slots_admit( &slots, 0, 7 ) == RN_SLOTS_DISCARD );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_slots_init_takes_zero_slots );

// ---------------------------------------------------------------------------
// client slots: a fresh slot has held no generation

// mutation gap: hunt 2/3734
//
// A fresh slot's generation is 0, which no owner has: an assign under any
// nonzero generation is taken, 0x0101010101010101 included (what a
// generation array filled with byte 1 reads as).

static void test_gaps_r2_tables_client_slots_fresh_slot_has_no_generation()
{
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 1 );
    check( slots );

    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0x33, sizeof( user ) );
    const int index = rocketnet_client_slots_assign( slots, 7, 0, 0x0101010101010101ULL, user );
    if ( index != 0 )
    {
        printf( "fault: a fresh slot refused owner generation 0x0101010101010101 as stale: its generation was not cleared at create\n" );
    }
    check( index == 0 );

    rocketnet_client_slots_destroy( slots );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_fresh_slot_has_no_generation );

#if ROCKETNET_DEV

extern void rocketnet_client_slots_fill_index_for_test( struct rocketnet_client_slots_t * slots, int client_index );

static void gaps_r2_tables_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}

// ---------------------------------------------------------------------------
// the guard and failure allocator

enum
{
    GAPS_R2_TABLES_GUARD_BYTES = 128
};

struct gaps_r2_tables_allocator_t
{
    uint8_t guard;
    int fail_at;                // the call that fails, counted from 1; 0 for none
    int calls;
    int failed;
    int live;
    int overwritten;
};

static gaps_r2_tables_allocator_t gaps_r2_tables_allocator;

// the pointer rocketnet_malloc hands out from the block the allocator returned
static uint8_t * gaps_r2_tables_handed_out( const uint8_t * block )
{
    const uintptr_t overhead = (uintptr_t) rocketnet_allocation_overhead();
    return (uint8_t *) ( ( (uintptr_t) block + overhead ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
}

// the block's base and its size sit in the 16 bytes before it; the bytes
// after the array rocketnet_malloc carves, to the end of a guard past the
// block, hold the guard byte
static void * gaps_r2_tables_allocate( void * context, size_t bytes )
{
    (void) context;
    gaps_r2_tables_allocator_t * state = &gaps_r2_tables_allocator;
    ++state->calls;
    if ( state->calls == state->fail_at )
    {
        ++state->failed;
        return NULL;
    }

    void * base = malloc( bytes + GAPS_R2_TABLES_GUARD_BYTES + 128 );
    if ( !base )
    {
        return NULL;
    }

    const uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memcpy( (void *) ( address - 2 * sizeof( void * ) ), &bytes, sizeof( bytes ) );
    memset( (void *) address, state->guard, bytes + GAPS_R2_TABLES_GUARD_BYTES );
    ++state->live;
    return (void *) address;
}

static void gaps_r2_tables_release( void * context, void * pointer )
{
    (void) context;
    gaps_r2_tables_allocator_t * state = &gaps_r2_tables_allocator;
    void * base = NULL;
    size_t bytes = 0;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    memcpy( &bytes, (uint8_t *) pointer - 2 * sizeof( void * ), sizeof( bytes ) );

    const uint8_t * end = gaps_r2_tables_handed_out( (const uint8_t *) pointer ) + ( bytes - (size_t) rocketnet_allocation_overhead() );
    const uint8_t * guard_end = (const uint8_t *) pointer + bytes + GAPS_R2_TABLES_GUARD_BYTES;
    for ( const uint8_t * p = end; p < guard_end; ++p )
    {
        if ( *p != state->guard )
        {
            ++state->overwritten;
            break;
        }
    }

    --state->live;
    free( base );
}

static void gaps_r2_tables_install( uint8_t guard )
{
    memset( &gaps_r2_tables_allocator, 0, sizeof( gaps_r2_tables_allocator ) );
    gaps_r2_tables_allocator.guard = guard;
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( NULL, gaps_r2_tables_allocate, gaps_r2_tables_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );
}

static void gaps_r2_tables_uninstall()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( gaps_r2_tables_allocator.live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

static const uint64_t GAPS_R2_TABLES_GUARD_ID = 0x0101010101010101ULL;

// ---------------------------------------------------------------------------
// client slots: a corrupt index is refused, never read past

// mutation gap: hunt 2/3795
// mutation gap: hunt 2/3804
//
// An index cell naming slot max_clients is corrupt: a lookup that meets it
// refuses (-1). Past the arrays the guard reads as a connected slot of
// generation and client id 0x0101010101010101, so a bound that admits
// max_clients, or a refusal that falls through, finds that id at slot
// max_clients.

static void test_gaps_r2_tables_client_slots_index_naming_max_clients_is_refused()
{
    gaps_r2_tables_install( 0x01 );

    const int max_clients = 2;
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, max_clients );
    check( slots );

    rocketnet_client_slots_fill_index_for_test( slots, max_clients );
    rocketnet_assert_function( gaps_r2_tables_quiet_assert );
    const int found = rocketnet_client_slots_find( slots, GAPS_R2_TABLES_GUARD_ID );
    rocketnet_assert_function( NULL );
    if ( found != -1 )
    {
        printf( "fault: a lookup through an index cell naming slot max_clients returned slot %d: it read the slot arrays past their end\n", found );
    }
    check( found == -1 );

    rocketnet_client_slots_destroy( slots );
    check( gaps_r2_tables_allocator.overwritten == 0 );
    gaps_r2_tables_uninstall();
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_index_naming_max_clients_is_refused );

// mutation gap: hunt 2/3803
// mutation gap: hunt 2/3810
//
// An assign through a corrupt index is refused and changes nothing: an index
// cell naming a free slot, and an index whose every cell names one connected
// slot (no empty cell, so the probe runs out).

static void test_gaps_r2_tables_client_slots_assign_through_a_corrupt_index_is_refused()
{
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0x44, sizeof( user ) );

    const int cell_names[2] = { 1, 0 };            // slot 1 is free; slot 0 is connected
    const char * shapes[2] = { "an index cell naming a free slot", "an index full of one connected slot" };
    for ( int shape = 0; shape < 2; ++shape )
    {
        struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 2 );
        check( slots );
        check( rocketnet_client_slots_assign( slots, 1001, 0, 1, user ) == 0 );

        rocketnet_client_slots_fill_index_for_test( slots, cell_names[shape] );
        rocketnet_assert_function( gaps_r2_tables_quiet_assert );
        const int index = rocketnet_client_slots_assign( slots, 2002, 1, 1, user );
        rocketnet_assert_function( NULL );
        if ( index != -1 )
        {
            printf( "fault: an assign through %s was taken (slot %d): the corrupt index was not refused\n", shapes[shape], index );
        }
        check( index == -1 );
        check( rocketnet_client_slots_num_connected( slots ) == 1 );

        rocketnet_client_slots_destroy( slots );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_assign_through_a_corrupt_index_is_refused );

// ---------------------------------------------------------------------------
// client slots: the DEV assign helper

// mutation gap: hunt 2/3858
// mutation gap: hunt 2/3859
// mutation gap: hunt 2/3860

static void test_gaps_r2_tables_client_slots_helper_refuses_no_table()
{
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0x55, sizeof( user ) );
    const int index = rocketnet_client_slots_assign( NULL, 1, 0, user );
    if ( index != -1 )
    {
        printf( "fault: the DEV assign helper returned %d for no table: it did not refuse with -1\n", index );
    }
    check( index == -1 );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_helper_refuses_no_table );

// mutation gap: hunt 2/3861
// mutation gap: hunt 2/3873
// mutation gap: hunt 2/3874
// mutation gap: hunt 2/3875
// mutation gap: hunt 2/3876
//
// The helper picks owner generation 1 for a slot that never held one, and 2
// for a slot whose last owner held 1, so a slot is reused through it.

static void test_gaps_r2_tables_client_slots_helper_reuses_a_slot()
{
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 1 );
    check( slots );
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0x66, sizeof( user ) );

    const int first = rocketnet_client_slots_assign( slots, 7, 0, user );
    check( first == 0 );
    const uint64_t first_generation = first == 0 ? rocketnet_client_slots_owner_generation( slots, 0 ) : 0;
    if ( first_generation != 1 )
    {
        printf( "fault: the DEV assign helper gave a fresh slot owner generation %llu, not 1\n", (unsigned long long) first_generation );
    }
    check( first_generation == 1 );

    rocketnet_client_slots_release( slots, 0 );
    rocketnet_client_slots_publish( slots );

    const int again = rocketnet_client_slots_assign( slots, 8, 0, user );
    if ( again != 0 )
    {
        printf( "fault: the DEV assign helper's reuse of a released slot was refused as stale: it picked the slot's last generation again\n" );
    }
    check( again == 0 );
    const uint64_t second_generation = again == 0 ? rocketnet_client_slots_owner_generation( slots, 0 ) : 0;
    if ( again == 0 && second_generation != 2 )
    {
        printf( "fault: the DEV assign helper reused a slot that last held generation 1 under generation %llu, not 2\n",
                (unsigned long long) second_generation );
    }
    check( second_generation == 2 );

    rocketnet_client_slots_destroy( slots );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_helper_reuses_a_slot );

// ---------------------------------------------------------------------------
// client slots: handles at the bound

// mutation gap: hunt 2/3953
// mutation gap: hunt 2/3957
//
// A handle is written only for a connected slot inside the table: index -1,
// index max_clients (past the arrays the guard reads as connected), a free
// slot and no handle are each refused.

static void test_gaps_r2_tables_client_slots_handle_refuses_outside_and_free()
{
    gaps_r2_tables_install( 0x01 );

    const int max_clients = 2;
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, max_clients );
    check( slots );
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0x77, sizeof( user ) );
    check( rocketnet_client_slots_assign( slots, 1001, 0, 1, user ) == 0 );

    struct rocketnet_client_slot_handle_t handle;
    const int refused[3] = { -1, max_clients, 1 };
    const char * names[3] = { "index -1", "index max_clients", "a free slot" };
    for ( int i = 0; i < 3; ++i )
    {
        memset( &handle, 0, sizeof( handle ) );
        const bool written = rocketnet_client_slots_handle( slots, refused[i], &handle );
        if ( written )
        {
            printf( "fault: a client slot handle was written for %s\n", names[i] );
        }
        check( !written );
    }
    const bool written_to_nothing = rocketnet_client_slots_handle( slots, 0, NULL );
    check( !written_to_nothing );
    check( rocketnet_client_slots_handle( slots, 0, &handle ) && handle.client_id == 1001 && handle.client_index == 0 );

    rocketnet_client_slots_destroy( slots );
    check( gaps_r2_tables_allocator.overwritten == 0 );
    gaps_r2_tables_uninstall();
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_handle_refuses_outside_and_free );

// mutation gap: hunt 2/3922
//
// A handle naming slot max_clients (a forged one; past the arrays the guard
// reads as a connected slot with this client id and generation) is refused at
// the bound, before the release reads a slot or probes the index.

static void test_gaps_r2_tables_client_slots_release_handle_refuses_max_clients()
{
    gaps_r2_tables_install( 0x01 );

    const int max_clients = 2;
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, max_clients );
    check( slots );

    struct rocketnet_client_slot_handle_t forged;
    memset( &forged, 0, sizeof( forged ) );
    forged.client_id = GAPS_R2_TABLES_GUARD_ID;
    forged.client_index = max_clients;
    forged.generation = GAPS_R2_TABLES_GUARD_ID;
    const bool released = rocketnet_client_slots_release_handle( slots, &forged );
    const uint64_t probes = rocketnet_client_slots_test_last_probes( slots );
    if ( released || probes != 0 )
    {
        printf( "fault: a handle naming slot max_clients passed the release's bound (released %d, %llu index probes): it read the slot arrays past their end\n",
                released ? 1 : 0, (unsigned long long) probes );
    }
    check( !released );
    check( probes == 0 );

    rocketnet_client_slots_destroy( slots );
    check( gaps_r2_tables_allocator.overwritten == 0 );
    gaps_r2_tables_uninstall();
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_release_handle_refuses_max_clients );

// mutation gap: hunt 2/3933
//
// A release whose client id the index cannot find (every cell names the
// other connected slot, so the probe runs out) is refused, and the slot stays
// connected. Without the refusal the release goes on at position -1: the int
// before the index cells (zero padding of the DEV index header) names slot 0,
// so slot 0 is released through a cell that is not one.

static void test_gaps_r2_tables_client_slots_release_with_its_id_unfindable_is_refused()
{
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 2 );
    check( slots );
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0x88, sizeof( user ) );
    check( rocketnet_client_slots_assign( slots, 1001, 0, 1, user ) == 0 );
    check( rocketnet_client_slots_assign( slots, 2002, 1, 1, user ) == 1 );

    struct rocketnet_client_slot_handle_t handle;
    check( rocketnet_client_slots_handle( slots, 0, &handle ) );

    rocketnet_client_slots_fill_index_for_test( slots, 1 );
    rocketnet_assert_function( gaps_r2_tables_quiet_assert );
    const bool released = rocketnet_client_slots_release_handle( slots, &handle );
    rocketnet_assert_function( NULL );
    if ( released || !rocketnet_client_slots_connected( slots, 0 ) )
    {
        printf( "fault: a release whose id the index could not find was taken: it went on at index position -1 and released slot 0\n" );
    }
    check( !released );
    check( rocketnet_client_slots_connected( slots, 0 ) );
    check( rocketnet_client_slots_num_connected( slots ) == 2 );

    rocketnet_client_slots_destroy( slots );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_release_with_its_id_unfindable_is_refused );

// mutation gap: hunt 2/3936
//
// With the hash index forced after an assign, a second assign of the same id
// is not seen as a duplicate (its probe starts at the forced, empty bucket),
// so two connected slots share the id. Releasing the first finds the second's
// index entry, which does not name it: the release is refused, and both
// entries stay findable.

static void test_gaps_r2_tables_client_slots_release_refuses_an_entry_naming_another_slot()
{
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 4 );
    check( slots );
    uint8_t user[ROCKETNET_USER_DATA_BYTES];
    memset( user, 0x99, sizeof( user ) );

    check( rocketnet_client_slots_assign( slots, 1001, 0, 1, user ) == 0 );
    struct rocketnet_client_slot_handle_t first;
    check( rocketnet_client_slots_handle( slots, 0, &first ) );

    const int buckets = 8;
    const int home = (int) rocketnet_client_slots_test_hash_index( slots, 1001 );
    const int forced = ( home + 3 ) % buckets;
    check( rocketnet_client_slots_test_force_hash_index( slots, forced ) );
    check( rocketnet_client_slots_assign( slots, 1001, 1, 1, user ) == 1 );

    const bool released = rocketnet_client_slots_release_handle( slots, &first );
    const int found = rocketnet_client_slots_find( slots, 1001 );
    if ( released || found != 1 )
    {
        printf( "fault: a release took slot 0 through an index entry naming slot 1 (released %d, find %d): it erased the other slot's entry\n",
                released ? 1 : 0, found );
    }
    check( !released );
    check( found == 1 );
    check( rocketnet_client_slots_num_connected( slots ) == 2 );

    check( rocketnet_client_slots_test_force_hash_index( slots, -1 ) );
    rocketnet_client_slots_destroy( slots );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_release_refuses_an_entry_naming_another_slot );

// mutation gap: hunt 2/3971
//
// The DEV fill seam writes every index cell and nothing past the last.

static void test_gaps_r2_tables_client_slots_fill_seam_writes_inside_the_index()
{
    gaps_r2_tables_install( 0x5A );

    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 3 );
    check( slots );
    rocketnet_client_slots_fill_index_for_test( slots, 0 );
    rocketnet_client_slots_destroy( slots );

    if ( gaps_r2_tables_allocator.overwritten != 0 )
    {
        printf( "fault: the DEV index fill wrote past the last index cell, into the bytes after the index block\n" );
    }
    check( gaps_r2_tables_allocator.overwritten == 0 );
    gaps_r2_tables_uninstall();
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_fill_seam_writes_inside_the_index );

// ---------------------------------------------------------------------------
// the reset witnesses under a failing allocator

// every allocation of a witness fails in turn: the witness reports false
// while one of its creates failed, and returns everything it allocated
static void gaps_r2_tables_witness_sweep( bool ( *witness )(), const char * name, int create_allocations )
{
    gaps_r2_tables_install( 0x00 );

    const int live = gaps_r2_tables_allocator.live;
    gaps_r2_tables_allocator.calls = 0;
    check( witness() );
    const int total = gaps_r2_tables_allocator.calls;
    check( total >= create_allocations );

    for ( int k = 1; k <= total; ++k )
    {
        gaps_r2_tables_allocator.calls = 0;
        gaps_r2_tables_allocator.failed = 0;
        gaps_r2_tables_allocator.fail_at = k;
        const bool clean = witness();
        gaps_r2_tables_allocator.fail_at = 0;

        if ( k <= create_allocations && clean )
        {
            printf( "fault: %s reported clean though its allocation %d failed\n", name, k );
        }
        check( !( k <= create_allocations && clean ) );
        if ( gaps_r2_tables_allocator.live != live )
        {
            printf( "fault: %s leaked %d allocations when its allocation %d failed\n", name, gaps_r2_tables_allocator.live - live, k );
        }
        check( gaps_r2_tables_allocator.live == live );
    }

    gaps_r2_tables_uninstall();
}

static int gaps_r2_tables_create_allocations( void * ( *create )(), void ( *destroy )( void * ) )
{
    gaps_r2_tables_install( 0x00 );
    gaps_r2_tables_allocator.calls = 0;
    void * object = create();
    check( object );
    const int calls = gaps_r2_tables_allocator.calls;
    destroy( object );
    gaps_r2_tables_uninstall();
    return calls;
}

static void * gaps_r2_tables_marks_create() { return rocketnet_server_replay_marks_create( NULL, 1 ); }
static void gaps_r2_tables_marks_destroy( void * p ) { rocketnet_server_replay_marks_destroy( (struct rocketnet_server_replay_marks_t *) p ); }
static void * gaps_r2_tables_slots_create() { return rocketnet_client_slots_create( NULL, 1 ); }
static void gaps_r2_tables_slots_destroy( void * p ) { rocketnet_client_slots_destroy( (struct rocketnet_client_slots_t *) p ); }
static void * gaps_r2_tables_map_create() { return rocketnet_encryption_map_create( NULL, 1 ); }
static void gaps_r2_tables_map_destroy( void * p ) { rocketnet_encryption_map_destroy( (struct rocketnet_encryption_map_t *) p ); }

// mutation gap: hunt 2/2726
// mutation gap: hunt 2/2727
// mutation gap: hunt 2/2728
// mutation gap: hunt 2/2729
// mutation gap: hunt 2/2730
// mutation gap: hunt 2/2731

static void test_gaps_r2_tables_replay_marks_witness_survives_each_failed_allocation()
{
    const int creates = gaps_r2_tables_create_allocations( gaps_r2_tables_marks_create, gaps_r2_tables_marks_destroy ) +
                        gaps_r2_tables_create_allocations( gaps_r2_tables_slots_create, gaps_r2_tables_slots_destroy ) +
                        gaps_r2_tables_create_allocations( gaps_r2_tables_map_create, gaps_r2_tables_map_destroy );
    gaps_r2_tables_witness_sweep( rocketnet_server_replay_marks_reset_witness_for_test, "the replay marks reset witness", creates );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_replay_marks_witness_survives_each_failed_allocation );

// mutation gap: hunt 2/3991

static void test_gaps_r2_tables_client_slots_witness_survives_each_failed_allocation()
{
    const int creates = gaps_r2_tables_create_allocations( gaps_r2_tables_slots_create, gaps_r2_tables_slots_destroy );
    gaps_r2_tables_witness_sweep( rocketnet_client_slots_reset_witness_for_test, "the client slots reset witness", creates );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_witness_survives_each_failed_allocation );

// ---------------------------------------------------------------------------
// the reset inventory's success path

// mutation gap: hunt 2/4202
//
// A matching inventory reports true with an empty failure string, whatever
// the caller's buffer held.

struct gaps_r2_tables_pair_t
{
    uint32_t first;
    uint32_t second;
};

static void test_gaps_r2_tables_reset_inventory_match_clears_the_failure()
{
    static const rocketnet_reset_member_t members[] = { ROCKETNET_RESET_MEMBER( gaps_r2_tables_pair_t, first, -1 ),
                                                        ROCKETNET_RESET_MEMBER( gaps_r2_tables_pair_t, second, -1 ) };
    static const rocketnet_reset_member_t decisions[] = { ROCKETNET_RESET_MEMBER( gaps_r2_tables_pair_t, first, 1 ),
                                                          ROCKETNET_RESET_MEMBER( gaps_r2_tables_pair_t, second, 0 ) };
    char failure[64];
    memset( failure, 'X', sizeof( failure ) );
    const bool matches = rocketnet_reset_inventory_matches( "gaps_r2_tables_pair_t", members, 2, decisions, 2, 0, sizeof( gaps_r2_tables_pair_t ),
                                                            alignof( gaps_r2_tables_pair_t ), failure, sizeof( failure ) );
    check( matches );
    if ( failure[0] != '\0' )
    {
        printf( "fault: a matching reset inventory left the caller's failure buffer as it was, not empty\n" );
    }
    check( failure[0] == '\0' );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_reset_inventory_match_clears_the_failure );

#endif // #if ROCKETNET_DEV
