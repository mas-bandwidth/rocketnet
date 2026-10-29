/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC proof of the userspace XDP replay-map mark and forget walk.

    The unit under proof is sdk/rocketnet_server_replay_marks.cpp, included
    directly below at max_clients = 2 together with rocketnet_address_equal.

    The harnesses verify the invariants of tla/RNXdpReplay.tla Walk:
      * Pass 1 (forgets) strictly precedes Pass 2 (marks): every forget
        callback runs before any mark callback;
      * NewSessionNeverDroppedByOldMark: when slot 0 disconnects from address A
        and slot 1 is connected at the same address A in the same walk, Pass 1
        forgets A first and Pass 2 marks A at slot 1's highest_opened (even
        when highest_opened == 0 on a first-seen slot), leaving A present in
        the replay map at slot 1's high-water;
      * Redundant-mark suppression: a second walk with unchanged address and
        highest_opened emits neither a forget nor a mark;
      * Unmarked-seen disconnect forget: a slot that was seen connected whose
        mark never landed (has_mark == 0) still triggers forget(seen_address)
        on disconnect so its reply MAC is dropped;
      * ForgetOwedUntilApplied and MarkOwedUntilApplied: a refused forget
        preserves the slot's seen_address and prevents Pass 2 from replacing
        it after an address change until the old address is forgotten; a
        refused mark sets mark_owed so the next walk retries the mark even if
        highest_opened has not changed.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check
           --unwinding-assertions
    Unwinding: 17 (covers 16-byte address comparisons and loops over
    max_clients = 2).
*/

#define ROCKETNET_DEV 0
#define ROCKETNET_ASSERTS 0
#define static_assert _Static_assert

#include "rocketnet_internal.h"
#include "rocketnet_address.h"
typedef struct rocketnet_address_t rocketnet_address_t;

#include "rocketnet_server_replay_marks.cpp"

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static unsigned int nondet_uint( void ) { return 0; }
static uint64_t nondet_uint64( void ) { return 0; }
#else
unsigned int nondet_uint( void );
uint64_t nondet_uint64( void );
#endif // #ifndef __CPROVER__

#define PROOF_MAX_CLIENTS 2

struct rocketnet_client_slots_t
{
    int connected[PROOF_MAX_CLIENTS];
    int mapping_index[PROOF_MAX_CLIENTS];
};

struct rocketnet_encryption_map_t
{
    struct rocketnet_address_t address[PROOF_MAX_CLIENTS];
    struct rocketnet_encryption_mapping_t entry[PROOF_MAX_CLIENTS];
};

void * rocketnet_malloc( void * context, size_t bytes )
{
    (void) context;
    (void) bytes;
    return NULL;
}

void rocketnet_free( void * context, void * p )
{
    (void) context;
    (void) p;
}

int rocketnet_client_slots_connected( const struct rocketnet_client_slots_t * slots, int client_index )
{
    return slots->connected[client_index];
}

int rocketnet_client_slots_mapping_index( const struct rocketnet_client_slots_t * slots, int client_index )
{
    return slots->mapping_index[client_index];
}

const struct rocketnet_address_t * rocketnet_encryption_map_address( const struct rocketnet_encryption_map_t * map, int index )
{
    return &map->address[index];
}

struct rocketnet_encryption_mapping_t * rocketnet_encryption_map_entry( struct rocketnet_encryption_map_t * map, int index )
{
    return &map->entry[index];
}

bool rocketnet_address_equal( const struct rocketnet_address_t * a, const struct rocketnet_address_t * b )
{
    if ( a->type != b->type || a->port != b->port )
        return false;

    if ( a->type == ROCKETNET_ADDRESS_IPV4 )
        return memcmp( a->data.ipv4, b->data.ipv4, 4 ) == 0;

    if ( a->type == ROCKETNET_ADDRESS_IPV6 )
        return memcmp( a->data.ipv6, b->data.ipv6, sizeof( a->data.ipv6 ) ) == 0;

    return false;
}

static struct rocketnet_address_t make_symbolic_ipv4_address( void )
{
    struct rocketnet_address_t addr;
    memset( &addr, 0, sizeof( addr ) );
    addr.type = ROCKETNET_ADDRESS_IPV4;
    unsigned int ip = nondet_uint();
    memcpy( addr.data.ipv4, &ip, 4 );
    unsigned int port = nondet_uint();
    __CPROVER_assume( port >= 1 && port <= 65535 );
    addr.port = (uint16_t) port;
    return addr;
}

struct walk_model_t
{
    int phase;
    int forget_calls;
    int mark_calls;
    bool forget_succeeds;
    bool mark_succeeds;
    struct rocketnet_address_t tracked_address;
    bool tracked_present;
    uint32_t tracked_high_water;
};

