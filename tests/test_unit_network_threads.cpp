/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Each server transport's own network thread count (sdk/rocketnet_network_threads.h):
// one config field and one environment variable per transport, never shared.
// Read by value for every transport: the default is used when nothing is set,
// the config field is honoured, the environment variable is honoured and wins
// over the field, and every refusal (a value past the transport's limit,
// below zero, or a variable that is not a number) is refused, never clamped.
// Then the memory plan of each transport this platform builds, which counts
// the threads its create starts.
//
// No sockets, no real time, no real environment: the development library's
// in-memory environment overlay (tests/test_env_source.h), and the plan is a
// pure size computation.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"
#include "rocketnet_layers.h"
#include "rocketnet_memory.h"
#include "rocketnet_network_threads.h"
#if ROCKETNET_DEV
#include "test_env_source.h"
#endif // #if ROCKETNET_DEV

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define check rocketnet_unit_check

struct network_threads_row_t
{
    const char * field;
    const char * variable;
    int default_count;
    int max_count;
    int rocketnet_server_config_t::*public_field;
    int rocketnet_server_transport_config_t::*transport_field;
};

static const network_threads_row_t network_threads_rows[] = {
    { "num_xdp_threads", "ROCKETNET_SERVER_NUM_XDP_THREADS", ROCKETNET_XDP_DEFAULT_THREADS, ROCKETNET_XDP_MAX_THREADS,
      &rocketnet_server_config_t::num_xdp_threads, &rocketnet_server_transport_config_t::num_xdp_threads },
    { "num_gso_workers", "ROCKETNET_SERVER_NUM_GSO_WORKERS", ROCKETNET_GSO_DEFAULT_WORKERS, ROCKETNET_GSO_MAX_WORKERS,
      &rocketnet_server_config_t::num_gso_workers, &rocketnet_server_transport_config_t::num_gso_workers },
    { "num_io_uring_workers", "ROCKETNET_SERVER_NUM_IO_URING_WORKERS", ROCKETNET_IO_URING_DEFAULT_WORKERS, ROCKETNET_IO_URING_MAX_WORKERS,
      &rocketnet_server_config_t::num_io_uring_workers, &rocketnet_server_transport_config_t::num_io_uring_workers },
    { "num_rio_lanes", "ROCKETNET_SERVER_NUM_RIO_LANES", ROCKETNET_RIO_DEFAULT_LANES, ROCKETNET_RIO_MAX_LANES,
      &rocketnet_server_config_t::num_rio_lanes, &rocketnet_server_transport_config_t::num_rio_lanes },
    { "num_iocp_completion_workers", "ROCKETNET_SERVER_NUM_IOCP_COMPLETION_WORKERS", ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS,
      ROCKETNET_IOCP_MAX_COMPLETION_WORKERS, &rocketnet_server_config_t::num_iocp_completion_workers,
      &rocketnet_server_transport_config_t::num_iocp_completion_workers },
    { "num_iocp_send_threads", "ROCKETNET_SERVER_NUM_IOCP_SEND_THREADS", ROCKETNET_IOCP_DEFAULT_SEND_THREADS, ROCKETNET_IOCP_MAX_SEND_THREADS,
      &rocketnet_server_config_t::num_iocp_send_threads, &rocketnet_server_transport_config_t::num_iocp_send_threads },
    { "num_msgx_transmit_workers", "ROCKETNET_SERVER_NUM_MSGX_TRANSMIT_WORKERS", ROCKETNET_MSGX_DEFAULT_TRANSMIT_WORKERS,
      ROCKETNET_MSGX_MAX_TRANSMIT_WORKERS, &rocketnet_server_config_t::num_msgx_transmit_workers,
      &rocketnet_server_transport_config_t::num_msgx_transmit_workers },
};

static const int network_threads_num_rows = (int) ( sizeof( network_threads_rows ) / sizeof( network_threads_rows[0] ) );

