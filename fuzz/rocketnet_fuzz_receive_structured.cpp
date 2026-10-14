/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The stateful receive fuzz target (nightly): a real client and a real server
// carried across a full handshake and into the session, with the fuzz input
// consumed as a DECISION STREAM that interleaves legitimate traffic, injected
// garbage, corrupted-envelope datagrams, and time advances. This is the
// analog of yojimbo's fuzz_connection_structured -- where raw bytes almost
// never form a valid packet, this target reaches the deep receive state (ack
// reconstruction, fragment reassembly, replay window, the session sequence
// paths) by producing structurally valid packets from the real write paths
// and then letting the fuzzer decide what to damage and when.
//
// THE WIRE IS THE IN-REPO SIMULATOR (sdk/rocketnet_network_simulator.*), so
// there are no sockets and no real clocks: time is a variable the decision
// stream advances, and the simulator's own randomness is seeded from the
// fuzz input. The loss/latency/jitter config is fuzz-chosen too, so the
// out-of-order and drop paths are reachable.
//
// FOUR THINGS THE STREAM CAN DO each step, chosen from one action byte:
//   - advance time and pump both directions (the ordinary path);
//   - have the connected client send a payload or a fragmented payload
//     (drives ack blocks and reassembly with valid bytes);
//   - inject a fully-fuzzed datagram at the server (pre-decrypt garbage: the
//     prefilter, size and type ladder, and the mapping lookup);
//   - capture the last real client->server datagram, corrupt its sealed body
//     in place, and inject that (a valid envelope with a broken body: the
//     path past the filter and the mapping, into the AEAD open and replay).
//
// DETERMINISM, and why it holds despite crypto RNG. The protocol draws random
// bytes internally -- the client's salt, the server's session nonce and its
// table hash keys. libFuzzer needs a crashing input to REPLAY, and it does:
// no branch that decides crash-or-not turns on the VALUE of a random key. An
// AEAD open either succeeds into fixed-shape plaintext or fails on the MAC,
// and reassembly/ack/replay indices are driven by sequence numbers and the
// decision stream, not by key bytes. So a given input walks the same control
// flow every run. The simulator's own delivery order is fully determined by
// its seed, which is taken from the input.

#include "rocketnet.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_fragment.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

// a minimal cursor over the fuzz input; reads past the end return zero, so
// the stream simply runs dry and the loop ends

struct rocketnet_fuzz_reader
{
    const uint8_t * data;
    size_t size;
    size_t pos;
};

static uint8_t fuzz_u8( struct rocketnet_fuzz_reader * r )
{
    return r->pos < r->size ? r->data[r->pos++] : 0;
}

static uint16_t fuzz_u16( struct rocketnet_fuzz_reader * r )
{
    const uint16_t a = fuzz_u8( r );
    const uint16_t b = fuzz_u8( r );
    return (uint16_t) ( a | ( b << 8 ) );
}

static void fuzz_bytes( struct rocketnet_fuzz_reader * r, uint8_t * out, int count )
{
    for ( int i = 0; i < count; i++ )
        out[i] = fuzz_u8( r );
}

static void generate_payload( uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;
    for ( int i = 0; i < payload_bytes; i++ )
        payload_data[i] = (uint8_t) ( start + i );
}

// the file's abort-oracle helper: a failed property aborts, so it survives
// NDEBUG and the sanitizers stop at the exact line the invariant broke.

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
        return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

// The connection state a refused packet is forbidden to touch: the idle
// clock, the two send sequence trackers, the fragment payload id, and both
// replay windows, byte for byte. A refusal that marks any of these is the
// slotless-session mark class: the packet is refused yet mutates state the
// refusal promised to leave alone.

struct rocketnet_fuzz_mapping_state
{
    double last_receive_time;
    uint64_t send_sequence_handshake;
    uint64_t send_sequence_session;
    uint64_t send_fragment_payload_id;
    struct rocketnet_replay_protection_t replay_handshake;
    struct rocketnet_replay_protection_t replay_session;
};

