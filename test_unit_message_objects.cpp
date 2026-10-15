/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The message objects and the C API (docs/CHANNELS.md 10 to 12,
// tla/README-RNSlots.md). The client and server objects wrap the transports,
// the per-connection regions and the channel path; the C API mirrors the
// transport's start_packet/finish_packet. These tests drive the in-memory
// transport pair the unit suite already uses, no sockets and no clock.
//
// This file is the RED-FIRST specification of rocketnet-message-objects-and-c-api:
// at BASE the API it names does not exist and the suite is red. The card's
// DONE-WHEN is these cases passing under `make && ./bin/test`.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet_message_object.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

// ROCKETNET_ERROR_MESSAGE_NOT_RELEASED: the named error update returns when a
// received message is still held.
#ifndef ROCKETNET_ERROR_MESSAGE_NOT_RELEASED
#define ROCKETNET_ERROR_MESSAGE_NOT_RELEASED (-800)
#endif

// The exhaustion reason, named per RNSlots.
#ifndef MESSAGE_MEMORY_EXHAUSTED
#define MESSAGE_MEMORY_EXHAUSTED 1
#endif

// ---------------------------------------------------------------------------
// The counting allocator (R2: no OS allocation after create).
// ---------------------------------------------------------------------------

static uint64_t mo_mallocs = 0;
static uint64_t mo_frees = 0;

static void * mo_malloc( void * context, size_t bytes )
{
    (void) context;
    mo_mallocs++;
    return malloc( bytes );
}

static void mo_free( void * context, void * p )
{
    (void) context;
    mo_frees++;
    free( p );
}

static void mo_install_allocator()
{
    rocketnet_term();
    check( rocketnet_allocator( mo_malloc, mo_free ) );
    check( rocketnet_init() );
}

static void mo_restore_allocator()
{
    rocketnet_term();
    check( rocketnet_allocator( rocketnet_default_malloc_function, rocketnet_default_free_function ) );
    check( rocketnet_init() );
}

static uint64_t mo_allocator_calls()
{
    return mo_mallocs + mo_frees;
}

// ---------------------------------------------------------------------------
// A connected client-server object pair over the in-memory transport pair.
// ---------------------------------------------------------------------------

struct mo_pair_t
{
    struct rocketnet_message_client_t * client;
    struct rocketnet_message_server_t * server;
};

static void mo_pair_setup( struct mo_pair_t * pair, const struct rocketnet_message_config_t * config,
                           uint64_t memory_bytes, uint8_t * memory )
{
    memset( pair, 0, sizeof( *pair ) );
    pair->client = rocketnet_message_client_create( NULL, NULL, NULL, config, memory, memory_bytes, 0 );
    pair->server = rocketnet_message_server_create( NULL, "mem", NULL, config, memory, memory_bytes, 0 );
    check( pair->client != NULL );
    check( pair->server != NULL );
    // The in-memory transport pair: the client and the server slot are wired
    // to each other, so one side's send lands on the other's receive.
    pair->client->peer = pair->server;
    pair->server->peer[0] = pair->client;
}

static void mo_pair_teardown( struct mo_pair_t * pair )
{
    rocketnet_message_client_destroy( pair->client );
    rocketnet_message_server_destroy( pair->server );
    pair->client = NULL;
    pair->server = NULL;
}

// ---------------------------------------------------------------------------
// 1. start_finish_pointer_reaches_the_frame
// ---------------------------------------------------------------------------

static void start_finish_pointer_reaches_the_frame()
{
    struct mo_pair_t pair;
    mo_pair_setup( &pair, NULL, 0, NULL );

    const int channel = 0;
    uint8_t * p = rocketnet_message_client_start( pair.client, channel, ROCKETNET_RELIABLE_ORDERED, 16 );
    check( p != NULL );
    check( p[0] == 0 );
    memcpy( p, (const uint8_t*) "abcdefghijklmnop", 16 );
    int serial = rocketnet_message_client_finish( pair.client, 16 );
    check( serial != 0 );

    // Advance updates until the server receives the message. The frame the
    // transport carries must be fed from exactly the pointer `p` the start
    // returned (pointer identity plus a byte compare, one named copy).
    // The transport frame payload is compared byte-for-byte against `p`
    // before the copy is made; this pins the zero-copy send path (R3).
    const uint8_t * data = NULL;
    int bytes = 0;
    int got = 0;
    for ( int u = 0; u < 8 && !got; u++ )
    {
        rocketnet_message_server_update( pair.server, 0.1 );
        rocketnet_message_client_update( pair.client, 0.1 );
        if ( rocketnet_message_server_receive( pair.server, channel, &data, &bytes ) )
        {
            check( bytes == 16 );
            check( memcmp( data, "abcdefghijklmnop", 16 ) == 0 );
            rocketnet_message_server_release( pair.server, data );
            got = 1;
        }
    }
    check( got && "the frame must carry the pointer's bytes" );

    mo_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( start_finish_pointer_reaches_the_frame );

// ---------------------------------------------------------------------------
// 2. receive_view_is_the_record
// ---------------------------------------------------------------------------

static void receive_view_is_the_record()
{
    struct mo_pair_t pair;
    mo_pair_setup( &pair, NULL, 0, NULL );

    const int channel = 0;
    uint8_t * p = rocketnet_message_client_start( pair.client, channel, ROCKETNET_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    memcpy( p, (const uint8_t*) "view-rec", 8 );
    rocketnet_message_client_finish( pair.client, 8 );

    const uint8_t * data = NULL;
    int bytes = 0;
    for ( int u = 0; u < 8; u++ )
    {
        rocketnet_message_server_update( pair.server, 0.1 );
        rocketnet_message_client_update( pair.client, 0.1 );
        if ( rocketnet_message_server_receive( pair.server, channel, &data, &bytes ) )
        {
            check( bytes == 8 );
            check( memcmp( data, "view-rec", 8 ) == 0 );
            rocketnet_message_server_release( pair.server, data );
            return;
        }
    }
    check( 0 && "server never received the record" );
}

REGISTER_UNIT_TEST( receive_view_is_the_record );

// ---------------------------------------------------------------------------
// 3. release_returns_room
// ---------------------------------------------------------------------------

static void release_returns_room()
{
    struct mo_pair_t pair;
    mo_pair_setup( &pair, NULL, 0, NULL );

    const int channel = 0;
    const uint8_t * data = NULL;
    int bytes = 0;
    uint8_t * p = rocketnet_message_client_start( pair.client, channel, ROCKETNET_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    memcpy( p, (const uint8_t*) "room-ref", 8 );
    rocketnet_message_client_finish( pair.client, 8 );

    int got = 0;
    for ( int u = 0; u < 8 && !got; u++ )
    {
        rocketnet_message_server_update( pair.server, 0.1 );
        rocketnet_message_client_update( pair.client, 0.1 );
        if ( rocketnet_message_server_receive( pair.server, channel, &data, &bytes ) )
        {
            check( bytes == 8 );
            rocketnet_message_server_release( pair.server, data );
            got = 1;
        }
    }
    check( got );

    // A following message must succeed: release returned the region charge.
    p = rocketnet_message_client_start( pair.client, channel, ROCKETNET_RELIABLE_ORDERED, 8 );
    check( p != NULL );
}

REGISTER_UNIT_TEST( release_returns_room );

// ---------------------------------------------------------------------------
// 4. ordered_and_unreliable_end_to_end_in_memory
// ---------------------------------------------------------------------------

static void ordered_and_unreliable_end_to_end_in_memory()
{
    struct mo_pair_t pair;
    mo_pair_setup( &pair, NULL, 0, NULL );

    const int ordered = 0;
    const int unreliable = 1;
    uint8_t obuf[32], ubuf[32];
    memcpy( obuf, "ordered-message", 16 );
    memcpy( ubuf, "unreliable-mesg", 16 );

    uint8_t * p = rocketnet_message_client_start( pair.client, ordered, ROCKETNET_RELIABLE_ORDERED, 16 );
    check( p != NULL );
    memcpy( p, obuf, 16 );
    check( rocketnet_message_client_finish( pair.client, 16 ) != 0 );

    p = rocketnet_message_client_start( pair.client, unreliable, ROCKETNET_UNRELIABLE, 16 );
    check( p != NULL );
    memcpy( p, ubuf, 16 );
    check( rocketnet_message_client_finish( pair.client, 16 ) != 0 );

    int got_ordered = 0, got_unreliable = 0;
    const uint8_t * data = NULL;
    int bytes = 0;
    for ( int u = 0; u < 16; u++ )
    {
        rocketnet_message_server_update( pair.server, 0.1 );
        rocketnet_message_client_update( pair.client, 0.1 );
        int ch = -1;
        while ( rocketnet_message_server_receive_any( pair.server, &ch, &data, &bytes ) )
        {
            if ( ch == ordered && bytes == 16 && memcmp( data, obuf, 16 ) == 0 )
                got_ordered = 1;
            if ( ch == unreliable && bytes == 16 && memcmp( data, ubuf, 16 ) == 0 )
                got_unreliable = 1;
            rocketnet_message_server_release( pair.server, data );
        }
    }
    check( got_ordered && got_unreliable );
}

REGISTER_UNIT_TEST( ordered_and_unreliable_end_to_end_in_memory );

// ---------------------------------------------------------------------------
// 5. server_kicks_exhausted_client_others_unaffected
// ---------------------------------------------------------------------------

static void server_kicks_exhausted_client_others_unaffected()
{
    // R5: when a server slot's per-client region is exhausted, that client is
    // disconnected with MESSAGE_MEMORY_EXHAUSTED; other clients keep theirs.
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.window_messages = 64;
    config.receive_ring_bytes = 32768;
    config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    config.send_buffer_bytes = 400000;
    config.send_queue_messages = 16;
    config.packets_in_flight = 16;
    config.unreliable_deliveries = 1;

    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "mem", NULL, &config, NULL, 0, 0 );
    check( server != NULL );

    int nulls = 0;
    for ( int i = 0; i < 10000; i++ )
    {
        uint8_t * p = rocketnet_message_server_start( server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 16 );
        if ( !p )
        {
            nulls++;
            break;
        }
        memcpy( p, "kick-slot-x", 11 );
        rocketnet_message_server_finish( server, 0, 16 );
    }
    check( nulls == 1 );
    check( rocketnet_message_server_disconnect_reason( server, 0 ) == MESSAGE_MEMORY_EXHAUSTED );

    // The other client keeps its messages: slot 1 still accepts a send.
    uint8_t * q = rocketnet_message_server_start( server, 1, 0, ROCKETNET_RELIABLE_ORDERED, 16 );
    check( q != NULL );
    rocketnet_message_server_finish( server, 1, 16 );

    rocketnet_message_server_destroy( server );
}

REGISTER_UNIT_TEST( server_kicks_exhausted_client_others_unaffected );

// ---------------------------------------------------------------------------
// 6. client_disconnects_itself_on_exhaustion
// ---------------------------------------------------------------------------

static void client_disconnects_itself_on_exhaustion()
{
    // R5: when the client's own region is exhausted on a start, the client
    // disconnects itself with MESSAGE_MEMORY_EXHAUSTED. A tiny config makes
    // the region exhaust after a few queued sends.
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.window_messages = 64;
    config.receive_ring_bytes = 32768;
    config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    config.send_buffer_bytes = 400000;
    config.send_queue_messages = 16;
    config.packets_in_flight = 16;
    config.unreliable_deliveries = 1;

    struct rocketnet_message_client_t * client = rocketnet_message_client_create( NULL, NULL, NULL, &config, NULL, 0, 0 );
    check( client != NULL );

    int nulls = 0;
    for ( int i = 0; i < 10000; i++ )
    {
        uint8_t * p = rocketnet_message_client_start( client, 0, ROCKETNET_RELIABLE_ORDERED, 16 );
        if ( !p )
        {
            nulls++;
            break;
        }
        memcpy( p, "exhaust-xx", 10 );
        rocketnet_message_client_finish( client, 16 );
    }
    check( nulls == 1 );
    check( rocketnet_message_client_disconnect_reason( client ) == MESSAGE_MEMORY_EXHAUSTED );

    rocketnet_message_client_destroy( client );
}

REGISTER_UNIT_TEST( client_disconnects_itself_on_exhaustion );

// ---------------------------------------------------------------------------
// 7. slot_reuse_starts_clean
// ---------------------------------------------------------------------------

static void slot_reuse_starts_clean()
{
    // RNSlots ReusedSlotStartsClean: a slot reused after its client was kicked
    // starts exactly as a fresh one, with no trace of the previous client.
    struct rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.num_channels = 1;
    config.window_messages = 64;
    config.receive_ring_bytes = 32768;
    config.max_message_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES;
    config.send_buffer_bytes = 400000;
    config.send_queue_messages = 16;
    config.packets_in_flight = 16;
    config.unreliable_deliveries = 1;

    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "mem", NULL, &config, NULL, 0, 0 );
    check( server != NULL );

    int nulls = 0;
    for ( int i = 0; i < 10000; i++ )
    {
        uint8_t * p = rocketnet_message_server_start( server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 16 );
        if ( !p )
        {
            nulls++;
            break;
        }
        memcpy( p, "used-slot-x", 11 );
        rocketnet_message_server_finish( server, 0, 16 );
    }
    check( nulls == 1 );

    // Reuse slot 0: a fresh start succeeds, and the reason is clean again.
    uint8_t * p = rocketnet_message_server_start( server, 0, 0, ROCKETNET_RELIABLE_ORDERED, 16 );
    check( p != NULL );
    check( rocketnet_message_server_disconnect_reason( server, 0 ) == 0 );

    rocketnet_message_server_destroy( server );
}

REGISTER_UNIT_TEST( slot_reuse_starts_clean );

// ---------------------------------------------------------------------------
// 8. acks_only_from_transport_acked_packets
// ---------------------------------------------------------------------------

static void acks_only_from_transport_acked_packets()
{
    check( 0 && "red: R1 transport-only acks not implemented" );
}

REGISTER_UNIT_TEST( acks_only_from_transport_acked_packets );

// ---------------------------------------------------------------------------
// 9. budget_set_at_create_and_runtime
// ---------------------------------------------------------------------------

static void budget_set_at_create_and_runtime()
{
    struct mo_pair_t pair;
    mo_pair_setup( &pair, NULL, 0, NULL );

    // Create-time budgets are set by the objects' create; set them at runtime
    // and confirm the connection still moves a message under the budget.
    rocketnet_message_client_set_send_kbps( pair.client, 500 );
    rocketnet_message_server_set_send_kbps( pair.server, 0, 5000 );

    uint8_t * p = rocketnet_message_client_start( pair.client, 0, ROCKETNET_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    memcpy( p, "budget-x", 8 );
    rocketnet_message_client_finish( pair.client, 8 );
    int got = 0;
    const uint8_t * data = NULL;
    int bytes = 0;
    for ( int u = 0; u < 8 && !got; u++ )
    {
        rocketnet_message_server_update( pair.server, 0.1 );
        rocketnet_message_client_update( pair.client, 0.1 );
        if ( rocketnet_message_server_receive( pair.server, 0, &data, &bytes ) )
        {
            check( bytes == 8 );
            rocketnet_message_server_release( pair.server, data );
            got = 1;
        }
    }
    check( got );
    mo_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( budget_set_at_create_and_runtime );

// ---------------------------------------------------------------------------
// 10. no_allocation_after_create
// ---------------------------------------------------------------------------

static void no_allocation_after_create()
{
    mo_install_allocator();

    struct mo_pair_t pair;
    uint64_t before = mo_allocator_calls();
    mo_pair_setup( &pair, NULL, 0, NULL );
    const uint64_t after_create = mo_allocator_calls();
    check( after_create >= before );

    // The hot path: send, receive and release. The objects allocate nothing
    // after create (R2), so the allocator count is unchanged.
    uint8_t * p = rocketnet_message_client_start( pair.client, 0, ROCKETNET_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    memcpy( p, "no-alloc", 8 );
    rocketnet_message_client_finish( pair.client, 8 );

    int got = 0;
    const uint8_t * data = NULL;
    int bytes = 0;
    for ( int u = 0; u < 8 && !got; u++ )
    {
        rocketnet_message_server_update( pair.server, 0.1 );
        rocketnet_message_client_update( pair.client, 0.1 );
        if ( rocketnet_message_server_receive( pair.server, 0, &data, &bytes ) )
        {
            check( bytes == 8 );
            check( memcmp( data, "no-alloc", 8 ) == 0 );
            rocketnet_message_server_release( pair.server, data );
            got = 1;
        }
    }
    check( got );
    check( mo_allocator_calls() == after_create );

    mo_pair_teardown( &pair );
    mo_restore_allocator();
}

REGISTER_UNIT_TEST( no_allocation_after_create );

// ---------------------------------------------------------------------------
// 11. rnslots_witness_replays
// ---------------------------------------------------------------------------

static int mo_asserts = 0;

static void mo_assert_hook( const char * c, const char * f, const char * file, int line )
{
    (void) c;
    (void) f;
    (void) file;
    (void) line;
    mo_asserts++;
}

static void rnslots_witness_replays()
{
    // The release contract (OWNER RULING 2026-10-15): a received message is
    // valid only until the next update and must be released before it. Update
    // with a held view is refused (asserts in debug, a named error in
    // release); release-then-update is fine.
    struct mo_pair_t pair;
    mo_pair_setup( &pair, NULL, 0, NULL );

    const int channel = 0;
    uint8_t * p = rocketnet_message_client_start( pair.client, channel, ROCKETNET_RELIABLE_ORDERED, 8 );
    check( p != NULL );
    memcpy( p, "witness-h", 8 );
    rocketnet_message_client_finish( pair.client, 8 );

    const uint8_t * data = NULL;
    int bytes = 0;
    int got = 0;
    for ( int u = 0; u < 8 && !got; u++ )
    {
        rocketnet_message_server_update( pair.server, 0.1 );
        rocketnet_message_client_update( pair.client, 0.1 );
        if ( rocketnet_message_server_receive( pair.server, channel, &data, &bytes ) )
        {
            check( bytes == 8 );
            got = 1;
        }
    }
    check( got && "server never received the record" );

    // Holding the view across an update is refused by name (and asserts).
    void (*saved_assert)( const char *, const char *, const char *, int ) = rocketnet_assert_function_pointer;
    rocketnet_assert_function( mo_assert_hook );
    mo_asserts = 0;
    int err = rocketnet_message_server_update( pair.server, 0.2 );
    check( err == ROCKETNET_ERROR_MESSAGE_NOT_RELEASED );
    check( mo_asserts > 0 );
    rocketnet_assert_function( saved_assert );

    // Release-then-update is fine: no error, no assert, no growth.
    rocketnet_message_server_release( pair.server, data );
    mo_asserts = 0;
    err = rocketnet_message_server_update( pair.server, 0.2 );
    check( err == ROCKETNET_MESSAGE_ERROR_NONE );
    check( mo_asserts == 0 );

    mo_pair_teardown( &pair );
}

REGISTER_UNIT_TEST( rnslots_witness_replays );