// the defaults by value, each the count that passed on a good machine (the
// rows named in sdk/rocketnet_network_threads.h), and the one-place helpers
static void test_network_threads_defaults_by_value()
{
    check( ROCKETNET_XDP_DEFAULT_THREADS == 1 );
    check( ROCKETNET_GSO_DEFAULT_WORKERS == 1 );
    check( ROCKETNET_IO_URING_DEFAULT_WORKERS == 2 );
    check( ROCKETNET_RIO_DEFAULT_LANES == 5 );
    check( ROCKETNET_IOCP_DEFAULT_COMPLETION_WORKERS == 2 );
    check( ROCKETNET_IOCP_DEFAULT_SEND_THREADS == 2 );
    check( ROCKETNET_MSGX_DEFAULT_TRANSMIT_WORKERS == 3 );

    for ( int i = 0; i < network_threads_num_rows; ++i )
    {
        const network_threads_row_t & row = network_threads_rows[i];
        check( row.default_count >= 1 && row.default_count <= row.max_count );
        check( rocketnet_network_threads_count( 0, row.default_count ) == row.default_count );
        check( rocketnet_network_threads_count( 1, row.default_count ) == 1 );
        check( rocketnet_network_threads_count( row.max_count, row.default_count ) == row.max_count );
    }

    // the per-socket transports: nothing asked on the wildcard is one
    check( rocketnet_network_threads_per_socket( 0, 6, 1 ) == 1 );
    check( rocketnet_network_threads_per_socket( 0, 6, 0 ) == 6 );
    check( rocketnet_network_threads_per_socket( 4, 6, 1 ) == 4 );
    check( rocketnet_network_threads_per_socket( 4, 6, 0 ) == 4 );
}

REGISTER_UNIT_TEST( test_network_threads_defaults_by_value );

// a negative count is refused by both helpers (0 back), never read as the
// default, as xdp and rio refuse one; and a plan of a negative count does not
// compute, so the list's size is 0, never the default's size
static void test_network_threads_negative_refused()
{
    for ( int i = 0; i < network_threads_num_rows; ++i )
    {
        check( rocketnet_network_threads_count( -1, network_threads_rows[i].default_count ) == 0 );
    }
    check( rocketnet_network_threads_per_socket( -1, 6, 1 ) == 0 );
    check( rocketnet_network_threads_per_socket( -1, 6, 0 ) == 0 );

    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    rocketnet_server_memory_plan_t plan;
    (void) plan;

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    config.num_gso_workers = -1;
    check( !rocketnet_builtin_server_memory_plan( &config, "gso", plan ) );
    snprintf( config.transport, sizeof( config.transport ), "%s", "gso" );
    bool unplannable = false;
    check( rocketnet_server_transport_order_allocator_bytes( &config, &unplannable ) == 0 );
    check( unplannable );

    // another candidate that plans gives the list its size
    snprintf( config.transport, sizeof( config.transport ), "%s", "gso,portable" );
    check( rocketnet_server_transport_order_allocator_bytes( &config, &unplannable ) > 0 );
    check( !unplannable );
    config.num_gso_workers = 0;

    config.num_io_uring_workers = -1;
    check( !rocketnet_builtin_server_memory_plan( &config, "io_uring", plan ) );
    config.num_io_uring_workers = 0;
#if ROCKETNET_XDP
    config.num_xdp_threads = -1;
    check( !rocketnet_builtin_server_memory_plan( &config, "xdp", plan ) );
    config.num_xdp_threads = 0;
#endif // #if ROCKETNET_XDP
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    config.num_msgx_transmit_workers = -1;
    check( !rocketnet_builtin_server_memory_plan( &config, "msgx", plan ) );
    config.num_msgx_transmit_workers = 0;
#endif // #if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    config.num_iocp_completion_workers = -1;
    check( !rocketnet_builtin_server_memory_plan( &config, "iocp", plan ) );
    config.num_iocp_completion_workers = 0;
    config.num_rio_lanes = -1;
    check( !rocketnet_builtin_server_memory_plan( &config, "rio", plan ) );
    config.num_rio_lanes = 0;
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

REGISTER_UNIT_TEST( test_network_threads_negative_refused );

// the one answer for the RIO lanes, which the plan and create both take
// (pure, every platform): asked wins; the wildcard is one; a shareable bind
// address takes the development test count when it is 2 .. the limit, else
// the default; a count outside the range is refused
static void test_network_threads_rio_lanes_one_answer()
{
    check( rocketnet_rio_lanes_for( 0, 0, 0 ) == 1 );
    check( rocketnet_rio_lanes_for( 0, 0, 8 ) == 1 );
    check( rocketnet_rio_lanes_for( 0, 1, 0 ) == ROCKETNET_RIO_DEFAULT_LANES );
    check( rocketnet_rio_lanes_for( 0, 1, 8 ) == 8 );
    check( rocketnet_rio_lanes_for( 0, 1, 16 ) == 16 );
    check( rocketnet_rio_lanes_for( 0, 1, ROCKETNET_RIO_MAX_LANES ) == ROCKETNET_RIO_MAX_LANES );
    check( rocketnet_rio_lanes_for( 0, 1, 1 ) == ROCKETNET_RIO_DEFAULT_LANES );
    check( rocketnet_rio_lanes_for( 0, 1, ROCKETNET_RIO_MAX_LANES + 1 ) == ROCKETNET_RIO_DEFAULT_LANES );
    check( rocketnet_rio_lanes_for( 1, 1, 8 ) == 1 );
    check( rocketnet_rio_lanes_for( 3, 0, 8 ) == 3 );
    check( rocketnet_rio_lanes_for( 3, 1, 8 ) == 3 );
    check( rocketnet_rio_lanes_for( ROCKETNET_RIO_MAX_LANES, 1, 0 ) == ROCKETNET_RIO_MAX_LANES );
    check( rocketnet_rio_lanes_for( -1, 1, 8 ) == 0 );
    check( rocketnet_rio_lanes_for( ROCKETNET_RIO_MAX_LANES + 1, 1, 8 ) == 0 );
}

REGISTER_UNIT_TEST( test_network_threads_rio_lanes_one_answer );

// one table row per transport: its own field, its own variable, zero is the
// default, the range is 1 .. the transport's limit
static void test_network_threads_table_rows()
{
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );

    for ( int i = 0; i < network_threads_num_rows; ++i )
    {
        const network_threads_row_t & row = network_threads_rows[i];
        int found = 0;
        for ( int f = 0; f < num_fields; ++f )
        {
            if ( strcmp( fields[f].name, row.field ) == 0 )
            {
                found++;
                check( fields[f].kind == ROCKETNET_CONFIG_KIND_INT );
                check( fields[f].min_value == 1 && fields[f].max_value == row.max_count );
                check( ( fields[f].flags & ROCKETNET_CONFIG_ZERO_MEANS_DEFAULT ) != 0 );
            }
        }
        check( found == 1 );

        char variable[128];
        check( rocketnet_config_env_var_name_for( "server", row.field, variable, sizeof( variable ) ) );
        check( strcmp( variable, row.variable ) == 0 );
    }

    // the shared count is gone
    for ( int f = 0; f < num_fields; ++f )
    {
        check( strcmp( fields[f].name, "num_xdp_queues" ) != 0 );
    }
}

