/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Config sizes across the ABI (docs/ABI-EVOLUTION.md).
//
// The internal layers' sized entry points (the transport and message
// configs): each struct is passed with its size, a known size is accepted, a
// size smaller than the current one reads the caller's prefix and defaults
// the rest, and an unknown size is refused with nothing written.
//
// The public configs carry their size in their first field, config_bytes, set
// by the header's inline default_config: a config_bytes below the field
// itself (a memset-zero config) is refused with CONFIG_NOT_INITIALIZED; a
// smaller one (an older caller) reads its prefix and the rest takes the
// defaults; a larger one (a newer caller) reads as far as the library knows
// and the tail is ignored. The library never reads or writes past
// config_bytes.
//
// Create is the one check of a public config (rocketnet_client_create and
// rocketnet_server_create in sdk/rocketnet_api.cpp). The regressions below
// check full defaults and simulation fields, including a real in-memory
// loopback create/connect.

#include "test_unit_registry.h"
#include "sdk/rocketnet_base64.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_fragment.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static void sized_config_quiet_assert( const char *, const char *, const char *, int ) {}

// the defaults, then the published development keys set by hand
static void sized_config_dev_server_config( struct rocketnet_server_config_t * config )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( config->backend_public_key, rocketnet_dev_backend_public_key, 32 );
}

static const size_t client_known_bytes[] = { ROCKETNET_CLIENT_TRANSPORT_CONFIG_KNOWN_BYTES };
static const size_t server_known_bytes[] = { ROCKETNET_SERVER_TRANSPORT_CONFIG_KNOWN_BYTES };
static const size_t message_known_bytes[] = { ROCKETNET_MESSAGE_CONFIG_KNOWN_BYTES };

#define COUNT( a ) ( (int) ( sizeof( a ) / sizeof( a[0] ) ) )

// the known-size tables are oldest first, strictly growing, and end at today's size
static void sized_config_check_table( const size_t * known, int count, size_t current )
{
    check( count >= 1 );
    for ( int i = 1; i < count; i++ )
        check( known[i] > known[i-1] );
    check( known[count-1] == current );
}

