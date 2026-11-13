/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The session replay window at every edge, asserted on what the server does
// (standard/TRANSPORT.md t14 Replay protection; sdk/rocketnet_replay.h;
// proofs/tla/ReplayWindow.tla). With W = ROCKETNET_REPLAY_WINDOW_BITS and H the
// highest sequence opened, a sequence above H always passes and moves H; one
// in (H - W, H] passes exactly once; one at or below H - W is refused.
//
// First on the window itself, against an independent reference (the set of
// sequences received and the floor), then through the server's sealed path:
// the first sequence, the last in the window, exactly at its edge and one
// past, a jump of exactly W, W - 1 and far ahead, the wire's 32-bit wrap and
// the 64-bit top, a duplicate before and after the window has moved, and a
// refused replay that costs nothing but its counters (no reply, no byte
// of server, mapping or slot state changed). Then the slot reuse: a new
// connection in a slot whose last connection timed out, at the same address
// and at another, accepts every sequence the old one had received, and the
// replay-mark walk marks the new session at zero (overwriting the old
// session's kernel entry, proofs/tla/RNXdpReplay.tla
// NewSessionNeverDroppedByOldMark).
//
// And two edges of the server's session sends: a keep-alive carrying the
// largest ack block the window can produce, and the keep-alive interval at
// exactly 1 / send_rate.
//
// A client and a server protocol run in memory on a mocked clock that starts
// at 1000.0: no sockets, no network, no real time. Everything big is heap held.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_read_write.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_server_replay_marks.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <set>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#if defined( __has_feature )
#if __has_feature( memory_sanitizer )
#include <sanitizer/msan_interface.h>
#define EDGES_MSAN 1
#endif // #if __has_feature( memory_sanitizer )
#endif // #if defined( __has_feature )

// Whether live state is the same bytes as the copy taken of it before a
// refused call. The ack endpoint is library memory written member by member,
// so its padding and any member not yet written hold no value. A memcmp of it
// reads them, and MemorySanitizer reports the compare (bench-nyc-3,
// 2026-11-12). So the compare runs on two fresh copies, and only the copies are
// unpoisoned. The live state keeps its shadow, so a library read of a byte
// nothing wrote is still reported where it happens, and a refused call that
// writes any byte of the state still fails the compare.

static bool edges_same_bytes( const void * before, const void * live, size_t bytes )
{
    uint8_t * a = (uint8_t *) malloc( bytes );
    uint8_t * b = (uint8_t *) malloc( bytes );
    check( a != NULL && b != NULL );
    memcpy( a, before, bytes );
    memcpy( b, live, bytes );
#if defined( EDGES_MSAN )
    __msan_unpoison( a, bytes );
    __msan_unpoison( b, bytes );
#endif // #if defined( EDGES_MSAN )
    const bool same = memcmp( a, b, bytes ) == 0;
    free( a );
    free( b );
    return same;
}


#define EDGES_SLOTS 4
#define EDGES_TIMEOUT 10            // the token's timeout seconds

