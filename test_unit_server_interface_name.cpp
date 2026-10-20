/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server config interface_name length check: rn_server_split
// (sdk/rocketnet_api.cpp) copies the field into the transport config's
// 16-byte interface_name row with rn_copy_config_string, so a name of IFNAMSIZ
// (16) bytes or more is refused at validation with
// ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE and one error line naming the field;
// NULL, "" and a 15-byte name pass. The env form, ROCKETNET_SERVER_INTERFACE_NAME,
// is refused by the server transport table's string row
// (rocketnet_config_apply_env, sdk/rocketnet_config.cpp). Every case runs in
// memory, with no clock, sockets or network.

#include "test_unit_registry.h"

#include "rocketnet.h"
#include "rocketnet_internal.h"
#include "rocketnet_config.h"

#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

static int g_interface_name_errors = 0;
static int g_interface_name_named = 0;
static int g_interface_name_env_named = 0;

static void interface_name_log_capture( void *, rocketnet_log_level_t level, const char * message, size_t length )
{
    if ( level != ROCKETNET_LOG_LEVEL_ERROR )
        return;
    g_interface_name_errors++;
    char line[256];
    const size_t n = length < sizeof( line ) - 1 ? length : sizeof( line ) - 1;
    memcpy( line, message, n );
    line[n] = 0;
    if ( strstr( line, "interface_name" ) )
        g_interface_name_named++;
    if ( strstr( line, "ROCKETNET_SERVER_INTERFACE_NAME" ) )
        g_interface_name_env_named++;
}

static int interface_name_setenv( const char * value )
{
#ifdef _WIN32
    return _putenv_s( "ROCKETNET_SERVER_INTERFACE_NAME", value ? value : "" );
#else
    return value ? setenv( "ROCKETNET_SERVER_INTERFACE_NAME", value, 1 ) : unsetenv( "ROCKETNET_SERVER_INTERFACE_NAME" );
#endif
}

static enum rocketnet_status_t interface_name_validate( const char * name, bool sized )
{
    struct rocketnet_server_config_t config;
    check( rocketnet_server_dev_config_sized( &config, sizeof( config ) ) == ROCKETNET_STATUS_OK );
    config.interface_name = name;
    return sized ? rocketnet_server_validate_config_sized( &config, sizeof( config ) ) : rocketnet_server_validate_config( &config );
}

static void server_config_interface_name_refused_at_ifnamsiz()
{
    char fifteen[16];
    memset( fifteen, 0x65, 15 );
    fifteen[15] = 0;
    char sixteen[17];
    memset( sixteen, 0x65, 16 );
    sixteen[16] = 0;

    // the defaults leave the field NULL
    struct rocketnet_server_config_t defaults;
    memset( &defaults, 0xAB, sizeof( defaults ) );
    check( rocketnet_server_default_config_sized( &defaults, sizeof( defaults ) ) == ROCKETNET_STATUS_OK );
    check( defaults.interface_name == NULL );

    const rocketnet_log_level_t saved = rocketnet_get_log_level();
    rocketnet_set_log_level( ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_set_log_callback( interface_name_log_capture, NULL );

    for ( int sized = 0; sized < 2; sized++ )
    {
        g_interface_name_errors = 0;
        g_interface_name_named = 0;
        check( interface_name_validate( NULL, sized != 0 ) == ROCKETNET_STATUS_OK );
        check( interface_name_validate( "", sized != 0 ) == ROCKETNET_STATUS_OK );
        check( interface_name_validate( fifteen, sized != 0 ) == ROCKETNET_STATUS_OK );
        check( g_interface_name_errors == 0 );

        // IFNAMSIZ bytes: refused, with one error line naming the field
        check( interface_name_validate( sixteen, sized != 0 ) == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );
        check( g_interface_name_errors == 1 );
        check( g_interface_name_named == 1 );
    }

    // the env form: the server transport table refuses IFNAMSIZ bytes, one error line naming the variable
    const char * prior = getenv( "ROCKETNET_SERVER_INTERFACE_NAME" );
    char prior_copy[64] = { 0 };
    const bool had_prior = prior != NULL && strlen( prior ) < sizeof( prior_copy );
    if ( had_prior )
        memcpy( prior_copy, prior, strlen( prior ) );
    int num_fields = 0;
    const struct rocketnet_config_field_t * fields = rocketnet_server_transport_config_fields( &num_fields );
    struct rocketnet_server_transport_config_t transport;
    rocketnet_server_transport_default_config( &transport );
    check( transport.interface_name[0] == 0 );

    g_interface_name_errors = 0;
    g_interface_name_env_named = 0;
    check( interface_name_setenv( fifteen ) == 0 );
    check( rocketnet_config_apply_env( "server", &transport, fields, num_fields ) );
    check( strcmp( transport.interface_name, fifteen ) == 0 );
    check( interface_name_setenv( sixteen ) == 0 );
    check( !rocketnet_config_apply_env( "server", &transport, fields, num_fields ) );
    check( g_interface_name_errors == 1 );
    check( g_interface_name_env_named == 1 );
    interface_name_setenv( had_prior ? prior_copy : NULL );

    rocketnet_set_log_callback( NULL, NULL );
    rocketnet_set_log_level( saved );
}
REGISTER_UNIT_TEST( server_config_interface_name_refused_at_ifnamsiz );
