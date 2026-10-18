/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// A program that sees only rocketnet.h and links only the prebuilt
// development static library: a message server and a message client on
// 127.0.0.1 through the portable driver, one reliable message each way, then
// a clean teardown. tools/check_public_header.py copies rocketnet.h and the
// library into an empty directory and builds this there, as C99 and (through
// main.cpp) as C++11. Real sockets and the wall clock: a functional-tier test
// program, never ./bin/test.

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
    CHECK( rocketnet_init( NULL ) );
    rocketnet_forbid_xdp_commit();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_ERROR );

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 1;
    strcpy( server_config.driver, "portable" );

    struct rocketnet_message_server_t * server = rocketnet_message_server_create( NULL, "127.0.0.1:0", &server_config, NULL, NULL, 0, 0 );
    CHECK( server != NULL );
    struct rocketnet_server_transport_t * server_transport = rocketnet_message_server_transport( server );
    CHECK( strcmp( rocketnet_server_transport_driver( server_transport ), "portable" ) == 0 );

    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", (int) rocketnet_server_transport_port( server_transport ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    CHECK( rocketnet_write_dev_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    strcpy( client_config.driver, "portable" );

    struct rocketnet_message_client_t * client = rocketnet_message_client_create( NULL, connect_token, &client_config, NULL, NULL, 0, 0 );
    CHECK( client != NULL );

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
        CHECK( rocketnet_message_server_update( server, now ) == ROCKETNET_MESSAGE_OK );
        CHECK( rocketnet_message_client_update( client, now ) == ROCKETNET_MESSAGE_OK );
        CHECK( rocketnet_message_client_state( client ) > ROCKETNET_CLIENT_TRANSPORT_STATE_DISCONNECTED );

        if ( !client_sent && rocketnet_message_client_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
        {
            CHECK( rocketnet_message_client_send( client, 0, ROCKETNET_RELIABLE_ORDERED, (const uint8_t *) client_message, (int) sizeof( client_message ) ) != 0 );
            client_sent = 1;
        }

        const uint8_t * data = NULL;
        int bytes = 0;

        while ( rocketnet_message_server_receive( server, 0, 0, &data, &bytes ) )
        {
            CHECK( bytes == (int) sizeof( client_message ) && memcmp( data, client_message, sizeof( client_message ) ) == 0 );
            rocketnet_message_server_release( server, 0, data );
            server_got = 1;
        }

        if ( server_got && !server_sent )
        {
            CHECK( rocketnet_message_server_send( server, 0, 0, ROCKETNET_RELIABLE_ORDERED, (const uint8_t *) server_message, (int) sizeof( server_message ) ) != 0 );
            server_sent = 1;
        }

        while ( rocketnet_message_client_receive( client, 0, &data, &bytes ) )
        {
            CHECK( bytes == (int) sizeof( server_message ) && memcmp( data, server_message, sizeof( server_message ) ) == 0 );
            rocketnet_message_client_release( client, data );
            client_got = 1;
        }

        rocketnet_message_client_send_packets( client );
        rocketnet_message_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    CHECK( server_got );
    CHECK( client_got );

    rocketnet_message_client_destroy( client );

    rocketnet_server_transport_stop( server_transport );
    const double stop_start = rocketnet_time();
    while ( rocketnet_server_transport_state( server_transport ) != ROCKETNET_SERVER_TRANSPORT_STOPPED && rocketnet_time() - stop_start < 5.0 )
    {
        rocketnet_sleep( 0.001 );
        now = 1000.0 + ( rocketnet_time() - wall_start );
        rocketnet_message_server_update( server, now );
    }
    CHECK( rocketnet_server_transport_state( server_transport ) == ROCKETNET_SERVER_TRANSPORT_STOPPED );
    rocketnet_message_server_destroy( server );

    CHECK( rocketnet_term_ex() == ROCKETNET_STATUS_OK );

#ifdef __cplusplus
    printf( "header_alone (C++): one message each way through rocketnet.h alone\n" );
#else
    printf( "header_alone (C): one message each way through rocketnet.h alone\n" );
#endif
    return 0;
}