static void test_sized_config_accepts_every_known_size()
{
    sized_config_check_table( client_known_bytes, COUNT( client_known_bytes ), sizeof( struct rocketnet_client_transport_config_t ) );
    sized_config_check_table( server_known_bytes, COUNT( server_known_bytes ), sizeof( struct rocketnet_server_transport_config_t ) );
    sized_config_check_table( message_known_bytes, COUNT( message_known_bytes ), sizeof( struct rocketnet_message_config_t ) );

    // a caller compiled against the current size: the sized defaults are the
    // unsized defaults, byte for byte, for every known size's prefix

    for ( int i = 0; i < COUNT( client_known_bytes ); i++ )
    {
        struct rocketnet_client_transport_config_t unsized, sized;
        memset( &unsized, 0xAB, sizeof( unsized ) );
        memset( &sized, 0xAB, sizeof( sized ) );
        rocketnet_client_transport_default_config( &unsized );
        check( rocketnet_client_transport_default_config_sized( &sized, client_known_bytes[i] ) == 1 );
        check( memcmp( &unsized, &sized, client_known_bytes[i] ) == 0 );
    }

    for ( int i = 0; i < COUNT( server_known_bytes ); i++ )
    {
        struct rocketnet_server_transport_config_t unsized, sized;
        memset( &unsized, 0xAB, sizeof( unsized ) );
        memset( &sized, 0xAB, sizeof( sized ) );
        rocketnet_server_transport_default_config( &unsized );
        check( rocketnet_server_transport_default_config_sized( &sized, server_known_bytes[i] ) == 1 );
        check( memcmp( &unsized, &sized, server_known_bytes[i] ) == 0 );
    }

    for ( int i = 0; i < COUNT( message_known_bytes ); i++ )
    {
        struct rocketnet_message_config_t unsized, sized;
        memset( &unsized, 0xAB, sizeof( unsized ) );
        memset( &sized, 0xAB, sizeof( sized ) );
        rocketnet_message_default_config( &unsized );
        check( rocketnet_message_default_config_sized( &sized, message_known_bytes[i] ) == 1 );
        check( memcmp( &unsized, &sized, message_known_bytes[i] ) == 0 );
    }

    // a caller compiled against an older revision: simulated as a client
    // config that ended before the transport field. the defaults write only the
    // old prefix; the read takes the caller's prefix and defaults the rest.

    const size_t older = offsetof( struct rocketnet_client_transport_config_t, transport );
    const size_t simulated_known[] = { older, sizeof( struct rocketnet_client_transport_config_t ) };

    struct rocketnet_client_transport_config_t defaults;
    rocketnet_client_transport_default_config( &defaults );

    {
        alignas( 8 ) uint8_t buffer[sizeof( struct rocketnet_client_transport_config_t ) + 16];
        memset( buffer, 0xCD, sizeof( buffer ) );
        check( rocketnet_config_write_sized( "rocketnet_client_transport_config_t", buffer, older, &defaults, sizeof( defaults ), simulated_known, COUNT( simulated_known ) ) == 1 );
        check( memcmp( buffer, &defaults, older ) == 0 );
        for ( size_t i = older; i < sizeof( buffer ); i++ )
            check( buffer[i] == 0xCD );
    }

    {
        struct rocketnet_client_transport_config_t old_caller;
        memset( &old_caller, 0xEE, sizeof( old_caller ) );      // past the old prefix is not the caller's memory
        memcpy( &old_caller, &defaults, older );
        old_caller.max_payload_bytes = 4096;
        old_caller.max_fragmented_messages = 3;

        struct rocketnet_client_transport_config_t read;
        memset( &read, 0, sizeof( read ) );
        check( rocketnet_config_read_sized( "rocketnet_client_transport_config_t", &read, &defaults, sizeof( defaults ), &old_caller, older, simulated_known, COUNT( simulated_known ) ) == 1 );
        check( read.max_payload_bytes == 4096 );
        check( read.max_fragmented_messages == 3 );
        check( memcmp( read.packet_filter, defaults.packet_filter, sizeof( read.packet_filter ) ) == 0 );
        check( strcmp( read.transport, defaults.transport ) == 0 );
        check( read.state_change_callback == NULL );
        check( read.time_callback == NULL );
    }

    // the current size reads the whole struct

    {
        struct rocketnet_client_transport_config_t caller = defaults;
        caller.max_fragmented_messages = 7;
        struct rocketnet_client_transport_config_t read;
        memset( &read, 0, sizeof( read ) );
        check( rocketnet_config_read_sized( "rocketnet_client_transport_config_t", &read, &defaults, sizeof( defaults ), &caller, sizeof( caller ), simulated_known, COUNT( simulated_known ) ) == 1 );
        check( memcmp( &read, &caller, sizeof( read ) ) == 0 );
    }

    // These size-acceptance checks pass NULL tokens or addresses, so they
    // exercise the create guards without opening sockets. The plain-create
    // simulation regression below uses an in-memory loopback connection.

    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( sized_config_quiet_assert );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config( &server_config );
    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );

    for ( int i = 0; i < COUNT( client_known_bytes ); i++ )
    {
        uint64_t before = rocketnet_api_misuse_count();
        check( rocketnet_client_transport_create_sized( NULL, NULL, &defaults, client_known_bytes[i], 1000.0 ) == NULL );
        check( rocketnet_api_misuse_count() == before + 1 );
    }

    for ( int i = 0; i < COUNT( server_known_bytes ); i++ )
    {
        uint64_t before = rocketnet_api_misuse_count();
        check( rocketnet_server_transport_create_sized( NULL, NULL, &server_config, server_known_bytes[i] ) == NULL );
        check( rocketnet_api_misuse_count() == before + 1 );
    }

    for ( int i = 0; i < COUNT( message_known_bytes ); i++ )
    {
        uint64_t before = rocketnet_api_misuse_count();
        check( rocketnet_message_client_create_sized( NULL, NULL, &defaults, sizeof( defaults ), &message_config, message_known_bytes[i], NULL, 0, 0 ) == NULL );
        check( rocketnet_api_misuse_count() == before + 1 );

        before = rocketnet_api_misuse_count();
        check( rocketnet_message_server_create_sized( NULL, NULL, &server_config, sizeof( server_config ), &message_config, message_known_bytes[i], NULL, 0, 0 ) == NULL );
        check( rocketnet_api_misuse_count() == before + 1 );
    }

    // NULL configs are the defaults, whatever size rides with them, as unsized

    {
        uint64_t before = rocketnet_api_misuse_count();
        check( rocketnet_client_transport_create_sized( NULL, NULL, NULL, 0, 1000.0 ) == NULL );
        check( rocketnet_server_transport_create_sized( NULL, NULL, NULL, 0 ) == NULL );
        check( rocketnet_message_client_create_sized( NULL, NULL, NULL, 0, NULL, 0, NULL, 0, 0 ) == NULL );
        check( rocketnet_message_server_create_sized( NULL, NULL, NULL, 0, NULL, 0, NULL, 0, 0 ) == NULL );
        check( rocketnet_api_misuse_count() == before + 4 );
    }

    rocketnet_assert_function( saved_assert );
}