static const uint64_t edges_window = (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS;

// ---------------------------------------------------------------------------
// The window alone, against the reference of the standard's rule: refused
// when s + W <= H or s was received; an advance raises H to s when s > H.
// ---------------------------------------------------------------------------

struct edges_reference_t
{
    uint64_t highest;
    std::set<uint64_t> received;

    // s + W <= H, written so that it never overflows near the 64-bit top
    bool below( uint64_t s ) const { return highest >= edges_window && s <= highest - edges_window; }

    int already_received( uint64_t s ) const
    {
        if ( below( s ) )
        {
            return 1;
        }
        return received.count( s ) != 0;
    }

    void advance( uint64_t s )
    {
        if ( s > highest )
        {
            highest = s;
        }
        received.insert( s );
        while ( !received.empty() && below( *received.begin() ) )
        {
            received.erase( received.begin() );
        }
    }
};

// every probe the reference and the window must agree on around H and its floor

static void edges_compare( const edges_reference_t & reference, const struct rocketnet_replay_protection_t * replay )
{
    const uint64_t h = reference.highest;
    uint64_t probes[12];
    int count = 0;
    probes[count++] = h + 1;
    probes[count++] = h + 2;
    probes[count++] = h + edges_window;
    if ( h > 0 )
    {
        probes[count++] = h;
    }
    if ( h > 1 )
    {
        probes[count++] = h - 1;
    }
    if ( h >= edges_window )
    {
        probes[count++] = h - edges_window + 1;
        if ( h - edges_window + 2 <= h )
        {
            probes[count++] = h - edges_window + 2;
        }
        if ( h > edges_window )
        {
            probes[count++] = h - edges_window;
        }
        if ( h > edges_window + 1 )
        {
            probes[count++] = h - edges_window - 1;
        }
    }
    probes[count++] = 1;

    for ( int i = 0; i < count; i++ )
    {
        if ( probes[i] == 0 )
        {
            continue;
        }
        check( rocketnet_replay_already_received( replay, probes[i] ) == reference.already_received( probes[i] ) );
    }
}

// one receive: the window and the reference agree that s is new or not, and a
// new s is advanced in both

static int edges_receive( edges_reference_t & reference, struct rocketnet_replay_protection_t * replay, uint64_t s )
{
    const int refused = reference.already_received( s );
    check( rocketnet_replay_already_received( replay, s ) == refused );
    if ( !refused )
    {
        const uint64_t old = reference.highest;
        rocketnet_replay_advance( replay, s );
        reference.advance( s );

        // every sequence the advance skipped that is inside the new window
        // was never received: none may inherit a bit from an earlier lap

        if ( s > old && s - old > 1 )
        {
            const uint64_t first = s - old > edges_window ? s - edges_window + 1 : old + 1;
            for ( uint64_t t = first; t < s; t++ )
            {
                check( !rocketnet_replay_already_received( replay, t ) );
            }
        }
    }
    check( replay->highest_opened == reference.highest );
    edges_compare( reference, replay );
    return !refused;
}

// the named edges, each by its own expectation, then a long walk of steps
// that land on and around every edge, checked against the reference

static void test_replay_window_edges_against_the_rule()
{
    struct rocketnet_replay_protection_t * replay = (struct rocketnet_replay_protection_t *) calloc( 1, sizeof( struct rocketnet_replay_protection_t ) );
    check( replay != NULL );
    edges_reference_t reference;
    reference.highest = 0;

    const uint64_t W = edges_window;

    // the first sequence of a session, then its duplicate

    check( edges_receive( reference, replay, 1 ) == 1 );
    check( edges_receive( reference, replay, 1 ) == 0 );
    check( edges_receive( reference, replay, 2 ) == 1 );

    // a jump of exactly W (the whole ring cleared): the floor is H - W + 1 = 3,
    // 2 is one past the edge (refused), 3 is exactly at it (taken once)

    check( edges_receive( reference, replay, W + 2 ) == 1 );
    check( edges_receive( reference, replay, 2 ) == 0 );
    check( edges_receive( reference, replay, 3 ) == 1 );
    check( edges_receive( reference, replay, 3 ) == 0 );
    check( edges_receive( reference, replay, W + 1 ) == 1 );
    check( edges_receive( reference, replay, W + 2 ) == 0 );

    // a jump of W - 1 (the longest partial clear): W + 2 is still in the
    // window and still refused; W + 1 has fallen out (refused); W + 3, never
    // received, is the new floor and taken

    const uint64_t h2 = W + 2 + W - 1;
    check( edges_receive( reference, replay, h2 ) == 1 );
    check( edges_receive( reference, replay, W + 2 ) == 0 );
    check( edges_receive( reference, replay, W + 1 ) == 0 );
    check( edges_receive( reference, replay, W + 3 ) == 1 );

    // a jump of W + 1 and one far ahead: nothing received before survives as
    // a refusal inside the new window, and the old top is refused below it

    check( edges_receive( reference, replay, h2 + W + 1 ) == 1 );
    check( edges_receive( reference, replay, h2 + 2 ) == 1 );
    check( edges_receive( reference, replay, h2 + 1 ) == 0 );
    const uint64_t far = h2 + 1000 * W + 17;
    check( edges_receive( reference, replay, far ) == 1 );
    check( edges_receive( reference, replay, far - W + 1 ) == 1 );
    check( edges_receive( reference, replay, far - W ) == 0 );

    // the ring index wraps at every multiple of W, and the wire's 32-bit
    // sequence at 2^32 (a multiple of W): the last before and the first after
    // are both taken once

    const uint64_t wrap = 1ULL << 32;
    check( edges_receive( reference, replay, wrap - 1 ) == 1 );
    check( edges_receive( reference, replay, wrap ) == 1 );
    check( edges_receive( reference, replay, wrap - 1 ) == 0 );
    check( edges_receive( reference, replay, wrap - 2 ) == 1 );
    check( edges_receive( reference, replay, wrap + 1 ) == 1 );
    check( edges_receive( reference, replay, wrap - W + 2 ) == 1 );
    check( edges_receive( reference, replay, wrap - W + 1 ) == 0 );

    // the 64-bit top: the last sequence the type holds is taken once, and the
    // window below it keeps its rule

    check( edges_receive( reference, replay, UINT64_MAX - W ) == 1 );
    check( edges_receive( reference, replay, UINT64_MAX ) == 1 );
    check( edges_receive( reference, replay, UINT64_MAX ) == 0 );
    check( edges_receive( reference, replay, UINT64_MAX - W ) == 0 );
    check( edges_receive( reference, replay, UINT64_MAX - W + 1 ) == 1 );
    check( edges_receive( reference, replay, UINT64_MAX - 1 ) == 1 );

    // the walk: a fresh window, steps that land exactly on, one inside and one
    // past each edge, forward and back, compared after every step

    memset( replay, 0, sizeof( *replay ) );
    reference.highest = 0;
    reference.received.clear();

    const uint64_t steps[] = { 1, 2, 63, 64, 65, W - 1, W, W + 1, 2 * W - 1, 2 * W, 2 * W + 1 };
    const int step_count = (int) ( sizeof( steps ) / sizeof( steps[0] ) );
    uint32_t lcg = 12345u;
    uint64_t s = 1;
    int taken = 0;
    for ( int i = 0; i < 4000; i++ )
    {
        lcg = lcg * 1103515245u + 12345u;
        const uint64_t step = steps[( lcg >> 16 ) % (uint32_t) step_count];
        const bool back = ( ( lcg >> 8 ) & 3u ) == 0;
        if ( back )
        {
            const uint64_t below = reference.highest > step ? reference.highest - step : 1;
            s = below;
        }
        else
        {
            s = reference.highest + step;
        }
        taken += edges_receive( reference, replay, s );
    }
    check( taken > 1000 );
    check( taken < 4000 );

    free( replay );
}

REGISTER_UNIT_TEST( test_replay_window_edges_against_the_rule );

// A full window: every sequence 1 .. H received, so every ring position holds
// a bit. Then one advance by each skip from each ring offset. Every sequence
// still inside the window stays refused (a clear that runs past the skipped
// range would let a received one in again), every skipped one is new (a clear
// that stops short would refuse one never received), and the edge holds.

static void test_replay_window_full_window_keeps_every_mark_across_a_skip()
{
    const uint64_t W = edges_window;
    struct rocketnet_replay_protection_t * full = (struct rocketnet_replay_protection_t *) calloc( 1, sizeof( struct rocketnet_replay_protection_t ) );
    struct rocketnet_replay_protection_t * replay = (struct rocketnet_replay_protection_t *) calloc( 1, sizeof( struct rocketnet_replay_protection_t ) );
    check( full != NULL && replay != NULL );

    const uint64_t starts[] = { 0, 1, 63, 64, 65, W / 2 + 7, W - 64, W - 63, W - 1 };
    const uint64_t skips[] = { 1, 2, 3, 63, 64, 65, 127, 128, 129, W / 2, W - 65, W - 64, W - 63, W - 2, W - 1, W, W + 1 };

    for ( const uint64_t start : starts )
    {
        memset( full, 0, sizeof( *full ) );
        const uint64_t h = W + start;
        for ( uint64_t s = 1; s <= h; s++ )
        {
            rocketnet_replay_advance( full, s );
        }

        for ( const uint64_t skip : skips )
        {
            *replay = *full;
            const uint64_t top = h + skip;
            check( !rocketnet_replay_already_received( replay, top ) );
            rocketnet_replay_advance( replay, top );
            check( replay->highest_opened == top );

            for ( uint64_t s = top - W + 1; s <= h; s++ )
            {
                check( rocketnet_replay_already_received( replay, s ) );
            }
            for ( uint64_t s = h + 1 > top - W + 1 ? h + 1 : top - W + 1; s < top; s++ )
            {
                check( !rocketnet_replay_already_received( replay, s ) );
            }
            check( rocketnet_replay_already_received( replay, top ) );
            check( rocketnet_replay_already_received( replay, top - W ) );
        }
    }

    free( full );
    free( replay );
}

REGISTER_UNIT_TEST( test_replay_window_full_window_keeps_every_mark_across_a_skip );

// ---------------------------------------------------------------------------
// Through the server: a connected client's session packets, sealed by hand
// under its mapping's keys at chosen 64-bit sequences (the wire carries the
// low 32 bits; the server reconstructs against its highest opened).
// ---------------------------------------------------------------------------

struct edges_world_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t server_counters;
    int end_reasons[EDGES_SLOTS];
    struct rocketnet_client_protocol_t client;
    struct rocketnet_counters_t client_counters;
    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int response_bytes;
    int payload_bytes;
};

