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
// Plain entry points use the current config implementation
// (sdk/rocketnet_api.cpp:498, :1206). The regressions below check full defaults
// and simulation fields, including a real in-memory loopback create/connect.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_fragment.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

static void sized_config_quiet_assert( const char *, const char *, const char *, int ) {}

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
    // config that ended before the driver field. the defaults write only the
    // old prefix; the read takes the caller's prefix and defaults the rest.

    const size_t older = offsetof( struct rocketnet_client_transport_config_t, driver );
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

    // These size-acceptance checks pass NULL tokens or addresses, so they
    // exercise the create guards without opening sockets. The plain-create
    // simulation regression below uses an in-memory loopback connection.

    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( sized_config_quiet_assert );

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

    rocketnet_set_assert_callback( saved_assert );
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
    rocketnet_client_transport_default_config( &defaults );
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

    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( sized_config_quiet_assert );

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

    rocketnet_set_assert_callback( saved_assert );
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

// The public creates and validate_config run one validation per side
// (rn_client_config_valid and rn_server_config_valid, sdk/rocketnet_api.cpp:380
// and :1047):
// for the config's own values, validate_config returns OK exactly when create
// accepts, and a refused config is refused by both. A create before
// rocketnet_init returns NULL and is counted once, as the transport creates
// count it. Every case runs in memory: the creates open no socket and read no
// clock, and validate_config reads no environment.

#define SIZED_CONFIG_CLIENT_AGREES( mutation, accepted )                                                        \
do                                                                                                              \
{                                                                                                               \
    struct rocketnet_client_config_t c;                                                                         \
    check( rocketnet_client_default_config_sized( &c, sizeof( c ) ) == ROCKETNET_STATUS_OK );                   \
    mutation;                                                                                                   \
    check( rocketnet_client_validate_config_sized( &c, sizeof( c ) ) == ( (accepted) ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE ) ); \
    struct rocketnet_client_t * made = rocketnet_client_create_sized( NULL, &c, sizeof( c ), NULL, 0 );         \
    check( ( made != NULL ) == (accepted) );                                                                    \
    if ( made )                                                                                                 \
        rocketnet_client_destroy( made );                                                                       \
} while ( 0 )

#define SIZED_CONFIG_SERVER_AGREES( mutation, accepted )                                                        \
do                                                                                                              \
{                                                                                                               \
    struct rocketnet_server_config_t c;                                                                         \
    check( rocketnet_server_dev_config_sized( &c, sizeof( c ) ) == ROCKETNET_STATUS_OK );                       \
    c.max_clients = 4;                                                                                          \
    mutation;                                                                                                   \
    check( rocketnet_server_validate_config_sized( &c, sizeof( c ) ) == ( (accepted) ? ROCKETNET_STATUS_OK : ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE ) ); \
    struct rocketnet_server_t * made = rocketnet_server_create_sized( NULL, "127.0.0.1:0", &c, sizeof( c ), NULL, 0 ); \
    check( ( made != NULL ) == (accepted) );                                                                    \
    if ( made )                                                                                                 \
        rocketnet_server_destroy( made );                                                                       \
} while ( 0 )