// R6 (docs/CHANNELS.md 11.5) at the ABI boundary (docs/ABI-EVOLUTION.md 4):
// a revision-1 caller's struct ends at reliable_message_timeout_ms, so reading it
// yields the two send budget fields at their defaults, never the bytes past
// the caller's prefix.
static void test_sized_config_message_revision_1_reads_budget_defaults()
{
    check( ROCKETNET_MESSAGE_CONFIG_BYTES_1 + 2 * sizeof( int ) == sizeof( struct rocketnet_message_config_t ) );

    struct rocketnet_message_config_t defaults;
    rocketnet_message_default_config( &defaults );
    check( defaults.client_send_kbps == 1000 );
    check( defaults.server_send_kbps == 10000 );

    struct rocketnet_message_config_t old_caller;
    memset( &old_caller, 0xEE, sizeof( old_caller ) );          // past the old prefix is not the caller's memory
    memcpy( &old_caller, &defaults, ROCKETNET_MESSAGE_CONFIG_BYTES_1 );
    old_caller.reliable_message_timeout_ms = 5000;

    struct rocketnet_message_config_t read;
    memset( &read, 0xAB, sizeof( read ) );
    check( rocketnet_config_read_sized( "rocketnet_message_config_t", &read, &defaults, sizeof( defaults ), &old_caller, ROCKETNET_MESSAGE_CONFIG_BYTES_1, message_known_bytes, COUNT( message_known_bytes ) ) == 1 );
    check( read.reliable_message_timeout_ms == 5000 );
    check( read.client_send_kbps == 1000 );
    check( read.server_send_kbps == 10000 );
}

static void test_sized_config_refuses_unknown_size()
{
    // a caller compiled against a newer, larger struct, an older size this
    // library never had, and nonsense: refused, and nothing written

    const size_t client_current = sizeof( struct rocketnet_client_transport_config_t );
    const size_t server_current = sizeof( struct rocketnet_server_transport_config_t );
    const size_t message_current = sizeof( struct rocketnet_message_config_t );

    // the end of state_change_callback was never a revision: time_callback is part of revision 1 (docs/ABI-EVOLUTION.md 4)
    const size_t client_unknown[] = { 0, 1, client_current - 1, offsetof( struct rocketnet_client_transport_config_t, time_callback ), client_current + 1, client_current + 8, SIZE_MAX };
    const size_t server_unknown[] = { 0, 1, server_current - 1, server_current + 1, server_current + 8, SIZE_MAX };
    const size_t message_unknown[] = { 0, 1, message_current - 1, message_current + 1, message_current + 8, SIZE_MAX };

    for ( int i = 0; i < COUNT( client_unknown ); i++ )
    {
        alignas( 8 ) uint8_t buffer[sizeof( struct rocketnet_client_transport_config_t ) + 16];
        memset( buffer, 0xCD, sizeof( buffer ) );
        check( rocketnet_client_transport_default_config_sized( (struct rocketnet_client_transport_config_t*) buffer, client_unknown[i] ) == 0 );
        for ( size_t j = 0; j < sizeof( buffer ); j++ )
            check( buffer[j] == 0xCD );
    }

    for ( int i = 0; i < COUNT( server_unknown ); i++ )
    {
        alignas( 8 ) uint8_t buffer[sizeof( struct rocketnet_server_transport_config_t ) + 16];
        memset( buffer, 0xCD, sizeof( buffer ) );
        check( rocketnet_server_transport_default_config_sized( (struct rocketnet_server_transport_config_t*) buffer, server_unknown[i] ) == 0 );
        for ( size_t j = 0; j < sizeof( buffer ); j++ )
            check( buffer[j] == 0xCD );
    }

    for ( int i = 0; i < COUNT( message_unknown ); i++ )
    {
        alignas( 8 ) uint8_t buffer[sizeof( struct rocketnet_message_config_t ) + 16];
        memset( buffer, 0xCD, sizeof( buffer ) );
        check( rocketnet_message_default_config_sized( (struct rocketnet_message_config_t*) buffer, message_unknown[i] ) == 0 );
        for ( size_t j = 0; j < sizeof( buffer ); j++ )
            check( buffer[j] == 0xCD );
    }

    // the read refuses a simulated table's unknown sizes and leaves the
    // defaults in place

    struct rocketnet_client_transport_config_t defaults;
    rocketnet_client_transport_default_config( &defaults );
    const size_t older = offsetof( struct rocketnet_client_transport_config_t, transport );
    const size_t simulated_known[] = { older, client_current };
    {
        struct rocketnet_client_transport_config_t caller = defaults;
        caller.max_fragmented_messages = 9;
        const size_t refused[] = { older - 1, older + 1, client_current + 8 };
        for ( int i = 0; i < COUNT( refused ); i++ )
        {
            struct rocketnet_client_transport_config_t read;
            memset( &read, 0, sizeof( read ) );
            check( rocketnet_config_read_sized( "rocketnet_client_transport_config_t", &read, &defaults, sizeof( defaults ), &caller, refused[i], simulated_known, COUNT( simulated_known ) ) == 0 );
            check( memcmp( &read, &defaults, sizeof( read ) ) == 0 );
        }
    }

    // the creates refuse an unknown size before any other check: uncounted,
    // never reaching the NULL token or address guard

    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( sized_config_quiet_assert );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config( &server_config );
    struct rocketnet_message_config_t message_config;
    rocketnet_message_default_config( &message_config );

    uint64_t before = rocketnet_api_misuse_count();
    for ( int i = 0; i < COUNT( client_unknown ); i++ )
    {
        check( rocketnet_client_transport_create_sized( NULL, NULL, &defaults, client_unknown[i], 1000.0 ) == NULL );
        check( rocketnet_message_client_create_sized( NULL, NULL, &defaults, client_unknown[i], &message_config, message_current, NULL, 0, 0 ) == NULL );
    }
    for ( int i = 0; i < COUNT( server_unknown ); i++ )
    {
        check( rocketnet_server_transport_create_sized( NULL, NULL, &server_config, server_unknown[i] ) == NULL );
        check( rocketnet_message_server_create_sized( NULL, NULL, &server_config, server_unknown[i], &message_config, message_current, NULL, 0, 0 ) == NULL );
    }
    for ( int i = 0; i < COUNT( message_unknown ); i++ )
    {
        check( rocketnet_message_client_create_sized( NULL, NULL, &defaults, client_current, &message_config, message_unknown[i], NULL, 0, 0 ) == NULL );
        check( rocketnet_message_server_create_sized( NULL, NULL, &server_config, server_current, &message_config, message_unknown[i], NULL, 0, 0 ) == NULL );
    }
    check( rocketnet_api_misuse_count() == before );

    // a NULL config to the sized defaults is a counted no-op that returns 0

    before = rocketnet_api_misuse_count();
    check( rocketnet_client_transport_default_config_sized( NULL, client_current ) == 0 );
    check( rocketnet_server_transport_default_config_sized( NULL, server_current ) == 0 );
    check( rocketnet_message_default_config_sized( NULL, message_current ) == 0 );
    check( rocketnet_api_misuse_count() == before + 3 );

    rocketnet_assert_function( saved_assert );
}

