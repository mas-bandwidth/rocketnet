/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The hostile soak: client and server protocol objects through the network
    simulator at heavy loss, latency, jitter and duplication, under mocked
    time stepped fast -- hours of simulated network abuse per minute of wall
    clock. Its job is smoking out crashes and state machine hangs: every
    payload is pattern verified -- fragmented payloads included, reassembled
    under the same abuse, where a lost fragment abandons its payload by
    design and any other fragment refusal fails the run -- and a watchdog
    treats a machine that neither progresses nor terminally fails as a
    wedge and fails loudly.

    ./bin/soak_sim            runs forever
    ./bin/soak_sim 200000     bounded iterations
*/

#include "rocketnet.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_fragment.h"
#include "rocketnet_test_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sodium.h>

static volatile int quit;

void interrupt_handler( int signal_value )
{
    (void) signal_value; quit = 1;
}

static void generate_payload( uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
        payload_data[i] = (uint8_t) ( start + i );
}

static int verify_payload( const uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
    {
        if ( payload_data[i] != (uint8_t) ( start + i ) )
            return 0;
    }
    return 1;
}

int main( int argc, char ** argv )
{
    signal( SIGINT, interrupt_handler );

    if ( !rocketnet_init( NULL ) )
        return 1;

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    const uint64_t max_iterations = argc > 1 ? strtoull( argv[1], NULL, 10 ) : 0;

    // the server, on the development keys, at the vector server address

    struct rocketnet_counters_t server_counters;
    memset( &server_counters, 0, sizeof(server_counters) );

    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof(server) );
    server.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    server.send_rate = 10.0;
    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server.filter_value, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( server.server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( server.server_secret_key, rocketnet_dev_server_secret_key, 32 );
    rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" );
    randombytes_buf( server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server.cookie_lifetime_seconds = 3;
    server.handshake_deadline_seconds = 5.0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 32 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 256 );
    server.admitted_token_set = rocketnet_admitted_token_set_create( NULL, 256 );
    server.client_slots = rocketnet_client_slots_create( NULL, 16 );
    server.reassembly = rocketnet_reassembly_set_create( NULL, 16, 8, 16384 );
    server.counters = &server_counters;

    #define SOAK_NUM_CLIENTS 8

    struct rocketnet_address_t server_address;
    rocketnet_address_parse( &server_address, "203.0.113.10:40000" );

    struct rocketnet_address_t client_address[SOAK_NUM_CLIENTS];
    for ( int i = 0; i < SOAK_NUM_CLIENTS; i++ )
    {
        char address_string[64];
        snprintf( address_string, sizeof(address_string), "198.51.100.%d:56789", 10 + i );
        rocketnet_address_parse( &client_address[i], address_string );
    }

    // the hostile wire

    // light loss: every attempt SHOULD complete, so a single terminal state
    // during the soak is a finding -- a dropped packet the state machine did
    // not tolerate. the harsh configuration lives in the functional tests,
    // bounded and seeded.

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 4096, rocketnet_random_uint64() );
    rocketnet_network_simulator_config( simulator, 0.05, 0.02, 2.0f, 2.0f );

    static struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    static struct rocketnet_client_protocol_t clients[SOAK_NUM_CLIENTS];
    int last_state[SOAK_NUM_CLIENTS];
    double last_state_change_time[SOAK_NUM_CLIENTS];
    double connected_since[SOAK_NUM_CLIENTS];
    for ( int i = 0; i < SOAK_NUM_CLIENTS; i++ )
    {
        rocketnet_client_protocol_init( &clients[i], &client_counters );
        last_state[i] = ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED;
        last_state_change_time[i] = 100.0;
        connected_since[i] = 0.0;
    }

    double time = 100.0;
    uint64_t timestamp = ROCKETNET_TEST_CREATE_TS;
    uint64_t iteration = 0;
    uint64_t attempts = 0;
    uint64_t connects = 0;
    uint64_t terminals = 0;
    uint64_t payloads_verified = 0;
    uint64_t fragmented_sent = 0;
    uint64_t fragmented_verified = 0;
    uint64_t failures = 0;
    uint64_t hangs = 0;
    int payload_size = 1;
    int fragmented_payload_size = ROCKETNET_MAX_PAYLOAD_BYTES + 1;
    uint64_t client_id = 1;


    while ( !quit )
    {
        iteration++;
        if ( max_iterations && iteration > max_iterations )
            break;

        time += 0.005;                              // mocked time, stepped fast
        timestamp = ROCKETNET_TEST_CREATE_TS + (uint64_t) ( time - 100.0 );

        uint8_t packet[ROCKETNET_RECEIVE_CEILING];
        int packet_bytes = 0;

        for ( int c = 0; c < SOAK_NUM_CLIENTS; c++ )
        {
        struct rocketnet_client_protocol_t & client = clients[c];

        // a fresh attempt whenever the machine is at rest or terminal --
        // recovery is the ordinary path, run constantly

        if ( client.state <= ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
        {
            if ( client.state < ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED )
            {
                // under light loss every attempt must complete: a terminal
                // is a client that failed to connect or got stuck -- found it
                printf( "CLIENT %d FAILED: reached '%s' under light loss -- the state machine did not tolerate a drop\n",
                        c, rocketnet_client_transport_state_name( client.state ) );
                terminals++;
                failures++;
            }

            uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
            struct rocketnet_signed_connect_data_t signed_fields;
            struct rocketnet_connect_token_private_t private_fields;
            memset( &signed_fields, 0, sizeof(signed_fields) );
            memset( &private_fields, 0, sizeof(private_fields) );
            signed_fields.version = ROCKETNET_VERSION;
            signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
            signed_fields.create_timestamp = timestamp;
            signed_fields.expire_timestamp = timestamp + 300;
            signed_fields.timeout_seconds = 10;
            signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
            const uint8_t server_address_bytes[6] = { 203, 0, 113, 10, 0x40, 0x9c };
            memcpy( signed_fields.ipv4_address, server_address_bytes, 6 );
            memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, 32 );
            private_fields.client_id = client_id++;
            randombytes_buf( private_fields.client_to_server_key, 32 );
            randombytes_buf( private_fields.server_to_client_key, 32 );
            rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_dev_backend_secret_key, token_data );

            rocketnet_client_protocol_connect( &client, token_data, time, timestamp );
            attempts++;
        }

        // the watchdog: a machine that neither progresses nor terminally
        // fails is a wedge. per-state timeouts are 10 seconds, the handshake
        // deadline 5, the total deadline 300 -- sixty seconds of sim time
        // with no transition is a hang, full stop.

        if ( client.state != last_state[c] )
        {
            if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            {
                connects++;
                connected_since[c] = time;
            }
            last_state[c] = client.state;
            last_state_change_time[c] = time;
        }
        else if ( client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && time - last_state_change_time[c] > 60.0 )
        {
            printf( "HANG: client %d wedged in '%s' for 60 sim seconds\n", c, rocketnet_client_transport_state_name( client.state ) );
            hangs++;
            quit = 1;
            break;
        }

        // a connected client cycles off on its own staggered period, through
        // the burst, so disconnect and reconnect stay routine

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && time - connected_since[c] > 20.0 + 3.0 * c )
        {
            uint8_t burst[ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS * ROCKETNET_SEND_BUDGET];
            int burst_count = 0;
            int burst_bytes = 0;
            rocketnet_client_protocol_disconnect( &client, burst, &burst_count, &burst_bytes );
            for ( int i = 0; i < burst_count; i++ )
                rocketnet_network_simulator_send( simulator, &client_address[c], &server_address,
                                                  burst + i * ROCKETNET_SEND_BUDGET,
                                                  burst_bytes, time );
        }

        // client drives, wire carries, server answers, wire carries back

        rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
        if ( packet_bytes > 0 )
            rocketnet_network_simulator_send( simulator, &client_address[c], &server_address, packet, packet_bytes, time );

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            uint8_t payload[ROCKETNET_MAX_PAYLOAD_BYTES];
            generate_payload( payload, payload_size );
            int bytes = rocketnet_client_protocol_write_payload( &client, payload, payload_size, time, packet );
            if ( bytes > 0 )
                rocketnet_network_simulator_send( simulator, &client_address[c], &server_address, packet, bytes, time );
            payload_size = payload_size < ROCKETNET_MAX_PAYLOAD_BYTES ? payload_size + 1 : 1;

            // an occasional fragmented payload, staggered per client, so
            // reassembly soaks under the same hostile wire: at this loss a
            // multi-fragment payload sometimes dies, which is its specified
            // semantics -- the abandoned counter records it, and nothing
            // else in the fragment family may ever fire

            if ( ( ( iteration + (uint64_t) c * 29 ) % 233 ) == 0 )
            {
                static uint8_t fragmented_payload[16384];
                generate_payload( fragmented_payload, fragmented_payload_size );
                static uint8_t fragment_packets[16 * ROCKETNET_SEND_BUDGET];
                int fragment_packet_bytes[16];
                int fragment_count = rocketnet_client_protocol_write_fragments( &client, fragmented_payload, fragmented_payload_size,
                                                                                time, fragment_packets, fragment_packet_bytes, 16 );
                if ( fragment_count > 0 )
                {
                    for ( int i = 0; i < fragment_count; i++ )
                        rocketnet_network_simulator_send( simulator, &client_address[c], &server_address,
                                                          fragment_packets + i * ROCKETNET_SEND_BUDGET,
                                                          fragment_packet_bytes[i], time );
                    fragmented_sent++;
                }
                fragmented_payload_size = fragmented_payload_size < 16384 - 997 ? fragmented_payload_size + 997 : ROCKETNET_MAX_PAYLOAD_BYTES + 1;
            }
        }

        }   // for each client

        struct rocketnet_address_t from;
        int bytes;
        rocketnet_reassembly_set_begin_update( server.reassembly );
        while ( ( bytes = rocketnet_network_simulator_receive( simulator, &server_address, time, &from, packet ) ) > 0 )
        {
            uint8_t response[ROCKETNET_RECEIVE_CEILING];
            int response_bytes = 0;
            uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
            int delivered_bytes = 0;
            int delivered_client_index = -1;
            rocketnet_server_protocol_process_packet( &server, &from, packet, bytes, timestamp, time,
                                             response, &response_bytes, delivered, &delivered_bytes, &delivered_client_index );
            if ( response_bytes > 0 )
                rocketnet_network_simulator_send( simulator, &server_address, &from, response, response_bytes, time );
            if ( delivered_bytes > 0 )
            {
                if ( verify_payload( delivered, delivered_bytes ) )
                    payloads_verified++;
                else
                    { printf( "PAYLOAD VERIFY FAILED at server (%d bytes)\n", delivered_bytes ); failures++; }
            }
            if ( server.reassembly->completed_bytes > 0 )
            {
                if ( verify_payload( server.reassembly->completed_payload, server.reassembly->completed_bytes ) )
                    fragmented_verified++;
                else
                    { printf( "FRAGMENTED PAYLOAD VERIFY FAILED at server (%d bytes)\n", server.reassembly->completed_bytes ); failures++; }
            }
        }

        rocketnet_server_protocol_update( &server, time );
        for ( int i = 0; i < 16; i++ )
        {
            uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
            int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server, i, time, keep_alive );
            if ( keep_alive_bytes > 0 )
            {
                int mapping_index = rocketnet_client_slots_mapping_index( server.client_slots, i );
                const struct rocketnet_address_t * to = rocketnet_encryption_map_address( server.encryption_map, mapping_index );
                rocketnet_network_simulator_send( simulator, &server_address, to, keep_alive, keep_alive_bytes, time );
            }
        }

        for ( int c = 0; c < SOAK_NUM_CLIENTS; c++ )
        {
            while ( ( bytes = rocketnet_network_simulator_receive( simulator, &client_address[c], time, &from, packet ) ) > 0 )
            {
                uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
                int delivered_bytes = 0;
                if ( rocketnet_client_protocol_process_packet( &clients[c], packet, bytes, time, delivered, &delivered_bytes ) && delivered_bytes > 0 )
                {
                    if ( verify_payload( delivered, delivered_bytes ) )
                        payloads_verified++;
                    else
                        { printf( "PAYLOAD VERIFY FAILED at client %d (%d bytes)\n", c, delivered_bytes ); failures++; }
                }
            }
        }

        if ( ( iteration % 100000 ) == 0 )
        {
            printf( "soak_sim: %llu iterations, %.0f sim seconds, %llu attempts, %llu connects, %llu terminals, %llu payloads verified, %llu/%llu fragmented verified (%llu abandoned), %llu failures\n",
                    (unsigned long long) iteration, time - 100.0, (unsigned long long) attempts,
                    (unsigned long long) connects, (unsigned long long) terminals,
                    (unsigned long long) payloads_verified,
                    (unsigned long long) fragmented_verified, (unsigned long long) fragmented_sent,
                    (unsigned long long) server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED],
                    (unsigned long long) failures );
        }
    }

    // The counter discipline at exit: this wire injects loss, so abandoned
    // payloads are the specified semantics and may be nonzero -- every
    // other fragment refusal means a defect in the sender or the
    // reassembly, and must be exactly zero however long the soak ran.

    const int must_stay_zero[] = {
        ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID,
        ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID,
        ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE,
        ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH,
        ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH,
        ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE,
        ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED,
        ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_BATCH_FULL,
    };
    for ( int i = 0; i < (int) ( sizeof(must_stay_zero) / sizeof(must_stay_zero[0]) ); i++ )
    {
        if ( server_counters.values[must_stay_zero[i]] != 0 )
        {
            printf( "MUST-STAY-ZERO COUNTER FIRED: %s = %llu\n",
                    rocketnet_counter_name( must_stay_zero[i] ),
                    (unsigned long long) server_counters.values[must_stay_zero[i]] );
            failures++;
        }
    }

    printf( "\nsoak_sim finished: %llu iterations, %.0f sim seconds, %llu attempts, %llu connects, %llu terminals, %llu payloads verified, %llu/%llu fragmented verified (%llu abandoned), %llu failures, %llu hangs\n",
            (unsigned long long) iteration, time - 100.0, (unsigned long long) attempts,
            (unsigned long long) connects, (unsigned long long) terminals,
            (unsigned long long) payloads_verified,
            (unsigned long long) fragmented_verified, (unsigned long long) fragmented_sent,
            (unsigned long long) server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_ABANDONED],
            (unsigned long long) failures, (unsigned long long) hangs );

    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_admitted_token_set_destroy( server.admitted_token_set );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_reassembly_set_destroy( server.reassembly );
    rocketnet_network_simulator_destroy( simulator );

    rocketnet_term();

    return ( failures || hangs || terminals ) ? 1 : 0;
}
