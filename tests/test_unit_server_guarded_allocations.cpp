/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Server-side allocations under a counting allocator that puts a guard
// pattern after every block (the shape of test_unit_token_table_bounds.cpp's):
// a write one past an array lands in the guard and is seen at free, and every
// block is counted, so a leak or a refused allocation is seen too.
//
// - The replay-mark records (sdk/rocketnet_server_replay_marks.cpp): the plan
//   is 0 below one client and exactly what create asks for at and above it;
//   create refuses fewer than one client before allocating; a refused
//   allocation at either step returns NULL and leaves nothing live; destroy
//   returns both blocks.
// - The acked batch (sdk/rocketnet_server_protocol.cpp, the sealed path): an
//   ack block that fills the batch to its capacity tags exactly the entries it
//   appended with their client, and writes nothing past either array.
//
// A client and a server protocol run in memory on a mocked clock that starts
// at 1000.0: no sockets, no network, no real time. Everything big is heap held.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_memory.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_server_replay_marks.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"
#include "loopback_hub.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

// ---- a counting allocator with a guard pattern after every block -------------

enum
{
    GA_GUARD_BYTES = 128
};

static const uint8_t GA_GUARD = 0x5A;

struct ga_allocator_t
{
    int live;
    int calls;
    int refuse_from;                // when > 0, call refuse_from and every later one is refused
    int refused;
    int overwritten;
    uint64_t requested;
};

