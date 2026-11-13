/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The worker-side prefilter (sdk/rocketnet_receive_prefilter.h) against the
// server's own receive check sequence, datagram by datagram. For every packet type,
// every size at and around each type's bounds, and each prefilter header
// fault, the prefilter's verdict is computed, then the same datagram is
// handed to a server protocol in memory, and the counters it moved are read:
//   - a refusal moves exactly the counters rocketnet_receive_drop_counters
//     names for it, each by one, and nothing else;
//   - an accept moves none of the counters any refusal names, so main never
//     refuses on a stateless rung what the worker let through.
// The transports count their drops in per-worker tallies and the server folds
// them into its counters: tests/test_unit_receive_drops.cpp.
//
// No sockets, no real time: the protocol runs on the test vector keys and a
// fixed clock.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_receive_prefilter.h"
#include "rocketnet_test_vectors.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

static void rp_server_setup( struct rocketnet_server_protocol_t * server, struct rocketnet_counters_t * counters )
{
    memset( server, 0, sizeof( *server ) );
    memset( counters, 0, sizeof( *counters ) );
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
    server->client_slots = rocketnet_client_slots_create( NULL, 4 );
    server->acks = rocketnet_server_acks_create( NULL, 4 );
    server->counters = counters;
    check( server->encryption_map && server->used_token_history && server->client_slots && server->acks );
    rocketnet_server_protocol_update( server, ROCKETNET_TEST_CREATE_TS );
}

static void rp_server_teardown( struct rocketnet_server_protocol_t * server )
{
    rocketnet_encryption_map_destroy( server->encryption_map );
    rocketnet_used_token_history_destroy( server->used_token_history );
    rocketnet_client_slots_destroy( server->client_slots );
    rocketnet_server_acks_destroy( server->acks );
}

// the counters a refusal on a stateless rung can move: an accepted datagram
// moves none of them
static bool rp_is_stateless_refusal_counter( int counter )
{
    for ( int drop = ROCKETNET_RECEIVE_ACCEPT + 1; drop < ROCKETNET_RECEIVE_DROP_BANK_FULL; ++drop )
    {
        int counters[ROCKETNET_RECEIVE_DROP_MAX_COUNTERS];
        const int num_counters = rocketnet_receive_drop_counters( (enum rocketnet_receive_drop_t) drop, counters );
        // the last counter of a drop is its reason; the ones before it are arrivals
        if ( counters[num_counters - 1] == counter )
        {
            return true;
        }
    }
    return false;
}

// the sizes worth trying for every type: the small sizes through the
// prefilter header, each type's bound and its neighbours, a game-sized
// payload, the receive ceiling and one past it
static int rp_sizes( int * sizes )
{
    const int bounds[] = { ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES, ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES,
                           ROCKETNET_CONNECTION_KEEP_ALIVE_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES,
                           ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES,
                           ROCKETNET_CONNECTION_DENIED_WIRE_BYTES, ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES,
                           ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES, 1200,
                           ROCKETNET_RECEIVE_CEILING };
    int count = 0;
    for ( int size = 0; size <= ROCKETNET_PREFILTER_BYTES + 1; ++size )
    {
        sizes[count++] = size;
    }
    for ( size_t i = 0; i < sizeof( bounds ) / sizeof( bounds[0] ); ++i )
    {
        sizes[count++] = bounds[i] - 1;
        sizes[count++] = bounds[i];
        sizes[count++] = bounds[i] + 1;
    }
    return count;
}