static void rocketnet_fuzz_snapshot_mapping( const struct rocketnet_encryption_mapping_t * mapping,
                                             struct rocketnet_fuzz_mapping_state * out )
{
    out->last_receive_time = mapping->last_receive_time;
    out->send_sequence_handshake = mapping->send_sequence_handshake;
    out->send_sequence_session = mapping->send_sequence_session;
    out->send_fragment_payload_id = mapping->send_fragment_payload_id;
    out->replay_handshake = mapping->replay_handshake;
    out->replay_session = mapping->replay_session;
}

static int rocketnet_fuzz_mapping_unchanged( const struct rocketnet_encryption_mapping_t * mapping,
                                             const struct rocketnet_fuzz_mapping_state * snapshot )
{
    return snapshot->last_receive_time == mapping->last_receive_time &&
           snapshot->send_sequence_handshake == mapping->send_sequence_handshake &&
           snapshot->send_sequence_session == mapping->send_sequence_session &&
           snapshot->send_fragment_payload_id == mapping->send_fragment_payload_id &&
           memcmp( &snapshot->replay_handshake, &mapping->replay_handshake, sizeof(snapshot->replay_handshake) ) == 0 &&
           memcmp( &snapshot->replay_session, &mapping->replay_session, sizeof(snapshot->replay_session) ) == 0;
}

// Counter ledger conservation, from the standard's observability section:
// every packet family's arrival equals the sum of its terminal outcomes, and
// the connected ledger balances. The harness mints one token for one client,
// so a token admitted once can never yield a second CLIENT_CONNECTED.

