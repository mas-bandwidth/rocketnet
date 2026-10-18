/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The status code surface: the stable enum, its count and its names
// (docs/ERRORS-AND-LOGGING.md 2.1). The table here is the published numbering
// and spelling: a code that moves, a name that changes and a name that is
// missing all fail. Out-of-range lookups return NULL and never assert.
//
// Every case runs in memory, with no clock, sockets or network.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_config.h"

#include <stdio.h>
#include <string.h>

#include <limits.h>

#define check rocketnet_unit_check

static const struct
{
    int code;
    const char * name;
} status_expected[] =
{
    { ROCKETNET_STATUS_OK, "OK" },
    { ROCKETNET_STATUS_ALREADY_INITIALIZED, "ALREADY_INITIALIZED" },
    { ROCKETNET_STATUS_NOT_INITIALIZED, "NOT_INITIALIZED" },
    { ROCKETNET_STATUS_LIVE_ALLOCATIONS, "LIVE_ALLOCATIONS" },
    { ROCKETNET_STATUS_PLATFORM_INIT_FAILED, "PLATFORM_INIT_FAILED" },
    { ROCKETNET_STATUS_CRYPTO_INIT_FAILED, "CRYPTO_INIT_FAILED" },
    { ROCKETNET_STATUS_INVALID_ARGUMENT, "INVALID_ARGUMENT" },
    { ROCKETNET_STATUS_CONFIG_ENV_INVALID, "CONFIG_ENV_INVALID" },
    { ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE, "CONFIG_OUT_OF_RANGE" },
    { ROCKETNET_STATUS_MEMORY_BUDGET_EXCEEDED, "MEMORY_BUDGET_EXCEEDED" },
    { ROCKETNET_STATUS_MISSING_KEYS, "MISSING_KEYS" },
    { ROCKETNET_STATUS_INVALID_PROTOCOL_ID, "INVALID_PROTOCOL_ID" },
    { ROCKETNET_STATUS_DEV_KEYS_IN_RELEASE, "DEV_KEYS_IN_RELEASE" },
    { ROCKETNET_STATUS_INVALID_ADDRESS, "INVALID_ADDRESS" },
    { ROCKETNET_STATUS_INVALID_DRIVER_LIST, "INVALID_DRIVER_LIST" },
    { ROCKETNET_STATUS_INVALID_CONNECT_TOKEN, "INVALID_CONNECT_TOKEN" },
    { ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED, "CONNECT_TOKEN_EXPIRED" },
    { ROCKETNET_STATUS_DRIVER_PORT_ZERO_UNSUPPORTED, "DRIVER_PORT_ZERO_UNSUPPORTED" },
    { ROCKETNET_STATUS_DRIVER_LADDER_EXHAUSTED, "DRIVER_LADDER_EXHAUSTED" },
    { ROCKETNET_STATUS_OUT_OF_MEMORY, "OUT_OF_MEMORY" },
    { ROCKETNET_STATUS_MUTEX_CREATE_FAILED, "MUTEX_CREATE_FAILED" },
    { ROCKETNET_STATUS_THREAD_CREATE_FAILED, "THREAD_CREATE_FAILED" },
    { ROCKETNET_STATUS_TIMER_CREATE_FAILED, "TIMER_CREATE_FAILED" },
};

static void status_strings_cover_every_code()
{
    check( ROCKETNET_STATUS_OK == 0 );
    check( ROCKETNET_NUM_STATUS_CODES == 23 );
    check( rocketnet_num_status_codes() == ROCKETNET_NUM_STATUS_CODES );
    check( sizeof( status_expected ) / sizeof( status_expected[0] ) == ROCKETNET_NUM_STATUS_CODES );

    for ( int i = 0; i < ROCKETNET_NUM_STATUS_CODES; i++ )
    {
        check( status_expected[i].code == i );

        const char * name = rocketnet_status_string( status_expected[i].code );
        check( name != NULL );
        check( name[0] != '\0' );
        check( strcmp( name, status_expected[i].name ) == 0 );
        check( strncmp( name, "ROCKETNET_STATUS_", 17 ) != 0 );

        for ( int j = 0; j < i; j++ )
            check( strcmp( name, status_expected[j].name ) != 0 );
    }
}

REGISTER_UNIT_TEST( status_strings_cover_every_code );

static void status_string_out_of_range_is_null()
{
    check( rocketnet_status_string( -1 ) == NULL );
    check( rocketnet_status_string( ROCKETNET_NUM_STATUS_CODES ) == NULL );
    check( rocketnet_status_string( ROCKETNET_NUM_STATUS_CODES + 1 ) == NULL );
    check( rocketnet_status_string( INT_MIN ) == NULL );
    check( rocketnet_status_string( INT_MAX ) == NULL );
}

REGISTER_UNIT_TEST( status_string_out_of_range_is_null );

// An env override is parsed as a 32-bit integer, refused past INT32 range
// or on ERANGE, leaving the output untouched on refusal
// (sdk/rocketnet_config.cpp, rocketnet_config_parse_int_value).
static void config_int_parse_refuses_out_of_range()
{
    int out = 77;

    check( rocketnet_config_parse_int_value( "2147483647", &out ) );
    check( out == 2147483647 );

    check( rocketnet_config_parse_int_value( "-2147483648", &out ) );
    check( out == -2147483648 );

    out = 77;
    check( !rocketnet_config_parse_int_value( "2147483648", &out ) );
    check( out == 77 );

    out = 77;
    check( !rocketnet_config_parse_int_value( "-2147483649", &out ) );
    check( out == 77 );

    out = 77;
    check( !rocketnet_config_parse_int_value( "99999999999999999999", &out ) );
    check( out == 77 );

    out = 77;
    check( !rocketnet_config_parse_int_value( "", &out ) );
    check( out == 77 );

    out = 77;
    check( !rocketnet_config_parse_int_value( "12x", &out ) );
    check( out == 77 );
}

REGISTER_UNIT_TEST( config_int_parse_refuses_out_of_range );

// An env var name is built into a caller buffer, refused when it does not
// fit, the terminator counted (sdk/rocketnet_config.cpp,
// rocketnet_config_env_var_name and rocketnet_config_env_var_name_for).
static void config_env_var_names_refuse_short_buffers()
{
    char buffer[128];

    check( !rocketnet_config_env_var_name( "max_clients", buffer, (int) strlen( "ROCKETNET_MAX_CLIENTS" ) ) );

    check( rocketnet_config_env_var_name( "max_clients", buffer, (int) strlen( "ROCKETNET_MAX_CLIENTS" ) + 1 ) );
    check( strcmp( buffer, "ROCKETNET_MAX_CLIENTS" ) == 0 );

    check( !rocketnet_config_env_var_name_for( "server", "max_clients", buffer, (int) strlen( "ROCKETNET_SERVER_MAX_CLIENTS" ) ) );

    check( rocketnet_config_env_var_name_for( "server", "max_clients", buffer, (int) strlen( "ROCKETNET_SERVER_MAX_CLIENTS" ) + 1 ) );
    check( strcmp( buffer, "ROCKETNET_SERVER_MAX_CLIENTS" ) == 0 );

    const char long_transport[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    check( !rocketnet_config_env_var_name_for( long_transport, "max_clients", buffer, (int) sizeof( buffer ) ) );
}

REGISTER_UNIT_TEST( config_env_var_names_refuse_short_buffers );