static bool model_forget( void * context, const struct rocketnet_address_t * address )
{
    struct walk_model_t * m = (struct walk_model_t *) context;
    __CPROVER_assert( m->phase == 0, "every forget runs before any mark" );
    m->forget_calls++;
    if ( !m->forget_succeeds )
        return false;
    if ( rocketnet_address_equal( address, &m->tracked_address ) )
    {
        m->tracked_present = false;
        m->tracked_high_water = 0;
    }
    return true;
}

static bool model_mark( void * context, const struct rocketnet_address_t * address, uint32_t high_water )
{
    struct walk_model_t * m = (struct walk_model_t *) context;
    m->phase = 1;
    m->mark_calls++;
    if ( !m->mark_succeeds )
        return false;
    if ( rocketnet_address_equal( address, &m->tracked_address ) )
    {
        m->tracked_present = true;
        m->tracked_high_water = high_water;
    }
    return true;
}

static void harness_two_phase_same_address_reuse( void )
{
    struct rocketnet_server_replay_slot_mark_t slot_storage[PROOF_MAX_CLIENTS];
    memset( slot_storage, 0, sizeof( slot_storage ) );
    struct rocketnet_server_replay_marks_t marks = { NULL, PROOF_MAX_CLIENTS, slot_storage };

    struct rocketnet_client_slots_t slots;
    memset( &slots, 0, sizeof( slots ) );
    static struct rocketnet_encryption_map_t map;
    memset( &map, 0, sizeof( map ) );

    struct rocketnet_address_t shared = make_symbolic_ipv4_address();
    uint64_t old_seq = nondet_uint64();
    uint64_t new_seq = nondet_uint64();
    __CPROVER_assume( old_seq <= 0xFFFFFFFFULL );
    __CPROVER_assume( new_seq <= 0xFFFFFFFFULL );

    /* Slot 0 was previously seen and marked at shared with old_seq, then disconnected. */
    slot_storage[0].seen = 1;
    slot_storage[0].seen_address = shared;
    slot_storage[0].has_mark = 1;
    slot_storage[0].marked_value = (uint32_t) old_seq;
    slots.connected[0] = 0;

    /* Slot 1 is newly connected at the exact same address (including new_seq == 0). */
    slots.connected[1] = 1;
    slots.mapping_index[1] = 1;
    map.address[1] = shared;
    map.entry[1].replay_session.highest_opened = new_seq;

    struct walk_model_t model;
    memset( &model, 0, sizeof( model ) );
    model.forget_succeeds = true;
    model.mark_succeeds = true;
    model.tracked_address = shared;
    model.tracked_present = true;
    model.tracked_high_water = (uint32_t) old_seq;

    struct rocketnet_server_replay_sink_t sink = { &model, model_forget, model_mark };
    rocketnet_server_replay_marks_walk( &marks, &slots, &map, &sink );

    __CPROVER_assert( model.forget_calls == 1, "disconnected slot 0 forgotten once" );
    __CPROVER_assert( model.mark_calls == 1, "first-seen slot 1 marked once even when highest_opened is 0" );
    __CPROVER_assert( model.tracked_present, "two-phase walk leaves reused address present in replay map" );
    __CPROVER_assert( model.tracked_high_water == (uint32_t) new_seq,
                      "reused address carries new session high-water, overwriting old mark" );
    __CPROVER_assert( slot_storage[0].seen == 0 && slot_storage[0].has_mark == 0,
                      "forgotten slot record is cleared" );
    __CPROVER_assert( slot_storage[1].seen == 1 && slot_storage[1].has_mark == 1 &&
                          slot_storage[1].marked_value == (uint32_t) new_seq && slot_storage[1].mark_owed == 0,
                      "marked slot caches new high-water" );

    /* A second walk with unchanged state emits no forget and no mark. */
    model.phase = 0;
    model.forget_calls = 0;
    model.mark_calls = 0;
    rocketnet_server_replay_marks_walk( &marks, &slots, &map, &sink );
    __CPROVER_assert( model.forget_calls == 0 && model.mark_calls == 0,
                      "unchanged slot state emits neither forget nor mark" );
}

