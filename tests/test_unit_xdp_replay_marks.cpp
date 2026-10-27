/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The kernel replay map's key and value layout and the userspace
// pending-marks buffer. The bytes here are written by hand, not taken from
// the conversion code, so a mis-layout in either direction shows up. No
// sockets, no kernel, no real time.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_xdp_limits.h"
#include "../sdk/rocketnet_xdp_replay_marks.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_server_replay_marks.h"

// the AF_XDP code is Linux only, so its tests are built there only
#if defined( __linux__ )

static struct rocketnet_address_t replay_marks_ipv4( uint16_t port )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ROCKETNET_ADDRESS_IPV4;
    a.data.ipv4[0] = 198;
    a.data.ipv4[1] = 51;
    a.data.ipv4[2] = 100;
    a.data.ipv4[3] = 7;
    a.port = port;
    return a;
}

static struct rocketnet_address_t replay_marks_ipv6( uint16_t port )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ROCKETNET_ADDRESS_IPV6;
    a.data.ipv6[0] = 0x2001;
    a.data.ipv6[1] = 0x0db8;
    a.data.ipv6[7] = 0x0007;
    a.port = port;
    return a;
}

static void test_xdp_transport_replay_key_matches_golden_bytes()
{
    // 198.51.100.7:50000 -> family 4, port 0xc3 0x50, IPv4 in the first 4
    // address bytes, twelve zero bytes after

    const uint8_t golden_v4[20] = { 4, 0, 0xc3, 0x50, 198, 51, 100, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    struct rocketnet_address_t a4 = replay_marks_ipv4( 50000 );
    struct rocketnet_server_xdp_replay_key_t key4;
    rocketnet_xdp_replay_key_from_address( &a4, &key4 );
    rocketnet_unit_check( sizeof( key4 ) == 20 );
    rocketnet_unit_check( memcmp( &key4, golden_v4, 20 ) == 0 );

    // [2001:db8::7]:50000 -> family 6, the 16 address bytes
    // 0x20 0x01 0x0d 0xb8 then eleven zeros then 0x07

    const uint8_t golden_v6[20] = { 6, 0, 0xc3, 0x50, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x07 };

    struct rocketnet_address_t a6 = replay_marks_ipv6( 50000 );
    struct rocketnet_server_xdp_replay_key_t key6;
    rocketnet_xdp_replay_key_from_address( &a6, &key6 );
    rocketnet_unit_check( sizeof( key6 ) == 20 );
    rocketnet_unit_check( memcmp( &key6, golden_v6, 20 ) == 0 );

    // two addresses differing only in port give different keys

    struct rocketnet_address_t a4b = replay_marks_ipv4( 50001 );
    struct rocketnet_server_xdp_replay_key_t key4b;
    rocketnet_xdp_replay_key_from_address( &a4b, &key4b );
    rocketnet_unit_check( memcmp( &key4, &key4b, 20 ) != 0 );
}

static void test_xdp_transport_replay_marks_commit_forgets_before_marks()
{
    struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    struct rocketnet_address_t a = replay_marks_ipv4( 50000 );
    struct rocketnet_address_t b = replay_marks_ipv4( 50001 );

    rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &a ) );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, 41 ) );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &b, 7 ) );

    uint32_t forget_count = 0;
    uint32_t mark_count = 0;
    const struct rocketnet_server_xdp_replay_key_t * forget_keys = NULL;
    const struct rocketnet_server_xdp_replay_key_t * mark_keys = NULL;
    const struct rocketnet_server_xdp_replay_value_t * mark_values = NULL;

    rocketnet_xdp_replay_marks_forgets( &marks, &forget_keys, &forget_count );
    rocketnet_unit_check( forget_count == 1 );

    // the one forget is A, in insertion order

    struct rocketnet_server_xdp_replay_key_t key_a;
    struct rocketnet_server_xdp_replay_key_t key_b;
    rocketnet_xdp_replay_key_from_address( &a, &key_a );
    rocketnet_xdp_replay_key_from_address( &b, &key_b );

    rocketnet_unit_check( memcmp( &forget_keys[0], &key_a, 20 ) == 0 );

    // the two marks are A at 41 and B at 7, in insertion order

    rocketnet_xdp_replay_marks_marks( &marks, &mark_keys, &mark_values, &mark_count );
    rocketnet_unit_check( mark_count == 2 );
    rocketnet_unit_check( memcmp( &mark_keys[0], &key_a, 20 ) == 0 );
    rocketnet_unit_check( mark_values[0].high_water == 41 );
    rocketnet_unit_check( memcmp( &mark_keys[1], &key_b, 20 ) == 0 );
    rocketnet_unit_check( mark_values[1].high_water == 7 );

    // reset empties both lists

    rocketnet_xdp_replay_marks_reset( &marks );
    rocketnet_xdp_replay_marks_forgets( &marks, &forget_keys, &forget_count );
    rocketnet_unit_check( forget_count == 0 );

    rocketnet_xdp_replay_marks_marks( &marks, &mark_keys, &mark_values, &mark_count );
    rocketnet_unit_check( mark_count == 0 );
}

