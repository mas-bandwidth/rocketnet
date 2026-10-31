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
// FIVE THINGS THE STREAM CAN DO each step, chosen from one action byte:
//   - advance time and pump both directions (the ordinary path);
//   - have the connected client send a payload or a fragmented payload
//     (drives ack blocks and reassembly with valid bytes);
//   - inject a fully-fuzzed datagram at the server (pre-decrypt garbage: the
//     prefilter, size and type ladder, and the mapping lookup);
//   - capture the last real client->server datagram, corrupt its sealed body
//     in place, and inject that (a valid envelope with a broken body: the
//     path past the filter and the mapping, into the AEAD open and replay);
//   - send that last real datagram again unmodified, from the client's own
//     address or from a second address (the replay refusals, a connection
//     response retransmitted after the slot is held, and one token's packets
//     arriving from two sources).
//
// THREE ORACLES run on every input, each through rocketnet_fuzz_require,
// which aborts, so each holds in an NDEBUG build too:
//   - a refused packet changes nothing in its mapping. The mapping survives
//     and its state is byte for byte what it was, except for exactly what
//     the standard's reading order says a refusal at that step leaves
//     behind (rocketnet_fuzz_expected_after_refusal);
//   - the counter ledgers balance after every packet, at both ends: every
//     RECEIVED counter equals the sum of its family's outcomes (the FAMILY
//     column of ROCKETNET_COUNTER_ROWS), and at the server CLIENT_CONNECTED
//     equals CLIENT_DISCONNECTED + CLIENT_TIMED_OUT + the slots connected now;
//   - one token, one live connection (TRANSPORT.md t12 rule 13): the token
//     holds at most one slot, and it connects only while it holds none, so
//     it may connect again once its connection has closed.
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

#include "rocketnet_internal.h"
#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_network_simulator.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_fragment.h"
#include "rocketnet_ack.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"
#include "rocketnet_replay.h"
#include "rocketnet_read_write.h"
#include "rocketnet_test_vectors.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

#include <cstdio>

#include "../portable_crt.h"

// the packet types the receive path dispatches on, one bit each, set when a
// counter shows the type arrived. The seed generator (ROCKETNET_FUZZ_SEED_DIR)
// writes one named seed per type and refuses to finish until every type is
// reached, so an input that stops covering a type is caught as INCOMPLETE.

enum
{
    ROCKETNET_FUZZ_RECEIVE_SERVER_REQUEST = 1ULL << 0,
    ROCKETNET_FUZZ_RECEIVE_SERVER_RESPONSE = 1ULL << 1,
    ROCKETNET_FUZZ_RECEIVE_SERVER_KEEP_ALIVE = 1ULL << 2,
    ROCKETNET_FUZZ_RECEIVE_SERVER_PAYLOAD = 1ULL << 3,
    ROCKETNET_FUZZ_RECEIVE_SERVER_DISCONNECT = 1ULL << 4,
    ROCKETNET_FUZZ_RECEIVE_SERVER_ACK = 1ULL << 5,
    ROCKETNET_FUZZ_RECEIVE_SERVER_FRAGMENT = 1ULL << 6,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_COOKIE = 1ULL << 7,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_CHALLENGE = 1ULL << 8,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_DENIED = 1ULL << 9,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_KEEP_ALIVE = 1ULL << 10,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_PAYLOAD = 1ULL << 11,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_DISCONNECT = 1ULL << 12,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_ACK = 1ULL << 13,
    ROCKETNET_FUZZ_RECEIVE_CLIENT_FRAGMENT = 1ULL << 14,

    // the oracles' own reach, one bit each, so the oracle seeds are held to
    // the same INCOMPLETE rule as the type seeds: a refusal checked against
    // a live mapping, a refusal after the open checked with its allowance
    // (a session packet marked received, or a connection response opened on
    // a mapping that holds its slot), a packet of the token's arriving from
    // the second address while its connection lives, the ledgers checked
    // with that connection live and refusals counted in three families, and
    // a request whose salt is not its half-open mapping's arriving later
    // than the mapping's last receive, so a wrongful idle refresh would show
    ROCKETNET_FUZZ_RECEIVE_ORACLE_REFUSAL = 1ULL << 15,
    ROCKETNET_FUZZ_RECEIVE_ORACLE_OPENED_REFUSAL = 1ULL << 16,
    ROCKETNET_FUZZ_RECEIVE_ORACLE_SECOND_SOURCE = 1ULL << 17,
    ROCKETNET_FUZZ_RECEIVE_ORACLE_LEDGER = 1ULL << 18,
    ROCKETNET_FUZZ_RECEIVE_ORACLE_SALT_MISMATCH = 1ULL << 19
};

static uint64_t rocketnet_fuzz_receive_coverage = 0;

static int rocketnet_fuzz_receive_generate_seed_dir( const char * directory );

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
    {
        out[i] = fuzz_u8( r );
    }
}

static void generate_payload( uint8_t * payload_data, int payload_bytes )
{
    const int start = payload_bytes % 256;

    for ( int i = 0; i < payload_bytes; i++ )
    {
        payload_data[i] = (uint8_t) ( start + i );
    }
}

// -----------------------------------------------------------------------
//  the oracles
// -----------------------------------------------------------------------

static void rocketnet_fuzz_require( bool condition, const char * property )
{
    if ( condition )
        return;
    fprintf( stderr, "fuzz oracle failed: %s\n", property );
    abort();
}

// the FAMILY column of the counter list, generated from the same rows the
// library's own tables are, so a counter added to a family is in its ledger
// here with no second list to keep

static const int rocketnet_fuzz_counter_family[ROCKETNET_NUM_COUNTERS] = {
#define ROCKETNET_FUZZ_COUNTER_FAMILY_ROW( name, klass, family ) ROCKETNET_COUNTER_##family,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_FUZZ_COUNTER_FAMILY_ROW )
#undef ROCKETNET_FUZZ_COUNTER_FAMILY_ROW
};

// every RECEIVED counter equals the sum of its family's outcomes: a packet
// counted as arrived takes exactly one outcome in the same call

