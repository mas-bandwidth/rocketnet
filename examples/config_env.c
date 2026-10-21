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
#include "dev_keys.h"

// the published development backend key, from rocketnet.h (development builds)
static const uint8_t dev_backend_public_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK( condition ) do { if ( !( condition ) ) { printf( "FAIL: %s\n", #condition ); goto cleanup; } } while ( 0 )

// Copies an environment variable into buffer and returns 1, or returns 0 when
// it is unset or does not fit. MSVC deprecates getenv, so it uses getenv_s.
static int example_getenv( const char * name, char * buffer, size_t size )
{
#if defined( _MSC_VER )
    size_t length = 0;
    return getenv_s( &length, buffer, size, name ) == 0 && length > 0;
#else
    const char * value = getenv( name );
    if ( !value || strlen( value ) >= size )
        return 0;
    memcpy( buffer, value, strlen( value ) + 1 );
    return 1;
#endif
}

// Show the default and the struct value: print two defaults from the dev
// config, set them, and prove create honors the struct value.
static int run_show_default_and_struct( void )
{
    struct rocketnet_server_config_t config;
    struct rocketnet_server_t * server = NULL;
    int result = 1;

    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );

    printf( "default max_clients: %d\n", config.max_clients );
    printf( "default max_timeout_seconds: %d\n", config.max_timeout_seconds );

    config.max_clients = 16;
    config.max_timeout_seconds = 90;
    config.transport = "loopback";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    CHECK( server != NULL );
    printf( "struct max_clients: %d\n", rocketnet_server_max_clients( server ) );

    rocketnet_server_destroy( server );
    server = NULL;
    result = 0;
cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (rocketnet_api_null_guard)
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

    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = 16;
    config.transport = "loopback";

    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &create_status_2 );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    CHECK( server != NULL );
    printf( "environment max_clients: %d\n", rocketnet_server_max_clients( server ) );
    CHECK( rocketnet_server_max_clients( server ) == 5 );

    rocketnet_server_destroy( server );
    server = NULL;
    result = 0;
cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (rocketnet_api_null_guard)
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

    // the published development backend key (rocketnet.h) and the examples protocol id (dev_keys.h); the server makes its own keypair at create. A real server brings its own backend key, recommended through ROCKETNET_SERVER_BACKEND_PUBLIC_KEY, or set in code
    rocketnet_server_default_config( &config );
    config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( config.backend_public_key, dev_backend_public_key, sizeof( config.backend_public_key ) );
    config.max_clients = 16;
    config.transport = "loopback";

    enum rocketnet_status_t create_status_3 = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &create_status_3 );
    CHECK( server == NULL );
    printf( "bad value refused: %s\n", rocketnet_status_name( create_status_3 ) );

    result = 0;
cleanup:
    // destroy on NULL is a counted misuse that asserts in a debug build (rocketnet_api_null_guard)
    if ( server )
        rocketnet_server_destroy( server );
    return result;
}

int main( int argc, char ** argv )
{
    (void) argc;
    int result = 1;
    char env[256];
    const int have_env = example_getenv( "ROCKETNET_SERVER_MAX_CLIENTS", env, sizeof( env ) );

    CHECK( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    if ( !have_env )
    {
        char command[512];

        // the example re-runs itself by the name it was started with
        CHECK( argc > 0 );

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
