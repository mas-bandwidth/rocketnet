/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server's reassembled-payload batch under legitimate load. A completed
// payload is copied out of its slot into the reassembly's delivered arena,
// so the slot is reusable at once and one client completes more payloads in
// one update than it holds slots. The server's delivered capacity therefore
// has to cover what one update's drain can complete, not connections x
// slots: 64 clients each completing nine two-fragment payloads in one
// update is 576 completions against the 512 that connections x slots
// sizes at the defaults.
//
// Every case runs 64 client protocols and the server protocol in memory,
// the packets carried by the in-memory network simulator with no loss,
// latency or duplication; time is an argument starting at 1000.0. No
// sockets, no real time.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_wire.h"
#include "rocketnet_protocol.h"
#include "rocketnet_constants.h"
#include "rocketnet_counters.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_replay.h"
#include "rocketnet_fragment.h"
#include "rocketnet_memory.h"
#include "rocketnet_keys.h"
#include "rocketnet_read_write.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "test/counters_expect.h"

#define SRB_NUM_CLIENTS             64
#define SRB_PAYLOADS_PER_CLIENT     9
#define SRB_PAYLOAD_BYTES           2000        // two fragments at the full stride
#define SRB_TOTAL_PAYLOADS          ( SRB_NUM_CLIENTS * SRB_PAYLOADS_PER_CLIENT )

static void srb_mint_token( uint8_t * token_data, uint64_t client_id )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = 10;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };   // 203.0.113.10:40000
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, 32 );

    struct rocketnet_connect_token_private_t private_fields;
    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = client_id;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, 32 );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, 32 );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// the payload's bytes name their client and their position, so a delivered
// payload is checked byte-exact against the one that was sent

static void srb_fill_payload( uint8_t * data, int client, int k )
{
    data[0] = (uint8_t) client;
    data[1] = (uint8_t) k;
    for ( int b = 2; b < SRB_PAYLOAD_BYTES; b++ )
        data[b] = (uint8_t) ( client * 31 + k * 7 + b );
}

static bool srb_payload_matches( const uint8_t * data, int client, int k )
{
    uint8_t expected[SRB_PAYLOAD_BYTES];
    srb_fill_payload( expected, client, k );
    return memcmp( data, expected, SRB_PAYLOAD_BYTES ) == 0;
}