static void test_receive_prefilter_matches_the_server_check_sequence()
{
    struct rocketnet_server_protocol_t server;
    struct rocketnet_counters_t counters;
    rp_server_setup( &server, &counters );

    struct rocketnet_address_t from;
    check( rocketnet_address_parse( &from, "198.51.100.7:50000" ) );

    static uint8_t packet[ROCKETNET_RECEIVE_CEILING + 1];
    int sizes[64];
    const int num_sizes = rp_sizes( sizes );

    enum
    {
        RP_HEADER_GOOD,
        RP_HEADER_WRONG_EPOCH,
        RP_HEADER_WRONG_VALUE,
        RP_NUM_HEADERS
    };

    int seen[ROCKETNET_RECEIVE_NUM_DROPS] = {};
    int cases = 0;

    for ( int header = 0; header < RP_NUM_HEADERS; ++header )
    {
        for ( int type = 0; type < 256; ++type )
        {
            for ( int s = 0; s < num_sizes; ++s )
            {
                const int packet_bytes = sizes[s];
                memset( packet, 0, sizeof( packet ) );
                packet[0] = (uint8_t) type;
                packet[1] = header == RP_HEADER_WRONG_EPOCH ? (uint8_t) ROCKETNET_FILTER_EPOCH_KEYED_FIRST : (uint8_t) ROCKETNET_FILTER_EPOCH_UNKEYED;
                memcpy( packet + 2, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
                if ( header == RP_HEADER_WRONG_VALUE )
                {
                    packet[2] ^= 0xFF;
                }

                const enum rocketnet_receive_drop_t drop = rocketnet_receive_prefilter( packet, packet_bytes, rocketnet_test_filter_value );
                check( drop >= ROCKETNET_RECEIVE_ACCEPT && drop < ROCKETNET_RECEIVE_DROP_BANK_FULL );
                seen[drop]++;
                cases++;

                const struct rocketnet_counters_t before = counters;
                uint8_t response[ROCKETNET_RECEIVE_CEILING];
                int response_bytes = 0;
                uint8_t payload[ROCKETNET_RECEIVE_CEILING];
                int payload_bytes = 0;
                int payload_client_index = -1;
                rocketnet_server_protocol_process_packet( &server, &from, packet, packet_bytes, ROCKETNET_TEST_CREATE_TS, 0.0, response,
                                                          &response_bytes, payload, &payload_bytes, &payload_client_index );

                if ( drop != ROCKETNET_RECEIVE_ACCEPT )
                {
                    int named[ROCKETNET_RECEIVE_DROP_MAX_COUNTERS];
                    const int num_named = rocketnet_receive_drop_counters( drop, named );
                    check( num_named >= 2 );
                    for ( int c = 0; c < ROCKETNET_NUM_COUNTERS; ++c )
                    {
                        uint64_t expected = 0;
                        for ( int n = 0; n < num_named; ++n )
                        {
                            expected += named[n] == c ? 1 : 0;
                        }
                        if ( counters.values[c] - before.values[c] != expected )
                        {
                            printf( "type %d bytes %d header %d drop %d: %s moved %" PRIu64 ", expected %" PRIu64 "\n", type, packet_bytes,
                                    header, (int) drop, rocketnet_counter_name( c ), counters.values[c] - before.values[c], expected );
                        }
                        check( counters.values[c] - before.values[c] == expected );
                    }
                    check( response_bytes == 0 );
                }
                else
                {
                    check( counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] - before.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == 1 );
                    for ( int c = 0; c < ROCKETNET_NUM_COUNTERS; ++c )
                    {
                        if ( rp_is_stateless_refusal_counter( c ) && counters.values[c] != before.values[c] )
                        {
                            printf( "type %d bytes %d accepted by the prefilter, refused by main on %s\n", type, packet_bytes,
                                    rocketnet_counter_name( c ) );
                        }
                        check( !rp_is_stateless_refusal_counter( c ) || counters.values[c] == before.values[c] );
                    }
                }
            }
        }
    }

    // every reason the prefilter can give was given at least once, and so was accept
    for ( int drop = ROCKETNET_RECEIVE_ACCEPT; drop < ROCKETNET_RECEIVE_DROP_BANK_FULL; ++drop )
    {
        check( seen[drop] > 0 );
    }
    check( cases == RP_NUM_HEADERS * 256 * num_sizes );

    rp_server_teardown( &server );
}

static void test_receive_prefilter_reads_only_its_arguments()
{
    // a short datagram is refused on its length before any byte past it is read:
    // the buffer is exactly the bytes given
    uint8_t one[1] = { ROCKETNET_CONNECTION_PAYLOAD_PACKET };
    check( rocketnet_receive_prefilter( one, 1, rocketnet_test_filter_value ) == ROCKETNET_RECEIVE_DROP_TOO_SHORT );
    check( rocketnet_receive_prefilter( one, 0, rocketnet_test_filter_value ) == ROCKETNET_RECEIVE_DROP_TOO_SHORT );

    // the filter value is the argument, not any server's: another value refuses the same bytes
    uint8_t packet[ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES] = {};
    packet[0] = ROCKETNET_CONNECTION_PAYLOAD_PACKET;
    packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;
    memcpy( packet + 2, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    check( rocketnet_receive_prefilter( packet, (int) sizeof( packet ), rocketnet_test_filter_value ) == ROCKETNET_RECEIVE_ACCEPT );
    const uint8_t other[ROCKETNET_FILTER_VALUE_BYTES] = { 1, 2, 3, 4 };
    check( rocketnet_receive_prefilter( packet, (int) sizeof( packet ), other ) == ROCKETNET_RECEIVE_DROP_WRONG_VALUE );
}

REGISTER_UNIT_TEST( test_receive_prefilter_matches_the_server_check_sequence );
REGISTER_UNIT_TEST( test_receive_prefilter_reads_only_its_arguments );