// the pointer rocketnet_malloc hands out from the block the allocator returned;
// an ASan build's overhead also holds the red zone after the array
// (sdk/rocketnet_memory.h), which is not in front of the pointer
static uint8_t * ga_handed_out( const uint8_t * block )
{
    const uintptr_t overhead = (uintptr_t) rocketnet_allocation_overhead() - ROCKETNET_RED_ZONE_BYTES;
    return (uint8_t *) ( ( (uintptr_t) block + overhead ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
}

// the block's base and its requested size sit in the 16 bytes before it
static void * ga_allocate( void * context, size_t bytes )
{
    ga_allocator_t * state = (ga_allocator_t *) context;
    ++state->calls;
    if ( state->refuse_from > 0 && state->calls >= state->refuse_from )
    {
        ++state->refused;
        return NULL;
    }

    void * base = malloc( bytes + GA_GUARD_BYTES + 128 );
    if ( !base )
    {
        return NULL;
    }

    uintptr_t address = ( ( (uintptr_t) base + sizeof( void * ) + 63 ) & ~(uintptr_t) 63 ) + 16;
    memcpy( (void *) ( address - sizeof( void * ) ), &base, sizeof( base ) );
    memcpy( (void *) ( address - 2 * sizeof( void * ) ), &bytes, sizeof( bytes ) );
    memset( (void *) address, GA_GUARD, bytes + GA_GUARD_BYTES );
    ++state->live;
    state->requested += bytes;
    return (void *) address;
}

// every byte from the end of the array the library asked for to the end of
// the guard must still be the guard: a write one past an array lands there
static void ga_release( void * context, void * pointer )
{
    ga_allocator_t * state = (ga_allocator_t *) context;
    void * base = NULL;
    size_t bytes = 0;
    memcpy( &base, (uint8_t *) pointer - sizeof( void * ), sizeof( base ) );
    memcpy( &bytes, (uint8_t *) pointer - 2 * sizeof( void * ), sizeof( bytes ) );

    const uint8_t * end = ga_handed_out( (const uint8_t *) pointer ) + ( bytes - (size_t) rocketnet_allocation_overhead() );
    const uint8_t * guard_end = (const uint8_t *) pointer + bytes + GA_GUARD_BYTES;
    for ( const uint8_t * p = end; p < guard_end; ++p )
    {
        if ( *p != GA_GUARD )
        {
            ++state->overwritten;
            break;
        }
    }

    --state->live;
    free( base );
}

// the worker starts initialized: terminate, install the counting allocator,
// and initialize with its state as the context
static void ga_allocator_begin( ga_allocator_t * state )
{
    memset( state, 0, sizeof( *state ) );
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );

    int status = -1;
    check( rocketnet_init_internal( state, ga_allocate, ga_release, &status ) );
    check( status == ROCKETNET_STATUS_OK );
}

static void ga_allocator_end( ga_allocator_t * state )
{
    check( rocketnet_test_loopback_hub_release() );
    check( rocketnet_term_internal() == ROCKETNET_STATUS_OK );
    check( state->live == 0 );
    check( state->overwritten == 0 );
    check( rocketnet_allocator_internal( rocketnet_default_malloc_function, rocketnet_default_free_function ) == ROCKETNET_STATUS_OK );
}

static void ga_allocator_mark( ga_allocator_t * state )
{
    state->calls = 0;
    state->refuse_from = 0;
    state->refused = 0;
    state->requested = 0;
}

// ---- the replay-mark records ------------------------------------------------

static bool ga_sink_forget( void * context, const struct rocketnet_address_t * )
{
    ( *(int *) context )++;
    return true;
}

static bool ga_sink_mark( void * context, const struct rocketnet_address_t *, uint32_t )
{
    ( *(int *) context )++;
    return true;
}

static void test_replay_marks_plan_create_and_destroy_at_their_bounds()
{
    // the plan: nothing below one client, then linear in the clients

    check( rocketnet_server_replay_marks_allocation_bytes( INT_MIN ) == 0 );
    check( rocketnet_server_replay_marks_allocation_bytes( -1 ) == 0 );
    check( rocketnet_server_replay_marks_allocation_bytes( 0 ) == 0 );
    const uint64_t one = rocketnet_server_replay_marks_allocation_bytes( 1 );
    const uint64_t two = rocketnet_server_replay_marks_allocation_bytes( 2 );
    check( one > 0 );
    check( two > one );
    check( rocketnet_server_replay_marks_allocation_bytes( 7 ) == one + 6 * ( two - one ) );

    ga_allocator_t state;
    ga_allocator_begin( &state );
    const int live = state.live;

    // fewer than one client is refused before the allocator is asked

    const int refused_counts[3] = { INT_MIN, -1, 0 };
    for ( int i = 0; i < 3; i++ )
    {
        ga_allocator_mark( &state );
        check( rocketnet_server_replay_marks_create( &state, refused_counts[i] ) == NULL );
        check( state.calls == 0 );
        check( state.live == live );
    }

    // at and above one client, create asks for exactly its plan; walked and
    // destroyed, it returns every block and wrote nothing past one

    const int counts[3] = { 1, 2, 7 };
    for ( int i = 0; i < 3; i++ )
    {
        ga_allocator_mark( &state );
        struct rocketnet_server_replay_marks_t * marks = rocketnet_server_replay_marks_create( &state, counts[i] );
        check( marks != NULL );
        check( state.calls == 2 );
        check( state.requested == rocketnet_server_replay_marks_allocation_bytes( counts[i] ) );
        check( state.live == live + 2 );

        struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( &state, counts[i] );
        struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( &state, counts[i] );
        check( slots != NULL && map != NULL );
        int calls = 0;
        struct rocketnet_server_replay_sink_t sink = { &calls, ga_sink_forget, ga_sink_mark };
        rocketnet_server_replay_marks_walk( marks, slots, map, &sink );
        check( calls == 0 );
        rocketnet_client_slots_destroy( slots );
        rocketnet_encryption_map_destroy( map );

        rocketnet_server_replay_marks_destroy( marks );
        check( state.live == live );
        check( state.overwritten == 0 );
    }

    // a refused allocation at either step: NULL, and nothing left live

    for ( int refuse = 1; refuse <= 2; refuse++ )
    {
        ga_allocator_mark( &state );
        state.refuse_from = refuse;
        check( rocketnet_server_replay_marks_create( &state, 3 ) == NULL );
        check( state.refused == 1 );
        check( state.live == live );
    }
    ga_allocator_mark( &state );

    ga_allocator_end( &state );
}

REGISTER_UNIT_TEST( test_replay_marks_plan_create_and_destroy_at_their_bounds );

// ---- the client slots -----------------------------------------------------------

// The bucket count is twice the capacity in a signed int, so create refuses a
// capacity above INT_MAX / 2 before asking the allocator for anything. At
// INT_MAX / 2 it does ask (the first request is refused here, so nothing
// large is ever allocated) and returns NULL with nothing live.

static void ga_quiet_assert( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
}

static void test_client_slots_create_refuses_above_half_int_before_allocating()
{
    // a count below one also trips create's debug assert: the refusal is what is checked here
    rocketnet_assert_function( ga_quiet_assert );

    ga_allocator_t state;
    ga_allocator_begin( &state );
    const int live = state.live;

    const int refused_counts[3] = { INT_MAX / 2 + 1, INT_MAX, 0 };
    for ( int i = 0; i < 3; i++ )
    {
        ga_allocator_mark( &state );
        check( rocketnet_client_slots_create( &state, refused_counts[i] ) == NULL );
        check( state.calls == 0 );
        check( state.live == live );
    }

    ga_allocator_mark( &state );
    state.refuse_from = 1;
    check( rocketnet_client_slots_create( &state, INT_MAX / 2 ) == NULL );
    check( state.calls >= 1 );
    check( state.refused >= 1 );
    check( state.live == live );
    ga_allocator_mark( &state );

    ga_allocator_end( &state );

    rocketnet_assert_function( NULL );
}

REGISTER_UNIT_TEST( test_client_slots_create_refuses_above_half_int_before_allocating );

// A slot released in this update is held above the free stack's top until
// publish. An assign before publish pops the top and moves the held index
// down into the popped entry, so after publish every free index is free and
// every connected one is not: the next two assigns take the released slot and
// the one never used, never the slot just assigned, and a full table refuses
// the third. Each tenure has its own owner generation.

static void test_client_slots_assign_before_publish_keeps_tenures_apart()
{
    ga_allocator_t state;
    ga_allocator_begin( &state );

    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( &state, 4 );
    check( slots != NULL );

    uint8_t * user_data = (uint8_t *) calloc( 1, ROCKETNET_USER_DATA_BYTES );
    check( user_data != NULL );

    check( rocketnet_client_slots_assign( slots, 0xA0, 10, 1, user_data ) == 0 );
    check( rocketnet_client_slots_assign( slots, 0xB0, 11, 2, user_data ) == 1 );

    rocketnet_client_slots_release( slots, 0 );
    check( rocketnet_client_slots_num_held( slots ) == 1 );
    check( !rocketnet_client_slots_connected( slots, 0 ) );

    // the released slot is not poppable in the update that freed it

    check( rocketnet_client_slots_assign( slots, 0xC0, 12, 3, user_data ) == 2 );

    rocketnet_client_slots_publish( slots );
    check( rocketnet_client_slots_num_held( slots ) == 0 );

    check( rocketnet_client_slots_assign( slots, 0xD0, 13, 4, user_data ) == 0 );
    check( rocketnet_client_slots_assign( slots, 0xE0, 14, 5, user_data ) == 3 );
    check( rocketnet_client_slots_assign( slots, 0xF0, 15, 6, user_data ) == -1 );

    const uint64_t ids[4] = { 0xD0, 0xB0, 0xC0, 0xE0 };
    const int mappings[4] = { 13, 11, 12, 14 };
    const uint64_t generations[4] = { 4, 2, 3, 5 };
    for ( int i = 0; i < 4; i++ )
    {
        check( rocketnet_client_slots_connected( slots, i ) );
        check( rocketnet_client_slots_client_id( slots, i ) == ids[i] );
        check( rocketnet_client_slots_mapping_index( slots, i ) == mappings[i] );
        check( rocketnet_client_slots_owner_generation( slots, i ) == generations[i] );
        check( rocketnet_client_slots_find( slots, ids[i] ) == i );
    }
    check( rocketnet_client_slots_find( slots, 0xA0 ) == -1 );
    check( rocketnet_client_slots_find( slots, 0xF0 ) == -1 );
    check( rocketnet_client_slots_num_connected( slots ) == 4 );

    free( user_data );
    rocketnet_client_slots_destroy( slots );
    check( state.overwritten == 0 );

    ga_allocator_end( &state );
}

REGISTER_UNIT_TEST( test_client_slots_assign_before_publish_keeps_tenures_apart );

// ---- the acked batch at its capacity ------------------------------------------

#define GA_TIMEOUT 10

struct ga_world_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    int end_reasons[1];
    struct rocketnet_address_t address;
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes;
};

