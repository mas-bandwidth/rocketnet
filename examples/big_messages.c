/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Big messages: over loopback, the client sends one 200 KB reliable message;
    the library fragments it, the server reassembles it and checks every byte.
    The client then sends an unreliable message larger than one packet: the
    library promotes it to its reliable path, which shows as one acknowledged
    receipt in the client's stats. Run: ./bin/example_big_messages
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BIG_BYTES       ( 200 * 1024 )
#define OVERSIZE_BYTES  4096
#define LIMIT_BYTES     ( 256 * 1024 )

static uint8_t pattern[BIG_BYTES];

static uint64_t receipts_acked( const struct rocketnet_client_t * client )
{
    struct rocketnet_client_stats_t stats;
    rocketnet_client_stats( client, &stats );
    return stats.receipts_acked;
}

int main( void )
{
    for ( int i = 0; i < BIG_BYTES; i++ )
    {
        pattern[i] = (uint8_t) ( i * 31 + 13 );
    }

    const enum rocketnet_status_t init_status = rocketnet_init( NULL, NULL, NULL );
    if ( init_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: init: %s\n", rocketnet_status_name( init_status ) );
        return 1;
    }

    // the 1 MiB defaults hold 200 KB already; both ends state the size they need

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    struct rocketnet_server_config_t server_config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &server_config );
    if ( server_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server default config: %s\n", rocketnet_status_name( server_config_status ) );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";
    server_config.max_message_bytes = LIMIT_BYTES;
    server_config.receive_message_buffer_bytes = LIMIT_BYTES;

    enum rocketnet_status_t server_status = ROCKETNET_STATUS_OK;
    struct rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &server_status );
    if ( server && server_status == ROCKETNET_STATUS_OK )
    {
        server_status = rocketnet_server_start( server );
    }

    if ( !server || server_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: server start: %s\n", rocketnet_status_name( server_status ) );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    char server_address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    uint8_t server_public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char connect_token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( !rocketnet_server_ipv4_address( server, server_address, sizeof( server_address ) ) )
    {
        printf( "FAIL: server ipv4 address\n" );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    rocketnet_server_public_key( server, server_public_key );

    const enum rocketnet_status_t token_status = rocketnet_create_dev_connect_token( server_address, NULL, server_public_key, 30, 10, 1, EXAMPLE_PROTOCOL_ID, (uint64_t) time( NULL ), NULL, connect_token );
    if ( token_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: connect token: %s\n", rocketnet_status_name( token_status ) );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    struct rocketnet_client_config_t client_config;
    const enum rocketnet_status_t client_config_status = rocketnet_client_default_config( &client_config );
    if ( client_config_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client default config: %s\n", rocketnet_status_name( client_config_status ) );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    client_config.transport = "loopback";
    client_config.max_message_bytes = LIMIT_BYTES;
    client_config.receive_message_buffer_bytes = LIMIT_BYTES;

    enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &client_status );
    if ( !client || client_status != ROCKETNET_STATUS_OK || rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create/connect: %s\n", rocketnet_status_name( client_status ) );
        rn_cli_client_stop_and_destroy( client );
        rn_cli_server_stop_and_destroy( server );

        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    int big_sent = 0, big_arrived = 0, oversize_sent = 0;
    uint64_t acked_before = 0;
    const double start = rocketnet_time();

    while ( !oversize_sent || receipts_acked( client ) != acked_before + 1 )
    {
        if ( rocketnet_time() - start > 20.0 )
        {
            printf( "FAIL: the messages did not arrive within 20 seconds\n" );
            rn_cli_client_stop_and_destroy( client );
            rn_cli_server_stop_and_destroy( server );

            const enum rocketnet_status_t term_status = rocketnet_term();
            if ( term_status != ROCKETNET_STATUS_OK )
            {
                printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
            }

            return 1;
        }

        const double now = 1000.0 + rocketnet_time() - start;
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
        }

        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
            for ( int i = 0; i < batch->num_messages; i++ )
            {
                if ( batch->bytes[i] != BIG_BYTES )
                    continue;

                if ( memcmp( batch->data[i], pattern, BIG_BYTES ) != 0 )
                {
                    printf( "FAIL: the big message arrived changed\n" );
                    rn_cli_client_stop_and_destroy( client );
                    rn_cli_server_stop_and_destroy( server );

                    const enum rocketnet_status_t term_status = rocketnet_term();
                    if ( term_status != ROCKETNET_STATUS_OK )
                    {
                        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
                    }

                    return 1;
                }

                printf( "server received the %d-byte reliable message intact after %.3f s\n", batch->bytes[i], rocketnet_time() - start );
                big_arrived = 1;
            }
        }

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( !big_sent )
            {
                // reliable: defer to a later frame when there is no room

                if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE, BIG_BYTES ) )
                {
                    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE, BIG_BYTES );
                    if ( p )
                    {
                        memcpy( p, pattern, BIG_BYTES );
                        big_sent = rocketnet_client_finish_message( client, BIG_BYTES ) != 0;
                    }
                }
            }
            else if ( big_arrived && !oversize_sent )
            {
                // above one packet's limit: an unreliable send with a receipt is
                // promoted, where one without a receipt is refused

                acked_before = receipts_acked( client );
                printf( "promotion counter before: %llu\n", (unsigned long long) acked_before );

                if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, OVERSIZE_BYTES ) )
                {
                    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, OVERSIZE_BYTES );
                    if ( p )
                    {
                        memset( p, 0x5a, OVERSIZE_BYTES );
                        oversize_sent = rocketnet_client_finish_message( client, OVERSIZE_BYTES ) != 0;
                    }
                }
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.001 );
    }

    printf( "promotion counter after: %llu\n", (unsigned long long) receipts_acked( client ) );
    rn_cli_client_stop_and_destroy( client );
    rn_cli_server_stop_and_destroy( server );

    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( "PASS\n" );

    return 0;
}