// what a refusal must leave as it was: everything but its counters
struct edges_state_t
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_encryption_mapping_t mapping;
    struct rocketnet_encryption_mapping_control_t control;
    struct rocketnet_ack_endpoint_t endpoint;
    struct rocketnet_counters_t counters;
    int connected;
    int confirmed;
    int num_acked;
};

static void edges_mint( uint8_t * token_data, uint64_t client_id, uint64_t expire_timestamp )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof( signed_fields ) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = expire_timestamp;
    signed_fields.timeout_seconds = EDGES_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof( private_fields ) );
    private_fields.client_id = client_id;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = expire_timestamp;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

static int edges_deliver( struct edges_world_t * world, const struct rocketnet_address_t * from, int bytes, double time )
{
    world->response_bytes = 0;
    world->payload_bytes = 0;
    int payload_client_index = -1;
    return rocketnet_server_protocol_process_packet( &world->server, from, world->packet, bytes, ROCKETNET_TEST_CREATE_TS, time,
                                                     world->response, &world->response_bytes,
                                                     world->payload, &world->payload_bytes, &payload_client_index );
}

static struct edges_world_t * edges_world_create()
{
    struct edges_world_t * world = (struct edges_world_t *) calloc( 1, sizeof( struct edges_world_t ) );
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
    server->encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server->used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server->client_slots = rocketnet_client_slots_create( NULL, EDGES_SLOTS );
    server->acks = rocketnet_server_acks_create( NULL, EDGES_SLOTS );
    server->end_reasons = world->end_reasons;
    server->counters = &world->server_counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, 1000.0 );
    return world;
}

