/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// One documented environment variable per config field, and the log level
// read before init logs. Each case sets variables through a setenv shim and
// reads what the env passes do through the log callback; none opens a
// socket, starts a transport or reads the clock.
//
//   - the public env pass (sdk/rocketnet_api.cpp:119)
//     reads the message table under the endpoint's prefix and the budget under
//     its own name; the budget is never read a second time under a message
//     field's name (ROCKETNET_CLIENT_CLIENT_SEND_KBPS)
//   - a message object's create (sdk/rocketnet_message_server.cpp:146)
//     reads no environment, so nothing overrides
//     ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS after it is applied
//   - rocketnet_init (sdk/rocketnet.cpp:715) applies ROCKETNET_LOG_LEVEL before
//     its first log line
//   - the step_driven row (sdk/rocketnet_config.cpp:88)
//     takes 0 or 1 only
//   - every variable those passes apply has the same unique inventory in
//     sdk/rocketnet.h and BUILDING.md
//   - transport env passes (sdk/rocketnet_client_transport.cpp:583-586,652-653
//     and sdk/rocketnet_server_transport.cpp rocketnet_config_apply_env,
//     rocketnet_config_apply_env_uint64) are fully inventoried,
//     including public fields and simulator/internal rows

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "test_unit_registry.h"
#include "portable_crt.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_config.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_message_object.h"

#define check rocketnet_unit_check

#if defined( _WIN32 )
static void env_names_setenv( const char * name, const char * value ) { _putenv_s( name, value ); }
static void env_names_unsetenv( const char * name ) { _putenv_s( name, "" ); }
#else
static void env_names_setenv( const char * name, const char * value ) { setenv( name, value, 1 ); }
static void env_names_unsetenv( const char * name ) { unsetenv( name ); }
#endif

// ---------------------------------------------------------------------------
// the log capture: every line, and the variable each override line names
// ---------------------------------------------------------------------------

struct env_names_log_t
{
    std::vector<std::string> lines;
    std::vector<int> levels;
};

static void env_names_log_capture( void * context, rocketnet_log_level_t level, const char * message, size_t length )
{
    env_names_log_t * log = (env_names_log_t *) context;
    log->lines.push_back( std::string( message, length ) );
    log->levels.push_back( (int) level );
}

// rocketnet_config_apply_env logs "<endpoint> config override NAME=value" (sdk/rocketnet_config.cpp:679)
static std::vector<std::string> env_names_overrides( const env_names_log_t & log )
{
    std::vector<std::string> names;
    for ( size_t i = 0; i < log.lines.size(); i++ )
    {
        const char * at = strstr( log.lines[i].c_str(), "config override " );
        if ( !at )
            continue;
        at += strlen( "config override " );
        const char * end = strchr( at, '=' );
        if ( end )
            names.push_back( std::string( at, (size_t) ( end - at ) ) );
    }
    return names;
}

static bool env_names_contains( const std::vector<std::string> & names, const char * name )
{
    for ( size_t i = 0; i < names.size(); i++ )
        if ( names[i] == name )
            return true;
    return false;
}

struct env_names_capture_t
{
    env_names_log_t log;
    rocketnet_log_level_t saved_level;
};

static void env_names_capture_begin( env_names_capture_t * capture )
{
    capture->saved_level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );            // the override lines are info
    rocketnet_log_function( env_names_log_capture, &capture->log );
}

static void env_names_capture_end( env_names_capture_t * capture )
{
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( capture->saved_level );
}

// ---------------------------------------------------------------------------
// a message object on a link that sends nothing
// ---------------------------------------------------------------------------

static void env_names_link_update( void *, double ) {}
static int env_names_link_payloads( void *, const int ** client_index, uint8_t * const ** payload_data, const int ** payload_bytes )
{
    *client_index = NULL;
    *payload_data = NULL;
    *payload_bytes = NULL;
    return 0;
}
static int env_names_link_acked( void *, const int ** client_index, const uint64_t ** packet_ids )
{
    *client_index = NULL;
    *packet_ids = NULL;
    return 0;
}
static int env_names_link_max_payload_bytes( void * ) { return 1200; }
static uint8_t * env_names_link_start_packet( void *, int, int, struct rocketnet_packet_context_t * ) { return NULL; }
static int env_names_link_finish_packet( void *, struct rocketnet_packet_context_t *, int ) { return 0; }
static void env_names_link_abort_packet( void *, struct rocketnet_packet_context_t * ) {}
static void env_names_link_send_packets( void * ) {}
static uint64_t env_names_link_connection( void *, int ) { return 0; }
static void env_names_link_disconnect( void *, int ) {}
static int env_names_link_state( void * ) { return 0; }