static void test_server_default_config_max_clients_is_1000()
{
    struct rocketnet_server_transport_config_t unsized, sized;
    rocketnet_server_transport_default_config( &unsized );
    memset( &sized, 0xA5, sizeof( sized ) );
    check( rocketnet_server_transport_default_config_sized( &sized, sizeof( sized ) ) == 1 );
    check( unsized.max_clients == 1000 );
    check( sized.max_clients == 1000 );
}

// The public creates run one validation per side (rn_client_config_valid
// and rn_server_config_valid in sdk/rocketnet_api.cpp): create is the one
// check, accepting a config with status OK and refusing one with NULL and a
// refusal status. A create before rocketnet_init returns NULL and is counted
// once, as the transport creates count it. Every case runs in memory: the
// creates open no socket and read no clock.

#define SIZED_CONFIG_CLIENT_CREATE( mutation, accepted )                                                        \
do                                                                                                              \
{                                                                                                               \
    struct rocketnet_client_config_t c;                                                                         \
    check( rocketnet_client_default_config( &c ) == ROCKETNET_STATUS_OK );                                      \
    mutation;                                                                                                   \
    enum rocketnet_status_t made_status = ROCKETNET_TEST_STATUS_UNSET;                                               \
    struct rocketnet_client_t * made = rocketnet_client_create( NULL, &c, &made_status );                       \
    check( ( made != NULL ) == (accepted) );                                                                    \
    check( (accepted) ? made_status == ROCKETNET_STATUS_OK                                                      \
                      : ( made_status != ROCKETNET_STATUS_OK && made_status != ROCKETNET_TEST_STATUS_UNSET ) );      \
    if ( made )                                                                                                 \
        rocketnet_client_destroy( made );                                                                       \
} while ( 0 )

#define SIZED_CONFIG_SERVER_CREATE( mutation, accepted )                                                        \
do                                                                                                              \
{                                                                                                               \
    struct rocketnet_server_config_t c;                                                                         \
    sized_config_dev_server_config( &c );                                                                       \
    c.max_clients = 4;                                                                                          \
    mutation;                                                                                                   \
    enum rocketnet_status_t made_status = ROCKETNET_TEST_STATUS_UNSET;                                               \
    struct rocketnet_server_t * made = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &c, &made_status );        \
    check( ( made != NULL ) == (accepted) );                                                                    \
    check( (accepted) ? made_status == ROCKETNET_STATUS_OK                                                      \
                      : ( made_status != ROCKETNET_STATUS_OK && made_status != ROCKETNET_TEST_STATUS_UNSET ) );      \
    if ( made )                                                                                                 \
        rocketnet_server_destroy( made );                                                                       \
} while ( 0 )

