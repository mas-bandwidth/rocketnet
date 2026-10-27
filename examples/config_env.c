/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    config_env: configuration precedence, default < struct < environment. It
    prints two server defaults, sets them in the config struct and creates a
    server, then shows the matching ROCKETNET_ environment variable winning,
    with the library's override log line, and a bad value refused.

    The C standard library cannot set an environment variable in-process, so
    the example runs itself again with system() and ROCKETNET_SERVER_MAX_CLIENTS
    set, once to 5 and once to 99999. Run: ./bin/example_config_env
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates getenv

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _WIN32 )
#define RERUN "set ROCKETNET_SERVER_MAX_CLIENTS=%s&& \"%s\""
#else
#define RERUN "ROCKETNET_SERVER_MAX_CLIENTS=%s %s"
#endif

int main( int argc, char ** argv )
{
    (void) argc;

    const char * environment = getenv( "ROCKETNET_SERVER_MAX_CLIENTS" );
    if ( !environment )
        environment = "";

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    config.transport = "loopback";
    if ( environment[0] == '\0' )
    {
        printf( "default max_clients: %d\n", config.max_clients );
        printf( "default max_timeout_seconds: %d\n", config.max_timeout_seconds );
    }
    config.max_clients = 16;
    config.max_timeout_seconds = 90;

    // create reads the environment, which overrides the struct
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );

    int result = 1;
    if ( environment[0] == '\0' && server )
    {
        printf( "struct max_clients: %d\n", rocketnet_server_max_clients( server ) );

        // run again with the variable set: a good value, then one out of range
        const char * const values[2] = { "5", "99999" };
        int children = 0;
        for ( int i = 0; i < 2; i++ )
        {
            char command[512];
            snprintf( command, sizeof( command ), RERUN, values[i], argv[0] );
            children += system( command );
        }
        result = children == 0 ? 0 : 1;
    }
    else if ( strcmp( environment, "5" ) == 0 && server )
    {
        printf( "environment max_clients: %d\n", rocketnet_server_max_clients( server ) );
        result = rocketnet_server_max_clients( server ) == 5 ? 0 : 1;
    }
    else if ( environment[0] != '\0' && !server )
    {
        printf( "bad value refused: %s\n", rocketnet_status_name( status ) );
        result = 0;
    }

    rn_cli_server_stop_and_destroy( server );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }
    if ( result != 0 )
        printf( "FAIL: max_clients is the default, then the struct, then the environment\n" );
    else if ( environment[0] == '\0' )
        printf( "PASS\n" );
    return result;
}