static struct rn_message_object_t * env_names_object_create( int is_server, int32_t kbps )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.update = env_names_link_update;
    link.payloads = env_names_link_payloads;
    link.acked = env_names_link_acked;
    link.max_payload_bytes = env_names_link_max_payload_bytes;
    link.start_packet = env_names_link_start_packet;
    link.finish_packet = env_names_link_finish_packet;
    link.abort_packet = env_names_link_abort_packet;
    link.send_packets = env_names_link_send_packets;
    link.connection = env_names_link_connection;
    link.disconnect = env_names_link_disconnect;
    link.state = env_names_link_state;
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    return rn_message_object_create( NULL, &link, is_server, 1, &config, NULL, 0, kbps, 1000.0 );
}

// ---------------------------------------------------------------------------
// (1) each budget has one name: ROCKETNET_CLIENT_SEND_KBPS, never a message
// field's name under the endpoint prefix
// ---------------------------------------------------------------------------

static void test_config_env_budget_is_never_a_message_field()
{
    env_names_setenv( "ROCKETNET_CLIENT_CLIENT_SEND_KBPS", "123" );
    env_names_setenv( "ROCKETNET_SERVER_SERVER_SEND_KBPS", "456" );

    env_names_capture_t capture;
    env_names_capture_begin( &capture );
    struct rocketnet_client_transport_config_t client_transport;
    struct rocketnet_server_transport_config_t server_transport;
    struct rocketnet_message_config_t message;
    uint32_t client_kbps = 0;
    uint32_t server_kbps = 0;
    const bool client_resolved = rocketnet_client_config_resolve( NULL, true, &client_transport, &message, &client_kbps );
    const bool server_resolved = rocketnet_server_config_resolve( NULL, true, &server_transport, &message, &server_kbps );
    env_names_capture_end( &capture );

    env_names_unsetenv( "ROCKETNET_CLIENT_CLIENT_SEND_KBPS" );
    env_names_unsetenv( "ROCKETNET_SERVER_SERVER_SEND_KBPS" );

    check( client_resolved );
    check( server_resolved );
    const std::vector<std::string> overrides = env_names_overrides( capture.log );
    check( !env_names_contains( overrides, "ROCKETNET_CLIENT_CLIENT_SEND_KBPS" ) );
    check( !env_names_contains( overrides, "ROCKETNET_SERVER_SERVER_SEND_KBPS" ) );
    check( client_kbps == ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS );
    check( server_kbps == ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS );
}
REGISTER_UNIT_TEST( test_config_env_budget_is_never_a_message_field );

// ---------------------------------------------------------------------------
// (1) the documented ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS is the budget the
// message object takes: ROCKETNET_MESSAGE_SERVER_SEND_KBPS changes nothing
// ---------------------------------------------------------------------------

static void test_config_env_per_client_send_kbps_is_the_server_budget()
{
    env_names_setenv( "ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS", "500" );
    env_names_setenv( "ROCKETNET_MESSAGE_SERVER_SEND_KBPS", "700" );
    env_names_setenv( "ROCKETNET_CLIENT_SEND_KBPS", "300" );
    env_names_setenv( "ROCKETNET_MESSAGE_CLIENT_SEND_KBPS", "400" );

    struct rocketnet_server_transport_config_t server_transport;
    struct rocketnet_client_transport_config_t client_transport;
    struct rocketnet_message_config_t message;
    uint32_t server_kbps = 0;
    uint32_t client_kbps = 0;
    const bool server_resolved = rocketnet_server_config_resolve( NULL, true, &server_transport, &message, &server_kbps );
    const bool client_resolved = rocketnet_client_config_resolve( NULL, true, &client_transport, &message, &client_kbps );
    struct rn_message_object_t * server = server_resolved ? env_names_object_create( 1, (int32_t) server_kbps ) : NULL;
    struct rn_message_object_t * client = client_resolved ? env_names_object_create( 0, (int32_t) client_kbps ) : NULL;
    const int32_t server_default = server ? server->default_kbps : 0;
    const int32_t client_default = client ? client->default_kbps : 0;
    if ( server )
        rn_message_object_destroy( server );
    if ( client )
        rn_message_object_destroy( client );

    env_names_unsetenv( "ROCKETNET_SERVER_PER_CLIENT_SEND_KBPS" );
    env_names_unsetenv( "ROCKETNET_MESSAGE_SERVER_SEND_KBPS" );
    env_names_unsetenv( "ROCKETNET_CLIENT_SEND_KBPS" );
    env_names_unsetenv( "ROCKETNET_MESSAGE_CLIENT_SEND_KBPS" );

    check( server_resolved && server_kbps == 500 );
    check( client_resolved && client_kbps == 300 );
    check( server != NULL );
    check( client != NULL );
    check( server_default == 500 );
    check( client_default == 300 );
}
REGISTER_UNIT_TEST( test_config_env_per_client_send_kbps_is_the_server_budget );