static void test_sized_config_create_refuses_before_init()
{
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( sized_config_quiet_assert );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( !rocketnet_library_initialized() );

    // default_config stays safe before init: it creates nothing
    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    sized_config_dev_server_config( &server_config );
    server_config.max_clients = 4;

    uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_client_create( NULL, NULL, NULL ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    before = rocketnet_api_misuse_count();
    check( rocketnet_client_create( NULL, &client_config, NULL ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    before = rocketnet_api_misuse_count();
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, NULL, NULL ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    before = rocketnet_api_misuse_count();
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    // after init the same calls create
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL );
    check( client != NULL );
    rocketnet_client_destroy( client );
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, NULL );
    check( server != NULL );
    rocketnet_server_destroy( server );

    rocketnet_assert_function( saved_assert );
}

static void test_sized_config_client_create_checks_the_config()
{
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( sized_config_quiet_assert );

    // accepted
    SIZED_CONFIG_CLIENT_CREATE( (void) 0, true );
    SIZED_CONFIG_CLIENT_CREATE( c.transport = "portable", true );
    SIZED_CONFIG_CLIENT_CREATE( c.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, true );
    SIZED_CONFIG_CLIENT_CREATE( c.max_fragmented_messages = ROCKETNET_MAX_FRAGMENTS, true );

    // transport fields out of range: refused
    SIZED_CONFIG_CLIENT_CREATE( c.max_payload_bytes = -1, false );
    SIZED_CONFIG_CLIENT_CREATE( c.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, false );
    SIZED_CONFIG_CLIENT_CREATE( c.max_fragmented_messages = -1, false );
    SIZED_CONFIG_CLIENT_CREATE( c.max_fragmented_messages = ROCKETNET_MAX_FRAGMENTS + 1, false );

    // a transport list that does not parse: refused
    SIZED_CONFIG_CLIENT_CREATE( c.transport = "bogus", false );
    SIZED_CONFIG_CLIENT_CREATE( c.transport = "auto,portable", false );

    rocketnet_assert_function( saved_assert );
}

static void test_sized_config_server_create_checks_the_config()
{
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_assert_function( sized_config_quiet_assert );

    // accepted
    SIZED_CONFIG_SERVER_CREATE( (void) 0, true );
    SIZED_CONFIG_SERVER_CREATE( c.transport = "portable", true );
    SIZED_CONFIG_SERVER_CREATE( c.max_timeout_seconds = 3600, true );
    SIZED_CONFIG_SERVER_CREATE( c.handshake_deadline_seconds = 60, true );

    // transport fields out of range: refused
    SIZED_CONFIG_SERVER_CREATE( c.max_clients = 0, false );
    SIZED_CONFIG_SERVER_CREATE( c.max_clients = ROCKETNET_MAX_CLIENTS + 1, false );
    SIZED_CONFIG_SERVER_CREATE( c.num_xdp_queues = -1, false );
    SIZED_CONFIG_SERVER_CREATE( c.num_xdp_queues = 1025, false );
    SIZED_CONFIG_SERVER_CREATE( c.num_os_cpus = -1, false );
    SIZED_CONFIG_SERVER_CREATE( c.num_os_cpus = 257, false );
    SIZED_CONFIG_SERVER_CREATE( c.max_payload_bytes = -1, false );
    SIZED_CONFIG_SERVER_CREATE( c.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, false );
    SIZED_CONFIG_SERVER_CREATE( c.max_fragmented_messages = -1, false );
    SIZED_CONFIG_SERVER_CREATE( c.max_fragmented_messages = ROCKETNET_MAX_FRAGMENTS + 1, false );
    SIZED_CONFIG_SERVER_CREATE( c.max_timeout_seconds = 0, false );
    SIZED_CONFIG_SERVER_CREATE( c.max_timeout_seconds = 3601, false );
    SIZED_CONFIG_SERVER_CREATE( c.handshake_deadline_seconds = 0, false );
    SIZED_CONFIG_SERVER_CREATE( c.handshake_deadline_seconds = 61, false );

    // a transport list that does not parse: refused
    SIZED_CONFIG_SERVER_CREATE( c.transport = "bogus", false );
    SIZED_CONFIG_SERVER_CREATE( c.transport = "auto,portable", false );

    rocketnet_assert_function( saved_assert );
}

// ---- config_bytes: the public configs' size, carried in the struct ----------

// default_config sets config_bytes to the caller's sizeof; a config_bytes
// below the field itself is refused by name at create and sizes nothing
static void test_public_config_bytes_zeroed_is_refused()
{
    struct rocketnet_client_config_t client_config;
    memset( &client_config, 0xAB, sizeof( client_config ) );
    check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );
    check( client_config.config_bytes == sizeof( client_config ) );
    struct rocketnet_server_config_t server_config;
    memset( &server_config, 0xAB, sizeof( server_config ) );
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    check( server_config.config_bytes == sizeof( server_config ) );

    memset( &client_config, 0, sizeof( client_config ) );
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    check( rocketnet_client_required_memory( &client_config ) == 0 );

    sized_config_dev_server_config( &server_config );
    server_config.max_clients = 4;
    server_config.config_bytes = 0;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    check( rocketnet_server_required_memory( &server_config ) == 0 );
    memset( &server_config, 0, sizeof( server_config ) );
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED );
    check( rocketnet_server_required_memory( &server_config ) == 0 );

    // config_bytes exactly the field: nothing else is the caller's, every
    // other field takes its default
    memset( &client_config, 0, sizeof( client_config ) );
    client_config.config_bytes = sizeof( size_t );
    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
    struct rocketnet_client_config_t defaults;
    check( rocketnet_client_default_config( &defaults ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_required_memory( &client_config ) == rocketnet_client_required_memory( &defaults ) );
}

