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
    (void) pair;
    (void) config;
    (void) memory_bytes;
    (void) memory;
    // Built by the objects card through rocketnet_message_client_create and
    // rocketnet_message_server_create over the transport pair, then the
    // handshake is driven until the client is connected.
    check( 0 && "mo_pair_setup: message objects not implemented (red)" );
}

static void mo_pair_teardown( struct mo_pair_t * pair )
{
    (void) pair;
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

REGISTER_UNIT_TEST( start_finish_pointer_reaches_the_frame )

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

REGISTER_UNIT_TEST( receive_view_is_the_record )

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

REGISTER_UNIT_TEST( release_returns_room )

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

REGISTER_UNIT_TEST( ordered_and_unreliable_end_to_end_in_memory )

// ---------------------------------------------------------------------------
// 5. server_kicks_exhausted_client_others_unaffected
// ---------------------------------------------------------------------------

static void server_kicks_exhausted_client_others_unaffected()
{
    // A server slot whose per-client region is exhausted disconnects only
    // that client with MESSAGE_MEMORY_EXHAUSTED and resets the slot; other
    // clients keep their messages. Built in the implementation card.
    check( 0 && "red: server kick-on-exhaustion not implemented" );
}

REGISTER_UNIT_TEST( server_kicks_exhausted_client_others_unaffected )

// ---------------------------------------------------------------------------
// 6. client_disconnects_itself_on_exhaustion
// ---------------------------------------------------------------------------

static void client_disconnects_itself_on_exhaustion()
{
    check( 0 && "red: client self-kick not implemented" );
}

REGISTER_UNIT_TEST( client_disconnects_itself_on_exhaustion )

// ---------------------------------------------------------------------------
// 7. slot_reuse_starts_clean
// ---------------------------------------------------------------------------

static void slot_reuse_starts_clean()
{
    check( 0 && "red: RNSlots ReusedSlotStartsClean not implemented" );
}

REGISTER_UNIT_TEST( slot_reuse_starts_clean )

// ---------------------------------------------------------------------------
// 8. acks_only_from_transport_acked_packets
// ---------------------------------------------------------------------------

static void acks_only_from_transport_acked_packets()
{
    check( 0 && "red: R1 transport-only acks not implemented" );
}

REGISTER_UNIT_TEST( acks_only_from_transport_acked_packets )

// ---------------------------------------------------------------------------
// 9. budget_set_at_create_and_runtime
// ---------------------------------------------------------------------------

static void budget_set_at_create_and_runtime()
{
    check( 0 && "red: R6 send budget not implemented" );
}

REGISTER_UNIT_TEST( budget_set_at_create_and_runtime )

// ---------------------------------------------------------------------------
// 10. no_allocation_after_create
// ---------------------------------------------------------------------------

static void no_allocation_after_create()
{
    check( 0 && "red: R2 no allocation after create not implemented" );
}

REGISTER_UNIT_TEST( no_allocation_after_create )

// ---------------------------------------------------------------------------
// 11. rnslots_witness_replays
// ---------------------------------------------------------------------------

static void rnslots_witness_replays()
{
    // The five reversed witnesses of tla/README-RNSlots.md, as unit tests:
    // no cross-connection apply, reset on reuse, exhaustion only leads to
    // disconnect, release returns the region charge, and update with held
    // views is refused (asserts in debug, ROCKETNET_ERROR_MESSAGE_NOT_RELEASED
    // in release).
    check( 0 && "red: RNSlots witness replays not implemented" );
}

REGISTER_UNIT_TEST( rnslots_witness_replays )