static void ga_mint( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = GA_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = 0x6A11ULL;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static int ga_deliver( struct ga_world_t * world, int bytes, double time )
{
    world->response_bytes = 0;
    int payload_bytes = 0;
    int payload_client_index = -1;
    return rocketnet_server_protocol_process_packet( &world->server, &world->address, world->packet, bytes, ROCKETNET_TEST_CREATE_TS, time,
                                                     world->response, &world->response_bytes,
                                                     world->payload, &payload_bytes, &payload_client_index );
}

// one slot, one client connected through the handshake at 1000.0
static struct ga_world_t * ga_world_create( void * context )
{
    struct ga_world_t * world = (struct ga_world_t *) calloc( 1, sizeof( struct ga_world_t ) );
    check( world != NULL );

    struct rocketnet_server_protocol_t * server = &world->server;
    server->protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server->packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server->backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server->server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server->server_secret_key, rocketnet_test_server_secret_key, 32 );
    check( rocketnet_address_parse( &server->advertised_ipv4, "203.0.113.10:40000" ) );
    memcpy( server->cookie_key, rocketnet_test_cookie_key, 32 );
    server->cookie_lifetime_seconds = 3;
    server->handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server->handshake_deadline_seconds = 5.0;
    server->encryption_map = rocketnet_encryption_map_create( context, 2 );
    server->used_token_history = rocketnet_used_token_history_create( context, 4 );
    server->client_slots = rocketnet_client_slots_create( context, 1 );
    server->acks = rocketnet_server_acks_create( context, 1 );
    server->end_reasons = world->end_reasons;
    server->counters = &world->server_counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, 1000.0 );
    check( rocketnet_address_parse( &world->address, "198.51.100.7:56789" ) );

    rocketnet_client_protocol_init( &world->client, &world->client_counters );
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    ga_mint( token_data );
    check( rocketnet_client_protocol_connect( &world->client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );

    int payload_bytes = 0;
    for ( int i = 0; i < 3; i++ )
    {
        const double sent = 1000.0 + 0.2 * i;
        int bytes = 0;
        rocketnet_client_protocol_update( &world->client, sent, world->packet, &bytes );
        check( bytes > 0 );
        (void) ga_deliver( world, bytes, sent );
        check( world->response_bytes > 0 );
        check( rocketnet_client_protocol_process_packet( &world->client, world->response, world->response_bytes, sent,
                                                         world->payload, (int) sizeof( world->payload ), &payload_bytes ) );
    }
    check( world->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_client_slots_connected( server->client_slots, 0 ) );
    return world;
}