// an older caller, compiled against a struct that ended before num_channels:
// the default writes only its prefix, and create reads only its prefix, so
// the bytes past it (not the caller's memory) are never read or written
static void test_public_config_bytes_older_smaller_reads_the_prefix()
{
    const size_t client_older = offsetof( struct rocketnet_client_config_t, num_channels );
    const size_t server_older = offsetof( struct rocketnet_server_config_t, num_channels );

    struct rocketnet_client_config_t client_config;
    memset( &client_config, 0xCD, sizeof( client_config ) );
    client_config.config_bytes = client_older;
    check( rocketnet_client_default_config_internal( &client_config ) == ROCKETNET_STATUS_OK );
    check( client_config.config_bytes == client_older );
    const uint8_t * client_bytes = (const uint8_t *) &client_config;
    for ( size_t i = client_older; i < sizeof( client_config ); i++ )
        check( client_bytes[i] == 0xCD );
    struct rocketnet_client_config_t client_defaults;
    check( rocketnet_client_default_config( &client_defaults ) == ROCKETNET_STATUS_OK );
    check( memcmp( client_bytes + sizeof( size_t ), (const uint8_t *) &client_defaults + sizeof( size_t ), client_older - sizeof( size_t ) ) == 0 );

    // past the prefix, a value create would refuse if it read it
    client_config.num_channels = 0;
    client_config.reliable_message_timeout_ms = -1;
    client_config.transport = "loopback";
    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
    client_defaults.transport = "loopback";
    check( rocketnet_client_required_memory( &client_config ) == rocketnet_client_required_memory( &client_defaults ) );

    // a prefix value is read: an out-of-range one in it is refused
    client_config.max_payload_bytes = -1;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &client_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    struct rocketnet_server_config_t server_config;
    memset( &server_config, 0xCD, sizeof( server_config ) );
    server_config.config_bytes = server_older;
    check( rocketnet_server_default_config_internal( &server_config ) == ROCKETNET_STATUS_OK );
    check( server_config.config_bytes == server_older );
    const uint8_t * server_bytes = (const uint8_t *) &server_config;
    for ( size_t i = server_older; i < sizeof( server_config ); i++ )
        check( server_bytes[i] == 0xCD );
    server_config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    server_config.max_clients = 4;
    server_config.num_channels = 0;
    server_config.reliable_message_timeout_ms = -1;
    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );
    struct rocketnet_server_config_t server_defaults;
    sized_config_dev_server_config( &server_defaults );
    server_defaults.max_clients = 4;
    check( rocketnet_server_required_memory( &server_config ) == rocketnet_server_required_memory( &server_defaults ) );

    server_config.max_clients = 0;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
}

// a newer caller, compiled against a larger struct: read as far as the
// library knows, the tail ignored and never written
struct sized_config_newer_client_t
{
    struct rocketnet_client_config_t config;
    int tail_field[4];
};

struct sized_config_newer_server_t
{
    struct rocketnet_server_config_t config;
    int tail_field[4];
};

