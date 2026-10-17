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

#include "rocketnet.h"
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
    signed_fields.version = ROCKETNET_VERSION;
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

static void server_reassembled_batch_holds_legitimate_load()
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
    memcpy( server.filter_value, rocketnet_test_filter_value, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_test_backend_public_key, 32 );
    memcpy( server.server_public_key, rocketnet_test_server_public_key, 32 );
    memcpy( server.server_secret_key, rocketnet_test_server_secret_key, 32 );
    struct rocketnet_address_t server_address;
    check( rocketnet_address_parse( &server_address, "203.0.113.10:40000" ) );
    server.advertised_ipv4 = server_address;
    memcpy( server.cookie_key, rocketnet_test_cookie_key, 32 );
    server.cookie_lifetime_seconds = 3;
    server.max_token_lifetime_seconds = 600;
    server.handshake_deadline_seconds = 5.0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, SRB_NUM_CLIENTS * 2 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, SRB_NUM_CLIENTS * 8 );
    server.admitted_token_set = rocketnet_admitted_token_set_create( NULL, SRB_NUM_CLIENTS * 2 );
    server.client_slots = rocketnet_client_slots_create( NULL, SRB_NUM_CLIENTS );
    server.acks = rocketnet_server_acks_create( NULL, SRB_NUM_CLIENTS );
    const int delivered_capacity = rocketnet_server_reassembly_delivered_capacity( SRB_NUM_CLIENTS, num_slots );
    check( delivered_capacity >= SRB_TOTAL_PAYLOADS );
    server.reassembly = rocketnet_reassembly_set_create( NULL, SRB_NUM_CLIENTS, num_slots, max_payload_bytes, delivered_capacity );
    server.counters = &server_counters;
    check( server.encryption_map && server.used_token_history && server.admitted_token_set &&
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
        srb_mint_token( token_data, 0x5B000000ULL + (uint64_t) i );
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
        printf( "server_reassembled_batch_holds_legitimate_load: reassembled %llu delivered %d BATCH_FULL %llu (delivered capacity %d)\n",
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
        check( !seen[delivered_client[d]][delivered_k[d]] );
        seen[delivered_client[d]][delivered_k[d]] = true;
    }

    rocketnet_server_protocol_update( &server, time );

    rocketnet_counters_assert_conserved( &server_counters );
    rocketnet_counters_assert_conserved( &client_counters );

    rocketnet_network_simulator_destroy( wire );
    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_admitted_token_set_destroy( server.admitted_token_set );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_reassembly_set_destroy( server.reassembly );

    printf( "server_reassembled_batch_holds_legitimate_load\n" );
}

REGISTER_UNIT_TEST( server_reassembled_batch_holds_legitimate_load );
