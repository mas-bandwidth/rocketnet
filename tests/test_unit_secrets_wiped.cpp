/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// test_unit_secrets_wiped.cpp -- every exit that leaves a secret behind wipes
// it, failures included:
//
//   - the client's ephemeral secret, on the handshake failure exits the
//     secret-wipe oracle (test_unit_secret_wipe.cpp) does not drive: the token
//     expiring from sending-connection-request, and the integrity limit
//     reached on a forged denied. rocketnet_client_set_state runs the
//     connection reset on every state <= 0 (sdk/rocketnet_client_protocol.cpp:56-57),
//     and the reset zeroes the ephemeral secret (sdk/rocketnet_connection_reset.cpp:40).
//   - the client's and the server's keys at destroy, and only the keys: when
//     rocketnet_client_destroy and rocketnet_server_destroy free the public
//     objects and their transports, each object's secret spans are zero and
//     no allocation has more than a handful of bytes (handles, keys) written
//     by the destroy. Message bytes are not secrets and are not wiped: the OS
//     zero-fills a page before another process gets it, and a wipe would
//     touch every page of a plan that can reach gigabytes (sdk/rocketnet_api.cpp).
//   - the connect token's user data, which can carry the game's secrets: the
//     server's mapping and slot copies, at the client's leaving and at a
//     destroy with the client still connected.
//   - a message arena at its destroy, owned or in a caller's block: nothing is
//     written, so a planted message is still there when the block is freed or
//     handed back (sdk/rocketnet_allocator.cpp, rn_arena_destroy).
//   - the base64 staging of a BYTES config field parsed from the environment,
//     on the parse and the refused exit (sdk/rocketnet_config.cpp, the
//     ROCKETNET_CONFIG_KIND_BYTES case of rocketnet_config_apply_env).
//   - the private block staging of rocketnet_open_connect_token_private, on
//     the open and the refused exit (sdk/rocketnet_connect_token.cpp).
//
// Time is an argument starting at 1000.0; the loopback transport carries the
// packets in memory, so no case opens a socket or reads a clock. No case
// prints a key: the checks compare bytes and report only true or false.

#include "unit_shutdown.h"
#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_client_protocol.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_keys.h"
#include "../sdk/rocketnet_client_slots.h"
#include "../sdk/rocketnet_encryption_map.h"
#include "../sdk/rocketnet_test_vectors.h"
#include "../sdk/rocketnet_memory.h"
#include "../sdk/rocketnet_driver_loopback.h"
#include "suite_runner.h"

#if defined(ROCKETNET_USE_SYSTEM_SODIUM)
#include <sodium.h>
#else
#include "../sodium/sodium.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <vector>

#define check rocketnet_unit_check

