/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A production-linked functional witness for the public client lifecycle.
// It uses the normal portable I/O worker; no caller-owned operations, test
// seam, development key or development token enters this executable.

#include "../../sdk/rocketnet.h"
#include "../../sdk/rocketnet_base64.h"
#include "../../sdk/rocketnet_connect_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static void production_lifecycle_check_handler( const char * condition, const char * file, int line )
{
    fprintf( stderr, "check failed: %s, file %s, line %d\n", condition, file, line );
    fflush( stderr );
    exit( 1 );
}

#define production_lifecycle_check( condition )                                      \
    do                                                                                \
    {                                                                                 \
        if ( !( condition ) )                                                         \
            production_lifecycle_check_handler( #condition, __FILE__, __LINE__ );     \
    } while ( 0 )

class production_environment_guard_t
{
public:
    explicit production_environment_guard_t( const char * variable )
        : name( variable ), present( false )
    {
#if defined( _WIN32 )
        char * current = NULL;
        size_t bytes = 0;
        production_lifecycle_check( _dupenv_s( &current, &bytes, name.c_str() ) == 0 );
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
#endif // #if defined( _WIN32 )
    }

    ~production_environment_guard_t()
    {
        if ( present )
            set( value.c_str() );
        else
            clear();
    }

    void set( const char * current )
    {
#if defined( _WIN32 )
        production_lifecycle_check( _putenv_s( name.c_str(), current ) == 0 );
#else
        production_lifecycle_check( setenv( name.c_str(), current, 1 ) == 0 );
#endif // #if defined( _WIN32 )
    }

    void clear()
    {
#if defined( _WIN32 )
        production_lifecycle_check( _putenv_s( name.c_str(), "" ) == 0 );
#else
        production_lifecycle_check( unsetenv( name.c_str() ) == 0 );
#endif // #if defined( _WIN32 )
    }

private:
    std::string name;
    bool present;
    std::string value;
};

int main()
{
    static const uint64_t protocol_id = 0x57E9011FEC7C1E01ULL;
    static const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 0x31, 0x43, 0x59, 0x71 };

    production_lifecycle_check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );

    uint8_t seed[32];
    for ( int i = 0; i < 32; ++i )
        seed[i] = (uint8_t) ( 0xA7 ^ i );

    uint8_t backend_public[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES];
    uint8_t backend_secret[64];
    production_lifecycle_check( rocketnet_generate_backend_keypair_from_seed( seed, backend_public, backend_secret ) == ROCKETNET_STATUS_OK );

    struct rocketnet_server_config_t server_config;
    production_lifecycle_check( rocketnet_server_default_config( &server_config ) == ROCKETNET_STATUS_OK );

    server_config.protocol_id = protocol_id;
    server_config.max_clients = 1;
    server_config.transport = "portable";
    memcpy( server_config.backend_public_key, backend_public, sizeof( server_config.backend_public_key ) );
    memcpy( server_config.packet_filter, packet_filter, sizeof( server_config.packet_filter ) );

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    production_environment_guard_t server_reverse( "ROCKETNET_SERVER_SIMULATED_REVERSE_WINDOW" );
    server_reverse.set( "4" );

    struct rocketnet_server_t * refused_server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    production_lifecycle_check( refused_server == NULL );
    production_lifecycle_check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    server_reverse.clear();

    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    production_lifecycle_check( server != NULL && status == ROCKETNET_STATUS_OK );
    production_lifecycle_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_OK );

    const int running_port = rocketnet_server_port( server );
    production_lifecycle_check( running_port != 0 );
    production_lifecycle_check( rocketnet_server_start( server ) == ROCKETNET_STATUS_SERVER_NOT_STOPPED );
    production_lifecycle_check( rocketnet_server_running( server ) );
    production_lifecycle_check( rocketnet_server_port( server ) == running_port );
    production_lifecycle_check( rocketnet_server_set_send_kbps( server, 0, 2000 ) == ROCKETNET_STATUS_OK );

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    production_lifecycle_check( rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) );

    uint8_t server_public[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    rocketnet_server_public_key( server, server_public );

    const char * server_addresses[] = { server_address };
    uint8_t token_bytes[ROCKETNET_CONNECT_TOKEN_BYTES];
    production_lifecycle_check( rocketnet_write_production_connect_token(
                                    backend_secret, server_public, protocol_id, server_addresses, 1,
                                    0xC11E17ULL, 0, 1ULL << 40, 5, NULL,
                                    token_bytes, sizeof( token_bytes ) ) == ROCKETNET_STATUS_OK );

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    production_lifecycle_check( rocketnet_base64_encode_data(
                                    token_bytes, sizeof( token_bytes ), token, sizeof( token ) ) ==
                                ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES - 1 );

    struct rocketnet_client_config_t client_config;
    production_lifecycle_check( rocketnet_client_default_config( &client_config ) == ROCKETNET_STATUS_OK );

    client_config.transport = "portable";
    memcpy( client_config.packet_filter, packet_filter, sizeof( client_config.packet_filter ) );

    production_environment_guard_t client_drop( "ROCKETNET_CLIENT_SIMULATED_DROP_EVERY_NTH" );
    client_drop.set( "7" );

    struct rocketnet_client_t * refused_client = rocketnet_client_create( NULL, &client_config, &status );
    production_lifecycle_check( refused_client == NULL );
    production_lifecycle_check( status == ROCKETNET_STATUS_CONFIG_OUT_OF_RANGE );

    client_drop.clear();

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &status );
    production_lifecycle_check( client != NULL && status == ROCKETNET_STATUS_OK );
    production_lifecycle_check( rocketnet_client_set_send_kbps( client, 2000 ) == ROCKETNET_STATUS_OK );
    production_lifecycle_check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_OK );

    const double wall_start = rocketnet_time();
    while ( rocketnet_time() - wall_start < 10.0 &&
            ( rocketnet_client_state( client ) != ROCKETNET_CLIENT_STATE_CONNECTED ||
              !rocketnet_server_client_connected( server, 0 ) ) )
    {
        const double now = 1000.0 + rocketnet_time() - wall_start;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }
    production_lifecycle_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    production_lifecycle_check( rocketnet_server_client_connected( server, 0 ) );
    production_lifecycle_check( rocketnet_client_connect( client, token ) == ROCKETNET_STATUS_CLIENT_NOT_DISCONNECTED );
    production_lifecycle_check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );
    production_lifecycle_check( rocketnet_client_set_send_kbps( client, 3000 ) == ROCKETNET_STATUS_OK );
    production_lifecycle_check( rocketnet_server_set_send_kbps( server, 0, 3000 ) == ROCKETNET_STATUS_OK );

    rocketnet_client_disconnect( client );

    const double disconnect_start = rocketnet_time();
    while ( !rocketnet_client_disconnected( client ) && rocketnet_time() - disconnect_start < 5.0 )
    {
        const double now = 1000.0 + rocketnet_time() - wall_start;
        rocketnet_client_update( client, now );
        rocketnet_client_send_packets( client );
        rocketnet_server_update( server, now );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    production_lifecycle_check( rocketnet_client_disconnected( client ) );
    production_lifecycle_check( rocketnet_client_set_send_kbps( client, 4000 ) == ROCKETNET_STATUS_OK );

    rocketnet_client_destroy( client );

    rocketnet_server_stop( server );

    const double stop_start = rocketnet_time();
    while ( !rocketnet_server_stopped( server ) && rocketnet_time() - stop_start < 5.0 )
    {
        rocketnet_server_update( server, 1000.0 + rocketnet_time() - wall_start );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    production_lifecycle_check( rocketnet_server_stopped( server ) );
    production_lifecycle_check( rocketnet_server_set_send_kbps( server, 0, 4000 ) == ROCKETNET_STATUS_OK );

    rocketnet_server_destroy( server );

    production_lifecycle_check( rocketnet_term() == ROCKETNET_STATUS_OK );

    printf( "test_production_client_lifecycle: create/start/connect/update/disconnect/destroy PASS\n" );

    return 0;
}
