/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client's reassembled-payload batch under legitimate load. A completed
// payload is copied out of its slot into the reassembly's delivered arena,
// so the slot is reusable at once and one connection completes more payloads
// in one update than it holds slots. The client's delivered capacity
// therefore has to cover what one update's drain can complete: a payload
// begun in an earlier update completes on one drained packet, and a payload
// begun in the update takes two, so a 1024-packet drain completes up to
// ( 1024 + min( 1024, slots ) ) / 2 = 516 payloads at the default 8 slots.
//
// The case runs one client protocol and the server protocol in memory, the
// packets carried by the in-memory network simulator with no loss, latency
// or duplication; time is an argument starting at 1000.0. No sockets, no
// real time.

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
#include "counters_expect.h"

#define CRB_PAYLOAD_BYTES           2000        // two fragments at the full stride
#define CRB_FRESH_PAYLOADS          ( ( ROCKETNET_NUM_CLIENT_PACKETS - ROCKETNET_DEFAULT_REASSEMBLY_SLOTS ) / 2 )
#define CRB_TOTAL_PAYLOADS          ( ROCKETNET_DEFAULT_REASSEMBLY_SLOTS + CRB_FRESH_PAYLOADS )

static void crb_mint_token( uint8_t * token_data, uint64_t client_id )
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

// the payload's bytes name their position, so a delivered payload is checked
// byte-exact against the one that was sent

static void crb_fill_payload( uint8_t * data, int k )
{
    data[0] = (uint8_t) ( k & 0xFF );
    data[1] = (uint8_t) ( ( k >> 8 ) & 0xFF );
    for ( int b = 2; b < CRB_PAYLOAD_BYTES; b++ )
        data[b] = (uint8_t) ( k * 31 + b );
}

static bool crb_payload_matches( const uint8_t * data, int k )
{
    uint8_t expected[CRB_PAYLOAD_BYTES];
    crb_fill_payload( expected, k );
    return memcmp( data, expected, CRB_PAYLOAD_BYTES ) == 0;
}