static void test_server_reassembled_batch_holds_legitimate_load()
{
    const int num_slots = ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    const int max_payload_bytes = ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;

    // the server protocol at the default geometry, its reassembly sized
    // exactly as the server transport sizes it at create

    struct rocketnet_counters_t server_counters;
    memset( &server_counters, 0, sizeof(server_counters) );

    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof(server) );
    server.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    memcpy( server.packet_filter, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server.server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server.server_secret_key, rocketnet_test_server_secret_key, 32 );
    struct rocketnet_address_t server_address;
    check( rocketnet_address_parse( &server_address, "203.0.113.10:40000" ) );
    server.advertised_ipv4 = server_address;
    memcpy( server.cookie_key, rocketnet_test_cookie_key, 32 );
    server.cookie_lifetime_seconds = 3;
    server.handshake_deadline_seconds = 5.0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, SRB_NUM_CLIENTS * 2 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, SRB_NUM_CLIENTS * 8 );
    server.client_slots = rocketnet_client_slots_create( NULL, SRB_NUM_CLIENTS );
    server.acks = rocketnet_server_acks_create( NULL, SRB_NUM_CLIENTS );
    const int delivered_capacity = rocketnet_server_reassembly_delivered_capacity( SRB_NUM_CLIENTS, num_slots );
    check( delivered_capacity >= SRB_TOTAL_PAYLOADS );
    const uint64_t delivered_bytes = rocketnet_server_reassembly_delivered_bytes( SRB_NUM_CLIENTS, num_slots, max_payload_bytes );
    check( delivered_bytes >= (uint64_t) SRB_TOTAL_PAYLOADS * SRB_PAYLOAD_BYTES );
    server.reassembly = rocketnet_reassembly_set_create_delivered( NULL, SRB_NUM_CLIENTS, num_slots, max_payload_bytes, delivered_capacity, delivered_bytes );
    server.counters = &server_counters;
    check( server.encryption_map && server.used_token_history &&
           server.client_slots && server.acks && server.reassembly );

    // the wire: no loss, no latency, no jitter, no duplicates

    struct rocketnet_network_simulator_t * wire = rocketnet_network_simulator_create( NULL, 4096, 0x5EB0ULL );
    check( wire );
    rocketnet_network_simulator_config( wire, 0.0, 0.0, 0.0f, 0.0f );

    static struct rocketnet_client_protocol_t clients[SRB_NUM_CLIENTS];
    struct rocketnet_address_t addresses[SRB_NUM_CLIENTS];
    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );

    double time = 1000.0;

    for ( int i = 0; i < SRB_NUM_CLIENTS; i++ )
    {
        char address_string[64];
        snprintf( address_string, sizeof(address_string), "10.2.0.%d:56789", 1 + i );
        check( rocketnet_address_parse( &addresses[i], address_string ) );
        rocketnet_client_protocol_init( &clients[i], &client_counters );
        uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
        srb_mint_token( token_data, UINT64_C( 0x5B000000 ) + (uint64_t) i );
        check( rocketnet_client_protocol_connect( &clients[i], token_data, time, ROCKETNET_TEST_CREATE_TS ) );
    }

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    // the handshake: every client's packets cross the wire to the server,
    // every response crosses back

    for ( int tick = 0; tick < 200; tick++, time += 0.05 )
    {
        rocketnet_reassembly_set_begin_update( server.reassembly );

        int num_connected = 0;
        for ( int i = 0; i < SRB_NUM_CLIENTS; i++ )
        {
            if ( clients[i].state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                num_connected++;
                continue;
            }
            int packet_bytes = 0;
            rocketnet_client_protocol_update( &clients[i], time, packet, &packet_bytes );
            if ( packet_bytes > 0 )
                rocketnet_network_simulator_send( wire, &addresses[i], &server_address, packet, packet_bytes, time );
        }
        if ( num_connected == SRB_NUM_CLIENTS )
            break;

        struct rocketnet_address_t from;
        for ( int got = rocketnet_network_simulator_receive( wire, &server_address, time, &from, packet ); got > 0;
              got = rocketnet_network_simulator_receive( wire, &server_address, time, &from, packet ) )
        {
            int response_bytes = 0;
            rocketnet_server_protocol_process_packet( &server, &from, packet, got, ROCKETNET_TEST_CREATE_TS, time,
                                                      response, &response_bytes, payload, &payload_bytes, &payload_client_index );
            if ( response_bytes > 0 )
                rocketnet_network_simulator_send( wire, &server_address, &from, response, response_bytes, time );
        }

        for ( int i = 0; i < SRB_NUM_CLIENTS; i++ )
        {
            for ( int got = rocketnet_network_simulator_receive( wire, &addresses[i], time, &from, packet ); got > 0;
                  got = rocketnet_network_simulator_receive( wire, &addresses[i], time, &from, packet ) )
            {
                int client_payload_bytes = 0;
                rocketnet_client_protocol_process_packet( &clients[i], packet, got, time, payload, (int) sizeof(payload), &client_payload_bytes );
            }
        }

        rocketnet_server_protocol_update( &server, time );
    }

    for ( int i = 0; i < SRB_NUM_CLIENTS; i++ )
        check( clients[i].state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_client_slots_num_connected( server.client_slots ) == SRB_NUM_CLIENTS );

    // the burst: every client sends nine two-fragment payloads, and the
    // server drains all 1152 fragment packets in one update

    time += 0.05;
    rocketnet_reassembly_set_begin_update( server.reassembly );

    static uint8_t fragment_packets[2 * ROCKETNET_SEND_BUDGET];
    for ( int k = 0; k < SRB_PAYLOADS_PER_CLIENT; k++ )
    {
        for ( int i = 0; i < SRB_NUM_CLIENTS; i++ )
        {
            uint8_t data[SRB_PAYLOAD_BYTES];
            srb_fill_payload( data, i, k );
            int fragment_packet_bytes[2];
            check( rocketnet_client_protocol_write_fragments( &clients[i], data, SRB_PAYLOAD_BYTES, time,
                                                              fragment_packets, fragment_packet_bytes, 2 ) == 2 );
            for ( int f = 0; f < 2; f++ )
                rocketnet_network_simulator_send( wire, &addresses[i], &server_address,
                                                  fragment_packets + f * ROCKETNET_SEND_BUDGET, fragment_packet_bytes[f], time );
        }
    }

    const uint64_t reassembled_before = server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED];
    const uint64_t batch_full_before = server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL];

    static const uint8_t * delivered_data[SRB_TOTAL_PAYLOADS];
    static int delivered_client[SRB_TOTAL_PAYLOADS];
    static int delivered_k[SRB_TOTAL_PAYLOADS];
    int num_delivered = 0;
    int num_drained = 0;

    struct rocketnet_address_t from;
    for ( int got = rocketnet_network_simulator_receive( wire, &server_address, time, &from, packet ); got > 0;
          got = rocketnet_network_simulator_receive( wire, &server_address, time, &from, packet ) )
    {
        num_drained++;
        int response_bytes = 0;
        payload_bytes = 0;
        const int result = rocketnet_server_protocol_process_packet( &server, &from, packet, got, ROCKETNET_TEST_CREATE_TS, time,
                                                                     response, &response_bytes, payload, &payload_bytes, &payload_client_index );
        check( result == ROCKETNET_SERVER_PROCESS_ACCEPTED );
        if ( server.reassembly->completed_bytes > 0 )
        {
            check( server.reassembly->completed_bytes == SRB_PAYLOAD_BYTES );
            const uint8_t * data = server.reassembly->completed_payload;
            const int client = data[0];
            const int k = data[1];
            check( client < SRB_NUM_CLIENTS && k < SRB_PAYLOADS_PER_CLIENT );
            check( rocketnet_address_equal( &from, &addresses[client] ) );
            check( srb_payload_matches( data, client, k ) );
            check( num_delivered < SRB_TOTAL_PAYLOADS );
            delivered_data[num_delivered] = data;
            delivered_client[num_delivered] = client;
            delivered_k[num_delivered] = k;
            num_delivered++;
        }
    }

    check( num_drained == 2 * SRB_TOTAL_PAYLOADS );

    const uint64_t batch_full = server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] - batch_full_before;
    const uint64_t reassembled = server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] - reassembled_before;
    if ( batch_full != 0 || num_delivered != SRB_TOTAL_PAYLOADS )
        printf( "test_server_reassembled_batch_holds_legitimate_load: reassembled %llu delivered %d BATCH_FULL %llu (delivered capacity %d)\n",
                (unsigned long long) reassembled, num_delivered, (unsigned long long) batch_full, server.reassembly->delivered_capacity );
    check( reassembled == SRB_TOTAL_PAYLOADS );
    check( batch_full == 0 );
    check( num_delivered == SRB_TOTAL_PAYLOADS );

    // every delivered payload is still whole at the end of the update: the
    // delivered arena hands each completion its own room

    bool seen[SRB_NUM_CLIENTS][SRB_PAYLOADS_PER_CLIENT];
    memset( seen, 0, sizeof(seen) );
    for ( int d = 0; d < num_delivered; d++ )
    {
        check( srb_payload_matches( delivered_data[d], delivered_client[d], delivered_k[d] ) );
        check( ( (uintptr_t) delivered_data[d] & ( ROCKETNET_REASSEMBLY_DELIVERED_ALIGN - 1 ) ) == 0 );
        check( !seen[delivered_client[d]][delivered_k[d]] );
        seen[delivered_client[d]][delivered_k[d]] = true;
    }

    rocketnet_server_protocol_update( &server, time );

    rocketnet_counters_assert_conserved( &server_counters );
    rocketnet_counters_assert_conserved( &client_counters );

    rocketnet_network_simulator_destroy( wire );
    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_reassembly_set_destroy( server.reassembly );

    printf( "test_server_reassembled_batch_holds_legitimate_load\n" );
}