static void test_public_config_bytes_newer_larger_ignores_the_tail()
{
    struct sized_config_newer_client_t newer_client;
    memset( &newer_client, 0xEE, sizeof( newer_client ) );
    newer_client.config.config_bytes = sizeof( newer_client );
    check( rocketnet_client_default_config_internal( &newer_client.config ) == ROCKETNET_STATUS_OK );
    check( newer_client.config.config_bytes == sizeof( newer_client ) );
    for ( int i = 0; i < 4; i++ )
        check( newer_client.tail_field[i] == (int) 0xEEEEEEEE );
    struct rocketnet_client_config_t client_defaults;
    check( rocketnet_client_default_config( &client_defaults ) == ROCKETNET_STATUS_OK );
    check( memcmp( (const uint8_t *) &newer_client.config + sizeof( size_t ), (const uint8_t *) &client_defaults + sizeof( size_t ),
                   sizeof( client_defaults ) - sizeof( size_t ) ) == 0 );

    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &newer_client.config, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
    check( rocketnet_client_required_memory( &newer_client.config ) == rocketnet_client_required_memory( &client_defaults ) );
    for ( int i = 0; i < 4; i++ )
        check( newer_client.tail_field[i] == (int) 0xEEEEEEEE );

    // the known fields are still read: an out-of-range one is refused
    newer_client.config.num_channels = 0;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_client_create( NULL, &newer_client.config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    struct sized_config_newer_server_t newer_server;
    memset( &newer_server, 0xEE, sizeof( newer_server ) );
    newer_server.config.config_bytes = sizeof( newer_server );
    check( rocketnet_server_default_config_internal( &newer_server.config ) == ROCKETNET_STATUS_OK );
    check( newer_server.config.config_bytes == sizeof( newer_server ) );
    for ( int i = 0; i < 4; i++ )
        check( newer_server.tail_field[i] == (int) 0xEEEEEEEE );
    newer_server.config.protocol_id = ROCKETNET_DEV_PROTOCOL_ID;
    memcpy( newer_server.config.backend_public_key, rocketnet_dev_backend_public_key, 32 );
    newer_server.config.max_clients = 4;
    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &newer_server.config, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );
    struct rocketnet_server_config_t server_defaults;
    sized_config_dev_server_config( &server_defaults );
    server_defaults.max_clients = 4;
    check( rocketnet_server_required_memory( &newer_server.config ) == rocketnet_server_required_memory( &server_defaults ) );

    newer_server.config.max_clients = 0;
    status = ROCKETNET_STATUS_OK;
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &newer_server.config, &status ) == NULL );
    check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
}

// The public client and server configs have one revision, the whole struct
// (docs/ABI-EVOLUTION.md section 4): the plain default fills every field, and
// the plain create reads every field. The network simulator is a call, not a
// field. Every case runs in memory: the loopback transport, step driven, time
// from 1000.0, no socket and no clock.

#define PUBLIC_CONFIG_PROTOCOL 0x0C0F16A11FEDULL

struct public_config_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[sizeof( rocketnet_dev_server_secret_key )];
};

static double public_config_now = 1000.0;
static double public_config_clock( void * ) { return public_config_now; }

static void public_config_identity( public_config_identity_t * identity )
{
    uint8_t seed[32];
    for ( int i = 0; i < 32; i++ )
        seed[i] = (uint8_t) ( 0x3C ^ i );
    check( rocketnet_generate_backend_keypair_from_seed( seed, identity->backend_public, identity->backend_secret ) == ROCKETNET_STATUS_OK );
    check( rocketnet_generate_server_keypair( identity->server_public, identity->server_secret ) == ROCKETNET_STATUS_OK );
}

static void public_config_server( struct rocketnet_server_config_t * config, const public_config_identity_t * identity )
{
    check( rocketnet_server_default_config( config ) == ROCKETNET_STATUS_OK );
    config->protocol_id = PUBLIC_CONFIG_PROTOCOL;
    config->max_clients = 1;
    memcpy( config->backend_public_key, identity->backend_public, sizeof( config->backend_public_key ) );
    config->transport = "loopback";
}

static void public_config_client( struct rocketnet_client_test_config_t * config )
{
    check( rocketnet_client_default_config( config ) == ROCKETNET_STATUS_OK );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = public_config_clock;
}

static void public_config_token( const struct rocketnet_server_t * server, const public_config_identity_t * identity, char * token_string )
{
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) rocketnet_server_port( server ) );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, server_public,
        PUBLIC_CONFIG_PROTOCOL, addresses, 1, 0xC0F1, 1000, 1ULL << 40, 300, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
    check( rocketnet_base64_encode_data( token, sizeof( token ), token_string, ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES ) == ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );
}

// pumps a loopback pair for four simulated seconds; returns whether the
// client reached CONNECTED
static bool public_config_pair_connects( int server_loss_percent )
{
    public_config_identity_t identity;
    public_config_identity( &identity );

    struct rocketnet_server_config_t server_config;
    public_config_server( &server_config, &identity );
    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &create_status );
    check( server != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    // above 100: set 100, then try the out-of-range value over it
    rocketnet_server_network_simulator( server, 0, 0, server_loss_percent > 100 ? 100 : server_loss_percent, 0 );
    if ( server_loss_percent > 100 )
        rocketnet_server_network_simulator( server, 0, 0, server_loss_percent, 0 );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    public_config_token( server, &identity, token );

    struct rocketnet_client_test_config_t client_config;
    public_config_client( &client_config );
    create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &create_status );
    check( client != NULL );
    check( create_status == ROCKETNET_STATUS_OK );
    check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    double time = 1000.0;
    for ( int i = 0; i < 400 && rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED; i++ )
    {
        public_config_now = time;
        rocketnet_client_update( client, time );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, time );
        rocketnet_server_send_packets( server );
        time += 0.01;
    }
    const bool connected = rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED;
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    return connected;
}