static void test_xdp_transport_replay_marks_full_refuses_and_counts()
{
    struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    for ( uint32_t i = 0; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( (uint16_t) ( 2000 + i ) );
        rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, i ) );
    }

    struct rocketnet_address_t extra = replay_marks_ipv4( 60000 );
    rocketnet_unit_check( !rocketnet_xdp_replay_marks_mark( &marks, &extra, 100 ) );
    rocketnet_unit_check( marks.refused == 1 );

    rocketnet_xdp_replay_marks_reset( &marks );

    for ( uint32_t i = 0; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( (uint16_t) ( 2000 + i ) );
        rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &a ) );
    }

    rocketnet_unit_check( !rocketnet_xdp_replay_marks_forget( &marks, &extra ) );
    rocketnet_unit_check( marks.refused == 1 );
}

static void test_xdp_transport_replay_marks_failed_forgets_kept()
{
    // tla/RNXdpReplay.tla ForgetOwedUntilApplied: a forget the kernel refused
    // stays pending, in order, ahead of the next walk's forgets

    struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    const uint16_t ports[6] = { 50000, 50001, 50002, 50003, 50004, 50005 };

    for ( int i = 0; i < 6; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( ports[i] );
        rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &a ) );
        rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, (uint32_t) ( 10 + i ) ) );
    }

    // the kernel refused the marks too, so none supersedes a refused forget

    const bool forget_failed[6] = { false, true, false, true, true, false };
    const bool mark_applied[6] = { false, false, false, false, false, false };
    rocketnet_unit_check( rocketnet_xdp_replay_marks_reset_keeping( &marks, forget_failed, mark_applied ) == 0 );

    const struct rocketnet_server_xdp_replay_key_t * forget_keys = NULL;
    uint32_t forget_count = 0;
    rocketnet_xdp_replay_marks_forgets( &marks, &forget_keys, &forget_count );
    rocketnet_unit_check( forget_count == 3 );

    const uint16_t kept_ports[3] = { 50001, 50003, 50004 };

    for ( uint32_t i = 0; i < 3; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( kept_ports[i] );
        struct rocketnet_server_xdp_replay_key_t key;
        rocketnet_xdp_replay_key_from_address( &a, &key );
        rocketnet_unit_check( memcmp( &forget_keys[i], &key, 20 ) == 0 );
    }

    const struct rocketnet_server_xdp_replay_key_t * mark_keys = NULL;
    const struct rocketnet_server_xdp_replay_value_t * mark_values = NULL;
    uint32_t mark_count = 0;
    rocketnet_xdp_replay_marks_marks( &marks, &mark_keys, &mark_values, &mark_count );

    // tla/RNXdpReplay.tla MarkOwedUntilApplied: the six refused marks are kept
    // too, in order, with their values

    rocketnet_unit_check( mark_count == 6 );

    for ( uint32_t i = 0; i < 6; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( ports[i] );
        struct rocketnet_server_xdp_replay_key_t key;
        rocketnet_xdp_replay_key_from_address( &a, &key );
        rocketnet_unit_check( memcmp( &mark_keys[i], &key, 20 ) == 0 );
        rocketnet_unit_check( mark_values[i].high_water == 10 + i );
    }

    // the next walk's forget follows the kept ones

    struct rocketnet_address_t next = replay_marks_ipv4( 50009 );
    struct rocketnet_server_xdp_replay_key_t next_key;
    rocketnet_xdp_replay_key_from_address( &next, &next_key );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &next ) );

    rocketnet_xdp_replay_marks_forgets( &marks, &forget_keys, &forget_count );
    rocketnet_unit_check( forget_count == 4 );
    rocketnet_unit_check( memcmp( &forget_keys[3], &next_key, 20 ) == 0 );
}

