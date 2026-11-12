/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server replay walk (sdk/rocketnet_server_replay_marks.h) against
// proofs/tla/RNXdpReplay.tla Walk: the forgets first, then the marks, once per
// update. The client slots and encryption map are built directly, time is an
// argument, and there is no transport, transport or clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_server_replay_marks.h"
#include "../sdk/rocketnet_xdp_replay_marks.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ROCKETNET_DEV
// every record the walk's supersede scans read, counted in development
// builds only (sdk/rocketnet_server_replay_marks.cpp)
uint64_t rocketnet_server_replay_marks_comparisons_for_test( const struct rocketnet_server_replay_marks_t * marks );
#endif // #if ROCKETNET_DEV

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
    bool queued;
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
    memset( &address, 0, sizeof( address ) );
    rocketnet_unit_check( rocketnet_address_parse( &address, text ) );
    return address;
}

static void server_replay_marks_fixture_init( struct server_replay_marks_fixture_t * fixture, int max_clients )
{
    memset( fixture, 0, sizeof( *fixture ) );
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
    memset( user_data, 0, sizeof( user_data ) );

    struct server_replay_marks_client_t client;
    client.mapping = rocketnet_encryption_map_add( fixture->map, address, ROCKETNET_REPLAY_MARKS_TEST_HANDSHAKE_DEADLINE );
    rocketnet_unit_check( client.mapping >= 0 );

    // the client arrives in a later update than any release before it: that
    // update's end has published the freed slots

    rocketnet_client_slots_publish( fixture->slots );

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
    call->queued = recording->forget_refusals == 0;
    if ( call->queued )
    {
        return true;
    }
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
    call->queued = recording->mark_refusals == 0;
    if ( call->queued )
    {
        return true;
    }
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
// proofs/tla/RNXdpReplay.tla NewSessionNeverDroppedByOldMark), a second walk with
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
    memset( &recording, 0, sizeof( recording ) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 3 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 41 );
    server_replay_marks_check_mark( &recording.calls[1], &b, 7 );
    server_replay_marks_check_mark( &recording.calls[2], &c, 0 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    rocketnet_encryption_map_entry( fixture.map, client_a.mapping )->replay_session.highest_opened = 42;

    memset( &recording, 0, sizeof( recording ) );
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
    memset( &recording, 0, sizeof( recording ) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );
    server_replay_marks_check_mark( &recording.calls[1], &b, 0 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client_a.mapping );
    rocketnet_encryption_map_entry( fixture.map, client_b.mapping )->replay_session.highest_opened = 9;

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );

    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &b, 9 );

    memset( &recording, 0, sizeof( recording ) );
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
    memset( &recording, 0, sizeof( recording ) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client_a.mapping );

    const struct server_replay_marks_client_t client_b = server_replay_marks_add_client( &fixture, &b, 200, 9 );
    rocketnet_unit_check( client_b.slot == client_a.slot );

    memset( &recording, 0, sizeof( recording ) );
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
    memset( &recording, 0, sizeof( recording ) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );

    rocketnet_client_slots_release( fixture.slots, client_a.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client_a.mapping );

    const struct server_replay_marks_client_t client_b = server_replay_marks_add_client( &fixture, &b, 200, 9 );
    rocketnet_unit_check( client_b.slot == client_a.slot );

    // the buffer refuses the forget: only the forget is asked for, the new
    // address is not marked while the old one is still owed

    memset( &recording, 0, sizeof( recording ) );
    recording.forget_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_forget( &recording.calls[0], &a );

    // the retry lands the old forget and only then marks the new address

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );

    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &b, 9 );

    memset( &recording, 0, sizeof( recording ) );
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
    memset( &recording, 0, sizeof( recording ) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 5 );

    rocketnet_client_slots_release( fixture.slots, client.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client.mapping );

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 0, sizeof( user_data ) );
    rocketnet_client_slots_publish( fixture.slots );   // stronger than the server: no walk between
    const int slot = rocketnet_client_slots_assign( fixture.slots, 200, -1, 200, user_data );
    rocketnet_unit_check( slot == client.slot );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_forget( &recording.calls[0], &a );

    memset( &recording, 0, sizeof( recording ) );
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
    memset( &recording, 0, sizeof( recording ) );

    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    rocketnet_client_slots_release( fixture.slots, client.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client.mapping );

    // A full pending buffer refuses this unmarked slot's forget. Its seen
    // identity remains responsible, so the next walk asks for it again.

    memset( &recording, 0, sizeof( recording ) );
    recording.forget_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_forget( &recording.calls[0], &a );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_forget( &recording.calls[0], &a );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// proofs/tla/RNXdpReplay.tla Walk, AdvanceOnQueueRefusal = FALSE: a mark the sink
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
    memset( &recording, 0, sizeof( recording ) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    // an advance refused twice is asked for each walk until queued

    rocketnet_encryption_map_entry( fixture.map, client.mapping )->replay_session.highest_opened = 9;
    memset( &recording, 0, sizeof( recording ) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 9 );

    memset( &recording, 0, sizeof( recording ) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 9 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 9 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    // a reconnect into the same slot before any walk: its zero mark (a fall
    // below the slot's mark) refused, then asked again and queued

    rocketnet_client_slots_release( fixture.slots, client.slot );
    rocketnet_encryption_map_remove_current( fixture.map, client.mapping );
    const struct server_replay_marks_client_t again = server_replay_marks_add_client( &fixture, &a, 200, 0 );
    rocketnet_unit_check( again.slot == client.slot );

    memset( &recording, 0, sizeof( recording ) );
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );

    server_replay_marks_check_mark( &recording.calls[0], &a, 0 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// The kernel replay map as the transport's commit leaves it: every queued
// forget, then every queued mark (proofs/tla/RNServerReplaySlots.tla Walk).

struct server_replay_marks_kernel_t
{
    int count;
    struct rocketnet_address_t address[ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS];
    uint32_t high_water[ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS];
};

static int server_replay_marks_kernel_find( const struct server_replay_marks_kernel_t * kernel,
                                            const struct rocketnet_address_t * address )
{
    for ( int i = 0; i < kernel->count; i++ )
    {
        if ( rocketnet_address_equal( &kernel->address[i], address ) )
        {
            return i;
        }
    }
    return -1;
}

static void server_replay_marks_kernel_commit( struct server_replay_marks_kernel_t * kernel,
                                               const struct server_replay_marks_recording_t * recording )
{
    for ( int i = 0; i < recording->count; i++ )
    {
        const struct server_replay_marks_call_t * call = &recording->calls[i];
        if ( call->kind != SERVER_REPLAY_MARKS_FORGET || !call->queued )
        {
            continue;
        }
        const int entry = server_replay_marks_kernel_find( kernel, &call->address );
        if ( entry < 0 )
        {
            continue;
        }
        kernel->count--;
        kernel->address[entry] = kernel->address[kernel->count];
        kernel->high_water[entry] = kernel->high_water[kernel->count];
    }

    for ( int i = 0; i < recording->count; i++ )
    {
        const struct server_replay_marks_call_t * call = &recording->calls[i];
        if ( call->kind != SERVER_REPLAY_MARKS_MARK || !call->queued )
        {
            continue;
        }
        int entry = server_replay_marks_kernel_find( kernel, &call->address );
        if ( entry < 0 )
        {
            rocketnet_unit_check( kernel->count < ROCKETNET_REPLAY_MARKS_TEST_MAX_CALLS );
            entry = kernel->count++;
            kernel->address[entry] = call->address;
        }
        kernel->high_water[entry] = call->high_water;
    }
}

static void server_replay_marks_walk_and_commit( struct server_replay_marks_fixture_t * fixture,
                                                 struct server_replay_marks_recording_t * recording,
                                                 struct server_replay_marks_kernel_t * kernel )
{
    server_replay_marks_walk( fixture, recording );
    server_replay_marks_kernel_commit( kernel, recording );
}

static void server_replay_marks_check_kernel( const struct server_replay_marks_kernel_t * kernel,
                                              const struct rocketnet_address_t * address,
                                              uint32_t high_water )
{
    const int entry = server_replay_marks_kernel_find( kernel, address );
    rocketnet_unit_check( entry >= 0 );
    if ( entry >= 0 )
    {
        rocketnet_unit_check( kernel->high_water[entry] == high_water );
    }
}

// proofs/tla/RNServerReplaySlots.tla Walk, MarkSupersedes: slot 0 is seen at A
// and released, and slot 1 connects at A. The walk's forget of A for slot 0 is
// refused and slot 1's mark of A is queued: the queued mark supersedes the owed
// forget, so no later walk forgets A and deletes slot 1's entry, which slot 1's
// record says is in place. Back at A after slot 1 leaves, slot 0 is first seen
// and marks again, even at the value its old record held, and that mark
// supersedes slot 1's refused forget of A in turn.

static void test_server_replay_marks_queued_mark_supersedes_an_owed_forget()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 2 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t c = server_replay_marks_address( "198.51.100.9:50000" );

    struct server_replay_marks_kernel_t kernel;
    memset( &kernel, 0, sizeof( kernel ) );

    const struct server_replay_marks_client_t first = server_replay_marks_add_client( &fixture, &a, 100, 5 );
    const struct server_replay_marks_client_t other = server_replay_marks_add_client( &fixture, &c, 200, 0 );
    rocketnet_unit_check( first.slot == 0 && other.slot == 1 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    server_replay_marks_check_kernel( &kernel, &a, 5 );

    // slot 0 leaves A, then slot 1 leaves C, so slot 1 is the next one assigned

    rocketnet_client_slots_release( fixture.slots, first.slot );
    rocketnet_encryption_map_remove_current( fixture.map, first.mapping );
    rocketnet_client_slots_release( fixture.slots, other.slot );
    rocketnet_encryption_map_remove_current( fixture.map, other.mapping );

    const struct server_replay_marks_client_t second = server_replay_marks_add_client( &fixture, &a, 300, 0 );
    rocketnet_unit_check( second.slot == 1 );

    // slot 0's forget of A is refused, slot 1's forget of C and its mark of A
    // are queued

    memset( &recording, 0, sizeof( recording ) );
    recording.forget_refusals = 1;
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    rocketnet_unit_check( recording.count == 3 );

    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_forget( &recording.calls[1], &c );
    server_replay_marks_check_mark( &recording.calls[2], &a, 0 );
    server_replay_marks_check_kernel( &kernel, &a, 0 );

    // nothing changed: no forget of A, and slot 1's entry stays

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    rocketnet_unit_check( recording.count == 0 );
    server_replay_marks_check_kernel( &kernel, &a, 0 );

    // slot 1 moves from A to C and its forget of A is refused; slot 0 comes
    // back to A at 5, the value its old record held, and is marked. That mark
    // supersedes slot 1's owed forget, and slot 1 is first seen at C.

    rocketnet_client_slots_release( fixture.slots, second.slot );
    rocketnet_encryption_map_remove_current( fixture.map, second.mapping );
    const struct server_replay_marks_client_t moved = server_replay_marks_add_client( &fixture, &c, 400, 0 );
    rocketnet_unit_check( moved.slot == 1 );
    const struct server_replay_marks_client_t back = server_replay_marks_add_client( &fixture, &a, 500, 5 );
    rocketnet_unit_check( back.slot == 0 );

    memset( &recording, 0, sizeof( recording ) );
    recording.forget_refusals = 1;
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    rocketnet_unit_check( recording.count == 3 );

    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &a, 5 );
    server_replay_marks_check_mark( &recording.calls[2], &c, 0 );
    server_replay_marks_check_kernel( &kernel, &a, 5 );
    server_replay_marks_check_kernel( &kernel, &c, 0 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    rocketnet_unit_check( recording.count == 0 );
    server_replay_marks_check_kernel( &kernel, &a, 5 );

    server_replay_marks_fixture_destroy( &fixture );
}

// proofs/tla/RNServerReplaySlots.tla Walk, ForgetSupersedes: slot 0 is marked at
// A and released; slot 1 connects at A, slot 0's forget is refused and slot
// 1's mark of A refused too, so slot 0's record still names its mark of A.
// Slot 1 leaves and slot 0 comes back to A; slot 1's queued forget of A
// deletes the entry, and slot 0, whose record held that mark, is marked again
// in the same walk.

static void test_server_replay_marks_queued_forget_supersedes_a_held_mark()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 2 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t c = server_replay_marks_address( "198.51.100.9:50000" );
    const struct rocketnet_address_t d = server_replay_marks_address( "198.51.100.10:50000" );

    struct server_replay_marks_kernel_t kernel;
    memset( &kernel, 0, sizeof( kernel ) );

    const struct server_replay_marks_client_t first = server_replay_marks_add_client( &fixture, &a, 100, 0 );
    const struct server_replay_marks_client_t other = server_replay_marks_add_client( &fixture, &c, 200, 0 );
    rocketnet_unit_check( first.slot == 0 && other.slot == 1 );

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    server_replay_marks_check_kernel( &kernel, &a, 0 );

    rocketnet_client_slots_release( fixture.slots, first.slot );
    rocketnet_encryption_map_remove_current( fixture.map, first.mapping );
    rocketnet_client_slots_release( fixture.slots, other.slot );
    rocketnet_encryption_map_remove_current( fixture.map, other.mapping );

    const struct server_replay_marks_client_t second = server_replay_marks_add_client( &fixture, &a, 300, 0 );
    rocketnet_unit_check( second.slot == 1 );

    // slot 0's forget of A refused, slot 1's forget of C queued, slot 1's mark
    // of A refused

    memset( &recording, 0, sizeof( recording ) );
    recording.forget_refusals = 1;
    recording.mark_refusals = 1;
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    rocketnet_unit_check( recording.count == 3 );

    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_forget( &recording.calls[1], &c );
    server_replay_marks_check_mark( &recording.calls[2], &a, 0 );

    // slot 1 leaves A for D and slot 0 comes back to A before the next walk

    rocketnet_client_slots_release( fixture.slots, second.slot );
    rocketnet_encryption_map_remove_current( fixture.map, second.mapping );
    const struct server_replay_marks_client_t moved = server_replay_marks_add_client( &fixture, &d, 400, 0 );
    rocketnet_unit_check( moved.slot == 1 );
    const struct server_replay_marks_client_t back = server_replay_marks_add_client( &fixture, &a, 500, 0 );
    rocketnet_unit_check( back.slot == 0 );

    // slot 1's forget of A is queued: the commit deletes A, and slot 0 marks it

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk_and_commit( &fixture, &recording, &kernel );
    rocketnet_unit_check( recording.count == 3 );

    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &a, 0 );
    server_replay_marks_check_mark( &recording.calls[2], &d, 0 );
    server_replay_marks_check_kernel( &kernel, &a, 0 );
    server_replay_marks_check_kernel( &kernel, &d, 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// the counting sink queues into the XDP transport's pending buffer
// (sdk/rocketnet_xdp_replay_marks.h), as the transport's sink does, so the
// cost case reads the buffer's search too

struct server_replay_marks_counts_t
{
    int forgets;
    int marks;
    struct rocketnet_xdp_replay_marks_t * pending;
};

static bool server_replay_marks_count_forget( void * context, const struct rocketnet_address_t * address )
{
    struct server_replay_marks_counts_t * counts = (struct server_replay_marks_counts_t *) context;
    counts->forgets++;
    return rocketnet_xdp_replay_marks_forget( counts->pending, address );
}

static bool server_replay_marks_count_mark( void * context, const struct rocketnet_address_t * address, uint32_t high_water )
{
    struct server_replay_marks_counts_t * counts = (struct server_replay_marks_counts_t *) context;
    counts->marks++;
    return rocketnet_xdp_replay_marks_mark( counts->pending, address, high_water );
}

static struct rocketnet_server_xdp_replay_key_t server_replay_marks_pending_forgets[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
static struct rocketnet_server_xdp_replay_key_t server_replay_marks_pending_mark_keys[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
static struct rocketnet_server_xdp_replay_value_t server_replay_marks_pending_mark_values[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
static bool server_replay_marks_pending_none[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];
static bool server_replay_marks_pending_all[ROCKETNET_XDP_REPLAY_MAX_ENTRIES];

// the transport's commit with every forget and mark landing in the kernel
static void server_replay_marks_pending_commit( struct rocketnet_xdp_replay_marks_t * pending )
{
    for ( uint32_t i = 0; i < pending->capacity; i++ )
    {
        server_replay_marks_pending_none[i] = false;
        server_replay_marks_pending_all[i] = true;
    }
    rocketnet_xdp_replay_marks_reset_keeping( pending, server_replay_marks_pending_none, server_replay_marks_pending_all );
}

// A mass disconnect reads each slot once. Every slot's forget is queued, and
// with no forget refused in the walk before, no two records name one address
// (proofs/tla/RNServerReplaySlots.tla DistinctUnlessOwed), so no queued forget
// scans the other slots for a record to clear. Scanning for each forget, as
// v1.0.0 did, reads max_clients records per forget: 1,000,000 at 1000 clients.
// The XDP pending buffer the forgets are queued into searches only the
// forgets the last commit kept, none here; searching every forget already
// queued, as v1.1.0 did, reads 499,500 at 1000 clients.

static void test_server_replay_marks_mass_disconnect_is_linear()
{
    const int clients = ROCKETNET_MAX_CLIENTS;

    struct server_replay_marks_fixture_t fixture;
    memset( &fixture, 0, sizeof( fixture ) );
    fixture.map = rocketnet_encryption_map_create( NULL, clients );
    fixture.slots = rocketnet_client_slots_create( NULL, clients );
    fixture.marks = rocketnet_server_replay_marks_create( NULL, clients );
    rocketnet_unit_check( fixture.map && fixture.slots && fixture.marks );

    static struct server_replay_marks_client_t records[ROCKETNET_MAX_CLIENTS];

    for ( int i = 0; i < clients; i++ )
    {
        struct rocketnet_address_t address = server_replay_marks_address( "198.51.100.7:2000" );
        address.port = (uint16_t) ( 2000 + i );
        records[i] = server_replay_marks_add_client( &fixture, &address, (uint64_t) ( 1000 + i ), (uint64_t) i );
    }

    struct rocketnet_xdp_replay_marks_t pending;
    rocketnet_xdp_replay_marks_bind( &pending, server_replay_marks_pending_forgets, server_replay_marks_pending_mark_keys,
                                     server_replay_marks_pending_mark_values, rocketnet_xdp_replay_entries( clients ) );
    rocketnet_unit_check( pending.capacity >= (uint32_t) clients );

    struct server_replay_marks_counts_t counts;
    memset( &counts, 0, sizeof( counts ) );
    counts.pending = &pending;
    struct rocketnet_server_replay_sink_t sink = { &counts, server_replay_marks_count_forget, server_replay_marks_count_mark };

    rocketnet_server_replay_marks_walk( fixture.marks, fixture.slots, fixture.map, &sink );
    rocketnet_unit_check( counts.marks == clients && counts.forgets == 0 );
    server_replay_marks_pending_commit( &pending );

    for ( int i = 0; i < clients; i++ )
    {
        rocketnet_client_slots_release( fixture.slots, records[i].slot );
        rocketnet_encryption_map_remove_current( fixture.map, records[i].mapping );
    }

    counts.forgets = 0;
    counts.marks = 0;
    rocketnet_server_replay_marks_walk( fixture.marks, fixture.slots, fixture.map, &sink );
    rocketnet_unit_check( counts.forgets == clients && counts.marks == 0 );
    rocketnet_unit_check( pending.forget_count == (uint32_t) clients && pending.refused == 0 );

#if ROCKETNET_DEV
    const uint64_t comparisons = rocketnet_server_replay_marks_comparisons_for_test( fixture.marks );
    if ( comparisons != 0 )
    {
        printf( "mass disconnect of %d clients: supersede scans read %" PRIu64 " records, expected 0\n", clients, comparisons );
    }
    rocketnet_unit_check( comparisons == 0 );
    if ( pending.comparisons != 0 )
    {
        printf( "mass disconnect of %d clients: the pending buffer's forget searches read %" PRIu64 " forgets, expected 0\n", clients, pending.comparisons );
    }
    rocketnet_unit_check( pending.comparisons == 0 );
#endif // #if ROCKETNET_DEV

    server_replay_marks_pending_commit( &pending );
    counts.forgets = 0;
    counts.marks = 0;
    rocketnet_server_replay_marks_walk( fixture.marks, fixture.slots, fixture.map, &sink );
    rocketnet_unit_check( counts.forgets == 0 && counts.marks == 0 );

    server_replay_marks_fixture_destroy( &fixture );
}

// proofs/tla/RNServerReplaySlots.tla Walk, supOwed: the forget pass scans the
// other slots for a record of the forgotten address only in the walk after
// one that left a refused forget, the one way two records come to name one
// address. The sequence is the one of
// test_server_replay_marks_queued_forget_supersedes_a_held_mark, with the
// records each walk's scans read counted (development builds).

static void test_server_replay_marks_refused_forget_owes_the_next_scan()
{
    struct server_replay_marks_fixture_t fixture;
    server_replay_marks_fixture_init( &fixture, 2 );

    const struct rocketnet_address_t a = server_replay_marks_address( "198.51.100.7:50000" );
    const struct rocketnet_address_t c = server_replay_marks_address( "198.51.100.9:50000" );
    const struct rocketnet_address_t d = server_replay_marks_address( "198.51.100.10:50000" );

    const struct server_replay_marks_client_t first = server_replay_marks_add_client( &fixture, &a, 100, 0 );
    const struct server_replay_marks_client_t other = server_replay_marks_add_client( &fixture, &c, 200, 0 );
    rocketnet_unit_check( first.slot == 0 && other.slot == 1 );

    // nothing refused yet: the first walk marks both and scans nothing

    struct server_replay_marks_recording_t recording;
    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 2 );
#if ROCKETNET_DEV
    rocketnet_unit_check( rocketnet_server_replay_marks_comparisons_for_test( fixture.marks ) == 0 );
#endif // #if ROCKETNET_DEV

    rocketnet_client_slots_release( fixture.slots, first.slot );
    rocketnet_encryption_map_remove_current( fixture.map, first.mapping );
    rocketnet_client_slots_release( fixture.slots, other.slot );
    rocketnet_encryption_map_remove_current( fixture.map, other.mapping );

    const struct server_replay_marks_client_t second = server_replay_marks_add_client( &fixture, &a, 300, 0 );
    rocketnet_unit_check( second.slot == 1 );

    // slot 0's forget of A is refused and slot 1's forget of C queued with no
    // scan owed; slot 1's mark of A is refused, so it clears nothing: slot 0
    // and slot 1 both name A, and the walk owes the next one its scan

    memset( &recording, 0, sizeof( recording ) );
    recording.forget_refusals = 1;
    recording.mark_refusals = 1;
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 3 );
    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_forget( &recording.calls[1], &c );
    server_replay_marks_check_mark( &recording.calls[2], &a, 0 );
#if ROCKETNET_DEV
    rocketnet_unit_check( rocketnet_server_replay_marks_comparisons_for_test( fixture.marks ) == 0 );
#endif // #if ROCKETNET_DEV

    // slot 1 leaves A for D and slot 0 comes back to A: slot 1's queued
    // forget of A scans both records and clears slot 0's, which is first seen
    // again and marked

    rocketnet_client_slots_release( fixture.slots, second.slot );
    rocketnet_encryption_map_remove_current( fixture.map, second.mapping );
    const struct server_replay_marks_client_t moved = server_replay_marks_add_client( &fixture, &d, 400, 0 );
    rocketnet_unit_check( moved.slot == 1 );
    const struct server_replay_marks_client_t back = server_replay_marks_add_client( &fixture, &a, 500, 0 );
    rocketnet_unit_check( back.slot == 0 );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 3 );
    server_replay_marks_check_forget( &recording.calls[0], &a );
    server_replay_marks_check_mark( &recording.calls[1], &a, 0 );
    server_replay_marks_check_mark( &recording.calls[2], &d, 0 );
#if ROCKETNET_DEV
    rocketnet_unit_check( rocketnet_server_replay_marks_comparisons_for_test( fixture.marks ) == 2 );
#endif // #if ROCKETNET_DEV

    // that walk refused nothing, so the next forget scans nothing

    rocketnet_client_slots_release( fixture.slots, back.slot );
    rocketnet_encryption_map_remove_current( fixture.map, back.mapping );

    memset( &recording, 0, sizeof( recording ) );
    server_replay_marks_walk( &fixture, &recording );
    rocketnet_unit_check( recording.count == 1 );
    server_replay_marks_check_forget( &recording.calls[0], &a );
#if ROCKETNET_DEV
    rocketnet_unit_check( rocketnet_server_replay_marks_comparisons_for_test( fixture.marks ) == 2 );
#endif // #if ROCKETNET_DEV

    server_replay_marks_fixture_destroy( &fixture );
}

REGISTER_UNIT_TEST( test_server_replay_marks_mark_connected_slots_once );
REGISTER_UNIT_TEST( test_server_replay_marks_mass_disconnect_is_linear );
REGISTER_UNIT_TEST( test_server_replay_marks_refused_forget_owes_the_next_scan );
REGISTER_UNIT_TEST( test_server_replay_marks_refused_mark_is_owed );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_released_slots_first );
REGISTER_UNIT_TEST( test_server_replay_marks_follow_an_address_change );
REGISTER_UNIT_TEST( test_server_replay_marks_refused_forget_keeps_the_slot );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_a_connected_slot_without_a_mapping );
REGISTER_UNIT_TEST( test_server_replay_marks_forget_a_released_slot_without_a_mark );
REGISTER_UNIT_TEST( test_server_replay_marks_queued_mark_supersedes_an_owed_forget );
REGISTER_UNIT_TEST( test_server_replay_marks_queued_forget_supersedes_a_held_mark );