REGISTER_UNIT_TEST( test_server_reassembled_batch_holds_legitimate_load );

// The delivered arena is sized by bytes beside the count: what one update
// can complete is its drained fragments plus the payloads half-held in
// slots, not capacity x max payload. A small server with a large maximum
// payload stays inside the reassembly budget, as it did when the arena was
// connections x slots x max payload.

static void test_server_reassembly_large_payload_small_geometry_fits_the_budget()
{
    const uint64_t budget = (uint64_t) ROCKETNET_REASSEMBLY_MEMORY_MIB * 1024 * 1024;

    // 1 client, 8 slots, 70000-byte payloads: accepted, as before the
    // count bound grew past connections x slots
    {
        const int capacity = rocketnet_server_reassembly_delivered_capacity( 1, 8 );
        const uint64_t bytes = rocketnet_server_reassembly_delivered_bytes( 1, 8, 70000 );
        check( capacity == ( ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS + 8 ) / 2 );
        check( bytes == (uint64_t) ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS * ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES +
                        8ULL * 70000 + (uint64_t) capacity * ( ROCKETNET_REASSEMBLY_DELIVERED_ALIGN - 1 ) );
        check( bytes < (uint64_t) capacity * 70000 );
        check( rocketnet_reassembly_memory_allowed_delivered( 1, 8, 70000, capacity, bytes ) );
        check( rocketnet_reassembly_memory_required_delivered( 1, 8, 70000, capacity, bytes ) < budget );

        struct rocketnet_server_transport_config_t config;
        rocketnet_server_transport_default_config( &config );
        config.max_clients = 1;
        config.max_fragmented_messages = 8;
        config.max_payload_bytes = 70000;
        check( rocketnet_server_transport_required_memory( &config, "portable" ) != 0 );

        struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create_delivered( NULL, 1, 8, 70000, capacity, bytes );
        check( set );
        check( set->delivered_capacity == capacity && set->delivered_bytes == bytes );
        rocketnet_reassembly_set_destroy( set );
    }

    // the largest fragmented payload at 1 client and the most slots: also
    // accepted, as before
    {
        const int capacity = rocketnet_server_reassembly_delivered_capacity( 1, ROCKETNET_MAX_FRAGMENTS );
        const uint64_t bytes = rocketnet_server_reassembly_delivered_bytes( 1, ROCKETNET_MAX_FRAGMENTS, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES );
        check( rocketnet_reassembly_memory_allowed_delivered( 1, ROCKETNET_MAX_FRAGMENTS, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, capacity, bytes ) );
    }

    // where the count bound is the smaller, it is the one used: few
    // completions of small payloads never exceed capacity x aligned max
    {
        const int capacity = rocketnet_server_reassembly_delivered_capacity( 1, 1 );
        const uint64_t bytes = rocketnet_server_reassembly_delivered_bytes( 1, 1, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        check( bytes <= (uint64_t) capacity * ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) );
    }

    // invalid geometry is zero
    check( rocketnet_server_reassembly_delivered_bytes( 0, 8, 16384 ) == 0 );
    check( rocketnet_server_reassembly_delivered_bytes( 1, 8, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 1 ) == 0 );
    check( rocketnet_server_reassembly_delivered_bytes( 1, 8, ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1 ) == 0 );
    check( rocketnet_reassembly_memory_required_delivered( 1, 8, 16384, 4, 16383 ) == 0 );

    printf( "test_server_reassembly_large_payload_small_geometry_fits_the_budget\n" );
}