static void rocketnet_fuzz_check_families( const struct rocketnet_counters_t * counters, const char * end )
{
    uint64_t outcomes[ROCKETNET_NUM_COUNTERS];
    uint8_t is_family[ROCKETNET_NUM_COUNTERS];
    memset( outcomes, 0, sizeof( outcomes ) );
    memset( is_family, 0, sizeof( is_family ) );

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        const int family = rocketnet_fuzz_counter_family[i];
        if ( family >= 0 )
        {
            outcomes[family] += counters->values[i];
            is_family[family] = 1;
        }
    }

    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        if ( is_family[i] && counters->values[i] != outcomes[i] )
        {
            char property[256];
            snprintf( property, sizeof( property ), "%s counter ledger: %s == the sum of its family's outcomes (%llu != %llu)",
                      end, rocketnet_counter_name( i ), (unsigned long long) counters->values[i], (unsigned long long) outcomes[i] );
            rocketnet_fuzz_require( false, property );
        }
    }
}

// the server's connection ledger: every connection made has ended by a
// disconnect (the client's, the server's or the integrity limit's, all
// counted CLIENT_DISCONNECTED) or a timeout, or holds a slot now

static void rocketnet_fuzz_check_connected( const struct rocketnet_server_protocol_t * server )
{
    const uint64_t * c = server->counters->values;
    rocketnet_fuzz_require( c[ROCKETNET_COUNTER_CLIENT_CONNECTED] ==
                                c[ROCKETNET_COUNTER_CLIENT_DISCONNECTED] + c[ROCKETNET_COUNTER_CLIENT_TIMED_OUT] +
                                    (uint64_t) rocketnet_client_slots_num_connected( server->client_slots ),
                            "connected ledger: CLIENT_CONNECTED == CLIENT_DISCONNECTED + CLIENT_TIMED_OUT + connected now" );
}

// the slots that hold a connection for this client id; the token carries
// one client id, so this is the token's live connections

static int rocketnet_fuzz_token_connections( const struct rocketnet_client_slots_t * slots, uint64_t client_id )
{
    int count = 0;
    const int max_clients = rocketnet_client_slots_max_clients( slots );
    for ( int i = 0; i < max_clients; i++ )
    {
        if ( rocketnet_client_slots_connected( slots, i ) && rocketnet_client_slots_client_id( slots, i ) == client_id )
            count++;
    }
    return count;
}

// One mapping's whole state: the cold payload and its control block, copied
// byte for byte.

struct rocketnet_fuzz_mapping_image
{
    struct rocketnet_encryption_mapping_t mapping;
    struct rocketnet_encryption_mapping_control_t control;
};

static void rocketnet_fuzz_mapping_copy( struct rocketnet_encryption_map_t * map, int index, struct rocketnet_fuzz_mapping_image * image )
{
    memset( image, 0, sizeof( *image ) );
    memcpy( &image->mapping, rocketnet_encryption_map_entry( map, index ), sizeof( image->mapping ) );
    memcpy( &image->control, rocketnet_encryption_map_control( map, index ), sizeof( image->control ) );
}

#define ROCKETNET_FUZZ_MOVED( name ) ( after[ROCKETNET_COUNTER_##name] != before[ROCKETNET_COUNTER_##name] )

