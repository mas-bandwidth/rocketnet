/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A program that sees only rocketnet.h and links only the prebuilt
// development static library: a server and a client on 127.0.0.1 through the
// portable transport, one reliable message each way, every query enum read
// once through its getter, then a clean teardown.
// tools/check_public_header.py copies rocketnet.h and the library into an
// empty directory and builds this there, as C99 and (through main.cpp) as
// C++11. Real sockets and the wall clock: a functional-tier test program,
// never ./bin/test.

#define ROCKETNET_DEVELOPMENT 1         // the development library: dev keys and dev connect tokens

#include "rocketnet.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK( condition )                                                                  \
    do                                                                                      \
    {                                                                                       \
        if ( !( condition ) )                                                               \
        {                                                                                   \
            fprintf( stderr, "header_alone failed: %s (line %d)\n", #condition, __LINE__ ); \
            return 1;                                                                       \
        }                                                                                   \
    } while ( 0 )

static const char client_message[] = "hello from the client";
static const char server_message[] = "hello from the server";

int main( void )
{
    CHECK( rocketnet_version() == ROCKETNET_VERSION );
    CHECK( ROCKETNET_VERSION == ROCKETNET_VERSION_MAJOR * 10000 + ROCKETNET_VERSION_MINOR * 100 + ROCKETNET_VERSION_PATCH );

    CHECK( rocketnet_init( NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_set_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    struct rocketnet_server_config_t server_config;
    rocketnet_server_dev_config( &server_config );
    server_config.max_clients = 1;
    server_config.transport = "portable";

    enum rocketnet_status_t create_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", &server_config, NULL, 0, &create_status );
    if ( !server ) printf( "create refused: %s\n", rocketnet_status_name( create_status ) );
    if ( server && rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_server_destroy( server );
        server = NULL;
    }
    CHECK( server != NULL );
    CHECK( strcmp( rocketnet_server_transport_name( server ), "portable" ) == 0 );
    CHECK( rocketnet_server_max_clients( server ) == 1 );

    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK( rocketnet_write_dev_connect_token( server_address, 1234, (uint64_t) time( NULL ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_config_t client_config;
    rocketnet_client_default_config( &client_config );
    client_config.transport = "portable";

    enum rocketnet_status_t create_status_2 = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, NULL, 0, &create_status_2 );
    if ( !client ) printf( "create refused: %s\n", rocketnet_status_name( create_status_2 ) );
    if ( client && rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        rocketnet_client_destroy( client );
        client = NULL;
    }
    CHECK( client != NULL );
    CHECK( strcmp( rocketnet_client_transport_name( client ), "portable" ) == 0 );

    // the game's clock starts at 1000.0, with real seconds on top
    const double wall_start = rocketnet_time();
    double now = 1000.0;

    int client_sent = 0;
    int server_sent = 0;
    int server_got = 0;
    int client_got = 0;

    while ( !( server_got && client_got ) && rocketnet_time() - wall_start < 10.0 )
    {
        now = 1000.0 + ( rocketnet_time() - wall_start );
        CHECK( rocketnet_server_update( server, now ) == ROCKETNET_UPDATE_OK );
        CHECK( rocketnet_client_update( client, now ) == ROCKETNET_UPDATE_OK );
        CHECK( rocketnet_client_state( client ) > ROCKETNET_CLIENT_STATE_DISCONNECTED );

        if ( !client_sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) sizeof( client_message ) );
            CHECK( out != NULL );
            memcpy( out, client_message, sizeof( client_message ) );
            CHECK( rocketnet_client_finish_message( client, (int) sizeof( client_message ) ) != 0 );
            client_sent = 1;
        }

        const uint8_t * data = NULL;
        int bytes = 0;

        while ( rocketnet_server_receive_message( server, 0, 0, &data, &bytes ) )
        {
            CHECK( bytes == (int) sizeof( client_message ) && memcmp( data, client_message, sizeof( client_message ) ) == 0 );
            rocketnet_server_release_message( server, 0, data );
            server_got = 1;
        }

        if ( server_got && !server_sent )
        {
            uint8_t * out = rocketnet_server_start_message( server, 0, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, (int) sizeof( server_message ) );
            CHECK( out != NULL );
            memcpy( out, server_message, sizeof( server_message ) );
            CHECK( rocketnet_server_finish_message( server, 0, (int) sizeof( server_message ) ) != 0 );
            server_sent = 1;
        }

        while ( rocketnet_client_receive_message( client, 0, &data, &bytes ) )
        {
            CHECK( bytes == (int) sizeof( server_message ) && memcmp( data, server_message, sizeof( server_message ) ) == 0 );
            rocketnet_client_release_message( client, data );
            client_got = 1;
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    CHECK( server_got );
    CHECK( client_got );

    // the queries, each value once through its getter. the traffic counters
    // moved; every value reads; a value outside its enum reads 0

    CHECK( rocketnet_server_num_connected_clients( server ) == 1 );
    CHECK( rocketnet_server_client_connected( server, 0 ) );
    CHECK( rocketnet_server_client_id( server, 0 ) == 1234 );
    CHECK( rocketnet_client_index( client ) == 0 );

    for ( int i = 0; i < ROCKETNET_CLIENT_COUNTER_COUNT; i++ )
    {
        const uint64_t value = rocketnet_client_counter( client, (enum rocketnet_client_counter_t) i );
        if ( i <= ROCKETNET_CLIENT_COUNTER_PAYLOADS_RECEIVED )
            CHECK( value > 0 );
    }
    CHECK( rocketnet_client_counter( client, ROCKETNET_CLIENT_COUNTER_COUNT ) == 0 );
    for ( int i = 0; i < ROCKETNET_CLIENT_STAT_COUNT; i++ )
        CHECK( rocketnet_client_stat( client, (enum rocketnet_client_stat_t) i ) >= 0.0 );
    CHECK( rocketnet_client_stat( client, ROCKETNET_CLIENT_STAT_COUNT ) == 0.0 );

    for ( int i = 0; i < ROCKETNET_SERVER_COUNTER_COUNT; i++ )
    {
        const uint64_t value = rocketnet_server_counter( server, (enum rocketnet_server_counter_t) i );
        if ( i <= ROCKETNET_SERVER_COUNTER_CLIENTS_CONNECTED || i == ROCKETNET_SERVER_COUNTER_PAYLOADS_RECEIVED )
            CHECK( value > 0 );
    }
    CHECK( rocketnet_server_counter( server, ROCKETNET_SERVER_COUNTER_COUNT ) == 0 );
    for ( int i = 0; i < ROCKETNET_SERVER_STAT_COUNT; i++ )
        CHECK( rocketnet_server_stat( server, (enum rocketnet_server_stat_t) i ) >= 0.0 );
    CHECK( rocketnet_server_stat( server, ROCKETNET_SERVER_STAT_COUNT ) == 0.0 );

    for ( int i = 0; i < ROCKETNET_SERVER_CLIENT_COUNTER_COUNT; i++ )
    {
        CHECK( rocketnet_server_client_counter( server, 0, (enum rocketnet_server_client_counter_t) i ) > 0 );
        CHECK( rocketnet_server_client_counter( server, 1, (enum rocketnet_server_client_counter_t) i ) == 0 );
    }
    CHECK( rocketnet_server_client_counter( server, 0, ROCKETNET_SERVER_CLIENT_COUNTER_COUNT ) == 0 );
    for ( int i = 0; i < ROCKETNET_SERVER_CLIENT_STAT_COUNT; i++ )
        CHECK( rocketnet_server_client_stat( server, 0, (enum rocketnet_server_client_stat_t) i ) >= 0.0 );
    CHECK( rocketnet_server_client_stat( server, 0, ROCKETNET_SERVER_CLIENT_STAT_COUNT ) == 0.0 );

    rocketnet_client_disconnect( client );
    CHECK( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_DISCONNECTED );
    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );

    rocketnet_term();

#ifdef __cplusplus
    printf( "header_alone (C++): one message each way through rocketnet.h alone\n" );
#else
    printf( "header_alone (C): one message each way through rocketnet.h alone\n" );
#endif
    return 0;
}