static void test_client_reassembled_batch_holds_half_received_load()
{
    const int num_slots = ROCKETNET_DEFAULT_REASSEMBLY_SLOTS;
    const int max_payload_bytes = ROCKETNET_DEFAULT_MAX_PAYLOAD_BYTES;

    // the server protocol at the one-client geometry

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
    server.encryption_map = rocketnet_encryption_map_create( NULL, 2 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 8 );
    server.client_slots = rocketnet_client_slots_create( NULL, 1 );
    server.acks = rocketnet_server_acks_create( NULL, 1 );
    const int server_capacity = rocketnet_server_reassembly_delivered_capacity( 1, num_slots );
    const uint64_t server_bytes = rocketnet_server_reassembly_delivered_bytes( 1, num_slots, max_payload_bytes );
    server.reassembly = rocketnet_reassembly_set_create_delivered( NULL, 1, num_slots, max_payload_bytes, server_capacity, server_bytes );
    server.counters = &server_counters;
    check( server.encryption_map && server.used_token_history &&
           server.client_slots && server.acks && server.reassembly );
    rocketnet_reassembly_set_begin_update( server.reassembly );
    rocketnet_server_protocol_update( &server, ROCKETNET_TEST_CREATE_TS );

    // the client protocol at address 10.2.0.1:56789, its reassembly sized
    // exactly as the client transport sizes it at create

    struct rocketnet_address_t client_address;
    check( rocketnet_address_parse( &client_address, "10.2.0.1:56789" ) );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );

    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );
    const int client_capacity = rocketnet_client_reassembly_delivered_capacity( ROCKETNET_DEFAULT_REASSEMBLY_SLOTS );
    check( client_capacity == 516 );
    client.reassembly = rocketnet_reassembly_set_create( NULL, 1, num_slots, max_payload_bytes, client_capacity );
    check( client.reassembly );
    rocketnet_reassembly_set_begin_update( client.reassembly );

    // the wire: no loss, no latency, no jitter, no duplicates

    struct rocketnet_network_simulator_t * wire = rocketnet_network_simulator_create( NULL, 4096, 0x5EB0ULL );
    check( wire );
    rocketnet_network_simulator_config( wire, 0.0, 0.0, 0.0f, 0.0f );

    double time = 1000.0;

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    crb_mint_token( token_data, 0x5B000000ULL );
    check( rocketnet_client_protocol_connect( &client, token_data, time, ROCKETNET_TEST_CREATE_TS ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    uint8_t response[ROCKETNET_RECEIVE_CEILING];
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    int payload_client_index = -1;

    // the handshake: the client's packets cross the wire to the server,
    // every response crosses back

    for ( int tick = 0; tick < 200; tick++, time += 0.05 )
    {
        rocketnet_reassembly_set_begin_update( server.reassembly );

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;

        int packet_bytes = 0;
        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
            rocketnet_network_simulator_send( wire, &client_address, &server_address, packet, packet_bytes, time );

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

        for ( int got = rocketnet_network_simulator_receive( wire, &client_address, time, &from, packet ); got > 0;
              got = rocketnet_network_simulator_receive( wire, &client_address, time, &from, packet ) )
        {
            int client_payload_bytes = 0;
            rocketnet_client_protocol_process_packet( &client, packet, got, time, payload, (int) sizeof(payload), &client_payload_bytes );
        }

        rocketnet_server_protocol_update( &server, time );
    }

    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );
    check( rocketnet_client_slots_num_connected( server.client_slots ) == 1 );

    // update one: begin eight two-fragment payloads, deliver each first
    // fragment and hold each second fragment

    static uint8_t fragment_packets[2 * ROCKETNET_SEND_BUDGET];
    static uint8_t held_packets[ROCKETNET_DEFAULT_REASSEMBLY_SLOTS][ROCKETNET_SEND_BUDGET];
    int held_bytes[ROCKETNET_DEFAULT_REASSEMBLY_SLOTS];

    time += 0.05;
    rocketnet_reassembly_set_begin_update( client.reassembly );

    for ( int k = 0; k < num_slots; k++ )
    {
        uint8_t data[CRB_PAYLOAD_BYTES];
        crb_fill_payload( data, k );
        int fragment_packet_bytes[2];
        check( rocketnet_server_protocol_write_fragments( &server, 0, data, CRB_PAYLOAD_BYTES, time,
                                                          fragment_packets, fragment_packet_bytes, 2 ) == 2 );
        rocketnet_network_simulator_send( wire, &server_address, &client_address, fragment_packets, fragment_packet_bytes[0], time );
        memcpy( held_packets[k], fragment_packets + ROCKETNET_SEND_BUDGET, (size_t) fragment_packet_bytes[1] );
        held_bytes[k] = fragment_packet_bytes[1];
    }

    {
        struct rocketnet_address_t from;
        int drained_one = 0;
        for ( int got = rocketnet_network_simulator_receive( wire, &client_address, time, &from, packet ); got > 0;
              got = rocketnet_network_simulator_receive( wire, &client_address, time, &from, packet ) )
        {
            drained_one++;
            int client_payload_bytes = 0;
            check( rocketnet_client_protocol_process_packet( &client, packet, got, time, payload, (int) sizeof(payload), &client_payload_bytes ) );
            check( client.reassembly->completed_bytes == 0 );
        }
        check( drained_one == num_slots );
    }

    // update two: the eight held second fragments complete the payloads begun
    // earlier, then 508 fresh two-fragment payloads arrive; 1024 packets in all

    time += 0.05;
    rocketnet_reassembly_set_begin_update( client.reassembly );

    const uint64_t reassembled_before = client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED];
    const uint64_t batch_full_before = client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL];

    for ( int k = 0; k < num_slots; k++ )
        rocketnet_network_simulator_send( wire, &server_address, &client_address, held_packets[k], held_bytes[k], time );

    for ( int k = 0; k < CRB_FRESH_PAYLOADS; k++ )
    {
        uint8_t data[CRB_PAYLOAD_BYTES];
        crb_fill_payload( data, num_slots + k );
        int fragment_packet_bytes[2];
        check( rocketnet_server_protocol_write_fragments( &server, 0, data, CRB_PAYLOAD_BYTES, time,
                                                          fragment_packets, fragment_packet_bytes, 2 ) == 2 );
        for ( int f = 0; f < 2; f++ )
            rocketnet_network_simulator_send( wire, &server_address, &client_address,
                                              fragment_packets + f * ROCKETNET_SEND_BUDGET, fragment_packet_bytes[f], time );
    }

    int drained = 0;
    int delivered = 0;
    {
        struct rocketnet_address_t from;
        for ( int got = rocketnet_network_simulator_receive( wire, &client_address, time, &from, packet ); got > 0;
              got = rocketnet_network_simulator_receive( wire, &client_address, time, &from, packet ) )
        {
            drained++;
            int client_payload_bytes = 0;
            check( rocketnet_client_protocol_process_packet( &client, packet, got, time, payload, (int) sizeof(payload), &client_payload_bytes ) );
            if ( client.reassembly->completed_bytes > 0 )
            {
                check( client.reassembly->completed_bytes == CRB_PAYLOAD_BYTES );
                const uint8_t * data = client.reassembly->completed_payload;
                const int k = (int) data[0] | ( (int) data[1] << 8 );
                check( k < CRB_TOTAL_PAYLOADS );
                check( crb_payload_matches( data, k ) );
                delivered++;
            }
        }
    }

    const uint64_t reassembled = client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] - reassembled_before;
    const uint64_t batch_full = client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_BATCH_FULL] - batch_full_before;

    if ( drained != ROCKETNET_NUM_CLIENT_PACKETS || reassembled != CRB_TOTAL_PAYLOADS || batch_full != 0 || delivered != CRB_TOTAL_PAYLOADS )
        printf( "test_client_reassembled_batch_holds_half_received_load: drained %d reassembled %llu delivered %d BATCH_FULL %llu (delivered capacity %d)\n",
                drained, (unsigned long long) reassembled, delivered, (unsigned long long) batch_full, client.reassembly->delivered_capacity );

    check( drained == ROCKETNET_NUM_CLIENT_PACKETS );
    check( reassembled == CRB_TOTAL_PAYLOADS );
    check( batch_full == 0 );
    check( delivered == CRB_TOTAL_PAYLOADS );

    rocketnet_counters_assert_conserved( &server_counters );
    rocketnet_counters_assert_conserved( &client_counters );

    rocketnet_network_simulator_destroy( wire );
    rocketnet_reassembly_set_destroy( client.reassembly );
    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_reassembly_set_destroy( server.reassembly );
}

REGISTER_UNIT_TEST( test_client_reassembled_batch_holds_half_received_load );