REGISTER_UNIT_TEST( test_network_threads_table_rows );

#if ROCKETNET_DEV

// the public config resolved as create resolves it, the environment read
static bool network_threads_resolve( const struct rocketnet_server_config_t * config, struct rocketnet_server_transport_config_t * transport )
{
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    return rocketnet_server_config_resolve( config, true, transport, &message, &kbps );
}

static bool network_threads_valid( const struct rocketnet_server_transport_config_t * transport )
{
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
    return rocketnet_config_validate( "server", transport, fields, num_fields );
}

static void test_network_threads_config_and_environment()
{
    rocketnet_quiet( 1 );

    for ( int i = 0; i < network_threads_num_rows; ++i )
    {
        const network_threads_row_t & row = network_threads_rows[i];
        for ( int j = 0; j < network_threads_num_rows; ++j )
        {
            check( rocketnet_test_env_unsetenv( network_threads_rows[j].variable ) == 0 );
        }

        struct rocketnet_server_config_t config;
        check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
        struct rocketnet_server_transport_config_t transport;

        // nothing set: zero, the transport's default
        check( config.*row.public_field == 0 );
        check( network_threads_resolve( &config, &transport ) );
        check( transport.*row.transport_field == 0 );
        check( network_threads_valid( &transport ) );

        // the config field
        config.*row.public_field = 1;
        check( network_threads_resolve( &config, &transport ) );
        check( transport.*row.transport_field == 1 );
        check( network_threads_valid( &transport ) );

        // the environment variable, with the field unset, and over the field:
        // the environment wins
        config.*row.public_field = 0;
        char text[16];
        snprintf( text, sizeof( text ), "%d", row.max_count );
        check( rocketnet_test_env_setenv( row.variable, text, 1 ) == 0 );
        check( network_threads_resolve( &config, &transport ) );
        check( transport.*row.transport_field == row.max_count );
        check( network_threads_valid( &transport ) );

        config.*row.public_field = 1;
        check( network_threads_resolve( &config, &transport ) );
        check( transport.*row.transport_field == row.max_count );

        // no other transport's field moved
        for ( int j = 0; j < network_threads_num_rows; ++j )
        {
            if ( j != i )
            {
                check( transport.*network_threads_rows[j].transport_field == 0 );
            }
        }

        // refusals: past the limit and below zero from either source, and a
        // variable that is not a number
        snprintf( text, sizeof( text ), "%d", row.max_count + 1 );
        check( rocketnet_test_env_setenv( row.variable, text, 1 ) == 0 );
        check( !network_threads_resolve( &config, &transport ) || !network_threads_valid( &transport ) );
        check( rocketnet_test_env_setenv( row.variable, "-1", 1 ) == 0 );
        check( !network_threads_resolve( &config, &transport ) || !network_threads_valid( &transport ) );
        check( rocketnet_test_env_setenv( row.variable, "four", 1 ) == 0 );
        check( !network_threads_resolve( &config, &transport ) );
        check( rocketnet_test_env_unsetenv( row.variable ) == 0 );

        config.*row.public_field = row.max_count + 1;
        check( !network_threads_resolve( &config, &transport ) || !network_threads_valid( &transport ) );
        config.*row.public_field = -1;
        check( !network_threads_resolve( &config, &transport ) || !network_threads_valid( &transport ) );
    }

    rocketnet_test_env_restore();
}

