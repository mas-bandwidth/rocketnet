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
//   - the client's and the server's memory at destroy: a message's bytes,
//     planted through the public API, are gone from the block when
//     rocketnet_client_destroy and rocketnet_server_destroy free it
//     (sdk/rocketnet_api.cpp).
//   - a message arena at its destroy, owned or in a caller's block: the
//     planted bytes in a region are gone (sdk/rocketnet_allocator.cpp,
//     rn_arena_destroy). This is the message layer's own destroy, beneath
//     the public objects' wipe.
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
#include "../sdk/rocketnet_test_vectors.h"

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

// how many times the first 32 bytes of the planted message occur in a block
static int secrets_wiped_pattern_count( const uint8_t * block, size_t bytes )
{
    uint8_t message[SECRETS_WIPED_MESSAGE_BYTES];
    secrets_wiped_message( message );
    int found = 0;
    for ( size_t i = 0; i + SECRETS_WIPED_PATTERN_BYTES <= bytes; i++ )
        if ( block[i] == message[0] && memcmp( block + i, message, SECRETS_WIPED_PATTERN_BYTES ) == 0 )
            found++;
    return found;
}

// the counting allocator: it keeps each block's size, and on the free of a
// block that holds the watched client or server it counts the planted
// message's occurrences still in the block. Its blocks start zeroed: the
// count reads every byte, including bytes the SDK never wrote, and an
// unwritten byte has no value to compare
static std::map<void *, size_t> * secrets_wiped_sizes = NULL;
static const void * secrets_wiped_watched[2] = { NULL, NULL };
static int secrets_wiped_watched_frees = 0;
static int secrets_wiped_watched_pattern = 0;

static void * secrets_wiped_malloc( void * context, size_t bytes )
{
    (void) context;
    void * p = calloc( 1, bytes );
    if ( p && secrets_wiped_sizes )
        ( *secrets_wiped_sizes )[p] = bytes;
    return p;
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
            for ( int i = 0; i < 2; i++ )
            {
                const uint8_t * watched = (const uint8_t *) secrets_wiped_watched[i];
                if ( watched && watched >= begin && watched < end )
                {
                    secrets_wiped_watched_frees++;
                    secrets_wiped_watched_pattern += secrets_wiped_pattern_count( begin, it->second );
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
static void secrets_wiped_pair_exchange( secrets_wiped_pair_t * pair )
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
        SECRETS_WIPED_PROTOCOL, addresses, 1, 0x5EED, 1000, 1ULL << 40, 300, NULL,
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

// Owned memory: the counting allocator sees the free of the block that holds
// the client and of the block that holds the server, and neither still holds
// the planted message.
static void test_secrets_wiped_destroy_owned_blocks()
{
    rocketnet_term();
    check( rocketnet_init( NULL, secrets_wiped_malloc, secrets_wiped_free ) == ROCKETNET_STATUS_OK );
    std::map<void *, size_t> sizes;
    secrets_wiped_sizes = &sizes;

    secrets_wiped_pair_t pair;
    secrets_wiped_pair_exchange( &pair );
    secrets_wiped_watched[0] = pair.client;
    secrets_wiped_watched[1] = pair.server;
    secrets_wiped_watched_frees = 0;
    secrets_wiped_watched_pattern = 0;

    rn_test_client_stop_and_destroy( pair.client );
    rn_test_server_stop_and_destroy( pair.server );

    const int watched_frees = secrets_wiped_watched_frees;
    const int watched_pattern = secrets_wiped_watched_pattern;
    secrets_wiped_watched[0] = NULL;
    secrets_wiped_watched[1] = NULL;
    secrets_wiped_sizes = NULL;
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    check( watched_frees == 2 );
    check( watched_pattern == 0 );
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

// An owned arena: the counting allocator sees its block freed, and the block
// no longer holds the planted message.
static void test_secrets_wiped_arena_destroy_owned()
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
    secrets_wiped_watched[0] = arena;
    secrets_wiped_watched_frees = 0;
    secrets_wiped_watched_pattern = 0;
    rn_arena_destroy( arena );

    const int watched_frees = secrets_wiped_watched_frees;
    const int watched_pattern = secrets_wiped_watched_pattern;
    secrets_wiped_watched[0] = NULL;
    secrets_wiped_sizes = NULL;
    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    check( watched_frees == 1 );
    check( watched_pattern == 0 );
}

// An arena in a caller's block: destroy gives the block back without the
// planted message.
static void test_secrets_wiped_arena_destroy_in_block()
{
    const uint64_t bytes = rn_arena_required_bytes( NULL, 2 );
    check( bytes > 0 );
    uint8_t * raw = (uint8_t *) malloc( (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT );
    check( raw != NULL );
    uint8_t * block = secrets_wiped_aligned( raw );

    // the scan below reads every byte of the block, including bytes the arena
    // never writes, so the block starts zeroed: an unwritten byte has no value
    memset( raw, 0, (size_t) bytes + ROCKETNET_ALLOCATION_ALIGNMENT );

    struct rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rn_arena_t * arena = rn_arena_create_in_block( block, bytes, NULL, 2, &counters );
    check( arena != NULL );
    secrets_wiped_plant_in_arena( arena );
    check( secrets_wiped_pattern_count( block, (size_t) bytes ) == 1 );
    rn_arena_destroy( arena );

    const int pattern = secrets_wiped_pattern_count( block, (size_t) bytes );
    free( raw );
    check( pattern == 0 );
}

REGISTER_UNIT_TEST( test_secrets_wiped_destroy_owned_blocks );
REGISTER_UNIT_TEST( test_secrets_wiped_arena_destroy_owned );
REGISTER_UNIT_TEST( test_secrets_wiped_arena_destroy_in_block );

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