#define ROCKETNET_FUZZ_SESSION_MOVED( suffix )                                                                  \
    ( ROCKETNET_FUZZ_MOVED( KEEP_ALIVE_##suffix ) || ROCKETNET_FUZZ_MOVED( PAYLOAD_##suffix ) ||               \
      ROCKETNET_FUZZ_MOVED( DISCONNECT_##suffix ) || ROCKETNET_FUZZ_MOVED( ACK_##suffix ) ||                    \
      ROCKETNET_FUZZ_MOVED( FRAGMENT_##suffix ) )

// What a refusal leaves behind in its mapping, by where in the reading order
// of TRANSPORT.md t10 it refused (the outcome counter that moved says
// where). Everything not set here must be unchanged, byte for byte.
//
//   - before the open (size, mapping, sequence, replay, slot gate): nothing;
//   - DID_NOT_OPEN (step 13): one integrity failure, and nothing else;
//   - a session packet refused after it opened, as BAD_ACK_BLOCK or
//     WRONG_OPENED_SIZE: its sequence is marked received (step 14);
//   - a session packet refused by its type-specific processing (a fragment
//     reassembly refuses, step 18): marked received, and the receive time
//     refreshed, which a fragment refusal does not undo;
//   - a connection response that opened and was then ignored, as
//     ALREADY_CONNECTED or CLIENT_ID_ALREADY_CONNECTED, or denied as
//     NO_SLOTS: the handshake reconstruction reference rises to its
//     sequence (the highest handshake sequence opened), and the idle clock
//     is refreshed only on a half-open mapping (t12 rule 14, t17 rule 4); a
//     mapping that holds its slot keeps its receive time. A denial is a
//     send, so it takes the next handshake sequence;
//   - a connection request from a half-open mapping's address is answered
//     with the stored challenge, CHALLENGE_RESENT, and refreshes the idle
//     clock only when its salt is the mapping's (t12 rule 3).
//
// opened is set when the packet opened under the mapping's keys.

static void rocketnet_fuzz_expected_after_refusal( const struct rocketnet_fuzz_mapping_image * previous,
                                                   const uint64_t * before,
                                                   const uint64_t * after,
                                                   const uint8_t * packet,
                                                   int packet_bytes,
                                                   double time,
                                                   struct rocketnet_fuzz_mapping_image * expected,
                                                   int * opened )
{
    memcpy( expected, previous, sizeof( *expected ) );
    *opened = 0;

    if ( ROCKETNET_FUZZ_MOVED( CONNECTION_RESPONSE_DID_NOT_OPEN ) || ROCKETNET_FUZZ_SESSION_MOVED( DID_NOT_OPEN ) )
    {
        expected->mapping.integrity_failures++;
        return;
    }

    const int type_specific = ROCKETNET_FUZZ_MOVED( FRAGMENT_ZERO_PAYLOAD_ID ) || ROCKETNET_FUZZ_MOVED( FRAGMENT_STALE ) ||
                              ROCKETNET_FUZZ_MOVED( FRAGMENT_COUNT_INVALID ) || ROCKETNET_FUZZ_MOVED( FRAGMENT_INDEX_OUT_OF_RANGE ) ||
                              ROCKETNET_FUZZ_MOVED( FRAGMENT_COUNT_MISMATCH ) || ROCKETNET_FUZZ_MOVED( FRAGMENT_SIZE_MISMATCH ) ||
                              ROCKETNET_FUZZ_MOVED( FRAGMENT_PAYLOAD_TOO_LARGE ) || ROCKETNET_FUZZ_MOVED( FRAGMENT_DUPLICATE_INDEX ) ||
                              ROCKETNET_FUZZ_MOVED( RECEIVE_CURSOR_TOO_SMALL );

    if ( packet_bytes < ROCKETNET_PREFILTER_BYTES + 4 )
        return;

    const uint8_t * p = packet + ROCKETNET_PREFILTER_BYTES;
    const uint32_t wire_sequence = rocketnet_read_uint32( &p );

    if ( type_specific || ROCKETNET_FUZZ_SESSION_MOVED( BAD_ACK_BLOCK ) || ROCKETNET_FUZZ_SESSION_MOVED( WRONG_OPENED_SIZE ) )
    {
        const uint64_t sequence = rocketnet_reconstruct_sequence( wire_sequence, previous->mapping.replay_session.highest_opened );
        rocketnet_replay_advance( &expected->mapping.replay_session, sequence );
        if ( type_specific )
            expected->control.last_receive_time = time;
        *opened = 1;
        return;
    }

    const int response_denied = ROCKETNET_FUZZ_MOVED( CONNECTION_RESPONSE_NO_SLOTS );
    if ( ROCKETNET_FUZZ_MOVED( CONNECTION_RESPONSE_ALREADY_CONNECTED ) ||
         ROCKETNET_FUZZ_MOVED( CONNECTION_RESPONSE_CLIENT_ID_ALREADY_CONNECTED ) || response_denied )
    {
        const uint64_t sequence = rocketnet_reconstruct_sequence( wire_sequence, previous->mapping.handshake_highest_opened );
        if ( sequence > expected->mapping.handshake_highest_opened )
            expected->mapping.handshake_highest_opened = sequence;
        if ( previous->control.client_index == -1 )
            expected->control.last_receive_time = time;
        if ( response_denied )
            expected->mapping.send_sequence_handshake++;
        *opened = 1;
        return;
    }

    if ( ROCKETNET_FUZZ_MOVED( CONNECTION_REQUEST_CHALLENGE_RESENT ) && packet_bytes == ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES )
    {
        const uint8_t * salt = packet + ROCKETNET_PREFILTER_BYTES + ROCKETNET_COOKIE_FIELD_BYTES;
        if ( memcmp( salt, previous->mapping.connection_salt, ROCKETNET_CONNECTION_SALT_BYTES ) == 0 )
            expected->control.last_receive_time = time;
        else if ( previous->control.last_receive_time != time )
            rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_ORACLE_SALT_MISMATCH;
    }
}

// a refused packet changes nothing in its mapping but what its refusal step
// leaves behind (above); returns whether the packet opened

static int rocketnet_fuzz_check_refusal( struct rocketnet_server_protocol_t * server,
                                         const struct rocketnet_address_t * from,
                                         int before_index,
                                         const struct rocketnet_fuzz_mapping_image * previous,
                                         const uint64_t * before,
                                         const uint8_t * packet,
                                         int packet_bytes,
                                         double time )
{
    const uint64_t * after = server->counters->values;

    // the integrity limit tears its mapping down by design (t14); it is
    // 2^36 failed opens, beyond any input, and stays exempt here

    if ( ROCKETNET_FUZZ_MOVED( INTEGRITY_LIMIT_REACHED ) )
        return 0;

    const int after_index = rocketnet_encryption_map_find( server->encryption_map, from );
    rocketnet_fuzz_require( after_index == before_index, "a refused packet leaves its mapping in place" );

    struct rocketnet_fuzz_mapping_image expected;
    int opened = 0;
    rocketnet_fuzz_expected_after_refusal( previous, before, after, packet, packet_bytes, time, &expected, &opened );

    struct rocketnet_fuzz_mapping_image now;
    rocketnet_fuzz_mapping_copy( server->encryption_map, after_index, &now );

    rocketnet_fuzz_require( memcmp( &now.control.last_receive_time, &expected.control.last_receive_time, sizeof( double ) ) == 0,
                            "a refused packet leaves the idle clock (control->last_receive_time) unchanged" );
    rocketnet_fuzz_require( now.mapping.send_sequence_handshake == expected.mapping.send_sequence_handshake &&
                                now.mapping.send_sequence_session == expected.mapping.send_sequence_session,
                            "a refused packet leaves the send sequences unchanged" );
    rocketnet_fuzz_require( now.mapping.send_fragment_payload_id == expected.mapping.send_fragment_payload_id,
                            "a refused packet leaves the fragment payload id unchanged" );
    rocketnet_fuzz_require( now.mapping.handshake_highest_opened == expected.mapping.handshake_highest_opened,
                            "a refused packet leaves handshake_highest_opened unchanged" );
    rocketnet_fuzz_require( memcmp( &now.mapping.replay_session, &expected.mapping.replay_session, sizeof( now.mapping.replay_session ) ) == 0,
                            "a refused packet leaves the replay window (replay_session) unchanged" );
    rocketnet_fuzz_require( memcmp( &now.mapping.accepted_session, &expected.mapping.accepted_session, sizeof( now.mapping.accepted_session ) ) == 0,
                            "a refused packet leaves the ack-eligible window (accepted_session) unchanged" );
    rocketnet_fuzz_require( now.mapping.integrity_failures == expected.mapping.integrity_failures,
                            "a refused packet moves the integrity-failure count only when it did not open" );
    rocketnet_fuzz_require( memcmp( &now, &expected, sizeof( now ) ) == 0,
                            "a refused packet leaves every other field of its mapping unchanged" );

    return opened;
}

#undef ROCKETNET_FUZZ_SESSION_MOVED
#undef ROCKETNET_FUZZ_MOVED

extern "C" int LLVMFuzzerInitialize( int * argc, char *** argv )
{
    (void) argc;
    (void) argv;

    if ( sodium_init() < 0 )
    {
        abort();
    }

    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    char seed_dir[1024];
    if ( rocketnet_test_getenv( "ROCKETNET_FUZZ_SEED_DIR", seed_dir, sizeof( seed_dir ) ) && seed_dir[0] != '\0' )
    {
        exit( rocketnet_fuzz_receive_generate_seed_dir( seed_dir ) ? 0 : 1 );
    }

    return 0;
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    rocketnet_fuzz_receive_coverage = 0;

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
    memset( &server_counters, 0, sizeof( server_counters ) );

    struct rocketnet_server_protocol_t server;
    memset( &server, 0, sizeof( server ) );
    server.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    server.send_rate = 10.0;

    const uint8_t tag[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( server.packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );
    memcpy( server.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    memcpy( server.server_public_key, rocketnet_dev_server_public_key, 32 );
    memcpy( server.server_secret_key, rocketnet_dev_server_secret_key, 32 );
    rocketnet_address_parse( &server.advertised_ipv4, "203.0.113.10:40000" );
    randombytes_buf( server.cookie_key, ROCKETNET_COOKIE_KEY_BYTES );
    server.cookie_lifetime_seconds = 3;
    server.handshakes_per_update = ROCKETNET_SERVER_MAX_HANDSHAKES_PER_UPDATE;
    server.handshake_deadline_seconds = 5.0;
    server.encryption_map = rocketnet_encryption_map_create( NULL, 8 );
    server.used_token_history = rocketnet_used_token_history_create( NULL, 64 );
    server.client_slots = rocketnet_client_slots_create( NULL, 4 );
    server.acks = rocketnet_server_acks_create( NULL, 4 );
    server.reassembly = rocketnet_reassembly_set_create( NULL, 4, 8, 16384, 4 * 8 );
    server.counters = &server_counters;

    // -------- one client, and the wire between them --------

    struct rocketnet_address_t server_address;
    rocketnet_address_parse( &server_address, "203.0.113.10:40000" );

    struct rocketnet_address_t client_address;
    rocketnet_address_parse( &client_address, "198.51.100.10:56789" );

    // a second source, which never holds the token: what it sends is a copy
    // of the client's own datagrams

    struct rocketnet_address_t second_address;
    rocketnet_address_parse( &second_address, "198.51.100.11:56789" );

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 256, simulator_seed );
    rocketnet_network_simulator_config( simulator, 0.05, 0.02, loss_percent, duplicate_percent );

    struct rocketnet_counters_t client_counters;
    memset( &client_counters, 0, sizeof( client_counters ) );

    struct rocketnet_client_protocol_t client;
    rocketnet_client_protocol_init( &client, &client_counters );
    memcpy( client.packet_filter, tag, ROCKETNET_FILTER_VALUE_BYTES );

    // the token the client connects with, minted with the real path

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    const uint64_t token_client_id = 1;

    {
        struct rocketnet_signed_connect_data_t signed_fields;
        struct rocketnet_connect_token_private_t private_fields;
        memset( &signed_fields, 0, sizeof( signed_fields ) );
        memset( &private_fields, 0, sizeof( private_fields ) );
        signed_fields.version = ROCKETNET_WIRE_VERSION;
        signed_fields.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
        signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
        signed_fields.expire_timestamp = ROCKETNET_TEST_CREATE_TS + 300;
        signed_fields.timeout_seconds = 30;
        signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;

        const uint8_t server_address_bytes[6] = { 203, 0, 113, 10, 0x40, 0x9c };
        memcpy( signed_fields.ipv4_address, server_address_bytes, 6 );
        memcpy( signed_fields.server_public_key, rocketnet_dev_server_public_key, 32 );
        private_fields.client_id = token_client_id;
        randombytes_buf( private_fields.client_to_server_key, 32 );
        randombytes_buf( private_fields.server_to_client_key, 32 );
        if ( !rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_dev_backend_secret_key, token_data ) )
        {
            abort();
        }
    }

    double time = 100.0;
    uint64_t timestamp = ROCKETNET_TEST_CREATE_TS;
    rocketnet_client_protocol_connect( &client, token_data, time, timestamp );

    // the last real client->server datagram, kept so the stream can corrupt a
    // genuine envelope rather than only inject pure garbage

    uint8_t last_c2s[ROCKETNET_RECEIVE_CEILING];
    int last_c2s_bytes = 0;

    uint64_t counters_before[ROCKETNET_NUM_COUNTERS];
    struct rocketnet_fuzz_mapping_image mapping_before;

    int payload_size = 1;
    const int MAX_OPS = 4096;

    for ( int op = 0; op < MAX_OPS && r.pos < r.size; op++ )
    {
        const uint8_t action = fuzz_u8( &r );

        // send the genuine client->server datagram of an earlier step again,
        // unmodified, from the client's address or from the second address,
        // ahead of this step's own: the copy arrives after its original was
        // processed, so the refusal it meets is measured against what the
        // original left behind

        if ( ( action & 0x30 ) == 0x30 )
        {
            const struct rocketnet_address_t * source = ( fuzz_u8( &r ) & 1 ) ? &second_address : &client_address;
            if ( last_c2s_bytes > 0 )
                rocketnet_network_simulator_send( simulator, source, &server_address, last_c2s, last_c2s_bytes, time );
        }

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
                    memcpy( last_c2s, packet, (size_t) packet_bytes );
                    last_c2s_bytes = packet_bytes;
                }

                rocketnet_network_simulator_send( simulator, &client_address, &server_address, packet, packet_bytes, time );
            }
        }

        // a connected client sends fuzz-sized traffic when the stream asks

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && ( action & 3 ) == 1 )
        {
            uint8_t packet[ROCKETNET_RECEIVE_CEILING];
            uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
            generate_payload( payload, payload_size );

            const int bytes = rocketnet_client_protocol_write_payload( &client, payload, payload_size, time, packet );
            if ( bytes > 0 )
            {
                rocketnet_network_simulator_send( simulator, &client_address, &server_address, packet, bytes, time );
            }

            payload_size = payload_size < ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ? payload_size + 1 : 1;
        }

        // occasionally a fragmented payload, so reassembly soaks under the
        // same hostile wire and the fragment sequence paths are reached

        if ( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED && ( action & 3 ) == 2 )
        {
            static uint8_t fragmented_payload[16384];
            const int fragmented_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1 + ( fuzz_u16( &r ) % 4096 );
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

        // inject a datagram at the client, mirroring the server-side injection
        // above: the server's answers (and hostile junk) ride back over the
        // wire, so the client-side receive types are reachable in the same run

        if ( ( action & 0xc0 ) == 0x40 )
        {
            const int garbage_bytes = 1 + fuzz_u16( &r ) % ROCKETNET_RECEIVE_CEILING;
            uint8_t garbage[ROCKETNET_RECEIVE_CEILING];
            fuzz_bytes( &r, garbage, garbage_bytes );
            rocketnet_network_simulator_send( simulator, &server_address, &client_address, garbage, garbage_bytes, time );
        }

        // corrupt a real envelope: take the last genuine client->server
        // datagram, flip fuzz-chosen bytes inside its sealed body (past the
        // 6-byte prefilter and the 4-byte sequence, so the filter and mapping
        // still admit it and the damage lands at the AEAD open / replay)

        if ( ( action & 0x30 ) == 0x20 && last_c2s_bytes > ROCKETNET_PREFILTER_BYTES + 4 )
        {
            uint8_t corrupt[ROCKETNET_RECEIVE_CEILING];
            memcpy( corrupt, last_c2s, (size_t) last_c2s_bytes );

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
            server.acks->num_acked = 0;

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

                // the state the oracles compare against: the counters, the
                // token's live connections, and the mapping this datagram
                // addresses, if it has one

                memcpy( counters_before, server_counters.values, sizeof( counters_before ) );
                const int token_connections_before = rocketnet_fuzz_token_connections( server.client_slots, token_client_id );
                const int before_index = rocketnet_encryption_map_find( server.encryption_map, &from );
                if ( before_index != -1 )
                    rocketnet_fuzz_mapping_copy( server.encryption_map, before_index, &mapping_before );

                const int result = rocketnet_server_protocol_process_packet( &server, &from, packet, bytes, timestamp, time,
                                                                             response, &response_bytes, delivered, &delivered_bytes, &delivered_client_index );

                const int refused = result == ROCKETNET_SERVER_PROCESS_IGNORED || result == ROCKETNET_SERVER_PROCESS_SEND_DENIED ||
                                    result == ROCKETNET_SERVER_PROCESS_BUDGET_EXCEEDED;
                if ( before_index != -1 && ( refused || result == ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE ) )
                {
                    const int opened = rocketnet_fuzz_check_refusal( &server, &from, before_index, &mapping_before, counters_before, packet, bytes, time );
                    if ( refused )
                        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_ORACLE_REFUSAL;
                    if ( refused && opened )
                        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_ORACLE_OPENED_REFUSAL;
                }

                // single connect first: a second connection also unbalances
                // the ledgers, and this names the cause

                if ( server_counters.values[ROCKETNET_COUNTER_CLIENT_CONNECTED] != counters_before[ROCKETNET_COUNTER_CLIENT_CONNECTED] )
                {
                    rocketnet_fuzz_require( token_connections_before == 0,
                                            "a token admitted once never connects again while that connection lives" );
                    rocketnet_fuzz_require( server_counters.values[ROCKETNET_COUNTER_CLIENT_CONNECTED] == counters_before[ROCKETNET_COUNTER_CLIENT_CONNECTED] + 1,
                                            "one packet connects at most once" );
                }
                rocketnet_fuzz_require( rocketnet_fuzz_token_connections( server.client_slots, token_client_id ) <= 1,
                                        "one token holds at most one live connection" );

                rocketnet_fuzz_check_families( &server_counters, "server" );
                rocketnet_fuzz_check_connected( &server );

                if ( token_connections_before == 1 && rocketnet_address_equal( &from, &second_address ) )
                    rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_ORACLE_SECOND_SOURCE;

                if ( response_bytes > 0 )
                {
                    if ( response_bytes > ROCKETNET_RECEIVE_CEILING )
                    {
                        abort();
                    }

                    rocketnet_network_simulator_send( simulator, &server_address, &from, response, response_bytes, time );
                }

                if ( delivered_bytes > 0 && delivered_bytes > ROCKETNET_RECEIVE_PAYLOAD_BYTES )
                {
                    abort();
                }
            }

            rocketnet_server_protocol_update( &server, time );

            rocketnet_fuzz_check_families( &server_counters, "server" );
            rocketnet_fuzz_check_connected( &server );
            rocketnet_fuzz_require( rocketnet_fuzz_token_connections( server.client_slots, token_client_id ) <= 1,
                                    "one token holds at most one live connection" );

            for ( int i = 0; i < 4; i++ )
            {
                uint8_t keep_alive[ROCKETNET_RECEIVE_CEILING];
                const int keep_alive_bytes = rocketnet_server_protocol_write_keep_alive_if_due( &server, i, time, keep_alive );
                if ( keep_alive_bytes > 0 )
                {
                    const int mapping_index = rocketnet_client_slots_mapping_index( server.client_slots, i );
                    const struct rocketnet_address_t * to = rocketnet_encryption_map_address( server.encryption_map, mapping_index );
                    if ( to )
                    {
                        rocketnet_network_simulator_send( simulator, &server_address, to, keep_alive, keep_alive_bytes, time );
                    }
                }

                uint8_t ack[ROCKETNET_RECEIVE_CEILING];
                const int ack_bytes = rocketnet_server_protocol_write_ack_if_due( &server, i, time, ack );
                if ( ack_bytes > 0 )
                {
                    const int mapping_index = rocketnet_client_slots_mapping_index( server.client_slots, i );
                    const struct rocketnet_address_t * to = rocketnet_encryption_map_address( server.encryption_map, mapping_index );
                    if ( to )
                    {
                        rocketnet_network_simulator_send( simulator, &server_address, to, ack, ack_bytes, time );
                    }
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
                rocketnet_client_protocol_process_packet( &client, packet, bytes, time, delivered, sizeof( delivered ), &delivered_bytes );
                if ( delivered_bytes > 0 && delivered_bytes > ROCKETNET_RECEIVE_PAYLOAD_BYTES )
                {
                    abort();
                }

                rocketnet_fuzz_check_families( &client_counters, "client" );
            }

            // the server's answers to the second address have no reader;
            // they are taken off the wire and dropped

            while ( rocketnet_network_simulator_receive( simulator, &second_address, time, &from, packet ) > 0 )
            {
            }
        }

        // time and the token clock advance by a fuzz-chosen small step, so
        // deadlines, key rotation and the replay window all move

        const double step = 0.005 + ( fuzz_u8( &r ) % 32 ) * 0.01;
        time += step;
        timestamp = ROCKETNET_TEST_CREATE_TS + (uint64_t) ( time - 100.0 );
    }

    rocketnet_fuzz_check_families( &server_counters, "server" );
    rocketnet_fuzz_check_families( &client_counters, "client" );
    rocketnet_fuzz_check_connected( &server );

    {
        uint8_t refused_family[ROCKETNET_NUM_COUNTERS];
        memset( refused_family, 0, sizeof( refused_family ) );
        int refused_families = 0;
        for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        {
            const int family = rocketnet_fuzz_counter_family[i];
            if ( family >= 0 && server_counters.values[i] > 0 && rocketnet_counter_class( i ) == ROCKETNET_COUNTER_CLASS_REFUSAL && !refused_family[family] )
            {
                refused_family[family] = 1;
                refused_families++;
            }
        }

        if ( refused_families >= 3 && rocketnet_fuzz_token_connections( server.client_slots, token_client_id ) == 1 )
            rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_ORACLE_LEDGER;
    }

    if ( server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_SERVER_REQUEST;
    }

    if ( server_counters.values[ROCKETNET_COUNTER_CONNECTION_RESPONSE_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_SERVER_RESPONSE;
    }

    if ( server_counters.values[ROCKETNET_COUNTER_KEEP_ALIVE_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_SERVER_KEEP_ALIVE;
    }

    if ( server_counters.values[ROCKETNET_COUNTER_PAYLOAD_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_SERVER_PAYLOAD;
    }

    if ( server_counters.values[ROCKETNET_COUNTER_DISCONNECT_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_SERVER_DISCONNECT;
    }

    if ( server_counters.values[ROCKETNET_COUNTER_ACK_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_SERVER_ACK;
    }

    if ( server_counters.values[ROCKETNET_COUNTER_FRAGMENT_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_SERVER_FRAGMENT;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_COOKIE_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_COOKIE;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_CHALLENGE_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_CHALLENGE;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_DENIED_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_DENIED;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_KEEP_ALIVE_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_KEEP_ALIVE;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_PAYLOAD_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_PAYLOAD;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_DISCONNECT_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_DISCONNECT;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_ACK;
    }

    if ( client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_RECEIVED] > 0 )
    {
        rocketnet_fuzz_receive_coverage |= ROCKETNET_FUZZ_RECEIVE_CLIENT_FRAGMENT;
    }

    rocketnet_encryption_map_destroy( server.encryption_map );
    rocketnet_used_token_history_destroy( server.used_token_history );
    rocketnet_client_slots_destroy( server.client_slots );
    rocketnet_server_acks_destroy( server.acks );
    rocketnet_reassembly_set_destroy( server.reassembly );
    rocketnet_network_simulator_destroy( simulator );

    return 0;
}

// -----------------------------------------------------------------------
//  the seed generator: ROCKETNET_FUZZ_SEED_DIR=<dir> writes one named seed
//  per packet type the receive path dispatches on and one per oracle, each a
//  deterministic decision stream (a completed handshake, then a real send or
//  a hostile injection), replayed through the target before it is written,
//  and returns failure (INCOMPLETE) unless every seed reaches its own type or
//  oracle bits. ROCKETNET_FUZZ_
//  WITHHOLD=<seed name> skips one seed so the run shows the INCOMPLETE path.
// -----------------------------------------------------------------------

enum
{
    ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE = 0,
    ROCKETNET_FUZZ_RECEIVE_SEED_PAYLOAD,
    ROCKETNET_FUZZ_RECEIVE_SEED_FRAGMENT,
    ROCKETNET_FUZZ_RECEIVE_SEED_SERVER_DISCONNECT,
    ROCKETNET_FUZZ_RECEIVE_SEED_SERVER_ACK,
    ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_DENIED,
    ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_PAYLOAD,
    ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_DISCONNECT,
    ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_ACK,
    ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_FRAGMENT,
    ROCKETNET_FUZZ_RECEIVE_SEED_REFUSE_IMMUTABLE,
    ROCKETNET_FUZZ_RECEIVE_SEED_LEDGER_BALANCE,
    ROCKETNET_FUZZ_RECEIVE_SEED_SALT_MISMATCH,
    ROCKETNET_FUZZ_RECEIVE_SEED_SINGLE_CONNECT
};

struct rocketnet_fuzz_receive_seed_spec_t
{
    const char * name;
    uint64_t mask;
    int kind;
};

static const struct rocketnet_fuzz_receive_seed_spec_t rocketnet_fuzz_receive_seed_specs[] = {
    { "seed_type_server_connection_request", ROCKETNET_FUZZ_RECEIVE_SERVER_REQUEST, ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE },
    { "seed_type_server_connection_response", ROCKETNET_FUZZ_RECEIVE_SERVER_RESPONSE, ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE },
    { "seed_type_server_keep_alive", ROCKETNET_FUZZ_RECEIVE_SERVER_KEEP_ALIVE, ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE },
    { "seed_type_server_payload", ROCKETNET_FUZZ_RECEIVE_SERVER_PAYLOAD, ROCKETNET_FUZZ_RECEIVE_SEED_PAYLOAD },
    { "seed_type_server_disconnect", ROCKETNET_FUZZ_RECEIVE_SERVER_DISCONNECT, ROCKETNET_FUZZ_RECEIVE_SEED_SERVER_DISCONNECT },
    { "seed_type_server_ack", ROCKETNET_FUZZ_RECEIVE_SERVER_ACK, ROCKETNET_FUZZ_RECEIVE_SEED_SERVER_ACK },
    { "seed_type_server_fragment", ROCKETNET_FUZZ_RECEIVE_SERVER_FRAGMENT, ROCKETNET_FUZZ_RECEIVE_SEED_FRAGMENT },
    { "seed_type_client_cookie", ROCKETNET_FUZZ_RECEIVE_CLIENT_COOKIE, ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE },
    { "seed_type_client_challenge", ROCKETNET_FUZZ_RECEIVE_CLIENT_CHALLENGE, ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE },
    { "seed_type_client_denied", ROCKETNET_FUZZ_RECEIVE_CLIENT_DENIED, ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_DENIED },
    { "seed_type_client_keep_alive", ROCKETNET_FUZZ_RECEIVE_CLIENT_KEEP_ALIVE, ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE },
    { "seed_type_client_payload", ROCKETNET_FUZZ_RECEIVE_CLIENT_PAYLOAD, ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_PAYLOAD },
    { "seed_type_client_disconnect", ROCKETNET_FUZZ_RECEIVE_CLIENT_DISCONNECT, ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_DISCONNECT },
    { "seed_type_client_ack", ROCKETNET_FUZZ_RECEIVE_CLIENT_ACK, ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_ACK },
    { "seed_type_client_fragment", ROCKETNET_FUZZ_RECEIVE_CLIENT_FRAGMENT, ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_FRAGMENT },
    { "seed_oracle_refuse_immutable", ROCKETNET_FUZZ_RECEIVE_ORACLE_REFUSAL | ROCKETNET_FUZZ_RECEIVE_ORACLE_OPENED_REFUSAL, ROCKETNET_FUZZ_RECEIVE_SEED_REFUSE_IMMUTABLE },
    { "seed_oracle_ledger_balance", ROCKETNET_FUZZ_RECEIVE_ORACLE_LEDGER, ROCKETNET_FUZZ_RECEIVE_SEED_LEDGER_BALANCE },
    { "seed_oracle_single_connect", ROCKETNET_FUZZ_RECEIVE_ORACLE_SECOND_SOURCE, ROCKETNET_FUZZ_RECEIVE_SEED_SINGLE_CONNECT },
    { "seed_oracle_salt_mismatch", ROCKETNET_FUZZ_RECEIVE_ORACLE_SALT_MISMATCH, ROCKETNET_FUZZ_RECEIVE_SEED_SALT_MISMATCH }
};

static void rocketnet_fuzz_receive_append_u8( uint8_t * output, size_t * size, uint8_t value )
{
    output[*size] = value;
    *size += 1;
}

static void rocketnet_fuzz_receive_append_u16( uint8_t * output, size_t * size, uint16_t value )
{
    output[*size] = (uint8_t) value;
    output[*size + 1] = (uint8_t) ( value >> 8 );
    *size += 2;
}

static void rocketnet_fuzz_receive_append_handshake( uint8_t * output, size_t * size )
{
    for ( int operation = 0; operation < 14; operation++ )
    {
        rocketnet_fuzz_receive_append_u8( output, size, 0 );
        rocketnet_fuzz_receive_append_u8( output, size, 31 );
    }
}

static void rocketnet_fuzz_receive_append_injection( uint8_t * output, size_t * size, uint8_t action, uint8_t packet_type, int packet_bytes )
{
    rocketnet_fuzz_receive_append_u8( output, size, action );
    rocketnet_fuzz_receive_append_u16( output, size, (uint16_t) ( packet_bytes - 1 ) );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    memset( packet, 0, sizeof( packet ) );
    packet[0] = packet_type;
    packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED;

    const uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES] = ROCKETNET_TEST_FILTER_VALUE;
    memcpy( packet + ROCKETNET_PREFILTER_BYTES - ROCKETNET_FILTER_VALUE_BYTES, filter_value, sizeof( filter_value ) );
    memcpy( output + *size, packet, (size_t) packet_bytes );
    *size += (size_t) packet_bytes;
    rocketnet_fuzz_receive_append_u8( output, size, 31 );
}

// the salt seed's handshake steps before the step that sends the request:
// cookie request, cookie, and the client's send period between them
static const int ROCKETNET_FUZZ_RECEIVE_SALT_SEED_STEPS = 5;

static size_t rocketnet_fuzz_receive_build_seed( int kind, uint8_t * output, size_t capacity )
{
    // the largest seed, the salt mismatch, carries one whole 710-byte request
    const size_t required_capacity = 1024;
    if ( capacity < required_capacity )
        return 0;

    size_t size = 0;
    rocketnet_fuzz_receive_append_u8( output, &size, 1 );
    rocketnet_fuzz_receive_append_u8( output, &size, 0 );
    rocketnet_fuzz_receive_append_u8( output, &size, 0 );
    rocketnet_fuzz_receive_append_u8( output, &size, 0 );
    rocketnet_fuzz_receive_append_u8( output, &size, 0 );
    rocketnet_fuzz_receive_append_u8( output, &size, 0 );
    if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_REFUSE_IMMUTABLE || kind == ROCKETNET_FUZZ_RECEIVE_SEED_SINGLE_CONNECT )
    {
        // the handshake with every step sending the client's last datagram
        // again, so its request, its response and its first session
        // packets each arrive twice: from the client's own address for the
        // refusal seed, from the second address for the single-connect seed.
        // The refusal seed alternates the replay with a copy whose sealed
        // body is damaged, so a response that does not open (DID_NOT_OPEN,
        // one integrity failure) and a request whose salt is not the
        // mapping's (CHALLENGE_RESENT) reach it as well; that request arrives
        // in the same step as a matching one, so the salt rule on its own is
        // seed_oracle_salt_mismatch's.
        const int refusal = kind == ROCKETNET_FUZZ_RECEIVE_SEED_REFUSE_IMMUTABLE;
        for ( int operation = 0; operation < 20; operation++ )
        {
            if ( refusal && ( operation & 1 ) )
            {
                rocketnet_fuzz_receive_append_u8( output, &size, 0x20 );
                rocketnet_fuzz_receive_append_u8( output, &size, 0 );
                rocketnet_fuzz_receive_append_u16( output, &size, 20 );
                rocketnet_fuzz_receive_append_u8( output, &size, 0xff );
            }
            else
            {
                rocketnet_fuzz_receive_append_u8( output, &size, 0x30 );
                rocketnet_fuzz_receive_append_u8( output, &size, refusal ? 0 : 1 );
            }
            rocketnet_fuzz_receive_append_u8( output, &size, 31 );
        }
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_SALT_MISMATCH )
    {
        // the handshake up to the client's connection request, each step
        // long enough for the client to send; the step that sends the
        // request (the server makes the half-open mapping) is 5 ms, inside
        // the client's 100 ms send period. The next step then forges a
        // request from the client's address with an all-zero salt: the
        // client sends nothing in that step, so it arrives alone and later
        // than the mapping's last receive, and must not refresh the idle
        // clock (t12 rule 3)
        for ( int operation = 0; operation < ROCKETNET_FUZZ_RECEIVE_SALT_SEED_STEPS; operation++ )
        {
            rocketnet_fuzz_receive_append_u8( output, &size, 0 );
            rocketnet_fuzz_receive_append_u8( output, &size, 31 );
        }
        rocketnet_fuzz_receive_append_u8( output, &size, 0 );
        rocketnet_fuzz_receive_append_u8( output, &size, 0 );
        rocketnet_fuzz_receive_append_injection( output, &size, 0x10, ROCKETNET_CONNECTION_REQUEST_PACKET, ROCKETNET_CONNECTION_REQUEST_WIRE_BYTES );
    }
    else
    {
        rocketnet_fuzz_receive_append_handshake( output, &size );
    }

    if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_LEDGER_BALANCE )
    {
        // traffic and refusals across the families on a live connection: a
        // payload, a fragmented payload, a damaged envelope, a replay, and
        // forged session packets at both ends
        rocketnet_fuzz_receive_append_u8( output, &size, 1 );
        rocketnet_fuzz_receive_append_u8( output, &size, 31 );
        rocketnet_fuzz_receive_append_u8( output, &size, 2 );
        rocketnet_fuzz_receive_append_u16( output, &size, 0 );
        rocketnet_fuzz_receive_append_u8( output, &size, 31 );
        rocketnet_fuzz_receive_append_u8( output, &size, 0x20 );
        rocketnet_fuzz_receive_append_u8( output, &size, 0 );
        rocketnet_fuzz_receive_append_u16( output, &size, 0 );
        rocketnet_fuzz_receive_append_u8( output, &size, 0xff );
        rocketnet_fuzz_receive_append_u8( output, &size, 31 );
        rocketnet_fuzz_receive_append_u8( output, &size, 0x30 );
        rocketnet_fuzz_receive_append_u8( output, &size, 0 );
        rocketnet_fuzz_receive_append_u8( output, &size, 31 );
        rocketnet_fuzz_receive_append_injection( output, &size, 0x10, ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
        rocketnet_fuzz_receive_append_injection( output, &size, 0x10, ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES );
        rocketnet_fuzz_receive_append_injection( output, &size, 0x40, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_PAYLOAD )
    {
        rocketnet_fuzz_receive_append_u8( output, &size, 1 );
        rocketnet_fuzz_receive_append_u8( output, &size, 31 );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_FRAGMENT )
    {
        rocketnet_fuzz_receive_append_u8( output, &size, 2 );
        rocketnet_fuzz_receive_append_u16( output, &size, 0 );
        rocketnet_fuzz_receive_append_u8( output, &size, 31 );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_SERVER_DISCONNECT )
    {
        rocketnet_fuzz_receive_append_injection( output, &size, 0x10, ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_SERVER_ACK )
    {
        rocketnet_fuzz_receive_append_injection( output, &size, 0x10, ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_DENIED )
    {
        rocketnet_fuzz_receive_append_injection( output, &size, 0x40, ROCKETNET_CONNECTION_DENIED_PACKET, ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_PAYLOAD )
    {
        rocketnet_fuzz_receive_append_injection( output, &size, 0x40, ROCKETNET_CONNECTION_PAYLOAD_PACKET, ROCKETNET_CONNECTION_PAYLOAD_MIN_WIRE_BYTES );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_DISCONNECT )
    {
        rocketnet_fuzz_receive_append_injection( output, &size, 0x40, ROCKETNET_CONNECTION_DISCONNECT_PACKET, ROCKETNET_CONNECTION_DISCONNECT_MIN_WIRE_BYTES );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_ACK )
    {
        rocketnet_fuzz_receive_append_injection( output, &size, 0x40, ROCKETNET_CONNECTION_ACK_PACKET, ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES );
    }
    else if ( kind == ROCKETNET_FUZZ_RECEIVE_SEED_CLIENT_FRAGMENT )
    {
        rocketnet_fuzz_receive_append_injection( output, &size, 0x40, ROCKETNET_CONNECTION_FRAGMENT_PACKET, ROCKETNET_CONNECTION_FRAGMENT_MIN_WIRE_BYTES );
    }

    if ( kind != ROCKETNET_FUZZ_RECEIVE_SEED_HANDSHAKE )
    {
        for ( int operation = 0; operation < 6; operation++ )
        {
            rocketnet_fuzz_receive_append_u8( output, &size, 0 );
            rocketnet_fuzz_receive_append_u8( output, &size, 31 );
        }
    }

    return size;
}

static int rocketnet_fuzz_receive_generate_seed_dir( const char * directory )
{
    char withheld[512];
    const int have_withheld = rocketnet_test_getenv( "ROCKETNET_FUZZ_WITHHOLD", withheld, sizeof( withheld ) );

    const size_t spec_count = sizeof( rocketnet_fuzz_receive_seed_specs ) / sizeof( rocketnet_fuzz_receive_seed_specs[0] );
    uint64_t coverage = 0;

    for ( size_t index = 0; index < spec_count; index++ )
    {
        const struct rocketnet_fuzz_receive_seed_spec_t * spec = &rocketnet_fuzz_receive_seed_specs[index];
        if ( have_withheld && strcmp( withheld, spec->name ) == 0 )
            continue;

        uint8_t data[1024];
        const size_t size = rocketnet_fuzz_receive_build_seed( spec->kind, data, sizeof( data ) );
        if ( size == 0 )
            return 0;

        rocketnet_fuzz_receive_coverage = 0;
        LLVMFuzzerTestOneInput( data, size );
        coverage |= rocketnet_fuzz_receive_coverage;

        // each seed reaches its own bits: an oracle seed that stops reaching
        // its oracle is INCOMPLETE even when another seed happens to
        if ( ( rocketnet_fuzz_receive_coverage & spec->mask ) != spec->mask )
        {
            fprintf( stderr, "INCOMPLETE target=rocketnet_fuzz_receive_structured missing=%s\n", spec->name );
            return 0;
        }
    }

    for ( size_t index = 0; index < spec_count; index++ )
    {
        const struct rocketnet_fuzz_receive_seed_spec_t * spec = &rocketnet_fuzz_receive_seed_specs[index];
        if ( ( coverage & spec->mask ) != spec->mask )
        {
            fprintf( stderr, "INCOMPLETE target=rocketnet_fuzz_receive_structured missing=%s\n", spec->name );
            return 0;
        }
    }

    for ( size_t index = 0; index < spec_count; index++ )
    {
        const struct rocketnet_fuzz_receive_seed_spec_t * spec = &rocketnet_fuzz_receive_seed_specs[index];
        if ( have_withheld && strcmp( withheld, spec->name ) == 0 )
            continue;

        uint8_t data[1024];
        const size_t size = rocketnet_fuzz_receive_build_seed( spec->kind, data, sizeof( data ) );
        if ( size == 0 )
            return 0;

        char path[1024];
        snprintf( path, sizeof( path ), "%s/%s", directory, spec->name );

        FILE * f = rocketnet_test_fopen( path, "wb" );
        if ( !f )
            return 0;

        const size_t written = fwrite( data, 1, size, f );
        if ( fclose( f ) != 0 || written != size )
            return 0;
    }

    return 1;
}
