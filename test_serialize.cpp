/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Compiles and exercises the serialize read of rocketnet.h: serialize.h
    is included first, so the C++ stream surface appears, and a client and
    server exchange a bit-packed payload through WriteStream and ReadStream
    bound directly over acquired frames. This is the only build that sees
    that surface, which is exactly why it exists.
*/

#include "serialize.h"
#include "rocketnet.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#define check( condition )                                                                  \
do                                                                                          \
{                                                                                           \
    if ( !(condition) )                                                                     \
    {                                                                                       \
        printf( "check failed: ( %s ), %s:%d\n", #condition, __FILE__, __LINE__ );          \
        fflush( stdout );                                                                   \
        exit( 1 );                                                                          \
    }                                                                                       \
} while(0)

int main()
{
    check( rocketnet_init( NULL ) );

    struct rocketnet_server_config_t server_config;
    rocketnet_server_dev_config( &server_config );
    server_config.max_clients = 4;

    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:40891", &server_config, rocketnet_time() );
    check( server );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_dev_connect_token( "127.0.0.1:40891", rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token ) );

    struct rocketnet_client_t * client = rocketnet_client_create( NULL, connect_token, NULL, rocketnet_time() );
    check( client );

    double start = rocketnet_time();
    while ( rocketnet_time() - start < 5.0 )
    {
        rocketnet_client_update( client, rocketnet_time() );
        rocketnet_server_update( server, rocketnet_time() );
        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
            break;
        rocketnet_sleep( 0.001 );
    }
    check( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED );

    // client -> server through the stream surface

    {
        serialize::WriteStream stream;
        struct rocketnet_packet_context_t context;
        check( rocketnet::client_start_packet( client, stream, context ) );

        check( stream.SerializeBits( 0xdecafbad, 32 ) );
        check( stream.SerializeInteger( -17, -100, 100 ) );
        check( stream.SerializeInteger64( (int64_t) 0x123456789abcdefLL, 0, (int64_t) 0x7fffffffffffffffLL ) );

        rocketnet::client_finish_packet( client, stream, context );
        rocketnet_client_send_packets( client );
    }

    int verified = 0;
    start = rocketnet_time();
    while ( rocketnet_time() - start < 5.0 && !verified )
    {
        rocketnet_client_update( client, rocketnet_time() );
        rocketnet_server_update( server, rocketnet_time() );

        int num_payloads = rocketnet_server_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            serialize::ReadStream stream;
            int client_index = -1;
            check( rocketnet::server_payload( server, i, stream, client_index ) );
            check( client_index == 0 );

            uint32_t magic = 0;
            check( stream.SerializeBits( magic, 32 ) );
            check( magic == 0xdecafbad );
            int32_t small = 0;
            check( stream.SerializeInteger( small, -100, 100 ) );
            check( small == -17 );
            int64_t big = 0;
            check( stream.SerializeInteger64( big, 0, (int64_t) 0x7fffffffffffffffLL ) );
            check( big == (int64_t) 0x123456789abcdefLL );

            verified = 1;
        }
        rocketnet_sleep( 0.001 );
    }
    check( verified );

    // server -> client the same way

    {
        serialize::WriteStream stream;
        struct rocketnet_packet_context_t context;
        check( rocketnet::server_start_packet( server, 0, stream, context ) );
        check( stream.SerializeBits( 0xabad1dea, 32 ) );
        rocketnet::server_finish_packet( server, stream, context );
        rocketnet_server_send_packets( server );
    }

    verified = 0;
    start = rocketnet_time();
    while ( rocketnet_time() - start < 5.0 && !verified )
    {
        rocketnet_client_update( client, rocketnet_time() );
        rocketnet_server_update( server, rocketnet_time() );

        int num_payloads = rocketnet_client_num_payloads( client );
        for ( int i = 0; i < num_payloads; i++ )
        {
            serialize::ReadStream stream;
            check( rocketnet::client_payload( client, i, stream ) );
            uint32_t reply = 0;
            check( stream.SerializeBits( reply, 32 ) );
            check( reply == 0xabad1dea );
            verified = 1;
        }
        rocketnet_sleep( 0.001 );
    }
    check( verified );

    rocketnet_client_destroy( client );
    rocketnet_server_destroy( server );
    rocketnet_term();

    printf( "serialize surface OK\n" );

    return 0;
}