// ---------------------------------------------------------------------------
// (1, 3) ROCKETNET_LOG_LEVEL is applied before init's first line: at error,
// init emits no info line
// ---------------------------------------------------------------------------

static void test_config_env_log_level_applies_before_init_logs()
{
    // the worker ran init at error; the default level is info
    check( rocketnet_term() == ROCKETNET_STATUS_OK );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    char level[8];
    snprintf( level, sizeof( level ), "%d", (int) ROCKETNET_LOG_LEVEL_ERROR );
    env_names_setenv( "ROCKETNET_LOG_LEVEL", level );

    env_names_log_t log;
    rocketnet_log_function( env_names_log_capture, &log );
    const rocketnet_status_t status = rocketnet_init( NULL, NULL, NULL );
    rocketnet_log_function( NULL, NULL );
    const rocketnet_log_level_t after = rocketnet_log_level_current();

    env_names_unsetenv( "ROCKETNET_LOG_LEVEL" );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    check( status == ROCKETNET_STATUS_OK );
    check( after == ROCKETNET_LOG_LEVEL_ERROR );
    for ( size_t i = 0; i < log.levels.size(); i++ )
        check( log.levels[i] <= (int) ROCKETNET_LOG_LEVEL_ERROR );
}
REGISTER_UNIT_TEST( test_config_env_log_level_applies_before_init_logs );

// ---------------------------------------------------------------------------
// (3) ROCKETNET_CLIENT_STEP_DRIVEN is 0 or 1: any other value is out of range
// ---------------------------------------------------------------------------

static bool env_names_step_driven_valid( const char * value )
{
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_client_transport_config_fields( &num_fields );
    struct rocketnet_client_transport_config_t config;
    rocketnet_client_transport_default_config_current( &config );
    env_names_setenv( "ROCKETNET_CLIENT_STEP_DRIVEN", value );
    const bool applied = rocketnet_config_apply_env( "client", &config, fields, num_fields );
    env_names_unsetenv( "ROCKETNET_CLIENT_STEP_DRIVEN" );
    check( applied );
    return rocketnet_config_validate( "client", &config, fields, num_fields );
}

static void test_config_env_step_driven_is_zero_or_one()
{
    check( env_names_step_driven_valid( "0" ) );
    check( env_names_step_driven_valid( "1" ) );
    check( !env_names_step_driven_valid( "2" ) );
    check( !env_names_step_driven_valid( "18446744073709551615" ) );
}
REGISTER_UNIT_TEST( test_config_env_step_driven_is_zero_or_one );


// ---------------------------------------------------------------------------
// the development simulator variables (rn_dev_env_int, sdk/rocketnet_api.cpp)
// take the config system's strict charset: ASCII digits only, so " 5" and
// "+5" are refused as every other numeric override refuses them
// (rocketnet_config_numeric_charset, sdk/rocketnet_config.cpp). A production
// library does not read them, and every spelling resolves.
// ---------------------------------------------------------------------------

static bool env_names_dev_latency_resolves( bool server, const char * value, int * latency_ms )
{
    const char * name = server ? "ROCKETNET_DEV_SERVER_LATENCY_MS" : "ROCKETNET_DEV_CLIENT_LATENCY_MS";
    env_names_setenv( name, value );
    env_names_capture_t capture;
    env_names_capture_begin( &capture );
    struct rocketnet_client_transport_config_t client_transport;
    struct rocketnet_server_transport_config_t server_transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    bool resolved;
    if ( server )
    {
        resolved = rocketnet_server_config_resolve( NULL, true, &server_transport, &message, &kbps );
        *latency_ms = server_transport.simulated_latency_ms;
    }
    else
    {
        resolved = rocketnet_client_config_resolve( NULL, true, &client_transport, &message, &kbps );
        *latency_ms = client_transport.simulated_latency_ms;
    }
    env_names_capture_end( &capture );
    env_names_unsetenv( name );
    return resolved;
}

