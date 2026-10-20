/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    config_env: configuration precedence, default < struct < environment. It
    prints a server field's default, sets it in the config struct and creates,
    then shows the matching ROCKETNET_ environment variable winning, with the
    library's override log line, and finally a bad value refused with the
    config-invalid status.

    The C standard library cannot set an environment variable in-process, and
    the header offers no config-read call that takes an explicit environment,
    so the example re-runs itself with the variable set: it spawns a child
    with system() and ROCKETNET_SERVER_MAX_CLIENTS set, and that child shows
    the override and the refusal. The example's own environment is unchanged.

    Run it from the repository root: ./bin/example_config_env
*/

#include "rocketnet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// Show the default and the struct value: print two defaults from the dev
// config, set them, and prove create honors the struct value.
static int run_show_default_and_struct( void )
{
    struct rocketnet_server_config_t config;
    struct rocketnet_server_t * server = NULL;
    int result = 1;

    rocketnet_server_dev_config( &config );

    printf( "default max_clients: %d\n", config.max_clients );
    printf( "default max_timeout_seconds: %d\n", config.max_timeout_seconds );

    config.max_clients = 16;
    config.max_timeout_seconds = 90;
    config.transport = "loopback";

    server = rocketnet_server_create( NULL, "127.0.0.1:0", &config, NULL, 0 );
    CHECK( server != NULL );
    printf( "struct max_clients: %d\n", rocketnet_server_max_clients( server ) );

    rocketnet_server_destroy( server );
    server = NULL;
    result = 0;
cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (sdk/rocketnet_internal.h:314)
    if ( server )
        rocketnet_server_destroy( server );
    return result;
}

// Run by a child with ROCKETNET_SERVER_MAX_CLIENTS=5: the environment value
// wins over the struct's 16, with the library's override log line.
static int run_show_env_wins( void )
{
    struct rocketnet_server_config_t config;
    struct rocketnet_server_t * server = NULL;
    int result = 1;

    rocketnet_server_dev_config( &config );
    config.max_clients = 16;
    config.transport = "loopback";

    server = rocketnet_server_create( NULL, "127.0.0.1:0", &config, NULL, 0 );
    CHECK( server != NULL );
    printf( "environment max_clients: %d\n", rocketnet_server_max_clients( server ) );
    CHECK( rocketnet_server_max_clients( server ) == 5 );

    rocketnet_server_destroy( server );
    server = NULL;
    result = 0;
cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (sdk/rocketnet_internal.h:314)
    if ( server )
        rocketnet_server_destroy( server );
    return result;
}

// Run by a child with ROCKETNET_SERVER_MAX_CLIENTS=99999 (out of range): the
// create is refused with the config-invalid status and its log line.
static int run_show_bad_refused( void )
{
    struct rocketnet_server_config_t config;
    struct rocketnet_server_t * server = NULL;
    int result = 1;

    rocketnet_server_dev_config( &config );
    config.max_clients = 16;
    config.transport = "loopback";

    server = rocketnet_server_create( NULL, "127.0.0.1:0", &config, NULL, 0 );
    CHECK( server == NULL );
    printf( "bad value refused: %s\n", rocketnet_status_name( ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE ) );

    result = 0;
cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (sdk/rocketnet_internal.h:314)
    if ( server )
        rocketnet_server_destroy( server );
    return result;
}

int main( int argc, char ** argv )
{
    int result = 1;
    const char * env = getenv( "ROCKETNET_SERVER_MAX_CLIENTS" );

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );

    if ( !env )
    {
        char command[512];

        // default and struct first, then spawn the environment demo children
        CHECK( run_show_default_and_struct() == 0 );

#if defined( _WIN32 )
        snprintf( command, sizeof( command ), "set ROCKETNET_SERVER_MAX_CLIENTS=5&& \"%s\"", argv[0] );
#else
        snprintf( command, sizeof( command ), "ROCKETNET_SERVER_MAX_CLIENTS=5 %s", argv[0] );
#endif
        CHECK( system( command ) == 0 );

#if defined( _WIN32 )
        snprintf( command, sizeof( command ), "set ROCKETNET_SERVER_MAX_CLIENTS=99999&& \"%s\"", argv[0] );
#else
        snprintf( command, sizeof( command ), "ROCKETNET_SERVER_MAX_CLIENTS=99999 %s", argv[0] );
#endif
        CHECK( system( command ) == 0 );

        printf( "PASS\n" );
        result = 0;
    }
    else if ( strcmp( env, "5" ) == 0 )
    {
        CHECK( run_show_env_wins() == 0 );
        result = 0;
    }
    else
    {
        CHECK( run_show_bad_refused() == 0 );
        result = 0;
    }

cleanup:
    rocketnet_term();
    return result;
}
