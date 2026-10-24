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
    client.slot = rocketnet_client_slots_assign( fixture->slots, client_id, client.mapping, user_data );
    rocketnet_unit_check( client.slot >= 0 );

    rocketnet_encryption_map_entry( fixture->map, client.mapping )->replay_session.highest_opened = highest_opened;

    return client;
}

static void server_replay_marks_record_forget( void * context, const struct rocketnet_address_t * address )
{
    struct server_replay_marks_recording_t * recording = (struct server_replay_marks_recording_t *) context;
    rocketnet_unit_check( recording->count < ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS );
    struct server_replay_marks_call_t * call = &recording->calls[recording->count++];
    call->kind = SERVER_REPLAY_MARKS_FORGET;
    call->address = *address;
    call->high_water = 0;
}

static void server_replay_marks_record_mark( void * context, const struct rocketnet_address_t * address, uint32_t high_water )
{
    struct server_replay_marks_recording_t * recording = (struct server_replay_marks_recording_t *) context;
    rocketnet_unit_check( recording->count < ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS );
    struct server_replay_marks_call_t * call = &recording->calls[recording->count++];
    call->kind = SERVER_REPLAY_MARKS_MARK;
    call->address = *address;
    call->high_water = high_water;
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
// highest_opened, a second walk with nothing changed marks none, an advance
// marks exactly that slot, and a slot at zero is never marked.

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
    rocketnet_unit_check( recording.count == 2 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 41 );
    server_replay_marks_check_mark( &recording.calls[1], &b, 7 );

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
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove( fixture.map, client_a.mapping );
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
    rocketnet_encryption_map_remove( fixture.map, client_a.mapping );

    const struct server_replay_marks_client_t client_b = server_replay_marks_add_client( &fixture, &b, 200, 9 );
    rocketnet_unit_check( client_b.slot == client_a.slot );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );
    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &b, 9 );

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
    rocketnet_encryption_map_remove( fixture.map, client.mapping );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof(user_data) );
    const int slot = rocketnet_client_slots_assign( fixture.slots, 200, -1, user_data );
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

// Walk forgets a released slot that never held a mark (highest_opened was 0)
// but was connected with a mapping during the mark pass.

static void test_server_replay_marks_forget_a_released_slot_without_a_mark()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 4 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );

    const struct server_replay_marks_client_t client_a = server_replay_marks_add_client( &fixture, &a, 100, 0 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof(recording) );

    server_replay_marks_walk( &fixture, &recording );
    // First walk marks nothing (highest_opened=0) but sets seen=a.
    rocketnet_unit_check( recording.count == 0 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove( fixture.map, client_a.mapping );

    memset( &recording, 0, sizeof(recording) );
    server_replay_marks_walk( &fixture, &recording );
    // Second walk forgets a because the slot was seen and is now released.
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_forget( &recording.calls[0], &a );

    server_replay_marks_fixture_destroy( &fixture );
}

REGISTER_UNIT_TEST( test_server_replay_marks_mark_connected_slots_once );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_released_slots_first );
REGISTER_UNIT_TEST( test_server_replay_marks_follow_an_address_change );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_a_connected_slot_without_a_mapping );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_a_released_slot_without_a_mark );
