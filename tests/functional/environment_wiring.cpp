/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_internal.h"
#include "rocketnet_test_seams.h"
#include "../../examples/shutdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
#include <stdlib.h>
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS

extern void rocketnet_functional_restore_log_function();

static void rocketnet_environment_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                              \
do                                                                                                                \
{                                                                                                                 \
    if ( !(condition) )                                                                                           \
        rocketnet_environment_check_handler( #condition, __FUNCTION__, __FILE__, __LINE__ );                     \
} while(0)

// The functional tier alone changes the real process environment. Each guard
// restores the exact prior value, including a value longer than an SDK read
// buffer, and never leaves one case's input for the next case.
class rocketnet_real_environment_guard_t
{
public:
    explicit rocketnet_real_environment_guard_t( const char * variable )
        : name( variable ), present( false )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        char * current = NULL;
        size_t current_bytes = 0;
        rocketnet_check( _dupenv_s( &current, &current_bytes, name.c_str() ) == 0 );
        if ( current )
        {
            present = true;
            value = current;
            free( current );
        }
#else
        const char * current = getenv( name.c_str() );
        if ( current )
        {
            present = true;
            value = current;
        }
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

    ~rocketnet_real_environment_guard_t()
    {
        if ( present )
        {
            set( value.c_str() );
        }
        else
        {
            clear();
        }
    }

    void set( const char * current )
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        rocketnet_check( _putenv_s( name.c_str(), current ) == 0 );
#else
        rocketnet_check( setenv( name.c_str(), current, 1 ) == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

    void clear()
    {
#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
        rocketnet_check( _putenv_s( name.c_str(), "" ) == 0 );
#else
        rocketnet_check( unsetenv( name.c_str() ) == 0 );
#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_WINDOWS
    }

private:
    std::string name;
    bool present;
    std::string value;
};

void rocketnet_test_real_environment_dev_session_sequence_start()
{
    rocketnet_real_environment_guard_t environment( "ROCKETNET_DEV_SESSION_SEQUENCE_START" );
    environment.set( "4294967294" );
    rocketnet_check( rocketnet_dev_session_sequence_start() == UINT64_C( 4294967294 ) );
}

void rocketnet_test_real_environment_log_level_before_init()
{
    rocketnet_real_environment_guard_t environment( "ROCKETNET_LOG_LEVEL" );

    const enum rocketnet_log_level_t prior_level = rocketnet_log_level_current();

    rocketnet_check( rocketnet_term() == ROCKETNET_STATUS_OK );
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    environment.set( "1" );
    rocketnet_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_log_level_current() == ROCKETNET_LOG_LEVEL_ERROR );
    rocketnet_log_level( prior_level );
    rocketnet_functional_restore_log_function();
}

void rocketnet_test_real_environment_config_precedence_and_invalid()
{
    rocketnet_real_environment_guard_t environment( "ROCKETNET_CLIENT_SEND_MESSAGE_BUFFER_BYTES" );

    rocketnet_client_config_t config;
    rocketnet_check( rocketnet_client_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.send_message_buffer_bytes = 8388608;

    environment.set( "4194304" );

    const size_t environment_bytes = rocketnet_client_required_memory( &config );
    rocketnet_check( environment_bytes > 0 );

    environment.clear();
    config.send_message_buffer_bytes = 4194304;
    rocketnet_check( rocketnet_client_required_memory( &config ) == environment_bytes );

    environment.set( "not-a-number" );
    rocketnet_check( rocketnet_client_required_memory( &config ) == 0 );
}

void rocketnet_test_real_environment_client_step_driven()
{
    // The functional executable links rocketnet_dev. Production libraries do
    // not carry this descriptor and are covered by the DEV=0 source/unit gates.

    rocketnet_real_environment_guard_t environment( "ROCKETNET_CLIENT_STEP_DRIVEN" );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;

    environment.set( "2" );

    rocketnet_client_t * refused = rocketnet_client_create( NULL, NULL, &status );
    rocketnet_check( refused == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    environment.set( "1" );

    rocketnet_client_t * client = rocketnet_client_create( NULL, NULL, &status );
    rocketnet_check( client != NULL );
    rocketnet_check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
}

void rocketnet_test_real_environment_dev_simulator_number()
{
    rocketnet_real_environment_guard_t environment( "ROCKETNET_DEV_CLIENT_LATENCY_MS" );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;

    environment.set( "invalid" );

    rocketnet_client_t * refused = rocketnet_client_create( NULL, NULL, &status );
    rocketnet_check( refused == NULL );
    rocketnet_check( status == ROCKETNET_STATUS_CONFIG_ENV_INVALID );

    environment.set( "17" );

    rocketnet_client_t * client = rocketnet_client_create( NULL, NULL, &status );
    rocketnet_check( client != NULL );
    rocketnet_check( status == ROCKETNET_STATUS_OK );
    rocketnet_client_destroy( client );
}

void rocketnet_test_real_environment_server_ipv4_address()
{
    rocketnet_real_environment_guard_t environment( "ROCKETNET_SERVER_IPV4_ADDRESS" );
    rocketnet_real_environment_guard_t transport( "ROCKETNET_SERVER_TRANSPORT" );
    environment.set( "192.0.2.55:0" );
    transport.set( "portable" );

    rocketnet_server_config_t config;
    rocketnet_check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.protocol_id = 1;
    memset( config.backend_public_key, 0x42, sizeof( config.backend_public_key ) );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    rocketnet_check( server != NULL );
    rocketnet_check( status == ROCKETNET_STATUS_OK );
    rocketnet_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    rocketnet_check( rocketnet_server_ipv4_address( server, address, sizeof( address ) ) );
    rocketnet_check( strncmp( address, "192.0.2.55:", strlen( "192.0.2.55:" ) ) == 0 );
    rn_cli_server_stop_and_destroy( server );
}
