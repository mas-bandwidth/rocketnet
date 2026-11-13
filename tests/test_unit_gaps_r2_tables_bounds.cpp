/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The refusals and bounds of the token writer, the size plans, rn_slots init,
// the replay marks create, the client slots force seam, the bindings' expiry
// floor and the server's connection reset that no other case reached
// (mutation gaps, round 2, r2-tables). No sockets, no real time and no
// threads: timestamps are arguments from 1000. Each failing check prints the
// fault first.

#include "test_unit_registry.h"
#include "loopback_hub.h"

#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_address.h"
#include "rocketnet_memory.h"
#include "rocketnet_slots.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_server_replay_marks.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_connection_reset.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

static bool gaps_r2_bounds_all( const uint8_t * bytes, size_t count, uint8_t value )
{
    for ( size_t i = 0; i < count; ++i )
    {
        if ( bytes[i] != value )
        {
            return false;
        }
    }
    return true;
}

static struct rocketnet_address_t gaps_r2_bounds_address( const char * text )
{
    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, text ) );
    return address;
}

static void gaps_r2_bounds_token_id( uint8_t * identifier, uint32_t ordinal )
{
    memset( identifier, 0x6B, ROCKETNET_TOKEN_IDENTIFIER_BYTES );
    memcpy( identifier, &ordinal, sizeof( ordinal ) );
}

// ---------------------------------------------------------------------------
// the production writer's address list

struct gaps_r2_bounds_keys_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[RN_TEST_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[RN_TEST_SERVER_SECRET_KEY_BYTES];
};