static void test_xdp_transport_replay_marks_refusals_reach_the_socket()
{
    // a full buffer's refusals are handed to the commit, which adds them to
    // the socket's commit failure count, rather than zeroed by the reset

    struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    for ( uint32_t i = 0; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
    {
        struct rocketnet_address_t a = replay_marks_ipv4( (uint16_t) ( 2000 + i ) );
        rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, i + 1 ) );
    }

    struct rocketnet_address_t extra = replay_marks_ipv4( 60000 );
    rocketnet_unit_check( !rocketnet_xdp_replay_marks_mark( &marks, &extra, 100 ) );
    rocketnet_unit_check( !rocketnet_xdp_replay_marks_mark( &marks, &extra, 101 ) );
    rocketnet_unit_check( marks.refused == 2 );

    // the kernel took every queued mark: only the buffer's refusals count

    static const bool no_failures[ROCKETNET_XDP_REPLAY_MAX_ENTRIES] = {};
    static bool marks_applied[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    for ( uint32_t i = 0; i < ROCKETNET_XDP_REPLAY_MAX_ENTRIES; i++ )
        marks_applied[i] = true;
    rocketnet_unit_check( rocketnet_xdp_replay_marks_reset_keeping( &marks, no_failures, marks_applied ) == 2 );
    rocketnet_unit_check( marks.refused == 0 );
    rocketnet_unit_check( marks.mark_count == 0 );
    rocketnet_unit_check( marks.forget_count == 0 );

    // the next commit with no refusal hands over none

    rocketnet_unit_check( rocketnet_xdp_replay_marks_reset_keeping( &marks, no_failures, marks_applied ) == 0 );
}

// A fake kernel replay map for the integrated case: a few entries, and a
// count of the next deletes and of the next updates it refuses. No kernel, no sockets, no real time.

#define REPLAY_FAKE_MAP_ENTRIES 4

struct replay_fake_map_t
{
    struct rocketnet_server_xdp_replay_key_t keys[REPLAY_FAKE_MAP_ENTRIES];
    uint32_t high_water[REPLAY_FAKE_MAP_ENTRIES];
    bool present[REPLAY_FAKE_MAP_ENTRIES];
    int delete_refusals;
    int update_refusals;
};

static int replay_fake_map_find( const struct replay_fake_map_t * map, const struct rocketnet_server_xdp_replay_key_t * key )
{
    for ( int i = 0; i < REPLAY_FAKE_MAP_ENTRIES; i++ )
    {
        if ( map->present[i] && memcmp( &map->keys[i], key, sizeof( *key ) ) == 0 )
            return i;
    }

    return -1;
}

static bool replay_fake_map_delete( struct replay_fake_map_t * map, const struct rocketnet_server_xdp_replay_key_t * key )
{
    if ( map->delete_refusals > 0 )
    {
        map->delete_refusals--;
        return false;
    }

    const int i = replay_fake_map_find( map, key );
    if ( i >= 0 )
    {
        map->present[i] = false;
    }

    return true;
}

static bool replay_fake_map_update( struct replay_fake_map_t * map, const struct rocketnet_server_xdp_replay_key_t * key, uint32_t high_water )
{
    if ( map->update_refusals > 0 )
    {
        map->update_refusals--;
        return false;
    }

    int i = replay_fake_map_find( map, key );
    for ( int j = 0; i < 0 && j < REPLAY_FAKE_MAP_ENTRIES; j++ )
    {
        if ( !map->present[j] )
        {
            i = j;
        }
    }

    if ( i < 0 )
        return false;
    map->keys[i] = *key;
    map->high_water[i] = high_water;
    map->present[i] = true;

    return true;
}