static void rocketnet_fuzz_check_ledger( const struct rocketnet_server_protocol_t * server )
{
    const uint64_t * c = server->counters->values;

    const uint64_t request_outcomes =
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SIZE] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_ALREADY_CONNECTED] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_CHALLENGE_RESENT] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_VERSION] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_PROTOCOL_ID] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_EXPIRED] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_TIMEOUT_INVALID] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_BAD_ADDRESS_FLAGS] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_ADDRESS_NOT_ADVERTISED] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_WRONG_SERVER_KEY] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_COOKIE] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_DID_NOT_VERIFY] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_COOKIE_EXPIRED] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_USED] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_SIGNATURE_DID_NOT_VERIFY] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_OPEN_SEALED_BLOCK] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_SEALED_MISMATCH] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_CLIENT_ID_ALREADY_CONNECTED] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_TOKEN_ALREADY_CONNECTED] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_SMALL_ORDER_KEY] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_COULD_NOT_ADD_MAPPING] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_SENT_CHALLENGE];
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] == request_outcomes,
                            "connection request ledger: received == sum of outcomes" );

    const uint64_t response_outcomes =
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_MAPPING] +
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ZERO_SEQUENCE] +
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_DID_NOT_OPEN] +
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ALREADY_CONNECTED] +
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED] +
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_ADMITTED_SET_FULL] +
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_NO_SLOTS] +
        c[ROCKETNET_COUNTER_CONNECTION_REQUEST_INTERNAL_ERROR] +
        c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_CONNECTED];
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED] == response_outcomes,
                            "connection response ledger: received == sum of outcomes" );

    const uint64_t keep_alive_outcomes =
        c[ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_SIZE] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_NO_MAPPING] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_ZERO_SEQUENCE] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_ALREADY_RECEIVED] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_DID_NOT_OPEN] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_NOT_CONNECTED] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_BAD_ACK_BLOCK] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_WRONG_OPENED_SIZE] +
        c[ROCKETNET_COUNTER_KEEP_ALIVE_ACCEPTED];
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED] == keep_alive_outcomes,
                            "keep alive ledger: received == sum of outcomes" );

    const uint64_t payload_outcomes =
        c[ROCKETNET_COUNTER_PAYLOAD_WRONG_SIZE] +
        c[ROCKETNET_COUNTER_PAYLOAD_NO_MAPPING] +
        c[ROCKETNET_COUNTER_PAYLOAD_ZERO_SEQUENCE] +
        c[ROCKETNET_COUNTER_PAYLOAD_ALREADY_RECEIVED] +
        c[ROCKETNET_COUNTER_PAYLOAD_DID_NOT_OPEN] +
        c[ROCKETNET_COUNTER_PAYLOAD_NOT_CONNECTED] +
        c[ROCKETNET_COUNTER_PAYLOAD_BAD_ACK_BLOCK] +
        c[ROCKETNET_COUNTER_PAYLOAD_WRONG_OPENED_SIZE] +
        c[ROCKETNET_COUNTER_PAYLOAD_DELIVERED];
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_PAYLOAD_RECEIVED] == payload_outcomes,
                            "payload ledger: received == sum of outcomes" );

    const uint64_t disconnect_outcomes =
        c[ROCKETNET_COUNTER_DISCONNECT_WRONG_SIZE] +
        c[ROCKETNET_COUNTER_DISCONNECT_NO_MAPPING] +
        c[ROCKETNET_COUNTER_DISCONNECT_ZERO_SEQUENCE] +
        c[ROCKETNET_COUNTER_DISCONNECT_ALREADY_RECEIVED] +
        c[ROCKETNET_COUNTER_DISCONNECT_DID_NOT_OPEN] +
        c[ROCKETNET_COUNTER_DISCONNECT_NOT_CONNECTED] +
        c[ROCKETNET_COUNTER_DISCONNECT_BAD_ACK_BLOCK] +
        c[ROCKETNET_COUNTER_DISCONNECT_WRONG_OPENED_SIZE] +
        c[ROCKETNET_COUNTER_DISCONNECT_DISCONNECTED];
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] == disconnect_outcomes,
                            "disconnect ledger: received == sum of outcomes" );

    const uint64_t ack_outcomes =
        c[ROCKETNET_COUNTER_ACK_WRONG_SIZE] +
        c[ROCKETNET_COUNTER_ACK_NO_MAPPING] +
        c[ROCKETNET_COUNTER_ACK_ZERO_SEQUENCE] +
        c[ROCKETNET_COUNTER_ACK_ALREADY_RECEIVED] +
        c[ROCKETNET_COUNTER_ACK_DID_NOT_OPEN] +
        c[ROCKETNET_COUNTER_ACK_NOT_CONNECTED] +
        c[ROCKETNET_COUNTER_ACK_BAD_ACK_BLOCK] +
        c[ROCKETNET_COUNTER_ACK_WRONG_OPENED_SIZE] +
        c[ROCKETNET_COUNTER_ACK_ACCEPTED];
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_ACK_RECEIVED] == ack_outcomes,
                            "ack ledger: received == sum of outcomes" );

    const uint64_t fragment_outcomes =
        c[ROCKETNET_COUNTER_FRAGMENT_WRONG_SIZE] +
        c[ROCKETNET_COUNTER_FRAGMENT_NO_MAPPING] +
        c[ROCKETNET_COUNTER_FRAGMENT_ZERO_SEQUENCE] +
        c[ROCKETNET_COUNTER_FRAGMENT_ALREADY_RECEIVED] +
        c[ROCKETNET_COUNTER_FRAGMENT_DID_NOT_OPEN] +
        c[ROCKETNET_COUNTER_FRAGMENT_NOT_CONNECTED] +
        c[ROCKETNET_COUNTER_FRAGMENT_BAD_ACK_BLOCK] +
        c[ROCKETNET_COUNTER_FRAGMENT_WRONG_OPENED_SIZE] +
        c[ROCKETNET_COUNTER_FRAGMENT_ZERO_PAYLOAD_ID] +
        c[ROCKETNET_COUNTER_FRAGMENT_STALE] +
        c[ROCKETNET_COUNTER_FRAGMENT_COUNT_INVALID] +
        c[ROCKETNET_COUNTER_FRAGMENT_INDEX_OUT_OF_RANGE] +
        c[ROCKETNET_COUNTER_FRAGMENT_COUNT_MISMATCH] +
        c[ROCKETNET_COUNTER_FRAGMENT_SIZE_MISMATCH] +
        c[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_TOO_LARGE] +
        c[ROCKETNET_COUNTER_FRAGMENT_DUPLICATE_INDEX] +
        c[ROCKETNET_COUNTER_FRAGMENT_ACCEPTED];
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] == fragment_outcomes,
                            "fragment ledger: received == sum of outcomes" );

    rocketnet_fuzz_require(
        c[ROCKETNET_COUNTER_CLIENT_CONNECTED] ==
            c[ROCKETNET_COUNTER_CLIENT_DISCONNECTED] +
            c[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] +
            (uint64_t) rocketnet_client_slots_num_connected( server->client_slots ),
        "connected ledger: CLIENT_CONNECTED == DISCONNECTED + TIMED_OUT + connected now" );

    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_CLIENT_CONNECTED] <= 1,
                            "a token admitted once never yields a second CLIENT_CONNECTED" );
}

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;
    if ( sodium_init() < 0 )
        abort();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    if ( size < 4 )
        return 0;

    struct rocketnet_fuzz_reader r = { data, size, 0 };

    // the wire's seed and its abuse profile come from the input's head, so a
    // reproducer replays the exact wire
    const uint64_t simulator_seed = (uint64_t) fuzz_u16( &r ) | ( (uint64_t) fuzz_u16( &r ) << 16 );
    const float loss_percent = (float) ( fuzz_u8( &r ) % 61 );          // up to 60% loss
    const float duplicate_percent = (float) ( fuzz_u8( &r ) % 21 );     // up to 20% duplicates

    // -------- the server, on the development keys, at the vector address --------

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
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server.admitted_token_set = rocketnet_admitted_token_set_create( NULL, 64 );
    server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    server.reassembly = rocketnet_reassembly_set_create( NULL, 4, 8, 16384, 4 * 8 );
    server.counters = &server_counters;

    // -------- one client, and the wire between them --------

    struct rocketnet_address_t server_address;
    rocketnet_address_parse( &server_address, "203.0.113.10:40000" );
    struct rocketnet_address_t client_address;
    rocketnet_address_parse( &client_address, "198.51.100.10:56789" );

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 256, simulator_seed );
    rocketnet_network_simulator_config( simulator, 0.05, 0.02, loss_percent, duplicate_percent );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof(client_counters) );
    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );
    memcpy( client.filter_value, tag, ROCKETNET_FILTER_VALUE_BYTES );

    // the token the client connects with, minted with the real path
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        memset( &signed_fields, 0, sizeof(signed_fields) );
        memset( &private_fields, 0, sizeof(private_fields) );
        signed_fields.version = ROCKETNET_VERSION;
        signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
        signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
        signed_fields.expire_timestamp = ROCKETNET_TEST_CREATE_TS + 300;
        signed_fields.timeout_seconds = 30;
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
        const uint8_t server_address_bytes[6] = { 203, 0, 113, 10, 0x40, 0x9c };
        memcpy( signed_fields.ipv4_address, server_address_bytes, 6 );
        memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, 32 );
        private_fields.client_id = 1;
        randombytes_buf( private_fields.client_to_server_key, 32 );
        randombytes_buf( private_fields.server_to_client_key, 32 );
        if ( !rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_dev_backend_secret_key, token_data ) )
            abort();
    }

    double time = 100.0;
    uint64_t timestamp = ROCKETNET_TEST_CREATE_TS;
    rocketnet_client_protocol_connect( &client, token_data, time, timestamp );

    // the last real client->server datagram, kept so the stream can corrupt a
    // genuine envelope rather than only inject pure garbage
    uint8_t last_c2s[ROCKETNET_RECEIVE_CEILING];
    int last_c2s_bytes = 0;

    int payload_size = 1;
    const int MAX_OPS = 4096;

    for ( int op = 0; op < MAX_OPS && r.pos < r.size; op++ )
    {
        const uint8_t action = fuzz_u8( &r );

        // client drive -> wire, always: the handshake only completes if the
        // client's packets actually reach the server, so this runs every step
        {
            uint8_t packet[ROCKETNET_RECEIVE_CEILING];
            int packet_bytes = 0;
            rocketnet_client_protocol_update( &client, time, packet, &packet_bytes );
            if ( packet_bytes > 0 )
            {
                if ( packet_bytes <= ROCKETNET_RECEIVE_CEILING )
                {
                    memcpy( last_c2s, packet, packet_bytes );
                    last_c2s_bytes = packet_bytes;
                }
                rocketnet_network_simulator_send( simulator, &client_address, &server_address, packet, packet_bytes, time );
            }
        }

        // a connected client sends fuzz-sized traffic when the stream asks
        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && ( action & 3 ) == 1 )
        {
            uint8_t packet[ROCKETNET_RECEIVE_CEILING];
            uint8_t payload[ROCKETNET_MAX_PAYLOAD_BYTES];
            generate_payload( payload, payload_size );
            const int bytes = rocketnet_client_protocol_write_payload( &client, payload, payload_size, time, packet );
            if ( bytes > 0 )
                rocketnet_network_simulator_send( simulator, &client_address, &server_address, packet, bytes, time );
            payload_size = payload_size < ROCKETNET_MAX_PAYLOAD_BYTES ? payload_size + 1 : 1;
        }

        // occasionally a fragmented payload, so reassembly soaks under the
        // same hostile wire and the fragment sequence paths are reached
        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && ( action & 3 ) == 2 )
        {
            static uint8_t fragmented_payload[16384];
            const int fragmented_bytes = ROCKETNET_MAX_PAYLOAD_BYTES + 1 + ( fuzz_u16( &r ) % 4096 );
            generate_payload( fragmented_payload, fragmented_bytes );
            static uint8_t fragment_packets[16 * ROCKETNET_SEND_BUDGET];
            int fragment_packet_bytes[16];
            const int fragment_count = rocketnet_client_protocol_write_fragments( &client, fragmented_payload, fragmented_bytes,
                                                                                  time, fragment_packets, fragment_packet_bytes, 16 );
            for ( int i = 0; i < fragment_count; i++ )
                rocketnet_network_simulator_send( simulator, &client_address, &server_address,
                                                  fragment_packets + i * ROCKETNET_SEND_BUDGET, fragment_packet_bytes[i], time );
        }

        // inject pure garbage at the server: a fuzz-sized, fuzz-filled datagram
        // straight into the receive entry (the pre-decrypt path). the size is a
        // legal wire size, [1, ROCKETNET_RECEIVE_CEILING] -- the simulator is
        // the wire and a datagram off the wire is always within those bounds;
        // the too-short and too-large rejections are the prefilter's, gated by
        // the stateless target's corpus, not something the wire can carry here.
        if ( ( action & 0x30 ) == 0x10 )
        {
            const int garbage_bytes = 1 + fuzz_u16( &r ) % ROCKETNET_RECEIVE_CEILING;
            uint8_t garbage[ROCKETNET_RECEIVE_CEILING];
            fuzz_bytes( &r, garbage, garbage_bytes );
            rocketnet_network_simulator_send( simulator, &client_address, &server_address, garbage, garbage_bytes, time );
        }

        // corrupt a real envelope: take the last genuine client->server
        // datagram, flip fuzz-chosen bytes inside its sealed body (past the
        // 6-byte prefilter and the 4-byte sequence, so the filter and mapping
        // still admit it and the damage lands at the AEAD open / replay)
        if ( ( action & 0x30 ) == 0x20 && last_c2s_bytes > ROCKETNET_PREFILTER_BYTES + 4 )
        {
            uint8_t corrupt[ROCKETNET_RECEIVE_CEILING];
            memcpy( corrupt, last_c2s, last_c2s_bytes );
            const int flips = 1 + ( fuzz_u8( &r ) % 4 );
            for ( int f = 0; f < flips; f++ )
            {
                const int body_span = last_c2s_bytes - ( ROCKETNET_PREFILTER_BYTES + 4 );
                const int position = ROCKETNET_PREFILTER_BYTES + 4 + ( fuzz_u16( &r ) % body_span );
                corrupt[position] ^= fuzz_u8( &r );
            }
            rocketnet_network_simulator_send( simulator, &client_address, &server_address, corrupt, last_c2s_bytes, time );
        }

        // server drains the wire, answers, and its answers ride back
        {
            rocketnet_reassembly_set_begin_update( server.reassembly );
            struct rocketnet_address_t from;
            uint8_t packet[ROCKETNET_RECEIVE_CEILING];
            int bytes;
            while ( ( bytes = rocketnet_network_simulator_receive( simulator, &server_address, time, &from, packet ) ) > 0 )
            {
                uint8_t response[ROCKETNET_RECEIVE_CEILING];
                int response_bytes = 0;
                uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
                int delivered_bytes = 0;
                int delivered_client_index = -1;

                // snapshot the mapping this datagram addresses, if any, so a
                // refused packet is proven to leave it byte-for-byte unchanged
                const int before_index = rocketnet_encryption_map_find( server.encryption_map, &from );
                struct rocketnet_fuzz_mapping_state before_state;
                const int had_mapping = before_index != -1;
                if ( had_mapping )
                    rocketnet_fuzz_snapshot_mapping( rocketnet_encryption_map_entry( server.encryption_map, before_index ), &before_state );

                const int process_result = rocketnet_server_protocol_process_packet( &server, &from, packet, bytes, timestamp, time,
                                                          response, &response_bytes, delivered, &delivered_bytes, &delivered_client_index );

                if ( had_mapping && ( process_result == ROCKETNET_SERVER_PROCESS_IGNORED || process_result == ROCKETNET_SERVER_PROCESS_SEND_DENIED ) )
                {
                    const int after_index = rocketnet_encryption_map_find( server.encryption_map, &from );
                    rocketnet_fuzz_require( after_index != -1, "a refused packet must not remove its mapping" );
                    rocketnet_fuzz_require(
                        rocketnet_fuzz_mapping_unchanged( rocketnet_encryption_map_entry( server.encryption_map, after_index ), &before_state ),
                        "a refused packet must leave last_receive_time, the sequence trackers and the replay window unchanged" );
                }
                if ( response_bytes > 0 )
                {
                    if ( response_bytes > ROCKETNET_RECEIVE_CEILING )
                        abort();
                    rocketnet_network_simulator_send( simulator, &server_address, &from, response, response_bytes, time );
                }
                if ( delivered_bytes > 0 && delivered_bytes > ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES )
                    abort();
            }

            rocketnet_server_protocol_update( &server, time );

            for ( int i = 0; i < 4; i++ )
            {
                uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
                const int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server, i, time, keep_alive );
                if ( keep_alive_bytes > 0 )
                {
                    const int mapping_index = rocketnet_client_slots_mapping_index( server.client_slots, i );
                    const struct rocketnet_address_t * to = rocketnet_encryption_map_address( server.encryption_map, mapping_index );
                    if ( to )
                        rocketnet_network_simulator_send( simulator, &server_address, to, keep_alive, keep_alive_bytes, time );
                }
            }
        }

        // client drains its inbound
        {
            struct rocketnet_address_t from;
            uint8_t packet[ROCKETNET_RECEIVE_CEILING];
            int bytes;
            while ( ( bytes = rocketnet_network_simulator_receive( simulator, &client_address, time, &from, packet ) ) > 0 )
            {
                uint8_t delivered[ROCKETNET_RECEIVE_CEILING];
                int delivered_bytes = 0;
                rocketnet_client_protocol_process_packet( &client, packet, bytes, time, delivered, &delivered_bytes );
                if ( delivered_bytes > 0 && delivered_bytes > ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES )
                    abort();
            }
        }

        // time and the token clock advance by a fuzz-chosen small step, so
        // deadlines, key rotation and the replay window all move
        const double step = 0.005 + ( fuzz_u8( &r ) % 32 ) * 0.01;
        time += step;
        timestamp = ROCKETNET_TEST_CREATE_TS + (uint64_t) ( time - 100.0 );
    }

    rocketnet_fuzz_check_ledger( &server );

    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_admitted_token_set_destroy( server.admitted_token_set );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_reassembly_set_destroy( server.reassembly );
    rocketnet_network_simulator_destroy( simulator );

    return 0;
}