REGISTER_UNIT_TEST( test_network_threads_config_and_environment );

struct network_threads_log_t
{
    int warnings_naming_retired;
    int lines;
};

static void network_threads_log_capture( void * context, rocketnet_log_level_t level, const char * message, size_t length )
{
    network_threads_log_t * log = (network_threads_log_t *) context;
    log->lines++;
    char line[1024];
    const size_t bytes = length < sizeof( line ) - 1 ? length : sizeof( line ) - 1;
    memcpy( line, message, bytes );
    line[bytes] = '\0';
    if ( level == ROCKETNET_LOG_LEVEL_WARN && strstr( line, "ROCKETNET_SERVER_NUM_XDP_QUEUES is no longer read" ) &&
         strstr( line, "ROCKETNET_SERVER_NUM_XDP_THREADS" ) && strstr( line, "ROCKETNET_SERVER_NUM_MSGX_TRANSMIT_WORKERS" ) )
    {
        log->warnings_naming_retired++;
    }
}

// the retired ROCKETNET_SERVER_NUM_XDP_QUEUES: unset, the resolve logs no
// warning about it; set, the resolve logs one warning naming the variables
// that replace it, and resolves exactly what it resolves without it
static void network_threads_resolve_logged( network_threads_log_t * log, struct rocketnet_server_transport_config_t * transport )
{
    struct rocketnet_server_config_t config;
    check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );

    const rocketnet_log_level_t saved_level = rocketnet_log_level_current();
    rocketnet_quiet( 0 );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_WARN );
    rocketnet_log_function( network_threads_log_capture, log );
    check( network_threads_resolve( &config, transport ) );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved_level );
    rocketnet_quiet( 1 );
}

static void test_network_threads_retired_variable_warns()
{
    for ( int j = 0; j < network_threads_num_rows; ++j )
    {
        check( rocketnet_test_env_unsetenv( network_threads_rows[j].variable ) == 0 );
    }
    check( rocketnet_test_env_unsetenv( "ROCKETNET_SERVER_NUM_XDP_QUEUES" ) == 0 );

    network_threads_log_t unset_log = { 0, 0 };
    struct rocketnet_server_transport_config_t unset_transport;
    network_threads_resolve_logged( &unset_log, &unset_transport );

    check( rocketnet_test_env_setenv( "ROCKETNET_SERVER_NUM_XDP_QUEUES", "7", 1 ) == 0 );
    network_threads_log_t set_log = { 0, 0 };
    struct rocketnet_server_transport_config_t set_transport;
    network_threads_resolve_logged( &set_log, &set_transport );

    rocketnet_test_env_restore();

    check( unset_log.warnings_naming_retired == 0 );
    check( set_log.warnings_naming_retired == 1 );
    check( set_log.lines == unset_log.lines + 1 );

    // no behaviour change: every count resolves as it does without it
    for ( int j = 0; j < network_threads_num_rows; ++j )
    {
        check( set_transport.*network_threads_rows[j].transport_field == unset_transport.*network_threads_rows[j].transport_field );
        check( set_transport.*network_threads_rows[j].transport_field == 0 );
    }
}

REGISTER_UNIT_TEST( test_network_threads_retired_variable_warns );

#endif // #if ROCKETNET_DEV

// the plan counts the threads each transport's create starts: its default
// when nothing is asked, else what was asked
static void network_threads_plan( const char * transport, rocketnet_server_transport_config_t * config, int expected_threads )
{
    rocketnet_server_memory_plan_t plan;
    check( rocketnet_builtin_server_memory_plan( config, transport, plan ) );
    check( plan.worker_threads == expected_threads );
}