static void gaps_r2_bounds_keys( gaps_r2_bounds_keys_t * keys )
{
    check( rocketnet_generate_backend_keypair( keys->backend_public, keys->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( keys->server_public, keys->server_secret ) == ROCKETNET_STATUS_OK );
}

static enum rocketnet_status_t gaps_r2_bounds_write( const gaps_r2_bounds_keys_t * keys, const char * const * addresses, int count, uint8_t * token )
{
    return rocketnet_write_production_connect_token( keys->backend_secret, keys->server_public, 0x1234ULL, addresses, count, 7, 1000, 2000, 10,
                                                     NULL, token, ROCKETNET_CONNECT_TOKEN_BYTES );
}

// mutation gap: hunt 2/160
// mutation gap: hunt 2/198
//
// A token names at most two servers, one per family. Three addresses are
// refused as a count before any address is read (the first is not an address,
// so a count let through would be refused as INVALID_ADDRESS instead), and two
// IPv4 addresses are refused, never the second written over the first.

static void test_gaps_r2_tables_production_token_refuses_three_addresses_and_two_ipv4()
{
    gaps_r2_bounds_keys_t keys;
    gaps_r2_bounds_keys( &keys );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    memset( token, 0xA5, sizeof( token ) );
    const char * three[] = { "not an address", "127.0.0.1:40000", "[::1]:40000" };
    const enum rocketnet_status_t three_status = gaps_r2_bounds_write( &keys, three, 3, token );
    if ( three_status != ROCKETNET_STATUS_INVALID_ARGUMENT )
    {
        printf( "fault: a production token listing three addresses got status %d, not INVALID_ARGUMENT: the count was let through to the address loop\n",
                (int) three_status );
    }
    check( three_status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( gaps_r2_bounds_all( token, sizeof( token ), 0xA5 ) );

    const char * two_ipv4[] = { "127.0.0.1:40000", "10.0.0.1:40000" };
    const enum rocketnet_status_t ipv4_status = gaps_r2_bounds_write( &keys, two_ipv4, 2, token );
    if ( ipv4_status != ROCKETNET_STATUS_INVALID_ARGUMENT )
    {
        printf( "fault: a production token listing two IPv4 addresses was not refused (status %d): the second overwrote the first\n", (int) ipv4_status );
    }
    check( ipv4_status == ROCKETNET_STATUS_INVALID_ARGUMENT );
    check( gaps_r2_bounds_all( token, sizeof( token ), 0xA5 ) );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_production_token_refuses_three_addresses_and_two_ipv4 );

// ---------------------------------------------------------------------------
// size plans refuse what create refuses

// mutation gap: hunt 2/647
// mutation gap: hunt 2/648
// mutation gap: hunt 2/2620
// mutation gap: hunt 2/2621
//
// A capacity create refuses has no size: the plans report exactly 0 for it.

static void test_gaps_r2_tables_size_plans_of_refused_capacities_are_zero()
{
    const int refused[3] = { 0, -1, INT_MAX / 2 + 1 };
    for ( int i = 0; i < 3; ++i )
    {
        const uint64_t bytes = rocketnet_token_table_allocation_bytes( refused[i] );
        if ( bytes != 0 )
        {
            printf( "fault: the used-token history's size plan for refused capacity %d is %llu bytes, not 0\n", refused[i], (unsigned long long) bytes );
        }
        check( bytes == 0 );
    }
    check( rocketnet_token_table_allocation_bytes( 1 ) > 0 );

    const int refused_clients[2] = { 0, -1 };
    for ( int i = 0; i < 2; ++i )
    {
        const uint64_t bytes = rocketnet_server_replay_marks_allocation_bytes( refused_clients[i] );
        if ( bytes != 0 )
        {
            printf( "fault: the replay marks' size plan for refused max_clients %d is %llu bytes, not 0\n", refused_clients[i], (unsigned long long) bytes );
        }
        check( bytes == 0 );
    }
    check( rocketnet_server_replay_marks_allocation_bytes( 1 ) > 0 );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_size_plans_of_refused_capacities_are_zero );

// ---------------------------------------------------------------------------
// rn_slots init: a refusal clears the machine, a success clears the storage

// mutation gap: hunt 2/3618
// mutation gap: hunt 2/3625
//
// Slots with no storage (either array missing) are refused as zero slots,
// whatever the machine's bytes held before: no field keeps the caller's bytes
// and neither array is written.

static void test_gaps_r2_tables_slots_init_refusal_clears_the_machine()
{
    struct rn_slots_t slots;
    uint64_t connection_ids[4];
    uint8_t changed[4];

    const char * shapes[3] = { "no storage", "no changed array", "no connection id array" };
    for ( int shape = 0; shape < 3; ++shape )
    {
        memset( &slots, 0xA5, sizeof( slots ) );
        memset( connection_ids, 0xA5, sizeof( connection_ids ) );
        memset( changed, 0xA5, sizeof( changed ) );
        uint64_t * ids = shape == 1 ? connection_ids : NULL;
        uint8_t * flags = shape == 2 ? changed : NULL;

        rn_slots_init( &slots, 4, ids, flags );

        const bool cleared = slots.max_clients == 0 && slots.connection_id == NULL && slots.changed == NULL;
        if ( !cleared )
        {
            printf( "fault: rn_slots_init refused 4 slots with %s but the machine kept bytes it held before (max_clients %d)\n", shapes[shape],
                    slots.max_clients );
        }
        check( cleared );
        const bool untouched = gaps_r2_bounds_all( (const uint8_t *) connection_ids, sizeof( connection_ids ), 0xA5 ) &&
                               gaps_r2_bounds_all( changed, sizeof( changed ), 0xA5 );
        if ( !untouched )
        {
            printf( "fault: rn_slots_init refused 4 slots with %s but wrote the caller's arrays\n", shapes[shape] );
        }
        check( untouched );
    }
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_slots_init_refusal_clears_the_machine );

// mutation gap: hunt 2/3633
// mutation gap: hunt 2/3638
//
// Init leaves every slot free over storage that held anything: one slot over
// dirty storage binds no connection, and every changed flag reads 0.

static void test_gaps_r2_tables_slots_init_frees_dirty_storage()
{
    struct rn_slots_t slots;

    uint64_t one_id[1];
    uint8_t one_changed[1];
    memset( one_id, 0xA5, sizeof( one_id ) );
    memset( one_changed, 0xA5, sizeof( one_changed ) );
    rn_slots_init( &slots, 1, one_id, one_changed );
    const int admitted = rn_slots_admit( &slots, 0, 0xA5A5A5A5A5A5A5A5ULL );
    if ( admitted != RN_SLOTS_DISCARD )
    {
        printf( "fault: a one-slot machine initialised over dirty storage admitted connection 0xA5A5A5A5A5A5A5A5: its slot was not freed\n" );
    }
    check( admitted == RN_SLOTS_DISCARD );
    check( rn_slots_observe( &slots, 0, 0 ) == RN_SLOTS_NONE );

    uint64_t ids[4];
    uint8_t changed[4];
    memset( ids, 0xA5, sizeof( ids ) );
    memset( changed, 0xA5, sizeof( changed ) );
    rn_slots_init( &slots, 4, ids, changed );
    const bool clean = gaps_r2_bounds_all( (const uint8_t *) ids, sizeof( ids ), 0 ) && gaps_r2_bounds_all( changed, sizeof( changed ), 0 );
    if ( !clean )
    {
        printf( "fault: a four-slot machine initialised over dirty storage kept the caller's bytes in its connection ids or changed flags\n" );
    }
    check( clean );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_slots_init_frees_dirty_storage );

// ---------------------------------------------------------------------------
// the claim table: an expired claim holds no address

// mutation gap: hunt 2/942
//
// A claim is free the moment the clock reaches its deadline: after it,
// another address is admitted.

static void test_gaps_r2_tables_token_expired_claim_admits_another_address()
{
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, 4 );
    check( history );

    const struct rocketnet_address_t a = gaps_r2_bounds_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t b = gaps_r2_bounds_address( "198.51.100.8:50000" );
    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    gaps_r2_bounds_token_id( id, 1 );

    check( rocketnet_used_token_history_claim_record( history, id, &a, 1005.0, 1000.0 ) == 1 );
    check( rocketnet_used_token_history_claim_check( history, id, &b, 1001.0 ) == 0 );
    const bool admitted = rocketnet_used_token_history_claim_check( history, id, &b, 1006.0 ) == 1;
    if ( !admitted )
    {
        printf( "fault: a claim past its deadline still refused another address: the expired claim was read as live\n" );
    }
    check( admitted );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_token_expired_claim_admits_another_address );

// ---------------------------------------------------------------------------
// the server's connection reset at the bound

// mutation gap: hunt 2/4161
//
// A reset names a slot inside the table or does nothing: index max_clients
// writes neither the connection id nor the end reason one past each array.

static void test_gaps_r2_tables_server_connection_reset_refuses_max_clients()
{
    const int max_clients = 2;
    static struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );
    server.client_slots = rocketnet_client_slots_create( NULL, max_clients );
    check( server.client_slots );

    uint64_t connection_ids[max_clients + 1] = { 11, 12, 0x5E5E5E5E5E5E5E5EULL };
    int end_reasons[max_clients + 1] = { 0, 0, 0x5E5E };
    server.connection_ids = connection_ids;
    server.end_reasons = end_reasons;

    rocketnet_server_connection_reset( &server, max_clients, ROCKETNET_DISCONNECT_REASON_SERVER_DISCONNECTED );

    const bool untouched = connection_ids[max_clients] == 0x5E5E5E5E5E5E5E5EULL && end_reasons[max_clients] == 0x5E5E;
    if ( !untouched )
    {
        printf( "fault: a connection reset of slot max_clients wrote one past the connection id and end reason arrays\n" );
    }
    check( untouched );
    check( connection_ids[0] == 11 && connection_ids[1] == 12 && end_reasons[0] == 0 && end_reasons[1] == 0 );

    rocketnet_client_slots_destroy( server.client_slots );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_server_connection_reset_refuses_max_clients );

