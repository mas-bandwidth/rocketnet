/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server replay walk (sdk/rocketnet_server_replay_marks.h) against
// tla/RNXdpReplay.tla Walk: the forgets first, then the marks, once per
// update. The client slots and encryption map are built directly, time is an
// argument, and there is no transport, transport or clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_server_replay_marks.h"

#include <stdint.h>
#include <string.h>

#define ROCKETNET_REPLAY_MARKS_TEST_HANDSHAKE_DEADLINE 1005.0
#define ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS 16

struct server_replay_marks_fixture_t
{
    struct rocketnet_encryption_map_t * map;
    struct rocketnet_client_slots_t * slots;
    struct rocketnet_server_replay_marks_t * marks;
};

struct server_replay_marks_client_t
{
    int slot;
    int mapping;
};

enum
{
    SERVER_REPLAY_MARKS_FORGET,
    SERVER_REPLAY_MARKS_MARK
};

struct server_replay_marks_call_t
{
    int kind;
    struct rocketnet_address_t address;
    uint32_t high_water;
};

struct server_replay_marks_recording_t
{
    int count;
    int forget_refusals;
    int mark_refusals;
    struct server_replay_marks_call_t calls[ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS];
};

static struct rocketnet_address_t server_replay_marks_address( const char * text )
{
    struct rocketnet_address_t address;
    memset( &address, 0, sizeof(address) );
    rocketnet_unit_check( rocketnet_address_parse( &address, text ) );
    return address;
}

static void server_replay_marks_fixture_init( struct server_replay_marks_fixture_t * fixture, int max_clients )
{
    memset( fixture, 0, sizeof(*fixture) );
    fixture->map = rocketnet_encryption_map_create( NULL, 8 );
    fixture->slots = rocketnet_client_slots_create( NULL, max_clients );
    fixture->marks = rocketnet_server_replay_marks_create( NULL, max_clients );
    rocketnet_unit_check( fixture->map && fixture->slots && fixture->marks );
}

static void server_replay_marks_fixture_destroy( struct server_replay_marks_fixture_t * fixture )
{
    rocketnet_server_replay_marks_destroy( fixture->marks );
    rocketnet_client_slots_destroy( fixture->slots );
    rocketnet_encryption_map_destroy( fixture->map );
}

static struct server_replay_marks_client_t server_replay_marks_add_client( struct server_replay_marks_fixture_t * fixture,
                                                                           const struct rocketnet_address_t * address,
                                                                           uint64_t client_id,
                                                                           uint64_t highest_opened )
{
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );

    struct server_replay_marks_client_t client;
    client.mapping = rocketnet_encryption_map_add( fixture->map, address, ROCKETNET_REPLAY_MARKS_TEST_HANDSHAKE_DEADLINE );
    rocketnet_unit_check( client.mapping >= 0 );
    const uint64_t owner_generation = rocketnet_encryption_map_owner_generation( fixture->map, client.mapping );
    client.slot = rocketnet_client_slots_assign( fixture->slots, client_id, client.mapping,
                                                 owner_generation, user_data );
    rocketnet_unit_check( client.slot >= 0 );

    rocketnet_encryption_map_entry( fixture->map, client.mapping )->replay_session.highest_opened = highest_opened;

    return client;
}

static bool server_replay_marks_record_forget( void * context, const struct rocketnet_address_t * address )
{
    struct server_replay_marks_recording_t * recording = (struct server_replay_marks_recording_t *) context;
    rocketnet_unit_check( recording->count < ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS );
    struct server_replay_marks_call_t * call = &recording->calls[recording->count++];
    call->kind = SERVER_REPLAY_MARKS_FORGET;
    call->address = *address;
    call->high_water = 0;
    if ( recording->forget_refusals == 0 )
        return true;
    recording->forget_refusals--;
    return false;
}

static bool server_replay_marks_record_mark( void * context, const struct rocketnet_address_t * address, uint32_t high_water )
{
    struct server_replay_marks_recording_t * recording = (struct server_replay_marks_recording_t *) context;
    rocketnet_unit_check( recording->count < ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS );
    struct server_replay_marks_call_t * call = &recording->calls[recording->count++];
    call->kind = SERVER_REPLAY_MARKS_MARK;
    call->address = *address;
    call->high_water = high_water;
    if ( recording->mark_refusals == 0 )
        return true;
    recording->mark_refusals--;
    return false;
}

