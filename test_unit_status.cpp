/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The status code surface: the enum and its names
// (docs/ERRORS-AND-LOGGING.md 2.1). OK is 0. The failures are -1, -2, ...
// with no gap. The table here is the numbering and spelling: a code that
// moves, a name that changes and a name that is missing all fail. A value
// that is no code names "UNKNOWN" and never asserts.
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
    { ROCKETNET_STATUS_DEV_KEYS_IN_PRODUCTION, "DEV_KEYS_IN_PRODUCTION" },
    { ROCKETNET_STATUS_INVALID_ADDRESS, "INVALID_ADDRESS" },
    { ROCKETNET_STATUS_INVALID_TRANSPORT_LIST, "INVALID_TRANSPORT_LIST" },
    { ROCKETNET_STATUS_INVALID_CONNECT_TOKEN, "INVALID_CONNECT_TOKEN" },
    { ROCKETNET_STATUS_CONNECT_TOKEN_EXPIRED, "CONNECT_TOKEN_EXPIRED" },
    { ROCKETNET_STATUS_TRANSPORT_PORT_ZERO_UNSUPPORTED, "TRANSPORT_PORT_ZERO_UNSUPPORTED" },
    { ROCKETNET_STATUS_TRANSPORT_LADDER_EXHAUSTED, "TRANSPORT_LADDER_EXHAUSTED" },
    { ROCKETNET_STATUS_OUT_OF_MEMORY, "OUT_OF_MEMORY" },
    { ROCKETNET_STATUS_MUTEX_CREATE_FAILED, "MUTEX_CREATE_FAILED" },
    { ROCKETNET_STATUS_THREAD_CREATE_FAILED, "THREAD_CREATE_FAILED" },
    { ROCKETNET_STATUS_TIMER_CREATE_FAILED, "TIMER_CREATE_FAILED" },
    { ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED, "CLIENT_NOT_DISCONNECTED" },
    { ROCKETNET_STATUS_SERVER_NOT_STOPPED, "SERVER_NOT_STOPPED" },
    { ROCKETNET_STATUS_FLOW_ID_RESERVED, "FLOW_ID_RESERVED" },
    { ROCKETNET_STATUS_TRANSPORT_FAMILIES_UNSUPPORTED, "TRANSPORT_FAMILIES_UNSUPPORTED" },
    { ROCKETNET_STATUS_CONFIG_NOT_INITIALIZED, "CONFIG_NOT_INITIALIZED" },
    { ROCKETNET_STATUS_TEST_PACKET_FILTER_IN_PRODUCTION, "TEST_PACKET_FILTER_IN_PRODUCTION" },
    { ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION, "DEV_CONNECT_TOKEN_IN_PRODUCTION" },
};

static void test_status_names_cover_every_code()
{
    const int num_codes = (int) ( sizeof( status_expected ) / sizeof( status_expected[0] ) );
    check( ROCKETNET_STATUS_OK == 0 );
    check( ROCKETNET_STATUS_DEV_CONNECT_TOKEN_IN_PRODUCTION == -29 );
    check( num_codes == 30 );

    for ( int i = 0; i < num_codes; i++ )
    {
        check( status_expected[i].code == -i );

        const char * name = rocketnet_status_name( (enum rocketnet_status_t) status_expected[i].code );
        check( name != NULL );
        check( name[0] != '\0' );
        check( strcmp( name, status_expected[i].name ) == 0 );
        check( strncmp( name, "ROCKETNET_STATUS_", 17 ) != 0 );

        for ( int j = 0; j < i; j++ )
            check( strcmp( name, status_expected[j].name ) != 0 );
    }
}

REGISTER_UNIT_TEST( test_status_names_cover_every_code );

static void test_status_name_out_of_range_is_unknown()
{
    check( strcmp( rocketnet_status_name( (enum rocketnet_status_t) 1 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_status_name( (enum rocketnet_status_t) -30 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_status_name( (enum rocketnet_status_t) -31 ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_status_name( (enum rocketnet_status_t) INT_MIN ), "UNKNOWN" ) == 0 );
    check( strcmp( rocketnet_status_name( (enum rocketnet_status_t) INT_MAX ), "UNKNOWN" ) == 0 );
}

REGISTER_UNIT_TEST( test_status_name_out_of_range_is_unknown );

// An env override is parsed as a 32-bit integer, refused past INT32 range
// or on ERANGE, leaving the output untouched on refusal
// (sdk/rocketnet_config.cpp, rocketnet_config_parse_int_value).
static void test_config_int_parse_refuses_out_of_range()
{
    int out = 77;

    check( rocketnet_config_parse_int_value( "2147483647", &out ) );
    check( out == 2147483647 );

    check( rocketnet_config_parse_int_value( "-2147483648", &out ) );
    check( out == -2147483647 - 1 );

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

REGISTER_UNIT_TEST( test_config_int_parse_refuses_out_of_range );

// An env var name is built into a caller buffer, refused when it does not
// fit, the terminator counted (sdk/rocketnet_config.cpp,
// rocketnet_config_env_var_name and rocketnet_config_env_var_name_for).
static void test_config_env_var_names_refuse_short_buffers()
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

REGISTER_UNIT_TEST( test_config_env_var_names_refuse_short_buffers );