static bool secrets_wiped_all_zero( const uint8_t * bytes, int count )
{
    for ( int i = 0; i < count; i++ )
        if ( bytes[i] != 0 )
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// the client's ephemeral secret on the handshake failure exits
// ---------------------------------------------------------------------------

static void secrets_wiped_mint_vector_token( uint8_t * token_data )
{
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_connect_token_private_t private_fields;

    memset( &signed_fields, 0, sizeof(signed_fields) );
    signed_fields.version = ROCKETNET_WIRE_VERSION;
    signed_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    signed_fields.create_timestamp = ROCKETNET_TEST_CREATE_TS;
    signed_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    signed_fields.timeout_seconds = ROCKETNET_TEST_TIMEOUT;
    signed_fields.address_flags = ROCKETNET_ADDRESS_FLAG_IPV4;
    const uint8_t server_address[6] = { 203, 0, 113, 10, 0x40, 0x9c };
    memcpy( signed_fields.ipv4_address, server_address, 6 );
    memcpy( signed_fields.server_public_key, rocketnet_test_server_public_key, ROCKETNET_KEY_BYTES );

    memset( &private_fields, 0, sizeof(private_fields) );
    private_fields.client_id = 0x5EC12E7ULL;
    private_fields.protocol_id = ROCKETNET_TEST_PROTOCOL_ID;
    private_fields.expire_timestamp = ROCKETNET_TEST_EXPIRE_TS;
    memcpy( private_fields.client_to_server_key, rocketnet_test_token_c2s_key, ROCKETNET_KEY_BYTES );
    memcpy( private_fields.server_to_client_key, rocketnet_test_token_s2c_key, ROCKETNET_KEY_BYTES );

    check( rocketnet_write_connect_token( &signed_fields, &private_fields, rocketnet_test_backend_secret_key, token_data ) );
}

// a client in sending-connection-request: the token parsed, the ephemeral
// keypair made, and a cookie that echoes this attempt's salt accepted
static void secrets_wiped_reach_connection_request( struct rocketnet_client_protocol_t * client,
                                                    struct rocketnet_counters_t * counters )
{
    memset( counters, 0, sizeof(*counters) );
    rocketnet_client_protocol_init( client, counters );

    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    secrets_wiped_mint_vector_token( token_data );
    check( rocketnet_client_protocol_connect( client, token_data, 1000.0, ROCKETNET_TEST_CREATE_TS ) );
    check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_REQUESTING_COOKIE );

    uint8_t cookie[ROCKETNET_CONNECTION_COOKIE_WIRE_BYTES];
    const uint8_t cookie_mac[ROCKETNET_COOKIE_MAC_BYTES] = { 0 };
    rocketnet_write_connection_cookie_packet( cookie, client->packet_filter, client->connection_salt, 0, cookie_mac );

    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_packet( client, cookie, sizeof(cookie), 1000.5,
                                                     payload, sizeof(payload), &payload_bytes ) );
    check( client->state == ROCKETNET_CLIENT_TRANSPORT_STATE_SENDING_CONNECTION_REQUEST );
    check( !secrets_wiped_all_zero( client->client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
}

// The token's lifetime elapses while the client sends connection requests:
// connect token expired (-7) ends the attempt, and the ephemeral secret that
// sealed the requests is zero.
static void test_secrets_wiped_ephemeral_on_token_expired_from_connection_request()
{
    struct rocketnet_counters_t counters;
    struct rocketnet_client_protocol_t client;
    secrets_wiped_reach_connection_request( &client, &counters );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int packet_bytes = 0;
    const double deadline = (double) ( ROCKETNET_TEST_EXPIRE_TS - ROCKETNET_TEST_CREATE_TS );
    rocketnet_client_protocol_update( &client, 1000.0 + deadline, packet, &packet_bytes );

    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECT_TOKEN_EXPIRED );
    check( secrets_wiped_all_zero( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
}

// One more forged denied than the integrity limit allows ends the attempt in
// disconnected (sdk/rocketnet_client_protocol.cpp, rocketnet_client_integrity_failure):
// the ephemeral secret is zero.
static void test_secrets_wiped_ephemeral_on_integrity_limit_from_connection_request()
{
    struct rocketnet_counters_t counters;
    struct rocketnet_client_protocol_t client;
    secrets_wiped_reach_connection_request( &client, &counters );

    // a denied sealed under keys this attempt does not hold: it never opens
    uint8_t wrong_key[ROCKETNET_KEY_BYTES];
    memset( wrong_key, 0x3B, sizeof(wrong_key) );
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    memset( session_nonce, 0x5A, sizeof(session_nonce) );
    uint8_t denied[ROCKETNET_RECEIVE_CEILING];
    const int denied_bytes = rocketnet_write_sealed_packet( denied, ROCKETNET_CONNECTION_DENIED_PACKET,
                                                            client.packet_filter, 1, session_nonce, NULL, 0,
                                                            client.signed_fields.protocol_id, client.connection_salt,
                                                            NULL, wrong_key );
    check( denied_bytes == ROCKETNET_CONNECTION_DENIED_WIRE_BYTES );

    client.integrity_failures = ROCKETNET_INTEGRITY_LIMIT - 1;
    uint8_t payload[ROCKETNET_RECEIVE_CEILING];
    int payload_bytes = 0;
    check( rocketnet_client_protocol_process_packet( &client, denied, denied_bytes, 1000.6,
                                                     payload, sizeof(payload), &payload_bytes ) == 0 );

    check( client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );
    check( secrets_wiped_all_zero( client.client_ephemeral_secret_key, ROCKETNET_KEY_BYTES ) );
}

REGISTER_UNIT_TEST( test_secrets_wiped_ephemeral_on_token_expired_from_connection_request );
REGISTER_UNIT_TEST( test_secrets_wiped_ephemeral_on_integrity_limit_from_connection_request );

// ---------------------------------------------------------------------------
// the client's and the server's memory at destroy
// ---------------------------------------------------------------------------

#define SECRETS_WIPED_PROTOCOL 0x5EC2E75D1BEDULL
#define SECRETS_WIPED_MESSAGE_BYTES 256
#define SECRETS_WIPED_PATTERN_BYTES 32

// the planted message: no run of 32 of its bytes occurs in a block by chance
static void secrets_wiped_message( uint8_t * message )
{
    for ( int i = 0; i < SECRETS_WIPED_MESSAGE_BYTES; i++ )
        message[i] = (uint8_t) ( 0x96 ^ ( i * 13 + 7 ) );
}

// the connect token's user data: no run of 32 of its bytes occurs in a block
// by chance, and none matches the planted message
static void secrets_wiped_user_data( uint8_t * user_data )
{
    for ( int i = 0; i < ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES; i++ )
        user_data[i] = (uint8_t) ( 0x3C ^ ( i * 29 + 11 ) );
}

// how many times the first 32 bytes of a pattern occur in a block
static int secrets_wiped_count( const uint8_t * block, size_t bytes, const uint8_t * pattern )
{
    int found = 0;
    for ( size_t i = 0; i + SECRETS_WIPED_PATTERN_BYTES <= bytes; i++ )
        if ( block[i] == pattern[0] && memcmp( block + i, pattern, SECRETS_WIPED_PATTERN_BYTES ) == 0 )
            found++;
    return found;
}

// how many times the first 32 bytes of the planted message occur in a block
static int secrets_wiped_pattern_count( const uint8_t * block, size_t bytes )
{
    uint8_t message[SECRETS_WIPED_MESSAGE_BYTES];
    secrets_wiped_message( message );
    return secrets_wiped_count( block, bytes, message );
}

// the counting allocator: it keeps each block's size, and, once armed, a copy
// of every live block taken just before the destroys. At each free of a copied
// block it counts the bytes the destroy wrote: outside a watched object's
// secret spans (secrets_wiped_written_outside), and inside them left nonzero
// (secrets_wiped_secret_left).
struct secrets_wiped_watch_t
{
    const void * block;
    size_t offsets[8];
    size_t bytes[8];
    int spans;
    int freed;
};

static std::map<void *, size_t> * secrets_wiped_sizes = NULL;
static std::map<void *, std::vector<uint8_t> > * secrets_wiped_copies = NULL;
static secrets_wiped_watch_t secrets_wiped_watches[3];
static int secrets_wiped_num_watches = 0;
static size_t secrets_wiped_written_most = 0;       // the most bytes a destroy wrote outside the spans of one block
static int secrets_wiped_secret_left = 0;
static const void * secrets_wiped_watched[2] = { NULL, NULL };
static int secrets_wiped_watched_frees = 0;
static int secrets_wiped_watched_pattern = 0;
static bool secrets_wiped_user_data_armed = false;   // count the user data in every block freed
static int secrets_wiped_user_data_freed = 0;

static void * secrets_wiped_malloc( void * context, size_t bytes )
{
    (void) context;
    void * p = malloc( bytes );
    // a canary, not zeros: the SDK may not rely on zeroed memory, and the
    // copies and comparisons below read every byte of a block, so every byte
    // is defined (memcheck reads them as written)
    if ( p )
        memset( p, 0xC5, bytes );
    if ( p && secrets_wiped_sizes )
        ( *secrets_wiped_sizes )[p] = bytes;
    return p;
}

static bool secrets_wiped_in_span( const secrets_wiped_watch_t * watch, size_t offset )
{
    for ( int s = 0; watch && s < watch->spans; s++ )
        if ( offset >= watch->offsets[s] && offset < watch->offsets[s] + watch->bytes[s] )
            return true;
    return false;
}

static void secrets_wiped_free( void * context, void * p )
{
    (void) context;
    if ( p && secrets_wiped_sizes )
    {
        std::map<void *, size_t>::iterator it = secrets_wiped_sizes->find( p );
        if ( it != secrets_wiped_sizes->end() )
        {
            const uint8_t * begin = (const uint8_t *) p;
            const uint8_t * end = begin + it->second;
            if ( secrets_wiped_user_data_armed )
            {
                uint8_t user_data[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
                secrets_wiped_user_data( user_data );
                secrets_wiped_user_data_freed += secrets_wiped_count( begin, it->second, user_data );
            }
            for ( int i = 0; i < 2; i++ )
            {
                const uint8_t * watched = (const uint8_t *) secrets_wiped_watched[i];
                if ( watched && watched >= begin && watched < end )
                {
                    secrets_wiped_watched_frees++;
                    secrets_wiped_watched_pattern += secrets_wiped_pattern_count( begin, it->second );
                }
            }
            if ( secrets_wiped_copies )
            {
                std::map<void *, std::vector<uint8_t> >::iterator copy = secrets_wiped_copies->find( p );
                if ( copy != secrets_wiped_copies->end() )
                {
                    secrets_wiped_watch_t * watch = NULL;
                    for ( int w = 0; w < secrets_wiped_num_watches; w++ )
                        if ( secrets_wiped_watches[w].block == p )
                            watch = &secrets_wiped_watches[w];
                    size_t written = 0;
                    for ( size_t i = 0; i < it->second; i++ )
                    {
                        if ( secrets_wiped_in_span( watch, i ) )
                            secrets_wiped_secret_left += begin[i] != 0;
                        else
                            written += begin[i] != copy->second[i];
                    }
                    if ( written > secrets_wiped_written_most )
                        secrets_wiped_written_most = written;
                    if ( watch )
                        watch->freed++;
                    secrets_wiped_copies->erase( copy );
                }
            }
            secrets_wiped_sizes->erase( it );
        }
    }
    free( p );
}

struct secrets_wiped_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static double secrets_wiped_now = 1000.0;
static double secrets_wiped_clock( void * ) { return secrets_wiped_now; }

struct secrets_wiped_pair_t
{
    rocketnet_server_t * server;
    rocketnet_client_t * client;
    double time;
};

static void secrets_wiped_pump( secrets_wiped_pair_t * pair )
{
    secrets_wiped_now = pair->time;
    rocketnet_client_update( pair->client, pair->time );
    rocketnet_client_send_packets( pair->client );
    rocketnet_server_update( pair->server, pair->time );
    rocketnet_server_send_packets( pair->server );
    pair->time += 0.01;
}

// a connected pair over the loopback transport, in blocks from the
// allocator, then one planted message each way, received and released
static void secrets_wiped_pair_exchange( secrets_wiped_pair_t * pair, const uint8_t * user_data = NULL )
{
    memset( pair, 0, sizeof(*pair) );
    pair->time = 1000.0;
    rocketnet_quiet( 1 );

    secrets_wiped_identity_t identity;
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x5E ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity.backend_public, identity.backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity.server_public, identity.server_secret ) == ROCKETNET_STATUS_OK );

    rocketnet_server_config_t server_config;
    rocketnet_server_default_config( &server_config );
    server_config.protocol_id = SECRETS_WIPED_PROTOCOL;
    server_config.max_clients = 1;
    memcpy( server_config.backend_public_key, identity.backend_public, sizeof( server_config.backend_public_key ) );
    server_config.transport = "loopback";
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    pair->server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( pair->server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    rocketnet_server_public_key( pair->server, identity.server_public );
    check( rocketnet_server_start( pair->server ) == ROCKETNET_STATUS_OK );

    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( pair->server ) );
    const char * addresses[] = { address };
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_production_connect_token( identity.backend_secret, identity.server_public,
        SECRETS_WIPED_PROTOCOL, addresses, 1, 0x5EED, 1000, 1ULL << 40, 300, user_data,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    char token_string[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, sizeof( token_string ) ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
    sodium_memzero( token, sizeof( token ) );
    sodium_memzero( &identity, sizeof( identity ) );
    sodium_memzero( &server_config, sizeof( server_config ) );

    rocketnet_client_test_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "loopback";
    client_config.step_driven = true;
    client_config.time_callback = secrets_wiped_clock;
    create_status = ROCKETNET_STATUS_OK;
    pair->client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( pair->client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( pair->client, token_string ) == ROCKETNET_STATUS_OK );
    sodium_memzero( token_string, sizeof( token_string ) );

    for ( int i = 0; i < 400 && rocketnet_client_state( pair->client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
        secrets_wiped_pump( pair );
    check( rocketnet_client_state( pair->client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    check( rocketnet_server_client_connected( pair->server, 0 ) != 0 );

    uint8_t message[SECRETS_WIPED_MESSAGE_BYTES];
    secrets_wiped_message( message );
    uint8_t * out = rocketnet_client_start_message( pair->client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SECRETS_WIPED_MESSAGE_BYTES );
    check( out != NULL );
    memcpy( out, message, SECRETS_WIPED_MESSAGE_BYTES );
    check( rocketnet_client_finish_message( pair->client, SECRETS_WIPED_MESSAGE_BYTES ) != 0 );
    out = rocketnet_server_start_message( pair->server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, SECRETS_WIPED_MESSAGE_BYTES );
    check( out != NULL );
    memcpy( out, message, SECRETS_WIPED_MESSAGE_BYTES );
    check( rocketnet_server_finish_message( pair->server, SECRETS_WIPED_MESSAGE_BYTES ) != 0 );

    int at_server = 0;
    int at_client = 0;
    for ( int step = 0; step < 400 && ( at_server < 1 || at_client < 1 ); step++ )
    {
        secrets_wiped_pump( pair );
        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_server_receive_messages( pair->server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                check( batch->client_index[i] == 0 && batch->channel[i] == 0 );
                check( batch->bytes[i] == SECRETS_WIPED_MESSAGE_BYTES && memcmp( batch->data[i], message, SECRETS_WIPED_MESSAGE_BYTES ) == 0 );
                at_server++;
            }
        }
        while ( ( batch = rocketnet_client_receive_messages( pair->client ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                check( batch->channel[i] == 0 );
                check( batch->bytes[i] == SECRETS_WIPED_MESSAGE_BYTES && memcmp( batch->data[i], message, SECRETS_WIPED_MESSAGE_BYTES ) == 0 );
                at_client++;
            }
        }
    }
    check( at_server == 1 && at_client == 1 );
}

// the allocator's block that holds p (rocketnet_malloc places the object
// past its own header, inside the block the allocator returned)
static void * secrets_wiped_containing( const void * p )
{
    for ( std::map<void *, size_t>::iterator it = secrets_wiped_sizes->begin(); it != secrets_wiped_sizes->end(); ++it )
        if ( (const uint8_t *) p >= (const uint8_t *) it->first && (const uint8_t *) p < (const uint8_t *) it->first + it->second )
            return it->first;
    return NULL;
}

static void secrets_wiped_watch( const void * object, int ( * spans )( size_t *, size_t *, int ) )
{
    check( object != NULL );
    void * block = secrets_wiped_containing( object );
    check( block != NULL );
    secrets_wiped_watch_t * watch = &secrets_wiped_watches[secrets_wiped_num_watches++];
    memset( watch, 0, sizeof( *watch ) );
    watch->block = block;
    watch->spans = spans( watch->offsets, watch->bytes, 8 );
    check( watch->spans > 0 );
    const size_t delta = (size_t) ( (const uint8_t *) object - (const uint8_t *) block );
    for ( int s = 0; s < watch->spans; s++ )
        watch->offsets[s] += delta;
}

// how many bytes of a live object's secret spans are nonzero now
static int secrets_wiped_secret_bytes( const secrets_wiped_watch_t * watch )
{
    int nonzero = 0;
    for ( int s = 0; s < watch->spans; s++ )
        for ( size_t i = 0; i < watch->bytes[s]; i++ )
            nonzero += ( (const uint8_t *) watch->block )[watch->offsets[s] + i] != 0;
    return nonzero;
}

// Owned memory, stopped and disconnected: the destroys wipe the server's
// secret key in its header, the server transport's secret and cookie keys and
// the client transport's keys, and write nothing else of note: no block,
// arena included, has more than a handful of bytes (handles, mutex state)
// written by a destroy. The planted message is still in the blocks: message
// bytes are not secrets.
static void test_secrets_wiped_destroy_writes_only_keys()
{
    rocketnet_term();
    check( rocketnet_init( NULL, secrets_wiped_malloc, secrets_wiped_free ) == ROCKETNET_STATUS_OK );
    std::map<void *, size_t> sizes;
    secrets_wiped_sizes = &sizes;

    secrets_wiped_pair_t pair;
    secrets_wiped_pair_exchange( &pair );

    rocketnet_client_disconnect( pair.client );
    rn_test_client_wait_disconnected( pair.client );
    rocketnet_server_stop( pair.server );
    rn_test_server_wait_stopped( pair.server );

    secrets_wiped_num_watches = 0;
    secrets_wiped_watch( pair.server, rocketnet_server_secret_spans_for_test );
    secrets_wiped_watch( rocketnet_message_server_transport( rocketnet_server_message_server( pair.server ) ), rocketnet_server_transport_secret_spans_for_test );
    secrets_wiped_watch( rocketnet_message_client_transport( rocketnet_client_message_client( pair.client ) ), rocketnet_client_transport_secret_spans_for_test );
    // the server's secret key and its first cookie key are live until destroy
    const int server_secret_before = secrets_wiped_secret_bytes( &secrets_wiped_watches[0] );
    const int transport_secret_before = secrets_wiped_secret_bytes( &secrets_wiped_watches[1] );

    std::map<void *, std::vector<uint8_t> > copies;
    for ( std::map<void *, size_t>::iterator it = sizes.begin(); it != sizes.end(); ++it )
        copies[it->first].assign( (const uint8_t *) it->first, (const uint8_t *) it->first + it->second );
    secrets_wiped_copies = &copies;
    secrets_wiped_written_most = 0;
    secrets_wiped_secret_left = 0;
    secrets_wiped_watched[0] = pair.client;
    secrets_wiped_watched[1] = pair.server;
    secrets_wiped_watched_frees = 0;
    secrets_wiped_watched_pattern = 0;

    rocketnet_client_destroy( pair.client );
    rocketnet_server_destroy( pair.server );

    const size_t written_most = secrets_wiped_written_most;
    const int secret_left = secrets_wiped_secret_left;
    const int watched_frees = secrets_wiped_watched_frees;
    const int watched_pattern = secrets_wiped_watched_pattern;
    int watches_freed = 0;
    for ( int w = 0; w < secrets_wiped_num_watches; w++ )
        watches_freed += secrets_wiped_watches[w].freed;
    secrets_wiped_copies = NULL;
    secrets_wiped_watched[0] = NULL;
    secrets_wiped_watched[1] = NULL;
    secrets_wiped_sizes = NULL;
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    check( server_secret_before > 0 );
    check( transport_secret_before > 0 );
    check( watches_freed == 3 );
    check( secret_left == 0 );
    check( written_most <= 256 );
    check( watched_frees == 2 );
    check( watched_pattern > 0 );
}

// how many times the user data occurs in the live blocks now
static int secrets_wiped_user_data_live()
{
    uint8_t user_data[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    secrets_wiped_user_data( user_data );
    int found = 0;
    for ( std::map<void *, size_t>::iterator it = secrets_wiped_sizes->begin(); it != secrets_wiped_sizes->end(); ++it )
        found += secrets_wiped_count( (const uint8_t *) it->first, it->second, user_data );
    return found;
}

// The connect token's user data comes out of the token's private part and
// can carry the game's secrets: the server's encryption mapping and client
// slot hold copies. No block is freed with it in, whether the client left
// before the server stopped (the mapping removed and the slot released) or
// the server stopped and was destroyed with the client still connected (the
// mapping live at destroy).
static void secrets_wiped_user_data_case( bool server_first )
{
    rocketnet_term();
    check( rocketnet_init( NULL, secrets_wiped_malloc, secrets_wiped_free ) == ROCKETNET_STATUS_OK );
    std::map<void *, size_t> sizes;
    secrets_wiped_sizes = &sizes;
    secrets_wiped_user_data_freed = 0;

    uint8_t user_data[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    secrets_wiped_user_data( user_data );
    secrets_wiped_pair_t pair;
    secrets_wiped_pair_exchange( &pair, user_data );
    const int live_connected = secrets_wiped_user_data_live();
    secrets_wiped_user_data_armed = true;

    if ( server_first )
    {
        rocketnet_server_stop( pair.server );
        rn_test_server_wait_stopped( pair.server );
        rocketnet_server_destroy( pair.server );
        rocketnet_client_disconnect( pair.client );
        rn_test_client_wait_disconnected( pair.client );
        rocketnet_client_destroy( pair.client );
    }
    else
    {
        rocketnet_client_disconnect( pair.client );
        for ( int i = 0; i < 100 && rocketnet_server_client_connected( pair.server, 0 ); i++ )
            secrets_wiped_pump( &pair );
        check( !rocketnet_server_client_connected( pair.server, 0 ) );
        // wiped at the leaving, not only at destroy: the mapping removed
        // and the slot released hold no copy
        check( secrets_wiped_user_data_live() == 0 );
        rn_test_client_wait_disconnected( pair.client );
        rocketnet_client_destroy( pair.client );
        rocketnet_server_stop( pair.server );
        rn_test_server_wait_stopped( pair.server );
        rocketnet_server_destroy( pair.server );
    }

    const int freed = secrets_wiped_user_data_freed;
    secrets_wiped_user_data_armed = false;
    secrets_wiped_sizes = NULL;
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    check( live_connected >= 2 );   // the mapping's copy and the slot's
    check( freed == 0 );
}

// The slots and the map at their destroy, each holding the user data: the
// server's paths release a slot and remove a mapping before destroy, so
// these are the destroys' own wipes, driven directly.
static void test_secrets_wiped_user_data_at_slots_and_map_destroy()
{
    rocketnet_term();
    check( rocketnet_init( NULL, secrets_wiped_malloc, secrets_wiped_free ) == ROCKETNET_STATUS_OK );
    std::map<void *, size_t> sizes;
    secrets_wiped_sizes = &sizes;
    secrets_wiped_user_data_freed = 0;

    uint8_t user_data[ROCKETNET_CONNECT_TOKEN_USER_DATA_BYTES];
    secrets_wiped_user_data( user_data );
    struct rocketnet_client_slots_t * slots = rocketnet_client_slots_create( NULL, 4 );
    check( slots != NULL );
    check( rocketnet_client_slots_assign( slots, 0x5EED, 0, user_data ) >= 0 );
    struct rocketnet_encryption_map_t * map = rocketnet_encryption_map_create( NULL, 4 );
    check( map != NULL );
    struct rocketnet_address_t address;
    check( rocketnet_address_parse( &address, "10.0.0.1:30000" ) );
    const int index = rocketnet_encryption_map_add( map, &address, 1000.0 );
    check( index >= 0 );
    memcpy( rocketnet_encryption_map_entry( map, index )->user_data, user_data, sizeof( user_data ) );
    const int live = secrets_wiped_user_data_live();

    secrets_wiped_user_data_armed = true;
    rocketnet_client_slots_destroy( slots );
    rocketnet_encryption_map_destroy( map );
    const int freed = secrets_wiped_user_data_freed;
    secrets_wiped_user_data_armed = false;
    secrets_wiped_sizes = NULL;
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    check( live == 2 );
    check( freed == 0 );
}

static void test_secrets_wiped_user_data_after_client_leaves()
{
    secrets_wiped_user_data_case( false );
}

static void test_secrets_wiped_user_data_with_client_connected()
{
    secrets_wiped_user_data_case( true );
}

static uint8_t * secrets_wiped_aligned( uint8_t * raw )
{
    return (uint8_t *) ( ( (uintptr_t) raw + ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) & ~(uintptr_t) ( ROCKETNET_ALLOCATION_ALIGNMENT - 1 ) );
}

// the planted message in the first region's receive ring
static void secrets_wiped_plant_in_arena( rn_arena_t * arena )
{
    rn_region_t * region = rn_arena_region( arena, 0 );
    check( region != NULL );
    check( region->ring_bytes >= SECRETS_WIPED_MESSAGE_BYTES );
    secrets_wiped_message( region->receive_ring );
}

// An owned arena: destroy frees the block without writing it, so the
// planted message is still there at the free.
static void test_secrets_wiped_arena_destroy_owned_writes_nothing()
{
    rocketnet_term();
    check( rocketnet_init( NULL, secrets_wiped_malloc, secrets_wiped_free ) == ROCKETNET_STATUS_OK );
    std::map<void *, size_t> sizes;
    secrets_wiped_sizes = &sizes;

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rn_arena_t * arena = rn_arena_create( NULL, NULL, 2, &counters );
    check( arena != NULL );
    secrets_wiped_plant_in_arena( arena );
    std::map<void *, std::vector<uint8_t> > copies;
    void * block = secrets_wiped_containing( arena );
    check( block != NULL );
    copies[block].assign( (const uint8_t *) block, (const uint8_t *) block + sizes[block] );
    secrets_wiped_copies = &copies;
    secrets_wiped_num_watches = 0;
    secrets_wiped_written_most = 0;
    secrets_wiped_watched[0] = arena;
    secrets_wiped_watched_frees = 0;
    secrets_wiped_watched_pattern = 0;
    rn_arena_destroy( arena );

    const size_t written_most = secrets_wiped_written_most;
    const int watched_frees = secrets_wiped_watched_frees;
    const int watched_pattern = secrets_wiped_watched_pattern;
    const bool copied_freed = copies.empty();
    secrets_wiped_copies = NULL;
    secrets_wiped_watched[0] = NULL;
    secrets_wiped_sizes = NULL;
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    check( copied_freed );
    check( written_most == 0 );
    check( watched_frees == 1 );
    check( watched_pattern == 1 );
}

// An arena in a caller's block: destroy hands the block back unwritten.
static void test_secrets_wiped_arena_destroy_in_block_writes_nothing()
{
    const uint64_t bytes = rn_arena_required_bytes( NULL, 2 );
    check( bytes > 0 );
    uint8_t * raw = (uint8_t *) malloc( (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT );
    check( raw != NULL );
    memset( raw, 0xC5, (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT );
    uint8_t * block = secrets_wiped_aligned( raw );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rn_arena_t * arena = rn_arena_create_in_block( block, bytes, NULL, 2, &counters );
    check( arena != NULL );
    secrets_wiped_plant_in_arena( arena );
    std::vector<uint8_t> copy( block, block + bytes );
    rn_arena_destroy( arena );

    const bool unwritten = memcmp( block, copy.data(), (size_t) bytes ) == 0;
    free( raw );
    check( unwritten );
}

// test_secrets_wiped_destroy_writes_only_keys's peak resident bound. The case
// copies every live block of its connected pair before destroy, so its peak
// is twice the pair's memory. The server transport's fixed tables grow with
// the build's ROCKETNET_MAX_CLIENTS whatever the case's max_clients
// (BUILDING.md, "Maximum clients"), so a 10000-client build puts the doubled
// pair over the suite-wide bound. The bound is twice the pair's plan:
// rocketnet_server_transport_memory_plan's total for one client, planned
// with the portable transport because the plan has no row for the loopback
// transport, the client's rocketnet_client_required_memory and its portable
// transport's rocketnet_client_transport_required_memory, and the two
// loopback endpoints' create-time queues; plus a stated 64 MiB margin for
// the process and the two block maps.
static long secrets_wiped_destroy_rss_bound_mb()
{
    rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config( &server_config );
    server_config.protocol_id = SECRETS_WIPED_PROTOCOL;
    server_config.max_clients = 1;
    rocketnet_server_memory_plan_t plan;
    if ( !rocketnet_server_transport_memory_plan( &server_config, "portable", plan ) )
        return 0;
    rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    const uint64_t client_bytes = (uint64_t) rocketnet_client_required_memory( &client_config );
    rocketnet_client_transport_config_t client_transport_config;
    rocketnet_client_transport_default_config( &client_transport_config );
    const uint64_t client_transport_bytes = rocketnet_client_transport_required_memory( &client_transport_config, "portable" );
    if ( client_bytes == 0 || client_transport_bytes == 0 )
        return 0;
    const uint64_t endpoint = rocketnet_loopback_queue_bytes( ROCKETNET_LOOPBACK_QUEUE_PACKETS ) + rocketnet_allocation_overhead();
    return suite_rss_bound_mb( 2 * ( plan.total_bytes + client_bytes + client_transport_bytes + 2 * endpoint ), 64 );
}

SUITE_CASE_RSS_BOUND( test_secrets_wiped_destroy_writes_only_keys, secrets_wiped_destroy_rss_bound_mb );

REGISTER_UNIT_TEST( test_secrets_wiped_destroy_writes_only_keys );
REGISTER_UNIT_TEST( test_secrets_wiped_user_data_at_slots_and_map_destroy );
REGISTER_UNIT_TEST( test_secrets_wiped_user_data_after_client_leaves );
REGISTER_UNIT_TEST( test_secrets_wiped_user_data_with_client_connected );
REGISTER_UNIT_TEST( test_secrets_wiped_arena_destroy_owned_writes_nothing );
REGISTER_UNIT_TEST( test_secrets_wiped_arena_destroy_in_block_writes_nothing );

#if defined(ROCKETNET_SECRET_WIPE_TEST_HOOKS)

// ---------------------------------------------------------------------------
// the config parse's base64 staging (sdk/rocketnet_config.cpp)
// ---------------------------------------------------------------------------

extern void (*rocketnet_config_decode_staging_hook)( const uint8_t * staging, int bytes );

static uint8_t secrets_wiped_staging[256];
static int secrets_wiped_staging_bytes = -1;

static void secrets_wiped_staging_seen( const uint8_t * staging, int bytes )
{
    check( bytes > 0 && bytes <= (int) sizeof( secrets_wiped_staging ) );
    memcpy( secrets_wiped_staging, staging, (size_t) bytes );
    secrets_wiped_staging_bytes = bytes;
}

static int secrets_wiped_setenv( const char * name, const char * value )
{
#ifdef _WIN32
    return _putenv_s( name, value );
#else
    return setenv( name, value, 1 );
#endif
}

static int secrets_wiped_unsetenv( const char * name )
{
#ifdef _WIN32
    return _putenv_s( name, "" );
#else
    return unsetenv( name );
#endif
}

struct secrets_wiped_config_t
{
    uint8_t secret_key[32];
};

static const struct rocketnet_config_field_t secrets_wiped_fields[] =
{
    { "secret_key", ROCKETNET_CONFIG_KIND_BYTES, 0, 32, 0, 0, ROCKETNET_CONFIG_SECRET },
};

// One BYTES field, secret, parsed from ROCKETNET_SECRETWIPE_SECRET_KEY. A
// whole value is copied into the config; a broken value is refused. Either
// way the staging the hook sees on the exit is all zero.
static bool secrets_wiped_parse( const char * value, struct secrets_wiped_config_t * config )
{
    char env_name[128];
    check( rocketnet_config_env_var_name_for( "secretwipe", "secret_key", env_name, sizeof( env_name ) ) );
    check( secrets_wiped_setenv( env_name, value ) == 0 );
    memset( config, 0, sizeof(*config) );
    memset( secrets_wiped_staging, 0xA5, sizeof( secrets_wiped_staging ) );
    secrets_wiped_staging_bytes = -1;
    rocketnet_config_decode_staging_hook = secrets_wiped_staging_seen;
    const bool parsed = rocketnet_config_apply_env( "secretwipe", config, secrets_wiped_fields, 1 );
    rocketnet_config_decode_staging_hook = NULL;
    check( secrets_wiped_unsetenv( env_name ) == 0 );
    return parsed;
}

static void test_secrets_wiped_config_staging_on_parse()
{
    uint8_t key[32];
    memset( key, 0x5C, sizeof( key ) );
    char encoded[64];
    check( rocketnet_base64_encode_data( key, sizeof( key ), encoded, sizeof( encoded ) ) == 44 );

    struct secrets_wiped_config_t config;
    check( secrets_wiped_parse( encoded, &config ) );
    check( memcmp( config.secret_key, key, sizeof( key ) ) == 0 );
    check( secrets_wiped_staging_bytes == 32 );
    check( secrets_wiped_all_zero( secrets_wiped_staging, secrets_wiped_staging_bytes ) );
    sodium_memzero( &config, sizeof( config ) );
}

// a value whose last group is broken: the decode writes the bytes before it
// into the staging, then refuses
static void test_secrets_wiped_config_staging_on_refused_parse()
{
    uint8_t key[32];
    memset( key, 0x5C, sizeof( key ) );
    char encoded[64];
    check( rocketnet_base64_encode_data( key, sizeof( key ), encoded, sizeof( encoded ) ) == 44 );
    encoded[41] = '!';

    struct secrets_wiped_config_t config;
    check( !secrets_wiped_parse( encoded, &config ) );
    check( secrets_wiped_all_zero( config.secret_key, sizeof( config.secret_key ) ) );
    check( secrets_wiped_staging_bytes == 32 );
    check( secrets_wiped_all_zero( secrets_wiped_staging, secrets_wiped_staging_bytes ) );
}

REGISTER_UNIT_TEST( test_secrets_wiped_config_staging_on_parse );
REGISTER_UNIT_TEST( test_secrets_wiped_config_staging_on_refused_parse );

// ---------------------------------------------------------------------------
// the private block staging of a token open (sdk/rocketnet_connect_token.cpp)
// ---------------------------------------------------------------------------

extern void (*rocketnet_token_private_block_hook)( uint8_t * block, int bytes, int at_exit );

static uint8_t secrets_wiped_block[ROCKETNET_PRIVATE_BLOCK_BYTES];
static int secrets_wiped_block_exits = 0;

// on entry the staging gets the canary, so an exit that skips the wipe is
// seen whatever the open wrote; on exit the staging is copied out
static void secrets_wiped_block_seen( uint8_t * block, int bytes, int at_exit )
{
    check( bytes == ROCKETNET_PRIVATE_BLOCK_BYTES );
    if ( !at_exit )
    {
        memset( block, 0xA5, (size_t) bytes );
        return;
    }
    memcpy( secrets_wiped_block, block, (size_t) bytes );
    secrets_wiped_block_exits++;
}

static int secrets_wiped_open( const uint8_t * server_public, const uint8_t * server_secret )
{
    uint8_t token_data[ROCKETNET_CONNECT_TOKEN_BYTES];
    secrets_wiped_mint_vector_token( token_data );
    struct rocketnet_connect_token_t token;
    rocketnet_read_connect_token( &token, token_data );

    struct rocketnet_connect_token_private_t opened;
    memset( &opened, 0, sizeof( opened ) );
    secrets_wiped_block_exits = 0;
    rocketnet_token_private_block_hook = secrets_wiped_block_seen;
    const int result = rocketnet_open_connect_token_private( &opened, token.signed_connect_data, server_public, server_secret );
    rocketnet_token_private_block_hook = NULL;
    sodium_memzero( &opened, sizeof( opened ) );
    sodium_memzero( &token, sizeof( token ) );
    return result;
}

static void test_secrets_wiped_token_block_on_open()
{
    check( secrets_wiped_open( rocketnet_test_server_public_key, rocketnet_test_server_secret_key ) == 1 );
    check( secrets_wiped_block_exits == 1 );
    check( secrets_wiped_all_zero( secrets_wiped_block, ROCKETNET_PRIVATE_BLOCK_BYTES ) );
}

// a server key pair the token was not sealed to: the open is refused
static void test_secrets_wiped_token_block_on_refused_open()
{
    uint8_t other_public[ROCKETNET_KEY_BYTES];
    uint8_t other_secret[ROCKETNET_KEY_BYTES];
    crypto_box_keypair( other_public, other_secret );
    check( secrets_wiped_open( other_public, other_secret ) == 0 );
    sodium_memzero( other_secret, sizeof( other_secret ) );
    check( secrets_wiped_block_exits == 1 );
    check( secrets_wiped_all_zero( secrets_wiped_block, ROCKETNET_PRIVATE_BLOCK_BYTES ) );
}

REGISTER_UNIT_TEST( test_secrets_wiped_token_block_on_open );
REGISTER_UNIT_TEST( test_secrets_wiped_token_block_on_refused_open );

#endif // #if defined(ROCKETNET_SECRET_WIPE_TEST_HOOKS)

// Unassigned token-user-data storage has no secret to erase. Only live slots
// are wiped at destroy; released slots were already wiped at release.
static void test_secrets_wiped_slots_touch_only_live_user_data()
{
    rocketnet_term();
    check( rocketnet_init( NULL, secrets_wiped_malloc, secrets_wiped_free ) == ROCKETNET_STATUS_OK );
    std::map<void *, size_t> sizes;
    secrets_wiped_sizes = &sizes;
    auto * slots = rocketnet_client_slots_create( NULL, 32 );
    check( slots );
    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    secrets_wiped_user_data( user_data );
    const int first = rocketnet_client_slots_assign( slots, 1, 0, user_data );
    const int second = rocketnet_client_slots_assign( slots, 2, 1, user_data );
    check( first == 0 && second == 1 );
    const uint8_t * data = rocketnet_client_slots_user_data( slots, first );
    bool unused_untouched = true;
    for ( size_t i = 2 * sizeof(user_data); i < 32 * sizeof(user_data); ++i )
        unused_untouched &= data[i] == 0xC5;
    rocketnet_client_slots_release( slots, first );
    const bool released_wiped = secrets_wiped_all_zero( data, sizeof(user_data) );
    std::map<void *, std::vector<uint8_t> > copies;
    for ( auto it = sizes.begin(); it != sizes.end(); ++it )
        copies[it->first].assign( (const uint8_t *)it->first, (const uint8_t *)it->first + it->second );
    secrets_wiped_copies = &copies;
    secrets_wiped_num_watches = 0;
    secrets_wiped_written_most = 0;
    secrets_wiped_user_data_freed = 0;
    secrets_wiped_user_data_armed = true;
    rocketnet_client_slots_destroy( slots );
    const size_t changed = secrets_wiped_written_most;
    const int secrets_left = secrets_wiped_user_data_freed;
    secrets_wiped_copies = NULL;
    secrets_wiped_user_data_armed = false;
    secrets_wiped_sizes = NULL;
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    check( unused_untouched && released_wiped );
    check( secrets_left == 0 );
    check( changed > 0 && changed <= sizeof(user_data) );
}
REGISTER_UNIT_TEST( test_secrets_wiped_slots_touch_only_live_user_data );
