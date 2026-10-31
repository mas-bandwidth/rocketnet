/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    config_env: configuration precedence, default < struct < environment. It
    prints two server defaults, sets them in the config struct and creates a
    server, then shows the matching ROCKETNET_ environment variable winning,
    with the library's override log line, and a bad value refused.

    The C standard library cannot set an environment variable in-process, so
    the example runs itself again as a child process whose environment is this
    one with ROCKETNET_SERVER_MAX_CLIENTS set, once to 5 and once to 99999. The
    child is spawned from its argument vector (posix_spawnp, or _spawnve on
    Windows), never through a shell, so a path with spaces or shell
    characters runs as it is. Run: ./bin/example_config_env
*/

#define _CRT_SECURE_NO_WARNINGS     // MSVC deprecates getenv

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _WIN32 )
#include <process.h>
#define child_environ _environ
#else
#include <spawn.h>
#include <sys/wait.h>
extern char ** environ;
#define child_environ environ
#endif // #if defined( _WIN32 )

// runs this program again with this environment plus ROCKETNET_SERVER_MAX_CLIENTS
// set to value, and waits for it: 0 when the child exits 0, else 1

static int rerun_with_max_clients( char * program, const char * value )
{
    static const char name[] = "ROCKETNET_SERVER_MAX_CLIENTS=";

    int count = 0;
    while ( child_environ[count] )
        count++;

    char ** environment = (char **) calloc( (size_t) count + 2, sizeof( char * ) );
    char * variable = (char *) malloc( sizeof( name ) + strlen( value ) );
    if ( !environment || !variable )
    {
        free( environment );
        free( variable );
        printf( "FAIL: child environment: out of memory\n" );
        return 1;
    }

    // every variable but an inherited ROCKETNET_SERVER_MAX_CLIENTS, then ours

    int used = 0;
    for ( int i = 0; i < count; i++ )
        if ( strncmp( child_environ[i], name, sizeof( name ) - 1 ) != 0 )
            environment[used++] = child_environ[i];

    memcpy( variable, name, sizeof( name ) - 1 );
    memcpy( variable + sizeof( name ) - 1, value, strlen( value ) + 1 );
    environment[used++] = variable;
    environment[used] = NULL;

    char * child_argv[2] = { program, NULL };
    int result = 1;

#if defined( _WIN32 )
    const intptr_t exit_code = _spawnve( _P_WAIT, program, (const char * const *) child_argv, (const char * const *) environment );
    result = exit_code == 0 ? 0 : 1;
#else
    pid_t pid;
    if ( posix_spawnp( &pid, program, NULL, NULL, child_argv, environment ) == 0 )
    {
        int wait_status = 0;
        if ( waitpid( pid, &wait_status, 0 ) == pid && WIFEXITED( wait_status ) && WEXITSTATUS( wait_status ) == 0 )
            result = 0;
    }
#endif // #if defined( _WIN32 )

    if ( result != 0 )
        printf( "FAIL: the run with ROCKETNET_SERVER_MAX_CLIENTS=%s did not exit 0\n", value );

    free( variable );
    free( environment );
    return result;
}

int main( int argc, char ** argv )
{
    (void) argc;

    const char * environment = getenv( "ROCKETNET_SERVER_MAX_CLIENTS" );
    if ( !environment )
    {
        environment = "";
    }

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the in-memory loopback transport keeps its endpoints in a hub the caller
    // owns, sized here for one server and one client; it is destroyed after them

    enum rocketnet_status_t hub_status = ROCKETNET_STATUS_OK;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 1, &hub_status );
    if ( !hub || hub_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: loopback hub create: %s\n", rocketnet_status_name( hub_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    config.transport = "loopback";
    config.loopback_hub = hub;

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
            fflush( stdout );
            children += rerun_with_max_clients( argv[0], values[i] );
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

    rn_cli_loopback_hub_destroy( hub );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    if ( result != 0 )
    {
        printf( "FAIL: max_clients is the default, then the struct, then the environment\n" );
    }
    else if ( environment[0] == '\0' )
    {
        printf( "PASS\n" );
    }

    return result;
}