static void sized_config_create_refuses_before_init()
{
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( sized_config_quiet_assert );

    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    check( !rocketnet_library_initialized() );

    struct rocketnet_client_config_t client_config;
    check( rocketnet_client_default_config_sized( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_dev_config_sized( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );
    server_config.max_clients = 4;

    // validate_config stays safe before init: it creates nothing
    check( rocketnet_client_validate_config_sized( &client_config, sizeof( client_config ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_validate_config_sized( &server_config, sizeof( server_config ) ) == ROCKETNET_STATUS_OK );

    uint64_t before = rocketnet_api_misuse_count();
    check( rocketnet_client_create( NULL, NULL, NULL, 0 ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    before = rocketnet_api_misuse_count();
    check( rocketnet_client_create_sized( NULL, &client_config, sizeof( client_config ), NULL, 0 ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    before = rocketnet_api_misuse_count();
    check( rocketnet_server_create( NULL, "127.0.0.1:0", NULL, NULL, 0 ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    before = rocketnet_api_misuse_count();
    check( rocketnet_server_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), NULL, 0 ) == NULL );
    check( rocketnet_api_misuse_count() == before + 1 );

    check( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

    // after init the same calls create
    struct rocketnet_client_t * client = rocketnet_client_create_sized( NULL, &client_config, sizeof( client_config ), NULL, 0 );
    check( client != NULL );
    rocketnet_client_destroy( client );
    struct rocketnet_server_t * server = rocketnet_server_create_sized( NULL, "127.0.0.1:0", &server_config, sizeof( server_config ), NULL, 0 );
    check( server != NULL );
    rocketnet_server_destroy( server );

    rocketnet_set_assert_callback( saved_assert );
}

static void sized_config_client_create_agrees_with_validate_config()
{
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( sized_config_quiet_assert );

    // accepted by both
    SIZED_CONFIG_CLIENT_AGREES( (void) 0, true );
    SIZED_CONFIG_CLIENT_AGREES( c.transport = "portable", true );
    SIZED_CONFIG_CLIENT_AGREES( c.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES, true );
    SIZED_CONFIG_CLIENT_AGREES( c.num_reassembly_slots = ROCKETNET_MAX_FRAGMENTS, true );
    // a nonzero simulated value: accepted by both in the development posture,
    // refused by both in the production one (rocketnet_public_link_simulator_development)
    rocketnet_public_link_simulator_test_development( 1 );
    SIZED_CONFIG_CLIENT_AGREES( c.simulated_loss_percent = 5, true );
    rocketnet_public_link_simulator_test_development( 0 );
    if ( !rocketnet_public_link_simulator_development() )
        SIZED_CONFIG_CLIENT_AGREES( c.simulated_loss_percent = 5, false );

    // transport fields out of range: refused by both
    SIZED_CONFIG_CLIENT_AGREES( c.max_payload_bytes = -1, false );
    SIZED_CONFIG_CLIENT_AGREES( c.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, false );
    SIZED_CONFIG_CLIENT_AGREES( c.num_reassembly_slots = -1, false );
    SIZED_CONFIG_CLIENT_AGREES( c.num_reassembly_slots = ROCKETNET_MAX_FRAGMENTS + 1, false );

    // negative or out-of-range simulated values: refused by both
    SIZED_CONFIG_CLIENT_AGREES( c.simulated_loss_percent = -1, false );
    SIZED_CONFIG_CLIENT_AGREES( c.simulated_latency_ms = -1, false );
    SIZED_CONFIG_CLIENT_AGREES( c.simulated_jitter_ms = -1, false );
    SIZED_CONFIG_CLIENT_AGREES( c.simulated_duplicate_percent = -1, false );
    SIZED_CONFIG_CLIENT_AGREES( c.simulated_loss_percent = 101, false );

    // a transport list that does not parse: refused by both
    SIZED_CONFIG_CLIENT_AGREES( c.transport = "bogus", false );
    SIZED_CONFIG_CLIENT_AGREES( c.transport = "auto,portable", false );

    rocketnet_set_assert_callback( saved_assert );
}

static void sized_config_server_create_agrees_with_validate_config()
{
    void (*saved_assert)( const char *, const char *, const char *, int ) = rn_assert_function;
    rocketnet_set_assert_callback( sized_config_quiet_assert );

    // accepted by both
    SIZED_CONFIG_SERVER_AGREES( (void) 0, true );
    SIZED_CONFIG_SERVER_AGREES( c.transport = "portable", true );
    SIZED_CONFIG_SERVER_AGREES( c.max_timeout_seconds = 3600, true );
    SIZED_CONFIG_SERVER_AGREES( c.handshake_deadline_seconds = 60, true );
    // a nonzero simulated value: accepted by both in the development posture,
    // refused by both in the production one (rocketnet_public_link_simulator_development)
    rocketnet_public_link_simulator_test_development( 1 );
    SIZED_CONFIG_SERVER_AGREES( c.simulated_loss_percent = 5, true );
    rocketnet_public_link_simulator_test_development( 0 );
    if ( !rocketnet_public_link_simulator_development() )
        SIZED_CONFIG_SERVER_AGREES( c.simulated_loss_percent = 5, false );

    // transport fields out of range: refused by both
    SIZED_CONFIG_SERVER_AGREES( c.max_clients = 0, false );
    SIZED_CONFIG_SERVER_AGREES( c.max_clients = ROCKETNET_MAX_CLIENTS + 1, false );
    SIZED_CONFIG_SERVER_AGREES( c.num_xdp_queues = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.num_xdp_queues = 1025, false );
    SIZED_CONFIG_SERVER_AGREES( c.num_os_cpus = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.num_os_cpus = 257, false );
    SIZED_CONFIG_SERVER_AGREES( c.max_payload_bytes = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.max_payload_bytes = ROCKETNET_MAX_FRAGMENTED_PAYLOAD_BYTES + 1, false );
    SIZED_CONFIG_SERVER_AGREES( c.num_reassembly_slots = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.num_reassembly_slots = ROCKETNET_MAX_FRAGMENTS + 1, false );
    SIZED_CONFIG_SERVER_AGREES( c.max_timeout_seconds = 0, false );
    SIZED_CONFIG_SERVER_AGREES( c.max_timeout_seconds = 3601, false );
    SIZED_CONFIG_SERVER_AGREES( c.handshake_deadline_seconds = 0, false );
    SIZED_CONFIG_SERVER_AGREES( c.handshake_deadline_seconds = 61, false );

    // negative or out-of-range simulated values: refused by both
    SIZED_CONFIG_SERVER_AGREES( c.simulated_loss_percent = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.simulated_latency_ms = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.simulated_jitter_ms = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.simulated_duplicate_percent = -1, false );
    SIZED_CONFIG_SERVER_AGREES( c.simulated_duplicate_percent = 101, false );

    // a transport list that does not parse: refused by both
    SIZED_CONFIG_SERVER_AGREES( c.transport = "bogus", false );
    SIZED_CONFIG_SERVER_AGREES( c.transport = "auto,portable", false );

    rocketnet_set_assert_callback( saved_assert );
}

// The public client and server configs have one revision, the whole struct
// (docs/ABI-EVOLUTION.md section 4): the plain default fills every field, and
// the plain create reads every field, the four simulated_* link-simulator
// controls included. Every case runs in memory: the loopback transport, step
// driven, time from 1000.0, no socket and no clock.

#define PUBLIC_CONFIG_PROTOCOL 0x0C0F16A11FEDULL

struct public_config_identity_t
{
    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[ROCKETNET_BACKEND_SECRET_KEY_BYTES];
    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    uint8_t server_secret[ROCKETNET_SERVER_SECRET_KEY_BYTES];
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
    memcpy( config->server_public_key, identity->server_public, sizeof( config->server_public_key ) );
    memcpy( config->server_secret_key, identity->server_secret, sizeof( config->server_secret_key ) );
    config->transport = "loopback";
}

static void public_config_client( struct rocketnet_client_config_t * config )
{
    check( rocketnet_client_default_config( config ) == ROCKETNET_STATUS_OK );
    config->transport = "loopback";
    config->step_driven = true;
    config->time_callback = public_config_clock;
}

static void public_config_token( uint16_t port, const public_config_identity_t * identity, uint8_t * token )
{
    char address[64];
    snprintf( address, sizeof( address ), "127.0.0.1:%u", (unsigned) port );
    const char * addresses[] = { address };
    check( rocketnet_write_production_connect_token( identity->backend_secret, identity->server_public,
        PUBLIC_CONFIG_PROTOCOL, addresses, 1, 0xC0F1, 1000, 1ULL << 40, 300, NULL,
        token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );
}

// pumps a loopback pair for four simulated seconds; returns whether the
// client reached CONNECTED
static bool public_config_pair_connects( int server_loss_percent )
{
    public_config_identity_t identity;
    public_config_identity( &identity );

    struct rocketnet_server_config_t server_config;
    public_config_server( &server_config, &identity );
    server_config.simulated_loss_percent = server_loss_percent;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
    check( server != NULL );
    check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    uint8_t token[ROCKETNET_CONNECT_TOKEN_BYTES];
    public_config_token( rocketnet_server_port( server ), &identity, token );

    struct rocketnet_client_config_t client_config;
    public_config_client( &client_config );
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
    check( client != NULL );
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

static void public_config_plain_create_applies_simulation()
{
    rocketnet_set_quiet( 1 );
    const bool development = rocketnet_public_link_simulator_development();

    // Client and server creates validate the full config before allocation
    // (sdk/rocketnet_api.cpp:459 and :1155), including simulated_* fields.

    rocketnet_public_link_simulator_test_development( 0 );
    {
        public_config_identity_t identity;
        public_config_identity( &identity );

        struct rocketnet_server_config_t server_config;
        public_config_server( &server_config, &identity );
        server_config.simulated_loss_percent = 25;
        struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0 );
        check( server == NULL );

        struct rocketnet_client_config_t client_config;
        public_config_client( &client_config );
        client_config.simulated_loss_percent = 25;
        struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0 );
        check( client == NULL );
    }

    // a development posture honours it: the server's total loss, set through
    // the plain create, keeps the pair from connecting; none connects. (A
    // step-driven client sends through its I/O rings, which bypass the client
    // link simulator, so the client's own loss is pinned by the refusal above.)

    rocketnet_public_link_simulator_test_development( 1 );
    check( public_config_pair_connects( 0 ) );
    const bool server_loss_connects = public_config_pair_connects( 100 );
    check( !server_loss_connects );

    rocketnet_public_link_simulator_test_development( development ? 1 : 0 );
    rocketnet_set_quiet( 0 );
}

static void public_config_default_fills_every_field()
{
    // the plain default and the sized default at sizeof write the same whole
    // struct, whatever the caller's memory held before

    struct rocketnet_client_config_t client_a, client_b, client_sized;
    memset( &client_a, 0xAB, sizeof( client_a ) );
    memset( &client_b, 0x5C, sizeof( client_b ) );
    memset( &client_sized, 0xE7, sizeof( client_sized ) );
    check( rocketnet_client_default_config( &client_a ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_default_config( &client_b ) == ROCKETNET_STATUS_OK );
    check( rocketnet_client_default_config_sized( &client_sized, sizeof( client_sized ) ) == ROCKETNET_STATUS_OK );
    check( client_a.simulated_loss_percent == 0 );
    check( client_a.simulated_latency_ms == 0 );
    check( client_a.simulated_jitter_ms == 0 );
    check( client_a.simulated_duplicate_percent == 0 );
    check( memcmp( &client_a, &client_b, sizeof( client_a ) ) == 0 );
    check( memcmp( &client_a, &client_sized, sizeof( client_a ) ) == 0 );
    // the default, then a sized read at sizeof: a whole, valid struct
    check( rocketnet_client_validate_config_sized( &client_a, sizeof( client_a ) ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_a, server_b, server_sized, dev_a, dev_sized;
    memset( &server_a, 0xAB, sizeof( server_a ) );
    memset( &server_b, 0x5C, sizeof( server_b ) );
    memset( &server_sized, 0xE7, sizeof( server_sized ) );
    memset( &dev_a, 0xAB, sizeof( dev_a ) );
    memset( &dev_sized, 0x5C, sizeof( dev_sized ) );
    check( rocketnet_server_default_config( &server_a ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_default_config( &server_b ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_default_config_sized( &server_sized, sizeof( server_sized ) ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_dev_config( &dev_a ) == ROCKETNET_STATUS_OK );
    check( rocketnet_server_dev_config_sized( &dev_sized, sizeof( dev_sized ) ) == ROCKETNET_STATUS_OK );
    check( server_a.simulated_loss_percent == 0 );
    check( server_a.simulated_latency_ms == 0 );
    check( server_a.simulated_jitter_ms == 0 );
    check( server_a.simulated_duplicate_percent == 0 );
    check( memcmp( &server_a, &server_b, sizeof( server_a ) ) == 0 );
    check( memcmp( &server_a, &server_sized, sizeof( server_a ) ) == 0 );
    check( memcmp( &dev_a, &dev_sized, sizeof( dev_a ) ) == 0 );
    check( rocketnet_server_validate_config_sized( &server_a, sizeof( server_a ) ) == ROCKETNET_STATUS_OK );
}


REGISTER_UNIT_TEST( public_config_plain_create_applies_simulation );
REGISTER_UNIT_TEST( public_config_default_fills_every_field );
REGISTER_UNIT_TEST( sized_config_accepts_every_known_size );
REGISTER_UNIT_TEST( sized_config_message_revision_1_reads_budget_defaults );
REGISTER_UNIT_TEST( sized_config_refuses_unknown_size );
REGISTER_UNIT_TEST( server_default_config_max_clients_is_1000 );
REGISTER_UNIT_TEST( sized_config_create_refuses_before_init );
REGISTER_UNIT_TEST( sized_config_client_create_agrees_with_validate_config );
REGISTER_UNIT_TEST( sized_config_server_create_agrees_with_validate_config );
