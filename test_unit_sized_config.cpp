/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The sized config entry points (docs/ABI-EVOLUTION.md). Each public config
// struct is passed with its size: a known size is accepted, and a size smaller
// than the current one reads the caller's prefix and defaults the rest; an
// unknown size, larger or smaller, is refused and nothing is written.
//
// Three callers are simulated: one compiled against the current size, one
// compiled against an older revision (a prefix of today's struct, through the
// same read the entry points use, with a simulated table of known sizes), and
// one compiled against a newer, unknown, larger struct.
//
// Every case runs in memory, with no clock, sockets or network: a create that
// accepts its sizes reaches the create's own NULL guard (a counted no-op) and
// stops there; a create that refuses a size stops before it, uncounted.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

static void sized_config_quiet_assert( const char *, const char *, const char *, int ) {}
static double sized_config_time( void * ) { return 1000.0; }

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

static void sized_config_accepts_every_known_size()
{
    sized_config_check_table( client_known_bytes, COUNT( client_known_bytes ), sizeof( struct rocketnet_client_transport_config_t ) );
    sized_config_check_table( server_known_bytes, COUNT( server_known_bytes ), sizeof( struct rocketnet_server_transport_config_t ) );
    sized_config_check_table( message_known_bytes, COUNT( message_known_bytes ), sizeof( struct rocketnet_message_config_t ) );

    // the ABI queries report the same current size the table ends at
    check( rocketnet_abi_type_size( "rocketnet_client_transport_config_t" ) == sizeof( struct rocketnet_client_transport_config_t ) );
    check( rocketnet_abi_type_size( "rocketnet_server_transport_config_t" ) == sizeof( struct rocketnet_server_transport_config_t ) );
    check( rocketnet_abi_type_size( "rocketnet_message_config_t" ) == sizeof( struct rocketnet_message_config_t ) );

    // a caller compiled against the current size: the sized defaults are the
    // unsized defaults, byte for byte, for every known size's prefix

    for ( int i = 0; i < COUNT( client_known_bytes ); i++ )
    {
        struct rocketnet_client_transport_config_t unsized, sized;
        memset( &unsized, 0xAB, sizeof( unsized ) );
        memset( &sized, 0xAB, sizeof( sized ) );
        rocketnet_client_transport_default_config_current( &unsized );
        check( rocketnet_client_transport_default_config_sized( &sized, client_known_bytes[i] ) == 1 );
        check( memcmp( &unsized, &sized, client_known_bytes[i] ) == 0 );
    }

    for ( int i = 0; i < COUNT( server_known_bytes ); i++ )
    {
        struct rocketnet_server_transport_config_t unsized, sized;
        memset( &unsized, 0xAB, sizeof( unsized ) );
        memset( &sized, 0xAB, sizeof( sized ) );
        rocketnet_server_transport_default_config_current( &unsized );
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
    // config that ended before the driver field. the defaults write only the
    // old prefix; the read takes the caller's prefix and defaults the rest.

    const size_t older = offsetof( struct rocketnet_client_transport_config_t, driver );
    const size_t simulated_known[] = { older, sizeof( struct rocketnet_client_transport_config_t ) };

    struct rocketnet_client_transport_config_t defaults;
    rocketnet_client_transport_default_config_current( &defaults );

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
        old_caller.num_reassembly_slots = 3;

        struct rocketnet_client_transport_config_t read;
        memset( &read, 0, sizeof( read ) );
        check( rocketnet_config_read_sized( "rocketnet_client_transport_config_t", &read, &defaults, sizeof( defaults ), &old_caller, older, simulated_known, COUNT( simulated_known ) ) == 1 );
        check( read.max_payload_bytes == 4096 );
        check( read.num_reassembly_slots == 3 );
        check( memcmp( read.filter_value, defaults.filter_value, sizeof( read.filter_value ) ) == 0 );
        check( strcmp( read.driver, defaults.driver ) == 0 );
        check( read.state_change_callback == NULL );
        check( read.time_callback == NULL );
    }

    // the current size reads the whole struct

    {
        struct rocketnet_client_transport_config_t caller = defaults;
        caller.num_reassembly_slots = 7;
        struct rocketnet_client_transport_config_t read;
        memset( &read, 0, sizeof( read ) );
        check( rocketnet_config_read_sized( "rocketnet_client_transport_config_t", &read, &defaults, sizeof( defaults ), &caller, sizeof( caller ), simulated_known, COUNT( simulated_known ) ) == 1 );
        check( memcmp( &read, &caller, sizeof( read ) ) == 0 );
    }

    // the creates accept every known size: each one reaches its own NULL
    // guard (the token or address), a counted no-op, so no socket opens

    void (*saved_assert)( const char *, const char *, const char *, int ) = rocketnet_assert_function_pointer;
    rocketnet_assert_function( sized_config_quiet_assert );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
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
// a revision-1 caller's struct ends at reliable_timeout_ms, so reading it
// yields the two send budget fields at their defaults, never the bytes past
// the caller's prefix.
static void sized_config_message_revision_1_reads_budget_defaults()
{
    check( ROCKETNET_MESSAGE_CONFIG_BYTES_1 + 2 * sizeof( int ) == sizeof( struct rocketnet_message_config_t ) );

    struct rocketnet_message_config_t defaults;
    rocketnet_message_default_config( &defaults );
    check( defaults.client_send_kbps == 1000 );
    check( defaults.server_send_kbps == 10000 );

    struct rocketnet_message_config_t old_caller;
    memset( &old_caller, 0xEE, sizeof( old_caller ) );          // past the old prefix is not the caller's memory
    memcpy( &old_caller, &defaults, ROCKETNET_MESSAGE_CONFIG_BYTES_1 );
    old_caller.reliable_timeout_ms = 5000;

    struct rocketnet_message_config_t read;
    memset( &read, 0xAB, sizeof( read ) );
    check( rocketnet_config_read_sized( "rocketnet_message_config_t", &read, &defaults, sizeof( defaults ), &old_caller, ROCKETNET_MESSAGE_CONFIG_BYTES_1, message_known_bytes, COUNT( message_known_bytes ) ) == 1 );
    check( read.reliable_timeout_ms == 5000 );
    check( read.client_send_kbps == 1000 );
    check( read.server_send_kbps == 10000 );
}

static void sized_config_refuses_unknown_size()
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
    rocketnet_client_transport_default_config_current( &defaults );
    const size_t older = offsetof( struct rocketnet_client_transport_config_t, driver );
    const size_t simulated_known[] = { older, client_current };
    {
        struct rocketnet_client_transport_config_t caller = defaults;
        caller.num_reassembly_slots = 9;
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

    void (*saved_assert)( const char *, const char *, const char *, int ) = rocketnet_assert_function_pointer;
    rocketnet_assert_function( sized_config_quiet_assert );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_default_config_current( &server_config );
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

static void server_default_config_max_clients_is_1000()
{
    struct rocketnet_server_transport_config_t unsized, sized;
    rocketnet_server_transport_default_config( &unsized );
    memset( &sized, 0xA5, sizeof( sized ) );
    check( rocketnet_server_transport_default_config_sized( &sized, sizeof( sized ) ) == 1 );
    check( unsized.max_clients == 1000 );
    check( sized.max_clients == 1000 );
}

REGISTER_UNIT_TEST( sized_config_accepts_every_known_size );
REGISTER_UNIT_TEST( sized_config_message_revision_1_reads_budget_defaults );
REGISTER_UNIT_TEST( sized_config_refuses_unknown_size );
REGISTER_UNIT_TEST( server_default_config_max_clients_is_1000 );


static void sized_public_config_revisions_and_simulator_fields()
{
    alignas(16) uint8_t client_buffer[ROCKETNET_CLIENT_CONFIG_BYTES_1 + 16];
    memset( client_buffer, 0xa5, sizeof(client_buffer) );
    check( rocketnet_client_default_config_sized( (rocketnet_client_config_t*) client_buffer,
                                                   ROCKETNET_CLIENT_CONFIG_BYTES_1 ) == 1 );
    for ( size_t i = ROCKETNET_CLIENT_CONFIG_BYTES_1; i < sizeof(client_buffer); i++ )
        check( client_buffer[i] == 0xa5 );
    memset( client_buffer, 0xa5, sizeof(client_buffer) );
    rocketnet_client_default_config( (rocketnet_client_config_t*) client_buffer );
    for ( size_t i = ROCKETNET_CLIENT_CONFIG_BYTES_1; i < sizeof(client_buffer); i++ )
        check( client_buffer[i] == 0xa5 );

    alignas(16) uint8_t server_buffer[ROCKETNET_SERVER_CONFIG_BYTES_1 + 16];
    memset( server_buffer, 0x5a, sizeof(server_buffer) );
    check( rocketnet_server_default_config_sized( (rocketnet_server_config_t*) server_buffer,
                                                   ROCKETNET_SERVER_CONFIG_BYTES_1 ) == 1 );
    for ( size_t i = ROCKETNET_SERVER_CONFIG_BYTES_1; i < sizeof(server_buffer); i++ )
        check( server_buffer[i] == 0x5a );
    check( rocketnet_server_dev_config_sized( (rocketnet_server_config_t*) server_buffer,
                                               ROCKETNET_SERVER_CONFIG_BYTES_1 ) == 1 );
    for ( size_t i = ROCKETNET_SERVER_CONFIG_BYTES_1; i < sizeof(server_buffer); i++ )
        check( server_buffer[i] == 0x5a );
    memset( server_buffer, 0x5a, sizeof(server_buffer) );
    rocketnet_server_default_config( (rocketnet_server_config_t*) server_buffer );
    for ( size_t i = ROCKETNET_SERVER_CONFIG_BYTES_1; i < sizeof(server_buffer); i++ )
        check( server_buffer[i] == 0x5a );
    memset( server_buffer, 0x5a, sizeof(server_buffer) );
    rocketnet_server_dev_config( (rocketnet_server_config_t*) server_buffer );
    for ( size_t i = ROCKETNET_SERVER_CONFIG_BYTES_1; i < sizeof(server_buffer); i++ )
        check( server_buffer[i] == 0x5a );

    rocketnet_client_config_t client;
    check( rocketnet_client_default_config_sized( &client, sizeof(client) ) == 1 );
    client.simulated_loss_percent = 17;
    client.simulated_latency_ms = 23;
    client.simulated_jitter_ms = 29;
    client.simulated_duplicate_percent = 31;
    rocketnet_client_transport_config_t client_transport;
    rocketnet_message_config_t client_message;
    uint32_t client_kbps = 0;
    check( rocketnet_client_config_resolve( &client, false, &client_transport, &client_message, &client_kbps ) );
    check( client_transport.simulated_loss_percent == 17 );
    check( client_transport.simulated_latency_ms == 23 );
    check( client_transport.simulated_jitter_ms == 29 );
    check( client_transport.simulated_duplicate_percent == 31 );

    rocketnet_server_config_t server;
    check( rocketnet_server_dev_config_sized( &server, sizeof(server) ) == 1 );
    server.simulated_loss_percent = 37;
    server.simulated_latency_ms = 41;
    server.simulated_jitter_ms = 43;
    server.simulated_duplicate_percent = 47;
    rocketnet_server_transport_config_t server_transport;
    rocketnet_message_config_t server_message;
    uint32_t server_kbps = 0;
    check( rocketnet_server_config_resolve( &server, false, &server_transport, &server_message, &server_kbps ) );
    check( server_transport.simulated_loss_percent == 37 );
    check( server_transport.simulated_latency_ms == 41 );
    check( server_transport.simulated_jitter_ms == 43 );
    check( server_transport.simulated_duplicate_percent == 47 );

    // The high-level current-sized entry points consume these exact configs.
    check( rocketnet_client_validate_config_sized( &client, sizeof(client) ) == 1 );
    check( rocketnet_client_required_memory_sized( &client, sizeof(client) ) > 0 );
    check( rocketnet_server_validate_config_sized( &server, sizeof(server) ) == 1 );
    check( rocketnet_server_required_memory_sized( &server, sizeof(server) ) > 0 );
#if ROCKETNET_DEVELOPMENT
    client.transport = "loopback";
    client.time_callback = sized_config_time;
    server.transport = "loopback";
    struct rocketnet_server_t * public_server = rocketnet_server_create_sized( NULL, "127.0.0.1:0", &server, sizeof(server), NULL, 0 );
    check( public_server );
    char token_address[64];
    snprintf( token_address, sizeof(token_address), "127.0.0.1:%u", (unsigned) rocketnet_server_port( public_server ) );
    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_dev_connect_token( token_address, 1, 0, token ) == 1 );
    struct rocketnet_client_t * public_client = rocketnet_client_create_sized( NULL, token, &client, sizeof(client), NULL, 0 );
    check( public_client );
    rocketnet_client_destroy( public_client );
    rocketnet_server_destroy( public_server );
#endif

    const char * names[] = { "simulated_loss_percent", "simulated_latency_ms", "simulated_jitter_ms", "simulated_duplicate_percent" };
    for ( int type = 0; type < 2; type++ )
    {
        const char * type_name = type == 0 ? "rocketnet_client_config_t" : "rocketnet_server_config_t";
        for ( int n = 0; n < 4; n++ )
        {
            const size_t offset = rocketnet_abi_field_offset( type_name, names[n] );
            check( offset != SIZE_MAX );
            check( rocketnet_abi_field_size( type_name, names[n] ) == sizeof(int) );
        }
    }
}

REGISTER_UNIT_TEST( sized_public_config_revisions_and_simulator_fields );