static void edges_world_destroy( struct edges_world_t * world )
{
    rocketnet_counters_assert_conserved( &world->server_counters );
    rocketnet_encryption_map_destroy( world->server.encryption_map );
    rocketnet_used_token_history_destroy( world->server.used_token_history );
    rocketnet_client_slots_destroy( world->server.client_slots );
    rocketnet_server_acks_destroy( world->server.acks );
    free( world );
}

// a fresh client at this address runs the handshake from t: request, request
// with its cookie, response, each answered at once. Returns its slot.

static int edges_connect( struct edges_world_t * world, const struct rocketnet_address_t * address, double t, uint64_t client_id, uint64_t expire_timestamp )
{
    memset( &world->client_counters, 0, sizeof( world->client_counters ) );
    rocketnet_client_protocol_init( &world->client, &world->client_counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    edges_mint( token_data, client_id, expire_timestamp );
    check( rocketnet_client_protocol_connect( &world->client, token_data, t, ROCKETNET_TEST_CREATE_TS ) );

    int payload_bytes = 0;
    for ( int i = 0; i < 3; i++ )
    {
        const double sent = t + 0.2 * i;
        int bytes = 0;
        rocketnet_client_protocol_update( &world->client, sent, world->packet, &bytes );
        check( bytes > 0 );
        (void) edges_deliver( world, address, bytes, sent );
        check( world->response_bytes > 0 );
        check( rocketnet_client_protocol_process_packet( &world->client, world->response, world->response_bytes, sent,
                                                         world->payload, (int) sizeof( world->payload ), &payload_bytes ) );
    }

    check( world->client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    const int mapping_index = rocketnet_encryption_map_find( world->server.encryption_map, address );
    check( mapping_index != -1 );
    const int client_index = rocketnet_encryption_map_entry( world->server.encryption_map, mapping_index )->control->client_index;
    check( client_index >= 0 && client_index < EDGES_SLOTS );
    check( rocketnet_client_slots_connected( world->server.client_slots, client_index ) );
    return client_index;
}

static struct rocketnet_encryption_mapping_t * edges_mapping( struct edges_world_t * world, const struct rocketnet_address_t * address )
{
    const int mapping_index = rocketnet_encryption_map_find( world->server.encryption_map, address );
    check( mapping_index != -1 );
    return rocketnet_encryption_map_entry( world->server.encryption_map, mapping_index );
}

// seals one session packet of this type at this 64-bit sequence under the
// mapping's client-to-server key, as the client would, and delivers it:
// an ack packet carries the empty block, a payload one byte behind it

static int edges_send( struct edges_world_t * world, const struct rocketnet_address_t * address, uint8_t type, uint64_t sequence, double time )
{
    struct rocketnet_encryption_mapping_t * mapping = edges_mapping( world, address );

    struct rocketnet_replay_protection_t * none = (struct rocketnet_replay_protection_t *) calloc( 1, sizeof( struct rocketnet_replay_protection_t ) );
    check( none != NULL );
    uint8_t plaintext[ROCKETNET_ACK_BLOCK_MAX_BYTES + 1];
    int plaintext_bytes = rocketnet_ack_block_build( plaintext, ROCKETNET_ACK_BLOCK_MIN_BYTES, none );
    free( none );
    check( plaintext_bytes == ROCKETNET_ACK_BLOCK_MIN_BYTES );
    if ( type == ROCKETNET_CONNECTION_PAYLOAD_PACKET )
    {
        plaintext[plaintext_bytes++] = 0x5C;
    }

    const int bytes = rocketnet_write_sealed_packet( world->packet, type, rocketnet_test_filter_value, sequence, NULL,
                                                     plaintext, plaintext_bytes, ROCKETNET_TEST_PROTOCOL_ID,
                                                     mapping->connection_salt, NULL, mapping->keys.session_client_to_server );
    check( bytes > 0 );
    return edges_deliver( world, address, bytes, time );
}

static void edges_read_state( struct edges_world_t * world, const struct rocketnet_address_t * address, int client_index, struct edges_state_t * state )
{
    struct rocketnet_encryption_mapping_t * mapping = edges_mapping( world, address );
    state->server = world->server;
    state->mapping = *mapping;
    state->control = *mapping->control;
    state->endpoint = world->server.acks->endpoints[client_index];
    state->counters = world->server_counters;
    state->connected = rocketnet_client_slots_connected( world->server.client_slots, client_index );
    state->confirmed = rocketnet_client_slots_confirmed( world->server.client_slots, client_index );
    state->num_acked = world->server.acks->num_acked;
}

static int edges_received_counter( uint8_t type )
{
    return type == ROCKETNET_CONNECTION_PAYLOAD_PACKET ? ROCKETNET_COUNTER_PAYLOAD_RECEIVED : ROCKETNET_COUNTER_ACK_RECEIVED;
}

// a refusal of this sequence: IGNORED, no reply and no payload, the refusal's
// counter, the type's received counter and the arrival count up one, and nothing else in the
// server, the mapping, its control, the slot's ack endpoint or the slot moved

static void edges_refused( struct edges_world_t * world, const struct rocketnet_address_t * address, int client_index,
                           uint8_t type, uint64_t sequence, double time, int counter )
{
    struct edges_state_t * before = (struct edges_state_t *) calloc( 1, sizeof( struct edges_state_t ) );
    struct edges_state_t * after = (struct edges_state_t *) calloc( 1, sizeof( struct edges_state_t ) );
    check( before != NULL && after != NULL );
    edges_read_state( world, address, client_index, before );

    check( edges_send( world, address, type, sequence, time ) == ROCKETNET_SERVER_PROCESS_IGNORED );
    check( world->response_bytes == 0 );
    check( world->payload_bytes == 0 );

    edges_read_state( world, address, client_index, after );
    check( edges_same_bytes( &before->server, &after->server, sizeof( before->server ) ) );
    check( edges_same_bytes( &before->mapping, &after->mapping, sizeof( before->mapping ) ) );
    check( edges_same_bytes( &before->control, &after->control, sizeof( before->control ) ) );
    check( edges_same_bytes( &before->endpoint, &after->endpoint, sizeof( before->endpoint ) ) );
    check( before->connected == after->connected && before->confirmed == after->confirmed );
    check( before->num_acked == after->num_acked );

    const int received = edges_received_counter( type );
    for ( int c = 0; c < ROCKETNET_NUM_COUNTERS; c++ )
    {
        const bool counted = c == counter || c == received || c == ROCKETNET_COUNTER_PACKETS_RECEIVED;
        check( after->counters.values[c] == before->counters.values[c] + ( counted ? 1u : 0u ) );
    }

    free( before );
    free( after );
}

// an acceptance: ACCEPTED, the window's top is the larger of the two, the
// mapping's idle clock is the packet's time, and a payload is delivered

static void edges_taken( struct edges_world_t * world, const struct rocketnet_address_t * address, uint8_t type, uint64_t sequence, double time )
{
    struct rocketnet_encryption_mapping_t * mapping = edges_mapping( world, address );
    const uint64_t highest = mapping->replay_session.highest_opened;
    check( edges_send( world, address, type, sequence, time ) == ROCKETNET_SERVER_PROCESS_ACCEPTED );
    check( mapping->replay_session.highest_opened == ( sequence > highest ? sequence : highest ) );
    check( mapping->control->last_receive_time == time );
    if ( type == ROCKETNET_CONNECTION_PAYLOAD_PACKET )
    {
        check( world->payload_bytes == 1 && world->payload[0] == 0x5C );
    }
}

static void test_server_replay_window_edges_on_the_wire()
{
    struct edges_world_t * world = edges_world_create();
    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "198.51.100.7:56789" ) );
    const int slot = edges_connect( world, &address, 1000.0, 0x4E91A7ULL, ROCKETNET_TEST_EXPIRE_TS );
    check( edges_mapping( world, &address )->replay_session.highest_opened == 0 );

    const uint8_t ACK = ROCKETNET_CONNECTION_ACK_PACKET;
    const uint8_t PAYLOAD = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    const int ACK_AGAIN = ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED;
    const int PAYLOAD_AGAIN = ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED;
    const uint64_t W = edges_window;
    double t = 1000.5;

    // sequence 0 never opens (the wire's 0 against a fresh window is 0)

    edges_refused( world, &address, slot, ACK, 1ULL << 32, t, ROCKETNET_COUNTER_ACK_ZERO_SEQUENCE );

    // the first sequence, its duplicate, and a payload replayed: never
    // delivered twice

    edges_taken( world, &address, ACK, 1, t += 0.01 );
    edges_refused( world, &address, slot, ACK, 1, t += 0.01, ACK_AGAIN );
    edges_taken( world, &address, PAYLOAD, 2, t += 0.01 );
    edges_refused( world, &address, slot, PAYLOAD, 2, t += 0.01, PAYLOAD_AGAIN );
    edges_refused( world, &address, slot, ACK, 2, t += 0.01, ACK_AGAIN );

    // a jump of exactly W: 2 is one past the edge, 3 exactly at it

    edges_taken( world, &address, ACK, W + 2, t += 0.01 );
    edges_refused( world, &address, slot, ACK, 2, t += 0.01, ACK_AGAIN );
    edges_taken( world, &address, PAYLOAD, 3, t += 0.01 );
    edges_refused( world, &address, slot, PAYLOAD, 3, t += 0.01, PAYLOAD_AGAIN );
    edges_taken( world, &address, ACK, W + 1, t += 0.01 );
    edges_refused( world, &address, slot, ACK, W + 2, t += 0.01, ACK_AGAIN );

    // a jump of W - 1: W + 2 is a duplicate inside the moved window, W + 1 has
    // fallen out of it, W + 3 (never received) is its floor

    const uint64_t h2 = 2 * W + 1;
    edges_taken( world, &address, ACK, h2, t += 0.01 );
    edges_refused( world, &address, slot, ACK, W + 2, t += 0.01, ACK_AGAIN );
    edges_refused( world, &address, slot, ACK, W + 1, t += 0.01, ACK_AGAIN );
    edges_taken( world, &address, ACK, W + 3, t += 0.01 );

    // far ahead (one packet may move the window by less than 2^31)

    const uint64_t far = h2 + 1000 * W + 17;
    edges_taken( world, &address, ACK, far, t += 0.01 );
    edges_refused( world, &address, slot, ACK, h2, t += 0.01, ACK_AGAIN );
    edges_taken( world, &address, PAYLOAD, far - W + 1, t += 0.01 );
    edges_refused( world, &address, slot, ACK, far - W, t += 0.01, ACK_AGAIN );

    // across the wire's 32-bit wrap, which is also a wrap of the ring

    const uint64_t wrap = 1ULL << 32;
    edges_taken( world, &address, ACK, 0x7FFFFFFFULL, t += 0.01 );
    edges_taken( world, &address, ACK, wrap - 16, t += 0.01 );
    edges_taken( world, &address, ACK, wrap - 1, t += 0.01 );
    edges_taken( world, &address, ACK, wrap, t += 0.01 );
    edges_refused( world, &address, slot, ACK, wrap - 1, t += 0.01, ACK_AGAIN );
    edges_refused( world, &address, slot, PAYLOAD, wrap, t += 0.01, PAYLOAD_AGAIN );
    edges_taken( world, &address, PAYLOAD, wrap - 2, t += 0.01 );
    edges_taken( world, &address, ACK, wrap + 1, t += 0.01 );
    edges_refused( world, &address, slot, ACK, wrap - 16, t += 0.01, ACK_AGAIN );
    edges_taken( world, &address, ACK, wrap - W + 2, t += 0.01 );
    edges_refused( world, &address, slot, ACK, wrap - W + 1, t += 0.01, ACK_AGAIN );
    check( edges_mapping( world, &address )->replay_session.highest_opened == wrap + 1 );

    // a session packet past the wrap is still the slot's: nothing ended

    check( rocketnet_client_slots_connected( world->server.client_slots, slot ) );
    check( world->server_counters.values[ROCKETNET_COUNTER_ACK_DID_NOT_OPEN] == 0 );
    check( world->server_counters.values[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN] == 0 );

    edges_world_destroy( world );
}