// The transport's commit against the fake map, in the order of
// rocketnet_server_socket_replay_commit: every forget, then every mark, then
// the reset keeping the refused forgets no applied mark superseded.

static void replay_fake_commit( struct rocketnet_xdp_replay_marks_t * marks, struct replay_fake_map_t * map )
{
    static bool forget_failed[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    static bool mark_applied[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    for ( uint32_t i = 0; i < marks->forget_count; i++ )
        forget_failed[i] = !replay_fake_map_delete( map, &marks->forgets[i] );
    for ( uint32_t i = 0; i < marks->mark_count; i++ )
        mark_applied[i] = replay_fake_map_update( map, &marks->mark_keys[i], marks->mark_values[i].high_water );
    rocketnet_xdp_replay_marks_reset_keeping( marks, forget_failed, mark_applied );
}

static bool replay_fake_sink_forget( void * context, const struct rocketnet_address_t * address )
{
    return rocketnet_xdp_replay_marks_forget( (struct rocketnet_xdp_replay_marks_t *) context, address );
}

static bool replay_fake_sink_mark( void * context, const struct rocketnet_address_t * address, uint32_t high_water )
{
    return rocketnet_xdp_replay_marks_mark( (struct rocketnet_xdp_replay_marks_t *) context, address, high_water );
}

static void test_xdp_transport_replay_marks_stale_forget_never_deletes_a_newer_mark()
{
    // tla/RNXdpReplay.tla RetryNeverDeletesNewerMark, walk to fake kernel
    // map: an old session's forget the kernel refuses, a new session from the
    // same address whose mark lands while the old forget is refused again,
    // then a commit whose deletes succeed. The new session's entry survives.

    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 4 );
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 2 );
    struct rocketnet_server_replay_marks_t * walk = rocketnet_server_replay_marks_create( NULL, 2 );
    rocketnet_unit_check( map && slots && walk );

    static struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    struct replay_fake_map_t kernel;
    memset( &kernel, 0, sizeof( kernel ) );

    struct rocketnet_server_replay_sink_t sink;
    sink.context = &marks;
    sink.forget = replay_fake_sink_forget;
    sink.mark = replay_fake_sink_mark;

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );

    const struct rocketnet_address_t a = replay_marks_ipv4( 50000 );
    struct rocketnet_server_xdp_replay_key_t key_a;
    rocketnet_xdp_replay_key_from_address( &a, &key_a );

    // the old session at A marks 40 and the kernel takes it

    const int old_mapping = rocketnet_encryption_map_add( map, &a, 1005.0 );
    rocketnet_unit_check( old_mapping >= 0 );

    const int old_slot = rocketnet_client_slots_assign( slots, 100, old_mapping,
                                                        rocketnet_encryption_map_owner_generation( map, old_mapping ), user_data );
    rocketnet_unit_check( old_slot >= 0 );

    rocketnet_encryption_map_entry( map, old_mapping )->replay_session.highest_opened = 40;

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( replay_fake_map_find( &kernel, &key_a ) >= 0 );

    // the old session leaves; its forget fails (one)

    rocketnet_client_slots_release( slots, old_slot );
    rocketnet_encryption_map_remove_current( map, old_mapping );
    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );

    rocketnet_unit_check( marks.forget_count == 1 );

    kernel.delete_refusals = 1;
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( marks.forget_count == 1 );

    // a new session from A opens 3; the held forget fails again (two) while
    // the new session's mark lands, so the stale forget is superseded

    const int new_mapping = rocketnet_encryption_map_add( map, &a, 1005.0 );
    rocketnet_unit_check( new_mapping >= 0 );

    const int new_slot = rocketnet_client_slots_assign( slots, 200, new_mapping,
                                                        rocketnet_encryption_map_owner_generation( map, new_mapping ), user_data );
    rocketnet_unit_check( new_slot >= 0 );

    rocketnet_encryption_map_entry( map, new_mapping )->replay_session.highest_opened = 3;

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );

    rocketnet_unit_check( marks.mark_count == 1 );

    kernel.delete_refusals = 1;
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( marks.forget_count == 0 );

    // nothing changed, the kernel now accepts every delete (success): the new
    // session's entry is still there, at its own high-water

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );

    rocketnet_unit_check( marks.forget_count == 0 && marks.mark_count == 0 );

    replay_fake_commit( &marks, &kernel );
    const int entry = replay_fake_map_find( &kernel, &key_a );
    rocketnet_unit_check( entry >= 0 );
    rocketnet_unit_check( kernel.high_water[entry] == 3 );

    // a refused forget whose same-key mark the kernel also refused is kept

    rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &a ) );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, 4 ) );

    const bool forget_failed[1] = { true };
    const bool mark_refused[1] = { false };
    rocketnet_xdp_replay_marks_reset_keeping( &marks, forget_failed, mark_refused );
    rocketnet_unit_check( marks.forget_count == 1 );

    rocketnet_server_replay_marks_destroy( walk );
    rocketnet_client_slots_destroy( slots );
    rocketnet_encryption_map_destroy( map );
}