static void test_config_env_dev_simulator_numbers_are_strict()
{
    for ( int side = 0; side < 2; side++ )
    {
        const bool server = side == 1;
        int latency_ms = -1;
#if ROCKETNET_DEV
        check( !env_names_dev_latency_resolves( server, " 5", &latency_ms ) );
        check( !env_names_dev_latency_resolves( server, "+5", &latency_ms ) );
        check( env_names_dev_latency_resolves( server, "5", &latency_ms ) );
        check( latency_ms == 5 );
#else // #if ROCKETNET_DEV
        check( env_names_dev_latency_resolves( server, " 5", &latency_ms ) );
        check( env_names_dev_latency_resolves( server, "+5", &latency_ms ) );
        check( env_names_dev_latency_resolves( server, "5", &latency_ms ) );
#endif // #if ROCKETNET_DEV
    }
}
REGISTER_UNIT_TEST( test_config_env_dev_simulator_numbers_are_strict );

// ---------------------------------------------------------------------------
// (4) transport passes also exercise development, internal and operator rows:
// SIMULATED_*, GAME_CLOCK_EPOCH, and MEMORY_BUDGET_BYTES (sdk/rocketnet_client_transport.cpp:583-586,652-653
// and sdk/rocketnet_server_transport.cpp rocketnet_config_apply_env and
// rocketnet_config_apply_env_uint64)
// ---------------------------------------------------------------------------