REGISTER_UNIT_TEST( test_server_replay_window_edges_on_the_wire );

// ---------------------------------------------------------------------------
// Slot reuse: the replay windows belong to the connection, not to the slot or
// the address. The mark walk records what it sees.
// ---------------------------------------------------------------------------

struct edges_sink_record_t
{
    int forgets;
    int marks;
    uint32_t last_mark;
    struct rocketnet_address_t last_address;
};

static bool edges_sink_forget( void * context, const struct rocketnet_address_t * address )
{
    struct edges_sink_record_t * record = (struct edges_sink_record_t *) context;
    record->forgets++;
    record->last_address = *address;
    return true;
}

static bool edges_sink_mark( void * context, const struct rocketnet_address_t * address, uint32_t high_water )
{
    struct edges_sink_record_t * record = (struct edges_sink_record_t *) context;
    record->marks++;
    record->last_mark = high_water;
    record->last_address = *address;
    return true;
}

static void edges_walk( struct edges_world_t * world, struct rocketnet_server_replay_marks_t * marks, struct edges_sink_record_t * record )
{
    memset( record, 0, sizeof( *record ) );
    struct rocketnet_server_replay_sink_t sink = { record, edges_sink_forget, edges_sink_mark };
    rocketnet_server_replay_marks_walk( marks, world->server.client_slots, world->server.encryption_map, &sink );
}