#if ROCKETNET_DEV

extern "C" void rocketnet_dev_token_table_reset_operation_counts( struct rocketnet_used_token_history_t * history );
extern "C" uint64_t rocketnet_dev_token_binding_slot_steps( const struct rocketnet_used_token_history_t * history );

// ---------------------------------------------------------------------------
// the bindings' expiry floor after a scan that finds nothing

// mutation gap: hunt 2/868
// mutation gap: hunt 2/869
//
// A full table of live bindings whose floor the clock has passed (the binding
// that set it was evicted while live) scans once, finds nothing expired, and
// raises the floor to the earliest live expiry: the next binding reads a slot
// or two, not every slot again.

static void test_gaps_r2_tables_token_binding_empty_scan_raises_the_floor()
{
    const int capacity = 8;
    struct rocketnet_used_token_history_t * history = rocketnet_used_token_history_create( NULL, capacity );
    check( history );

    const struct rocketnet_address_t holder = gaps_r2_bounds_address( "198.51.100.7:50000" );
    uint8_t id[ROCKETNET_TOKEN_IDENTIFIER_BYTES];
    for ( int i = 0; i < capacity; ++i )
    {
        gaps_r2_bounds_token_id( id, (uint32_t) i );
        rocketnet_used_token_history_record( history, id, &holder, i == 0 ? 1005 : 2000, 1000 );
    }

    // the oldest (expiry 1005, the floor) is evicted live at 1001
    gaps_r2_bounds_token_id( id, 100 );
    rocketnet_used_token_history_record( history, id, &holder, 2000, 1001 );

    // at 1006 the clock has passed the floor: one scan, nothing expired
    gaps_r2_bounds_token_id( id, 101 );
    rocketnet_used_token_history_record( history, id, &holder, 2000, 1006 );

    gaps_r2_bounds_token_id( id, 102 );
    rocketnet_dev_token_table_reset_operation_counts( history );
    rocketnet_used_token_history_record( history, id, &holder, 2000, 1007 );
    const uint64_t steps = rocketnet_dev_token_binding_slot_steps( history );
    if ( steps > 2 )
    {
        printf( "fault: a binding after a scan that found nothing expired read %llu slots: the scan left the floor below the clock\n",
                (unsigned long long) steps );
    }
    check( steps <= 2 );

    rocketnet_used_token_history_destroy( history );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_token_binding_empty_scan_raises_the_floor );