static void test_config_env_transport_passes_exercise_all_rows()
{
    // 1. Internal & simulator rows:
    env_names_setenv( "ROCKETNET_CLIENT_GAME_CLOCK_EPOCH", "2000" );
    env_names_setenv( "ROCKETNET_SERVER_GAME_CLOCK_EPOCH", "3000" );
    env_names_setenv( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH", "10" );
    env_names_setenv( "ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH", "20" );
    env_names_setenv( "ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW", "8" );
    env_names_setenv( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW", "16" );
    env_names_setenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", "104857600" );
    env_names_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "209715200" );

    env_names_capture_t capture;
    env_names_capture_begin( &capture );

    int num_client_fields = 0;
    const struct rocketnet_config_field_t * client_fields = rocketnet_client_transport_config_fields( &num_client_fields );
    struct rocketnet_client_transport_config_t client_transport;
    rocketnet_client_transport_default_config_current( &client_transport );
    const bool client_applied = rocketnet_config_apply_env( "client", &client_transport, client_fields, num_client_fields );

    int num_server_fields = 0;
    const struct rocketnet_config_field_t * server_fields = rocketnet_server_transport_config_fields( &num_server_fields );
    struct rocketnet_server_transport_config_t server_transport;
    rocketnet_server_transport_default_config_current( &server_transport );
    const bool server_applied = rocketnet_config_apply_env( "server", &server_transport, server_fields, num_server_fields );

    uint64_t client_budget = 0;
    const bool client_budget_ok = rocketnet_config_apply_env_uint64( "client", "memory_budget_bytes", &client_budget );
    uint64_t server_budget = 0;
    const bool server_budget_ok = rocketnet_config_apply_env_uint64( "server", "memory_budget_bytes", &server_budget );

    env_names_capture_end( &capture );

    env_names_unsetenv( "ROCKETNET_CLIENT_GAME_CLOCK_EPOCH" );
    env_names_unsetenv( "ROCKETNET_SERVER_GAME_CLOCK_EPOCH" );
    env_names_unsetenv( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH" );
    env_names_unsetenv( "ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH" );
    env_names_unsetenv( "ROCKETNET_CLIENT_SIMULATED_REVERSE_WINDOW" );
    env_names_unsetenv( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW" );
    env_names_unsetenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES" );
    env_names_unsetenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" );

    check( client_applied );
    check( server_applied );
    check( client_budget_ok && client_budget == 104857600 );
    check( server_budget_ok && server_budget == 209715200 );

    check( client_transport.game_clock_epoch == 2000 );
    check( client_transport.simulated_drop_every_nth == 10 );
    check( client_transport.simulated_reverse_window == 8 );

    check( server_transport.game_clock_epoch == 3000 );
    check( server_transport.simulated_drop_every_nth == 20 );
    check( server_transport.simulated_reverse_window == 16 );

    const std::vector<std::string> overrides = env_names_overrides( capture.log );
    check( env_names_contains( overrides, "ROCKETNET_CLIENT_GAME_CLOCK_EPOCH" ) );
    check( env_names_contains( overrides, "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH" ) );
    check( env_names_contains( overrides, "ROCKETNET_SERVER_SIMULATED_DROP_EVERY_NTH" ) );
    check( env_names_contains( overrides, "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW" ) );
}
REGISTER_UNIT_TEST( test_config_env_transport_passes_exercise_all_rows );

// the memory budget reader (rocketnet_config_apply_env_uint64) logs like the
// table pass: a set value logs one override line naming the variable and the
// displaced value, a set-but-empty value logs one ignores-empty line, and an
// unset variable logs nothing
// ---------------------------------------------------------------------------

static int env_names_count_named( const env_names_log_t & log, const char * name, const char * phrase )
{
    int count = 0;
    for ( size_t i = 0; i < log.lines.size(); i++ )
        if ( strstr( log.lines[i].c_str(), name ) && strstr( log.lines[i].c_str(), phrase ) )
            count++;
    return count;
}

static void test_config_env_memory_budget_logs_its_override()
{
    // client: a set value assigns and logs exactly one override line
    env_names_setenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", "104857600" );
    uint64_t client_budget = 111;
    env_names_capture_t client_value_capture;
    env_names_capture_begin( &client_value_capture );
    const bool client_value_ok = rocketnet_config_apply_env_uint64( "client", "memory_budget_bytes", &client_budget );
    env_names_capture_end( &client_value_capture );

    // client: a set-but-empty value leaves the value and logs ignores empty
    env_names_setenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", "" );
    uint64_t client_empty_budget = 222;
    env_names_capture_t client_empty_capture;
    env_names_capture_begin( &client_empty_capture );
    const bool client_empty_ok = rocketnet_config_apply_env_uint64( "client", "memory_budget_bytes", &client_empty_budget );
    env_names_capture_end( &client_empty_capture );

    // server: a set value assigns and logs exactly one override line
    env_names_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "209715200" );
    uint64_t server_budget = 333;
    env_names_capture_t server_value_capture;
    env_names_capture_begin( &server_value_capture );
    const bool server_value_ok = rocketnet_config_apply_env_uint64( "server", "memory_budget_bytes", &server_budget );
    env_names_capture_end( &server_value_capture );

    // server: a set-but-empty value leaves the value and logs ignores empty
    env_names_setenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "" );
    uint64_t server_empty_budget = 444;
    env_names_capture_t server_empty_capture;
    env_names_capture_begin( &server_empty_capture );
    const bool server_empty_ok = rocketnet_config_apply_env_uint64( "server", "memory_budget_bytes", &server_empty_budget );
    env_names_capture_end( &server_empty_capture );

    // unset: nothing is logged and the value is left alone
    env_names_unsetenv( "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES" );
    env_names_unsetenv( "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES" );
    uint64_t unset_budget = 555;
    env_names_capture_t unset_capture;
    env_names_capture_begin( &unset_capture );
    const bool unset_client_ok = rocketnet_config_apply_env_uint64( "client", "memory_budget_bytes", &unset_budget );
    const bool unset_server_ok = rocketnet_config_apply_env_uint64( "server", "memory_budget_bytes", &unset_budget );
    env_names_capture_end( &unset_capture );

    check( client_value_ok && client_budget == 104857600 );
    check( env_names_count_named( client_value_capture.log, "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", "config override" ) == 1 );
    check( env_names_count_named( client_value_capture.log, "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", "(displaces 111)" ) == 1 );
    check( client_empty_ok && client_empty_budget == 222 );
    check( env_names_count_named( client_empty_capture.log, "ROCKETNET_CLIENT_MEMORY_BUDGET_BYTES", "ignores empty" ) == 1 );

    check( server_value_ok && server_budget == 209715200 );
    check( env_names_count_named( server_value_capture.log, "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "config override" ) == 1 );
    check( env_names_count_named( server_value_capture.log, "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "(displaces 333)" ) == 1 );
    check( server_empty_ok && server_empty_budget == 444 );
    check( env_names_count_named( server_empty_capture.log, "ROCKETNET_SERVER_MEMORY_BUDGET_BYTES", "ignores empty" ) == 1 );

    check( unset_client_ok && unset_server_ok && unset_budget == 555 );
    check( unset_capture.log.lines.empty() );
}
REGISTER_UNIT_TEST( test_config_env_memory_budget_logs_its_override );