// The kernel rule of server_xdp/server_xdp.c against the fake map: a session
// packet from a source with an entry is dropped when its sequence is
// ROCKETNET_XDP_REPLAY_WINDOW or more behind the entry's high-water.

static bool replay_fake_kernel_admits( const struct replay_fake_map_t * map, const struct rocketnet_server_xdp_replay_key_t * key, uint32_t sequence )
{
    const int i = replay_fake_map_find( map, key );
    if ( i < 0 )
        return true;
    return (int32_t) ( map->high_water[i] - sequence ) < (int32_t) ROCKETNET_XDP_REPLAY_WINDOW;
}

static void test_xdp_transport_replay_marks_reconnect_admits_the_new_session()
{
    // tla/RNXdpReplay.tla NewSessionNeverDroppedByOldMark, walk to fake
    // kernel map: an old session's mark far past the window, then a new
    // session from the same address while the old delete is still owed --
    // (1) queued and refused by the kernel, the new session in another slot;
    // (2) never queued, the new session taking the same slot before any walk.
    // The commit before the new session's first keep-alive leaves admits its
    // sequence 1 in the kernel.

    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 8 );
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 4 );
    struct rocketnet_server_replay_marks_t * walk = rocketnet_server_replay_marks_create( NULL, 4 );
    rocketnet_unit_check( map && slots && walk );

    static struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    struct replay_fake_map_t kernel;
    memset( &kernel, 0, sizeof( kernel ) );

    struct rocketnet_server_replay_sink_t sink;
    sink.context = &marks;
    sink.forget = replay_fake_sink_forget;
    sink.mark = replay_fake_sink_mark;

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );

    const uint32_t old_high_water = ROCKETNET_XDP_REPLAY_WINDOW + 1000;

    // (1) the old session at A marks far past the window, then leaves; its
    // delete is queued and the kernel refuses it

    const struct rocketnet_address_t a = replay_marks_ipv4( 50000 );
    struct rocketnet_server_xdp_replay_key_t key_a;
    rocketnet_xdp_replay_key_from_address( &a, &key_a );

    const int old_mapping_a = rocketnet_encryption_map_add( map, &a, 1005.0 );
    rocketnet_unit_check( old_mapping_a >= 0 );

    const int old_slot_a = rocketnet_client_slots_assign( slots, 100, old_mapping_a,
                                                          rocketnet_encryption_map_owner_generation( map, old_mapping_a ), user_data );
    rocketnet_unit_check( old_slot_a >= 0 );

    rocketnet_encryption_map_entry( map, old_mapping_a )->replay_session.highest_opened = old_high_water;

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( !replay_fake_kernel_admits( &kernel, &key_a, 1 ) );

    rocketnet_client_slots_release( slots, old_slot_a );
    rocketnet_encryption_map_remove_current( map, old_mapping_a );
    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );

    rocketnet_unit_check( marks.forget_count == 1 );

    kernel.delete_refusals = 1;
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( marks.forget_count == 1 );

    // a new session from A, in another slot, before the held delete lands;
    // the kernel refuses the delete again in the commit of its first update

    const int other_slot = rocketnet_client_slots_assign( slots, 300, -1, 300, user_data );
    rocketnet_unit_check( other_slot >= 0 );

    const int new_mapping_a = rocketnet_encryption_map_add( map, &a, 1005.0 );
    rocketnet_unit_check( new_mapping_a >= 0 );

    const int new_slot_a = rocketnet_client_slots_assign( slots, 200, new_mapping_a,
                                                          rocketnet_encryption_map_owner_generation( map, new_mapping_a ), user_data );
    rocketnet_unit_check( new_slot_a >= 0 && new_slot_a != old_slot_a );

    rocketnet_client_slots_release( slots, other_slot );

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    kernel.delete_refusals = 1;
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( replay_fake_kernel_admits( &kernel, &key_a, 1 ) );

    // (2) the old session at B marks far past the window, then leaves and a
    // new session from B takes the same slot before any walk queues the delete

    const struct rocketnet_address_t b = replay_marks_ipv4( 50001 );
    struct rocketnet_server_xdp_replay_key_t key_b;
    rocketnet_xdp_replay_key_from_address( &b, &key_b );

    const int old_mapping_b = rocketnet_encryption_map_add( map, &b, 1005.0 );
    rocketnet_unit_check( old_mapping_b >= 0 );

    const int old_slot_b = rocketnet_client_slots_assign( slots, 400, old_mapping_b,
                                                          rocketnet_encryption_map_owner_generation( map, old_mapping_b ), user_data );
    rocketnet_unit_check( old_slot_b >= 0 );

    rocketnet_encryption_map_entry( map, old_mapping_b )->replay_session.highest_opened = old_high_water;

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( !replay_fake_kernel_admits( &kernel, &key_b, 1 ) );

    rocketnet_client_slots_release( slots, old_slot_b );
    rocketnet_encryption_map_remove_current( map, old_mapping_b );
    const int new_mapping_b = rocketnet_encryption_map_add( map, &b, 1005.0 );
    rocketnet_unit_check( new_mapping_b >= 0 );

    const int new_slot_b = rocketnet_client_slots_assign( slots, 500, new_mapping_b,
                                                          rocketnet_encryption_map_owner_generation( map, new_mapping_b ), user_data );
    rocketnet_unit_check( new_slot_b == old_slot_b );

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( replay_fake_kernel_admits( &kernel, &key_b, 1 ) );

    // both new sessions' own marks then land as they open

    rocketnet_encryption_map_entry( map, new_mapping_a )->replay_session.highest_opened = 1;
    rocketnet_encryption_map_entry( map, new_mapping_b )->replay_session.highest_opened = 1;
    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( replay_fake_kernel_admits( &kernel, &key_a, 2 ) );
    rocketnet_unit_check( replay_fake_kernel_admits( &kernel, &key_b, 2 ) );
    rocketnet_unit_check( marks.forget_count == 0 );

    rocketnet_server_replay_marks_destroy( walk );
    rocketnet_client_slots_destroy( slots );
    rocketnet_encryption_map_destroy( map );
}