// client A receives 1..5 and 100, then goes silent and times out. Client B
// connects into the same slot: walk_between says whether an update's walk ran
// while the slot was free. Every sequence A had received is new to B.

static void edges_reuse( const char * b_address_text, bool walk_between )
{
    struct edges_world_t * world = edges_world_create();
    struct rocketnet_server_replay_marks_t * marks = rocketnet_server_replay_marks_create( NULL, EDGES_SLOTS );
    check( marks != NULL );
    struct edges_sink_record_t record;

    struct rocketnet_address_t a_address;
    struct rocketnet_address_t b_address;
    check( rocketnet_address_parse( &a_address, "198.51.100.7:56789" ) );
    check( rocketnet_address_parse( &b_address, b_address_text ) );
    const uint8_t ACK = ROCKETNET_CONNECTION_ACK_PACKET;
    const uint64_t received[6] = { 1, 2, 3, 4, 5, 100 };

    const int slot = edges_connect( world, &a_address, 1000.0, 0xA1ULL, ROCKETNET_TEST_EXPIRE_TS );
    edges_walk( world, marks, &record );
    check( record.forgets == 0 && record.marks == 1 && record.last_mark == 0 );

    double t = 1000.5;
    for ( int i = 0; i < 6; i++ )
    {
        edges_taken( world, &a_address, ACK, received[i], t += 0.01 );
    }
    edges_refused( world, &a_address, slot, ACK, 3, t += 0.01, ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED );
    edges_walk( world, marks, &record );
    check( record.forgets == 0 && record.marks == 1 && record.last_mark == 100 );
    check( rocketnet_address_equal( &record.last_address, &a_address ) );

    // A goes silent and times out

    const double heard = t;
    rocketnet_server_protocol_update( &world->server, heard + EDGES_TIMEOUT );
    check( !rocketnet_client_slots_connected( world->server.client_slots, slot ) );
    check( rocketnet_encryption_map_find( world->server.encryption_map, &a_address ) == -1 );

    if ( walk_between )
    {
        edges_walk( world, marks, &record );
        check( record.forgets == 1 && record.marks == 0 );
        check( rocketnet_address_equal( &record.last_address, &a_address ) );
    }

    // B connects into the same slot, with a token of its own

    const double b_start = heard + EDGES_TIMEOUT + 1.0;
    rocketnet_server_protocol_update( &world->server, b_start );
    const int b_slot = edges_connect( world, &b_address, b_start, 0xB2ULL, ROCKETNET_TEST_EXPIRE_TS - 1 );
    check( b_slot == slot );
    check( edges_mapping( world, &b_address )->replay_session.highest_opened == 0 );

    // the walk marks B at zero: as a slot first seen, or (same address, no
    // walk while it was free) as a session whose window fell below its mark.
    // At another address with no walk between, A's address is forgotten first.

    edges_walk( world, marks, &record );
    check( record.marks == 1 && record.last_mark == 0 );
    check( rocketnet_address_equal( &record.last_address, &b_address ) );
    const bool same_address = rocketnet_address_equal( &a_address, &b_address );
    check( record.forgets == ( !walk_between && !same_address ? 1 : 0 ) );

    // every sequence A had received is new to B, once

    t = b_start + 1.0;
    for ( int i = 0; i < 6; i++ )
    {
        edges_taken( world, &b_address, ACK, received[i], t += 0.01 );
    }
    for ( int i = 0; i < 6; i++ )
    {
        edges_refused( world, &b_address, slot, ACK, received[i], t += 0.01, ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED );
    }
    edges_walk( world, marks, &record );
    check( record.forgets == 0 && record.marks == 1 && record.last_mark == 100 );

    rocketnet_server_replay_marks_destroy( marks );
    edges_world_destroy( world );
}