static void harness_refused_forget_and_mark_retry( void )
{
    struct rocketnet_server_replay_slot_mark_t slot_storage[PROOF_MAX_CLIENTS];
    memset( slot_storage, 0, sizeof( slot_storage ) );
    struct rocketnet_server_replay_marks_t marks = { NULL, PROOF_MAX_CLIENTS, slot_storage };

    struct rocketnet_client_slots_t slots;
    memset( &slots, 0, sizeof( slots ) );
    static struct rocketnet_encryption_map_t map;
    memset( &map, 0, sizeof( map ) );

    struct rocketnet_address_t addr_a = make_symbolic_ipv4_address();
    struct rocketnet_address_t addr_b = make_symbolic_ipv4_address();
    __CPROVER_assume( !rocketnet_address_equal( &addr_a, &addr_b ) );

    uint32_t hw_a = nondet_uint();
    uint32_t hw_b = nondet_uint();

    /* Slot 0 was seen at addr_a, but its mapping now points to addr_b. */
    slot_storage[0].seen = 1;
    slot_storage[0].seen_address = addr_a;
    slot_storage[0].has_mark = 1;
    slot_storage[0].marked_value = hw_a;

    slots.connected[0] = 1;
    slots.mapping_index[0] = 0;
    map.address[0] = addr_b;
    map.entry[0].replay_session.highest_opened = hw_b;

    /* Walk 1: forget(addr_a) is refused by the sink. Pass 2 must skip slot 0. */
    struct walk_model_t model;
    memset( &model, 0, sizeof( model ) );
    model.forget_succeeds = false;
    model.mark_succeeds = true;
    model.tracked_address = addr_a;
    model.tracked_present = true;
    model.tracked_high_water = hw_a;

    struct rocketnet_server_replay_sink_t sink = { &model, model_forget, model_mark };
    rocketnet_server_replay_marks_walk( &marks, &slots, &map, &sink );

    __CPROVER_assert( model.forget_calls == 1, "refused forget was attempted" );
    __CPROVER_assert( model.mark_calls == 0, "Pass 2 skips slot whose old address forget was refused" );
    __CPROVER_assert( slot_storage[0].seen == 1 && rocketnet_address_equal( &slot_storage[0].seen_address, &addr_a ),
                      "refused forget preserves old seen_address for retry" );

    /* Walk 2: forget(addr_a) succeeds, but mark(addr_b, hw_b) is refused. */
    model.phase = 0;
    model.forget_calls = 0;
    model.mark_calls = 0;
    model.forget_succeeds = true;
    model.mark_succeeds = false;
    rocketnet_server_replay_marks_walk( &marks, &slots, &map, &sink );

    __CPROVER_assert( model.forget_calls == 1 && !model.tracked_present, "retry forgets addr_a" );
    __CPROVER_assert( model.mark_calls == 1, "Pass 2 attempts mark(addr_b)" );
    __CPROVER_assert( slot_storage[0].seen == 1 && rocketnet_address_equal( &slot_storage[0].seen_address, &addr_b ),
                      "slot is now seen at addr_b" );
    __CPROVER_assert( slot_storage[0].mark_owed == 1 && slot_storage[0].has_mark == 0,
                      "refused mark sets mark_owed and leaves has_mark unset" );

    /* Walk 3: mark(addr_b, hw_b) succeeds on retry even though highest_opened did not change. */
    model.phase = 0;
    model.forget_calls = 0;
    model.mark_calls = 0;
    model.mark_succeeds = true;
    model.tracked_address = addr_b;
    rocketnet_server_replay_marks_walk( &marks, &slots, &map, &sink );

    __CPROVER_assert( model.forget_calls == 0 && model.mark_calls == 1, "owed mark is retried on next walk" );
    __CPROVER_assert( model.tracked_present && model.tracked_high_water == hw_b, "addr_b is marked at hw_b" );
    __CPROVER_assert( slot_storage[0].mark_owed == 0 && slot_storage[0].has_mark == 1 &&
                          slot_storage[0].marked_value == hw_b,
                      "successful retry clears mark_owed and records marked_value" );

    /* Walk 4: an unmarked seen slot (slot 1, seen == 1, has_mark == 0) that disconnects still forgets its address. */
    slot_storage[1].seen = 1;
    slot_storage[1].seen_address = addr_a;
    slot_storage[1].has_mark = 0;
    slot_storage[1].mark_owed = 1;
    slots.connected[1] = 0;

    model.phase = 0;
    model.forget_calls = 0;
    model.mark_calls = 0;
    model.tracked_address = addr_a;
    model.tracked_present = true;
    rocketnet_server_replay_marks_walk( &marks, &slots, &map, &sink );

    __CPROVER_assert( model.forget_calls == 1 && !model.tracked_present,
                      "unmarked seen slot still forgets its address on disconnect" );
    __CPROVER_assert( slot_storage[1].seen == 0 && slot_storage[1].mark_owed == 0,
                      "forgotten unmarked slot is cleared" );
}

int main( void )
{
    harness_two_phase_same_address_reuse();
    harness_refused_forget_and_mark_retry();
#if defined( REACHABILITY_CONTROL )
    __CPROVER_assert( 0, "replay-marks reachability control" );
#endif // #if defined( REACHABILITY_CONTROL )
    return 0;
}