static void test_xdp_transport_replay_marks_refused_reconnect_mark_is_retried()
{
    // tla/RNXdpReplay.tla MarkOwedUntilApplied, HoldGreetingUntilMarked and
    // NewSessionNeverDroppedByOldMark, walk to fake kernel map: a new session
    // takes the old session's slot before any walk and the kernel refuses its
    // zero mark once. Its greeting is held while the old entry stands, so it
    // sends nothing; the next commit, with nothing new from the walk (its
    // cache has already advanced), retries the kept mark, the greeting is
    // released, and the new session's sequence 1, its first, is admitted.

    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 4 );
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 2 );
    struct rocketnet_server_replay_marks_t * walk = rocketnet_server_replay_marks_create( NULL, 2 );
    rocketnet_unit_check( map && slots && walk );

    static struct rocketnet_xdp_replay_marks_t marks;
    rocketnet_xdp_replay_marks_reset( &marks );

    struct replay_fake_map_t kernel;
    memset( &kernel, 0, sizeof( kernel ) );

    struct rocketnet_server_replay_sink_t sink;
    sink.context = &marks;
    sink.forget = replay_fake_sink_forget;
    sink.mark = replay_fake_sink_mark;

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );

    const struct rocketnet_address_t a = replay_marks_ipv4( 50000 );
    struct rocketnet_server_xdp_replay_key_t key_a;
    rocketnet_xdp_replay_key_from_address( &a, &key_a );

    // the old session at A marks far past the window

    const int old_mapping = rocketnet_encryption_map_add( map, &a, 1005.0 );
    rocketnet_unit_check( old_mapping >= 0 );

    const int old_slot = rocketnet_client_slots_assign( slots, 100, old_mapping,
                                                        rocketnet_encryption_map_owner_generation( map, old_mapping ), user_data );
    rocketnet_unit_check( old_slot >= 0 );

    rocketnet_encryption_map_entry( map, old_mapping )->replay_session.highest_opened = ROCKETNET_XDP_REPLAY_WINDOW + 1000;

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( !replay_fake_kernel_admits( &kernel, &key_a, 1 ) );

    // a new session from A takes the same slot before any walk; the kernel
    // refuses its zero mark, so the old entry still drops sequence 1

    rocketnet_client_slots_release( slots, old_slot );
    rocketnet_encryption_map_remove_current( map, old_mapping );
    const int new_mapping = rocketnet_encryption_map_add( map, &a, 1005.0 );
    rocketnet_unit_check( new_mapping >= 0 );

    const int new_slot = rocketnet_client_slots_assign( slots, 200, new_mapping,
                                                        rocketnet_encryption_map_owner_generation( map, new_mapping ), user_data );
    rocketnet_unit_check( new_slot == old_slot );

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );

    rocketnet_unit_check( marks.mark_count == 1 );

    kernel.update_refusals = 1;
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( marks.mark_count == 1 );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_holding_greetings( &marks ) );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_holds_greeting( &marks, &a ) );

    // the next update: the walk takes nothing new, the kept mark is retried
    // and lands, the greeting leaves, and the client's sequence 1 is admitted

    rocketnet_server_replay_marks_walk( walk, slots, map, &sink );
    replay_fake_commit( &marks, &kernel );
    rocketnet_unit_check( marks.mark_count == 0 );
    rocketnet_unit_check( !rocketnet_xdp_replay_marks_holds_greeting( &marks, &a ) );
    rocketnet_unit_check( replay_fake_kernel_admits( &kernel, &key_a, 1 ) );

    // a newer mark of a key replaces its kept mark in place, and a queued
    // forget of a key drops its kept mark

    rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, 5 ) );

    static bool forget_none[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    static bool mark_refused_all[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
    rocketnet_xdp_replay_marks_reset_keeping( &marks, forget_none, mark_refused_all );
    rocketnet_unit_check( marks.mark_count == 1 );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_mark( &marks, &a, 6 ) );
    rocketnet_unit_check( marks.mark_count == 1 && marks.mark_values[0].high_water == 6 );
    rocketnet_unit_check( rocketnet_xdp_replay_marks_forget( &marks, &a ) );
    rocketnet_unit_check( marks.mark_count == 0 && marks.forget_count == 1 );

    rocketnet_server_replay_marks_destroy( walk );
    rocketnet_client_slots_destroy( slots );
    rocketnet_encryption_map_destroy( map );
}

REGISTER_UNIT_TEST( test_xdp_transport_replay_key_matches_golden_bytes );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_commit_forgets_before_marks );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_full_refuses_and_counts );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_failed_forgets_kept );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_refusals_reach_the_socket );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_stale_forget_never_deletes_a_newer_mark );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_reconnect_admits_the_new_session );
REGISTER_UNIT_TEST( test_xdp_transport_replay_marks_refused_reconnect_mark_is_retried );

#endif // #if defined(__linux__)