static void server_replay_marks_walk( struct server_replay_marks_fixture_t * fixture,
                                      struct server_replay_marks_recording_t * recording )
{
    struct rocketnet_server_replay_sink_t sink;
    sink.context = recording;
    sink.forget = server_replay_marks_record_forget;
    sink.mark = server_replay_marks_record_mark;
    rocketnet_server_replay_marks_walk( fixture->marks, fixture->slots, fixture->map, &sink );
}

static void server_replay_marks_check_mark( const struct server_replay_marks_call_t * call,
                                            const struct rocketnet_address_t * address,
                                            uint32_t high_water )
{
    rocketnet_unit_check( call->kind == SERVER_REPLAY_MARKS_MARK );
    rocketnet_unit_check( rocketnet_address_equal( &call->address, address ) );
    rocketnet_unit_check( call->high_water == high_water );
}

static void server_replay_marks_check_forget( const struct server_replay_marks_call_t * call,
                                              const struct rocketnet_address_t * address )
{
    rocketnet_unit_check( call->kind == SERVER_REPLAY_MARKS_FORGET );
    rocketnet_unit_check( rocketnet_address_equal( &call->address, address ) );
}

// Walk pass 2: each connected slot is marked once for its current
// highest_opened, a slot first seen at zero included (that zero mark
// overwrites an older session's kernel entry for the address,
// tla/RNXdpReplay.tla NewSessionNeverDroppedByOldMark), a second walk with
// nothing changed marks none, and an advance marks exactly that slot.

static void test_server_replay_marks_mark_connected_slots_once()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 4 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t b = server_replay_marks_address( "198.51.100.8:50000" );
    const struct rocketnet_address_t c = server_replay_marks_address( "198.51.100.9:50000" );

    const struct server_replay_marks_client_t client_a = server_replay_marks_add_client( &fixture, &a, 100, 41 );
    server_replay_marks_add_client( &fixture, &b, 200, 7 );
    server_replay_marks_add_client( &fixture, &c, 300, 0 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 3 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 41 );
    server_replay_marks_check_mark( &recording.calls[1], &b, 7 );
    server_replay_marks_check_mark( &recording.calls[2], &c, 0 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    rocketnet_encryption_map_entry( fixture.map, client_a.mapping )->replay_session.highest_opened = 42;

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 42 );

    server_replay_marks_fixture_destroy( &fixture );
}

// Walk pass 1 before pass 2: a released slot that held a mark gets a forget
// for its old address before a connected slot in the same walk is marked,
// and a further walk with nothing changed calls nothing.

static void test_server_replay_marks_forget_released_slots_first()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 4 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t b = server_replay_marks_address( "198.51.100.8:50000" );

    const struct server_replay_marks_client_t client_a = server_replay_marks_add_client( &fixture, &a, 100, 5 );
    const struct server_replay_marks_client_t client_b = server_replay_marks_add_client( &fixture, &b, 200, 0 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );
    server_replay_marks_check_mark( &recording.calls[1], &b, 0 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client_a.mapping );
    rocketnet_encryption_map_entry( fixture.map, client_b.mapping )->replay_session.highest_opened = 9;

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );
    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &b, 9 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// Walk pass 1's address branch: a slot whose mapping is replaced by one for
// another address forgets the old address and then marks the new one, in
// that order.

static void test_server_replay_marks_follow_an_address_change()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 2 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t b = server_replay_marks_address( "198.51.100.8:50000" );

    const struct server_replay_marks_client_t client_a = server_replay_marks_add_client( &fixture, &a, 100, 5 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client_a.mapping );

    const struct server_replay_marks_client_t client_b = server_replay_marks_add_client( &fixture, &b, 200, 9 );
    rocketnet_unit_check( client_b.slot == client_a.slot );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );
    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &b, 9 );

    server_replay_marks_fixture_destroy( &fixture );
}

// A forget the sink refuses stays owed: the slot keeps the old address, the
// second pass does not replace it with the new mapping, and the next walk
// forgets the old address again before marking the new one.