REGISTER_UNIT_TEST( test_server_reassembly_large_payload_small_geometry_fits_the_budget );

// The byte bound is enforced, not only planned: completions pack at the
// 16-byte stride, and one that would run past the arena's bytes counts
// BATCH_FULL even with count to spare, and delivers nothing.

static const struct rocketnet_reassembly_counters_t srb_fragment_counters = {
    ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
    ROCKETNET_COUNTER_FRAGMENT_STALE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
    ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
    ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
    ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX,
    ROCKETNET_COUNTER_FRAGMENT_ACCEPTED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED,
    ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL };

static int srb_complete_payload( struct rocketnet_reassembly_set_t * set, uint32_t payload_id, int first_bytes, int final_bytes,
                                 uint64_t * counters )
{
    uint8_t fragment[ROCKETNET_FRAGMENT_HEADER_BYTES + ROCKETNET_MAX_RECEIVE_FRAGMENT_BYTES];
    rocketnet_write_fragment_header( fragment, payload_id, 0, 2 );
    memset( fragment + ROCKETNET_FRAGMENT_HEADER_BYTES, (int) payload_id, (size_t) first_bytes );
    check( rocketnet_reassembly_process_fragment( set, 0, fragment, ROCKETNET_FRAGMENT_HEADER_BYTES + first_bytes, counters, &srb_fragment_counters ) );
    rocketnet_write_fragment_header( fragment, payload_id, 1, 2 );
    memset( fragment + ROCKETNET_FRAGMENT_HEADER_BYTES, (int) payload_id, (size_t) final_bytes );
    check( rocketnet_reassembly_process_fragment( set, 0, fragment, ROCKETNET_FRAGMENT_HEADER_BYTES + final_bytes, counters, &srb_fragment_counters ) );
    return set->completed_bytes;
}