static void test_server_reused_slot_accepts_what_the_old_session_received()
{
    edges_reuse( "198.51.100.7:56789", true );
    edges_reuse( "198.51.100.7:56789", false );
    edges_reuse( "198.51.100.8:40001", true );
    edges_reuse( "198.51.100.8:40001", false );
}

REGISTER_UNIT_TEST( test_server_reused_slot_accepts_what_the_old_session_received );

// The ACK-eligible window at its widest: the client's acks arrive at every
// other sequence, 300 isolated runs, more than a block holds. The server's
// next keep-alive carries the largest block (255 ranges, the oldest
// truncated) beside its 8 bytes of type data, inside the send budget, and it
// opens under the session key to exactly that block and that data.

static void test_server_keep_alive_carries_the_largest_ack_block()
{
    struct edges_world_t * world = edges_world_create();
    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "198.51.100.7:56789" ) );
    const int slot = edges_connect( world, &address, 1000.0, 0x4E91A7ULL, ROCKETNET_TEST_EXPIRE_TS );

    const int runs = 300;
    double t = 1000.5;
    for ( int i = 0; i < runs; i++ )
    {
        edges_taken( world, &address, ROCKETNET_CONNECTION_ACK_PACKET, (uint64_t) ( 2 * i + 1 ), t += 0.001 );
    }

    uint8_t * packet = (uint8_t *) calloc( 1, ROCKETNET_RECEIVE_CEILING );
    uint8_t * opened = (uint8_t *) calloc( 1, ROCKETNET_RECEIVE_CEILING );
    check( packet != NULL && opened != NULL );

    const int bytes = rocketnet_server_write_keep_alive( &world->server, slot, t, packet );
    check( bytes == ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MAX_BYTES + 8 );
    check( bytes <= ROCKETNET_SEND_BUDGET );
    check( packet[0] == ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET );

    const uint8_t * p = packet + ROCKETNET_PREFILTER_BYTES;
    const uint64_t sequence = rocketnet_read_uint32( &p );
    struct rocketnet_encryption_mapping_t * mapping = edges_mapping( world, &address );
    const int opened_bytes = rocketnet_open_sealed_packet( opened, NULL, ROCKETNET_RECEIVE_CEILING, packet, bytes,
                                                           ROCKETNET_CONNECTION_KEEP_ALIVE_PACKET, sequence, ROCKETNET_TEST_PROTOCOL_ID,
                                                           mapping->connection_salt, NULL, mapping->keys.session_server_to_client );
    check( opened_bytes == ROCKETNET_ACK_BLOCK_MAX_BYTES + 8 );

    struct rocketnet_ack_block_t block;
    const uint64_t largest = (uint64_t) ( 2 * runs - 1 );
    check( rocketnet_ack_block_read( &block, opened, opened_bytes, largest ) == ROCKETNET_ACK_BLOCK_MAX_BYTES );
    check( block.largest == largest );
    check( block.first_range_length == 1 );
    check( block.num_ranges == ROCKETNET_ACK_BLOCK_MAX_RANGES );
    for ( int i = 0; i < block.num_ranges; i++ )
    {
        check( block.range_gap[i] == 1 && block.range_length[i] == 1 );
    }

    const uint8_t * data = opened + ROCKETNET_ACK_BLOCK_MAX_BYTES;
    check( rocketnet_read_uint32( &data ) == (uint32_t) slot );
    check( rocketnet_read_uint32( &data ) == EDGES_SLOTS );

    free( packet );
    free( opened );
    edges_world_destroy( world );
}

