/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
// Mutation gaps (delta, core): the line rocketnet_transport_ladder_parse
// (sdk/rocketnet_config.cpp) logs for a known transport it skips. A pin-only
// transport named inside a longer list is skipped as pin-only; a transport
// this platform does not have is skipped as not available. The availability
// table is the test's own, so the case is the same in every build. No socket,
// no thread, no real time.

#include "test_unit_registry.h"
#include "rocketnet.h"
#include "rocketnet_config.h"
#include "rocketnet_driver.h"
#include "rocketnet_layers.h"

#include <string.h>

struct gaps_config_log_t
{
    int pin_only_lines;
    int not_available_lines;
};

static void gaps_config_log_capture( void * context, rocketnet_log_level_t, const char * message, size_t length )
{
    gaps_config_log_t * log = (gaps_config_log_t *) context;
    char line[512];
    const size_t bytes = length < sizeof( line ) - 1 ? length : sizeof( line ) - 1;
    memcpy( line, message, bytes );
    line[bytes] = '\0';
    if ( strstr( line, "is pin-only" ) != NULL )
    {
        log->pin_only_lines++;
    }
    if ( strstr( line, "is not available here" ) != NULL )
    {
        log->not_available_lines++;
    }
}

static bool gaps_config_parse_logged( const char * value, gaps_config_log_t * log, rocketnet_transport_ladder_t * ladder )
{
    const int available[2] = { ROCKETNET_TRANSPORT_PORTABLE, ROCKETNET_TRANSPORT_LOOPBACK };

    memset( log, 0, sizeof( *log ) );
    const rocketnet_log_level_t level = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_log_function( gaps_config_log_capture, log );
    const bool parsed = rocketnet_transport_ladder_parse( value, available, 2, ladder, true );
    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( level );
    return parsed;
}

// mutation gap: delta sdk/rocketnet_config.cpp:1146:13:negate_condition (if ( pin_only_in_list ))

static void test_gaps_delta_core_config_skipped_transport_logs_its_reason()
{
    rocketnet_unit_check( rocketnet_transport_pin_only( ROCKETNET_TRANSPORT_LOOPBACK, true ) );

    gaps_config_log_t log;
    rocketnet_transport_ladder_t ladder;

    // loopback is here but pin-only: skipped as pin-only, never as absent

    rocketnet_unit_check( gaps_config_parse_logged( "portable,loopback", &log, &ladder ) );
    rocketnet_unit_check( ladder.num_candidates == 1 && ladder.candidates[0] == ROCKETNET_TRANSPORT_PORTABLE );
    rocketnet_unit_check( ladder.num_skipped == 1 && ladder.skipped[0] == ROCKETNET_TRANSPORT_LOOPBACK );
    const bool pin_only_skip_logged_as_pin_only = log.pin_only_lines == 1 && log.not_available_lines == 0;
    rocketnet_unit_check( pin_only_skip_logged_as_pin_only );

    // gso is not in this table: skipped as not available, never as pin-only

    rocketnet_unit_check( gaps_config_parse_logged( "portable,gso", &log, &ladder ) );
    rocketnet_unit_check( ladder.num_candidates == 1 && ladder.candidates[0] == ROCKETNET_TRANSPORT_PORTABLE );
    rocketnet_unit_check( ladder.num_skipped == 1 && ladder.skipped[0] == ROCKETNET_TRANSPORT_GSO );
    const bool absent_skip_logged_as_not_available = log.not_available_lines == 1 && log.pin_only_lines == 0;
    rocketnet_unit_check( absent_skip_logged_as_not_available );
}

REGISTER_UNIT_TEST( test_gaps_delta_core_config_skipped_transport_logs_its_reason );