static void test_server_reassembly_delivered_packs_by_bytes()
{
    struct rocketnet_counters_t counters_storage;
    memset( &counters_storage, 0, sizeof(counters_storage) );
    uint64_t * counters = counters_storage.values;

    // count 8, bytes 4096: two 1500-byte payloads pack (1500 pads to 1504),
    // the third would end at 4508 > 4096 and is BATCH_FULL
    struct rocketnet_reassembly_set_t * set = rocketnet_reassembly_set_create_delivered( NULL, 1, 4, 4096, 8, 4096 );
    check( set );
    rocketnet_reassembly_set_begin_update( set );

    check( srb_complete_payload( set, 1, 1000, 500, counters ) == 1500 );
    const uint8_t * first = set->completed_payload;
    check( first == set->delivered );
    check( srb_complete_payload( set, 2, 1000, 500, counters ) == 1500 );
    check( set->completed_payload == set->delivered + 1504 );
    check( ( (uintptr_t) set->completed_payload & ( ROCKETNET_REASSEMBLY_DELIVERED_ALIGN - 1 ) ) == ( (uintptr_t) set->delivered & ( ROCKETNET_REASSEMBLY_DELIVERED_ALIGN - 1 ) ) );
    check( srb_complete_payload( set, 3, 1000, 500, counters ) == 0 );
    check( set->num_delivered == 2 );
    check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == 3 );
    check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 1 );
    for ( int b = 0; b < 1500; b++ )
        check( first[b] == 1 );

    // the next update reclaims the bytes
    rocketnet_reassembly_set_begin_update( set );
    check( set->delivered_used == 0 );
    check( srb_complete_payload( set, 4, 1000, 500, counters ) == 1500 );
    check( set->completed_payload == set->delivered );
    check( counters[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL] == 1 );

    rocketnet_reassembly_set_destroy( set );

    // the plain create sizes bytes at capacity x the aligned maximum, so
    // capacity maximum payloads always fit
    set = rocketnet_reassembly_set_create( NULL, 1, 2, 4100, 2 );
    check( set );
    check( set->delivered_bytes == 2 * ROCKETNET_REASSEMBLY_DELIVERED_ALIGNED( 4100 ) );
    rocketnet_reassembly_set_destroy( set );

    printf( "test_server_reassembly_delivered_packs_by_bytes\n" );
}

REGISTER_UNIT_TEST( test_server_reassembly_delivered_packs_by_bytes );