static void test_server_replay_marks_refused_forget_keeps_the_slot()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 2 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t b = server_replay_marks_address( "198.51.100.8:50000" );

    const struct server_replay_marks_client_t client_a = server_replay_marks_add_client( &fixture, &a, 100, 5 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client_a.mapping );

    const struct server_replay_marks_client_t client_b = server_replay_marks_add_client( &fixture, &b, 200, 9 );
    rocketnet_unit_check( client_b.slot == client_a.slot );

    // the buffer refuses the forget: only the forget is asked for, the new
    // address is not marked while the old one is still owed

    memset( &recording, 0, sizeof(recording) );
    recording.forget_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_forget( &recording.calls[0], &a );

    // the retry lands the old forget and only then marks the new address

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );
    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &b, 9 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// A connected slot may hold mapping index -1 (rocketnet_client_slots_assign
// accepts it): the forget pass treats that as a changed address, so the old
// mark is forgotten before the mapping array is ever indexed, and the mark
// pass skips the slot. A Release build must not read in front of the array.

static void test_server_replay_marks_forget_a_connected_slot_without_a_mapping()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 2 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );

    const struct server_replay_marks_client_t client = server_replay_marks_add_client( &fixture, &a, 100, 5 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );

    rocketnet_client_slots_release( fixture.slots, client.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client.mapping );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );
    const int slot = rocketnet_client_slots_assign( fixture.slots, 200, -1, 200, user_data );
    rocketnet_unit_check( slot == client.slot );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_forget( &recording.calls[0], &a );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// A slot that is connected with nothing opened yet -- its highest_opened is
// 0, so the walk marked it only at zero -- and then leaves is still
// forgotten: the
// transport's forget drops the reply MAC of that address too, and a learned
// MAC outranks the gateway in the reply path, so a stale one would send the
// next client's replies to the old link-layer address.

static void test_server_replay_marks_forget_a_released_slot_without_a_mark()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 4 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );

    const struct server_replay_marks_client_t client = server_replay_marks_add_client( &fixture, &a, 100, 0 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    rocketnet_client_slots_release( fixture.slots, client.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client.mapping );

    // A full pending buffer refuses this unmarked slot's forget. Its seen
    // identity remains responsible, so the next walk asks for it again.
    memset( &recording, 0, sizeof(recording) );
    recording.forget_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_forget( &recording.calls[0], &a );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_forget( &recording.calls[0], &a );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// tla/RNXdpReplay.tla Walk, AdvanceOnQueueRefusal = FALSE: a mark the sink
// cannot queue leaves the slot's cache where it was and the slot owes it, so
// the next walk asks again with the same value: a reconnect's zero mark, a
// first-seen zero mark and an advanced mark alike. Once queued, nothing more.

static void test_server_replay_marks_refused_mark_is_owed()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 2 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );

    // first seen at zero, refused, owed, then queued
    const struct server_replay_marks_client_t client = server_replay_marks_add_client( &fixture, &a, 100, 0 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    // an advance refused twice is asked for each walk until queued
    rocketnet_encryption_map_entry( fixture.map, client.mapping )->replay_session.highest_opened = 9;
    memset( &recording, 0, sizeof(recording) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 9 );

    memset( &recording, 0, sizeof(recording) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 9 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 9 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    // a reconnect into the same slot before any walk: its zero mark (a fall
    // below the slot's mark) refused, then asked again and queued
    rocketnet_client_slots_release( fixture.slots, client.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client.mapping );
    const struct server_replay_marks_client_t again = server_replay_marks_add_client( &fixture, &a, 200, 0 );
    rocketnet_unit_check( again.slot == client.slot );

    memset( &recording, 0, sizeof(recording) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

REGISTER_UNIT_TEST( test_server_replay_marks_mark_connected_slots_once );
REGISTER_UNIT_TEST( test_server_replay_marks_refused_mark_is_owed );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_released_slots_first );
REGISTER_UNIT_TEST( test_server_replay_marks_follow_an_address_change );
REGISTER_UNIT_TEST( test_server_replay_marks_refused_forget_keeps_the_slot );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_a_connected_slot_without_a_mapping );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_a_released_slot_without_a_mark );