static void test_network_simulator_applies_to_outgoing_packets()
{
    rocketnet_quiet( 1 );
    const bool development = rocketnet_public_link_simulator_development();

#if !ROCKETNET_DEVELOPMENT
    // a production posture has no network simulator: the call is counted
    // misuse and changes nothing, so the pair connects under "100% loss"
    rocketnet_public_link_simulator_test_development( 0 );
    {
        const uint64_t misuse = rocketnet_api_misuse_count();
        check( public_config_pair_connects( 100 ) );
        check( rocketnet_api_misuse_count() == misuse + 1 );
    }
#endif // !ROCKETNET_DEVELOPMENT

    // a development posture honours it: the server's total loss keeps the
    // pair from connecting; none connects
    rocketnet_public_link_simulator_test_development( 1 );
    check( public_config_pair_connects( 0 ) );
    check( !public_config_pair_connects( 100 ) );

    // an out-of-range value is counted misuse and leaves the 100% set before it
    {
        const uint64_t misuse = rocketnet_api_misuse_count();
        check( !public_config_pair_connects( 101 ) );
        check( rocketnet_api_misuse_count() == misuse + 1 );
    }

    rocketnet_public_link_simulator_test_development( development ? 1 : 0 );
    rocketnet_quiet( 0 );
}

static void test_public_config_default_fills_every_field()
{
    // the inline default and its library half at sizeof write the same whole
    // struct, whatever the caller's memory held before

    struct rocketnet_client_config_t client_a, client_b, client_sized;
    memset( &client_a, 0xAB, sizeof( client_a ) );
    memset( &client_b, 0x5C, sizeof( client_b ) );
    memset( &client_sized, 0xE7, sizeof( client_sized ) );
    check( rocketnet_client_default_config( &client_a ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_default_config( &client_b ) == ROCKETNET_STATUS_OK );
    client_sized.config_bytes = sizeof( client_sized );
    check( rocketnet_client_default_config_internal( &client_sized ) == ROCKETNET_STATUS_OK );
    check( memcmp( &client_a, &client_b, sizeof( client_a ) ) == 0 );
    check( memcmp( &client_a, &client_sized, sizeof( client_a ) ) == 0 );
    // the default, read whole by create: a valid struct
    enum rocketnet_status_t status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_a, &status );
    check( client != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );

    struct rocketnet_server_config_t server_a, server_b, server_sized, dev_a, dev_sized;
    memset( &server_a, 0xAB, sizeof( server_a ) );
    memset( &server_b, 0x5C, sizeof( server_b ) );
    memset( &server_sized, 0xE7, sizeof( server_sized ) );
    memset( &dev_a, 0xAB, sizeof( dev_a ) );
    memset( &dev_sized, 0x5C, sizeof( dev_sized ) );
    check( rocketnet_server_default_config( &server_a ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_default_config( &server_b ) == ROCKETNET_STATUS_OK );
    server_sized.config_bytes = sizeof( server_sized );
    check( rocketnet_server_default_config_internal( &server_sized ) == ROCKETNET_STATUS_OK );
    sized_config_dev_server_config( &dev_a );
    sized_config_dev_server_config( &dev_sized );
    check( memcmp( &server_a, &server_b, sizeof( server_a ) ) == 0 );
    check( memcmp( &server_a, &server_sized, sizeof( server_a ) ) == 0 );
    check( memcmp( &dev_a, &dev_sized, sizeof( dev_a ) ) == 0 );
    // the dev keys over the defaults, read whole by create: a valid struct
    dev_a.max_clients = 4;
    status = ROCKETNET_TEST_STATUS_UNSET;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &dev_a, &status );
    check( server != NULL );
    check( status == ROCKETNET_STATUS_OK );
    rocketnet_server_destroy( server );
}


REGISTER_UNIT_TEST( test_network_simulator_applies_to_outgoing_packets );
REGISTER_UNIT_TEST( test_public_config_default_fills_every_field );
REGISTER_UNIT_TEST( test_sized_config_accepts_every_known_size );
REGISTER_UNIT_TEST( test_sized_config_message_revision_1_reads_budget_defaults );
REGISTER_UNIT_TEST( test_sized_config_refuses_unknown_size );
REGISTER_UNIT_TEST( test_server_default_config_max_clients_is_1000 );
REGISTER_UNIT_TEST( test_sized_config_create_refuses_before_init );
REGISTER_UNIT_TEST( test_sized_config_client_create_checks_the_config );
REGISTER_UNIT_TEST( test_sized_config_server_create_checks_the_config );
REGISTER_UNIT_TEST( test_public_config_bytes_zeroed_is_refused );
REGISTER_UNIT_TEST( test_public_config_bytes_older_smaller_reads_the_prefix );
REGISTER_UNIT_TEST( test_public_config_bytes_newer_larger_ignores_the_tail );