static void test_network_threads_plan_per_transport()
{
    rocketnet_server_transport_config_t config;
    rocketnet_server_transport_default_config( &config );
    (void) config;

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX
    network_threads_plan( "gso", &config, ROCKETNET_GSO_DEFAULT_WORKERS );
    config.num_gso_workers = 3;
    network_threads_plan( "gso", &config, 3 );
    network_threads_plan( "io_uring", &config, ROCKETNET_IO_URING_DEFAULT_WORKERS );
    config.num_io_uring_workers = 3;
    network_threads_plan( "io_uring", &config, 3 );
#if ROCKETNET_XDP
    network_threads_plan( "xdp", &config, ROCKETNET_XDP_DEFAULT_THREADS );
    config.num_xdp_threads = 3;
    network_threads_plan( "xdp", &config, 3 );
#endif // #if ROCKETNET_XDP
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX

#if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
    // the transmit workers and the one receive worker
    network_threads_plan( "msgx", &config, ROCKETNET_MSGX_DEFAULT_TRANSMIT_WORKERS + 1 );
    config.num_msgx_transmit_workers = 2;
    network_threads_plan( "msgx", &config, 2 + 1 );
#endif // #if defined( __APPLE__ ) && ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#if ROCKETNET_DEV
    // the development lane knob would stand in for the default
    check( rocketnet_test_env_unsetenv( "ROCKETNET_TEST_RIO_LANES" ) == 0 );
#endif // #if ROCKETNET_DEV
    // which path was taken, by value: nothing asked on the wildcard is one
    // lane, on a specific bind address the default; asked is that many
    network_threads_plan( "rio", &config, 1 );
    snprintf( config.bind_ipv4_address, sizeof( config.bind_ipv4_address ), "10.0.0.5" );
    network_threads_plan( "rio", &config, ROCKETNET_RIO_DEFAULT_LANES );
    snprintf( config.bind_ipv6_address, sizeof( config.bind_ipv6_address ), "::ffff:10.0.0.5" );
    config.bind_ipv4_address[0] = '\0';
    network_threads_plan( "rio", &config, 1 );
    config.bind_ipv6_address[0] = '\0';
    config.num_rio_lanes = 1;
    network_threads_plan( "rio", &config, 1 );
    config.num_rio_lanes = 3;
    network_threads_plan( "rio", &config, 3 );
#if ROCKETNET_DEV
    // the development lane count: the plan sizes the lanes create makes for
    // nothing asked on a shareable address (8 or 16), and the wildcard keeps
    // its one lane, as create does
    config.num_rio_lanes = 0;
    check( rocketnet_test_env_setenv( "ROCKETNET_TEST_RIO_LANES", "8", 1 ) == 0 );
    network_threads_plan( "rio", &config, 1 );
    snprintf( config.bind_ipv4_address, sizeof( config.bind_ipv4_address ), "10.0.0.5" );
    network_threads_plan( "rio", &config, 8 );
    check( rocketnet_test_env_setenv( "ROCKETNET_TEST_RIO_LANES", "16", 1 ) == 0 );
    network_threads_plan( "rio", &config, 16 );
    config.bind_ipv4_address[0] = '\0';
    rocketnet_test_env_restore();
#endif // #if ROCKETNET_DEV

    // send threads past the send lanes (max_clients 64 has one): the iocp plan
    // does not compute, and a list of iocp alone has no size
    config.max_clients = 64;
    config.num_iocp_send_threads = 2;
    rocketnet_server_memory_plan_t iocp_plan;
    check( !rocketnet_builtin_server_memory_plan( &config, "iocp", iocp_plan ) );
    snprintf( config.transport, sizeof( config.transport ), "%s", "iocp" );
    check( rocketnet_server_transport_order_allocator_bytes( &config ) == 0 );
    config.num_iocp_send_threads = 0;

    // and the public size of that config is 0 (start refuses it), never the
    // size without its transport
    struct rocketnet_server_config_t server_config;
    check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );
    server_config.transport = "iocp";
    server_config.max_clients = 64;
    static const uint8_t filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x49, 0x4F, 0x43, 0x50 };
    memcpy( server_config.packet_filter, filter, sizeof( server_config.packet_filter ) );   // a production build refuses the test filter
    check( rocketnet_server_required_memory( &server_config ) > 0 );
    server_config.num_iocp_send_threads = 2;
    check( rocketnet_server_required_memory( &server_config ) == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
}

REGISTER_UNIT_TEST( test_network_threads_plan_per_transport );