static void ga_world_destroy( struct ga_world_t * world )
{
    rocketnet_counters_assert_conserved( &world->server_counters );
    rocketnet_encryption_map_destroy( world->server.encryption_map );
    rocketnet_used_token_history_destroy( world->server.used_token_history );
    rocketnet_client_slots_destroy( world->server.client_slots );
    rocketnet_server_acks_destroy( world->server.acks );
    free( world );
}

// The server has sent a full history of packets that carry ids; one ack
// packet acknowledges them all. The batch fills to exactly its capacity, each
// entry is tagged with the client, and freeing the arrays finds no write past
// either of them.

static void test_server_ack_that_fills_the_acked_batch_writes_nothing_past_it()
{
    ga_allocator_t state;
    ga_allocator_begin( &state );

    struct ga_world_t * world = ga_world_create( &state );
    struct rocketnet_server_acks_t * acks = world->server.acks;
    const int capacity = acks->acked_capacity;
    check( capacity == ROCKETNET_SENT_PACKET_BUFFER_SIZE );

    // the handshake's last answer was the server's first session packet (a
    // keep-alive, no id): the packets with ids follow it, a full history

    struct rocketnet_ack_endpoint_t * endpoint = &acks->endpoints[0];
    const uint64_t first = endpoint->highest_sequence + 1;
    const uint64_t last = first + (uint64_t) capacity - 1;
    for ( uint64_t s = first; s <= last; s++ )
    {
        rocketnet_ack_endpoint_record_sent( endpoint, s, 100, 0x10000 + s, 1, 1000.5 );
    }
    check( endpoint->highest_sequence == last );

    struct rocketnet_ack_block_t block;
    memset( &block, 0, sizeof( block ) );
    block.largest = last;
    block.first_range_length = (uint32_t) capacity;
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    const int block_bytes = rocketnet_ack_block_write( plaintext, &block );
    check( block_bytes == ROCKETNET_ACK_BLOCK_MIN_BYTES );

    const int mapping_index = rocketnet_encryption_map_find( world->server.encryption_map, &world->address );
    check( mapping_index != -1 );
    struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( world->server.encryption_map, mapping_index );
    const int bytes = rocketnet_write_sealed_packet( world->packet, ROCKETNET_CONNECTION_ACK_PACKET, rocketnet_test_filter_value, 1, NULL,
                                                     plaintext, block_bytes, ROCKETNET_TEST_PROTOCOL_ID,
                                                     mapping->connection_salt, NULL, mapping->keys.session_client_to_server );
    check( bytes > 0 );

    check( acks->num_acked == 0 );
    check( ga_deliver( world, bytes, 1000.6 ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    check( acks->num_acked == capacity );
    check( world->server_counters.values[ROCKETNET_COUNTER_ACKED_BATCH_FULL] == 0 );
    for ( int i = 0; i < capacity; i++ )
    {
        check( acks->acked_client_index[i] == 0 );
    }

    ga_world_destroy( world );
    check( state.overwritten == 0 );
    ga_allocator_end( &state );
}

REGISTER_UNIT_TEST( test_server_ack_that_fills_the_acked_batch_writes_nothing_past_it );