REGISTER_UNIT_TEST( test_server_keep_alive_carries_the_largest_ack_block );

// The keep-alive falls due at exactly 1 / send_rate after the last one (a
// client that has acknowledged none paces on the keep-alive clock). At rate 8
// the interval, 0.125 s, is exact on a clock at 1001: due at 1001.125, not at
// 1001.0625 nor at 1001.1875.

static void test_server_keep_alive_falls_due_at_exactly_the_interval()
{
    struct edges_world_t * world = edges_world_create();
    world->server.send_rate = 8.0;
    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "198.51.100.7:56789" ) );
    const int slot = edges_connect( world, &address, 1000.0, 0x4E91A7ULL, ROCKETNET_TEST_EXPIRE_TS );
    check( world->server.acks->endpoints[slot].keep_alive_acked == 0 );

    uint8_t * packet = (uint8_t *) calloc( 1, ROCKETNET_RECEIVE_CEILING );
    check( packet != NULL );

    check( rocketnet_server_protocol_write_keep_alive_if_due( &world->server, slot, 1001.0, packet ) > 0 );
    check( edges_mapping( world, &address )->last_keep_alive_send_time == 1001.0 );
    check( rocketnet_server_protocol_write_keep_alive_if_due( &world->server, slot, 1001.0625, packet ) == 0 );
    check( rocketnet_server_protocol_write_keep_alive_if_due( &world->server, slot, 1001.125, packet ) > 0 );
    check( edges_mapping( world, &address )->last_keep_alive_send_time == 1001.125 );
    check( rocketnet_server_protocol_write_keep_alive_if_due( &world->server, slot, 1001.1875, packet ) == 0 );

    free( packet );
    edges_world_destroy( world );
}

REGISTER_UNIT_TEST( test_server_keep_alive_falls_due_at_exactly_the_interval );
