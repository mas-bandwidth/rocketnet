/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional test: it reads a real clock and sleeps with rocketnet_sleep, so
    it is a functional sibling binary, never a ./bin/test case.

    Exercises serialize::WriteStream and serialize::ReadStream bound directly
    over frames acquired and received through the C transport API.
*/

#include "native_shutdown.h"
#include "loopback_hub.h"
#include "serialize.h"
#include "rocketnet_internal.h"
#include "rocketnet_connect_token.h"
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
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
    rocketnet_forbid_xdp_commit();

    struct rocketnet_server_transport_config_t server_config;
    rocketnet_server_transport_dev_config( &server_config );
    server_config.max_clients = 4;
    server_config.loopback_hub = rocketnet_test_loopback_hub();

    struct rocketnet_server_transport_t * server = rocketnet_server_transport_create( NULL, "127.0.0.1", &server_config );
    check( server );

    char server_address[64];
    snprintf( server_address, sizeof( server_address ), "127.0.0.1:%d", rocketnet_server_transport_port( server ) );

    uint8_t connect_token[ROCKETNET_CONNECT_TOKEN_BYTES];
    check( rocketnet_write_test_connect_token( server_address, rocketnet_random_uint64(), (uint64_t) time( NULL ), connect_token, ROCKETNET_CONNECT_TOKEN_BYTES ) == ROCKETNET_STATUS_OK );

    struct rocketnet_client_transport_config_t client_config;
    rocketnet_client_transport_default_config( &client_config );
    client_config.loopback_hub = rocketnet_test_loopback_hub();
    struct rocketnet_client_transport_t * client = rocketnet_client_transport_create( NULL, connect_token, &client_config, rocketnet_time() );
    check( client );

    double start = rocketnet_time();
    while ( rocketnet_time() - start < 5.0 )
    {
        rocketnet_client_transport_update( client, rocketnet_time() );
        rocketnet_server_transport_update( server, rocketnet_time() );
        if ( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED )
            break;
        rocketnet_sleep( 0.001 );
    }

    check( rocketnet_client_transport_state( client ) == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED );

    // client -> server through the stream surface
    {
        serialize::WriteStream stream;
        struct rocketnet_packet_context_t context;
        uint8_t * frame = rocketnet_client_transport_start_packet( client, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        check( frame != NULL );
        stream.Initialize( frame + context.payload_index, context.payload_capacity );

        check( stream.SerializeBits( 0xdecafbad, 32 ) );
        check( stream.SerializeInteger( -17, -100, 100 ) );
        check( stream.SerializeInteger64( (int64_t) 0x123456789abcdefLL, 0, (int64_t) 0x7fffffffffffffffLL ) );

        stream.Flush();
        check( rocketnet_client_transport_finish_packet( client, &context, (int) stream.GetBytesProcessed() ) != 0 );
        rocketnet_client_transport_send_packets( client );
    }

    int verified = 0;
    start = rocketnet_time();
    while ( rocketnet_time() - start < 5.0 && !verified )
    {
        rocketnet_client_transport_update( client, rocketnet_time() );
        rocketnet_server_transport_update( server, rocketnet_time() );

        int num_payloads = rocketnet_server_transport_num_payloads( server );
        for ( int i = 0; i < num_payloads; i++ )
        {
            serialize::ReadStream stream;
            int client_index = -1;
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_server_transport_payload( server, i, &client_index, &payload_bytes );
            check( payload != NULL );
            stream.Initialize( payload, payload_bytes );
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
        uint8_t * frame = rocketnet_server_transport_start_packet( server, 0, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, &context );
        check( frame != NULL );
        stream.Initialize( frame + context.payload_index, context.payload_capacity );
        check( stream.SerializeBits( 0xabad1dea, 32 ) );
        stream.Flush();
        check( rocketnet_server_transport_finish_packet( server, &context, (int) stream.GetBytesProcessed() ) != 0 );
        rocketnet_server_transport_send_packets( server );
    }

    verified = 0;
    start = rocketnet_time();
    while ( rocketnet_time() - start < 5.0 && !verified )
    {
        rocketnet_client_transport_update( client, rocketnet_time() );
        rocketnet_server_transport_update( server, rocketnet_time() );

        int num_payloads = rocketnet_client_transport_num_payloads( client );
        for ( int i = 0; i < num_payloads; i++ )
        {
            serialize::ReadStream stream;
            int payload_bytes = 0;
            uint8_t * payload = rocketnet_client_transport_payload( client, i, &payload_bytes );
            check( payload != NULL );
            stream.Initialize( payload, payload_bytes );

            uint32_t reply = 0;
            check( stream.SerializeBits( reply, 32 ) );
            check( reply == 0xabad1dea );
            verified = 1;
        }

        rocketnet_sleep( 0.001 );
    }

    check( verified );

    rn_native_client_transport_stop_and_destroy( client );
    rn_native_server_transport_stop_and_destroy( server );
    check( rocketnet_test_loopback_hub_release() );
    rocketnet_term();

    printf( "serialize surface OK\n" );

    return 0;
}