// mutation gap: hunt 2/227
//
// A production token holds the crypto source while it seals and lets it go:
// the holder count after the write is the count before it.

static void test_gaps_r2_tables_production_token_releases_the_crypto_source()
{
    gaps_r2_bounds_keys_t keys;
    gaps_r2_bounds_keys( &keys );
    const char * addresses[] = { "127.0.0.1:40000" };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];

    const int before = rocketnet_crypto_holders_for_test();
    check( gaps_r2_bounds_write( &keys, addresses, 1, token ) == ROCKETNET_STATUS_OK );
    const int after = rocketnet_crypto_holders_for_test();
    if ( after != before )
    {
        printf( "fault: minting a production token changed the crypto holder count from %d to %d: its hold was never released\n", before, after );
    }
    check( after == before );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_production_token_releases_the_crypto_source );

// ---------------------------------------------------------------------------
// the replay marks create allocates nothing for a refused count

static int gaps_r2_bounds_calls = 0;
static int gaps_r2_bounds_live = 0;

static void * gaps_r2_bounds_malloc( void * context, size_t bytes )
{
    (void) context;
    ++gaps_r2_bounds_calls;
    void * p = malloc( bytes );
    if ( p )
    {
        ++gaps_r2_bounds_live;
    }
    return p;
}

static void gaps_r2_bounds_free( void * context, void * p )
{
    (void) context;
    --gaps_r2_bounds_live;
    free( p );
}

// mutation gap: hunt 2/2624
// mutation gap: hunt 2/2626
//
// Zero and negative client counts are refused before any allocation: create
// returns NULL and the allocator is never called.

static void test_gaps_r2_tables_replay_marks_create_refuses_no_clients()
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    int status = -1;
    check( rocketnet_init_internal( NULL, gaps_r2_bounds_malloc, gaps_r2_bounds_free, &status ) );
    check( status == ROCKETNET_STATUS_OK );

    const int refused[2] = { 0, -1 };
    for ( int i = 0; i < 2; ++i )
    {
        gaps_r2_bounds_calls = 0;
        struct rocketnet_server_replay_marks_t * marks = rocketnet_server_replay_marks_create( NULL, refused[i] );
        const int calls = gaps_r2_bounds_calls;
        if ( marks || calls != 0 )
        {
            printf( "fault: replay marks create for max_clients %d returned %s after %d allocator calls: the count was not refused first\n", refused[i],
                    marks ? "marks" : "NULL", calls );
        }
        check( marks == NULL );
        check( calls == 0 );
        if ( marks )
        {
            rocketnet_server_replay_marks_destroy( marks );
        }
    }

    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( gaps_r2_bounds_live == 0 );
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_replay_marks_create_refuses_no_clients );

// ---------------------------------------------------------------------------
// the client slots force seam at its bound

// mutation gap: hunt 2/3965
// mutation gap: hunt 2/3969
//
// The force seam takes -1 (no force) and the buckets 0 .. num_buckets - 1;
// num_buckets and -2 are refused, so no probe starts outside the index.

static void test_gaps_r2_tables_client_slots_force_refuses_outside_the_index()
{
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 4 );
    check( slots );
    const int buckets = 8;

    check( rocketnet_client_slots_test_force_hash_index( slots, buckets - 1 ) );
    const bool past = rocketnet_client_slots_test_force_hash_index( slots, buckets );
    if ( past )
    {
        printf( "fault: the force seam took bucket %d of an index of %d buckets: a probe would start past the last cell\n", buckets, buckets );
    }
    check( !past );
    const bool below = rocketnet_client_slots_test_force_hash_index( slots, -2 );
    if ( below )
    {
        printf( "fault: the force seam took bucket -2\n" );
    }
    check( !below );

    check( rocketnet_client_slots_test_force_hash_index( slots, -1 ) );
    rocketnet_client_slots_destroy( slots );
}

REGISTER_UNIT_TEST( test_gaps_r2_tables_client_slots_force_refuses_outside_the_index );

#endif // #if ROCKETNET_DEV
