/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Ack receipts: the client sends unreliable state updates with
    ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT over loopback while the
    development network simulator loses 30% of its packets. It keeps the
    serial each finish returns and reads rocketnet_client_receipts every frame,
    so it learns which updates the server got (ACKED) and which it did not
    (LOSS). Run: ./bin/example_ack_receipts
*/

#include "rocketnet.h"
#include "dev_keys.h"
#include "shutdown.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NUM_UPDATES 20

int main( void )
{
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
    struct rocketnet_server_config_t server_config;
    const enum rocketnet_status_t server_config_status = rocketnet_server_default_config( &server_config );
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

    server_config.protocol_id = EXAMPLE_PROTOCOL_ID;
    memcpy( server_config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;

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

        rn_cli_loopback_hub_destroy( hub );
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

        rn_cli_loopback_hub_destroy( hub );
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

        rn_cli_loopback_hub_destroy( hub );
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

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    client_config.transport = "loopback";
    client_config.loopback_hub = hub;

    enum rocketnet_status_t client_status = ROCKETNET_STATUS_OK;
    struct rocketnet_client_t * client = rocketnet_client_create( NULL, &client_config, &client_status );
    if ( client )
    {
        rocketnet_client_network_simulator( client, 0, 0, 30, 0 );
    }

    if ( !client || client_status != ROCKETNET_STATUS_OK || rocketnet_client_connect( client, connect_token ) != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: client create/connect: %s\n", rocketnet_status_name( client_status ) );
        rn_cli_client_stop_and_destroy( client, 1000.0 );
        rn_cli_server_stop_and_destroy( server );

        rn_cli_loopback_hub_destroy( hub );
        const enum rocketnet_status_t term_status = rocketnet_term();
        if ( term_status != ROCKETNET_STATUS_OK )
        {
            printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        }

        return 1;
    }

    uint32_t serials[NUM_UPDATES];
    int sent = 0, acked = 0, lost = 0;
    double now = 1000.0;

    for ( int frame = 0; frame < 12000 && acked + lost < NUM_UPDATES; frame++, now += 0.005 )
    {
        rocketnet_server_update( server, now );
        rocketnet_client_update( client, now );

        const struct rocketnet_message_batch_t * batch;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
        {
        }

        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
        {
        }

        // receipts resolve at update; match each one to its send by serial

        struct rocketnet_receipt_t receipts[32];
        const int count = rocketnet_client_receipts( client, receipts, 32 );
        // count is the receipts available and may exceed the 32 the array holds
        for ( int r = 0; r < count && r < 32; r++ )
        {
            for ( int update = 0; update < sent; update++ )
            {
                if ( serials[update] != receipts[r].serial )
                    continue;

                const int ok = receipts[r].status == ROCKETNET_RECEIPT_STATUS_ACKED;
                printf( "update %d (serial %u): %s\n", update, receipts[r].serial, ok ? "ACKED" : "LOSS" );
                acked += ok;
                lost += !ok;
            }
        }

        if ( rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( sent < NUM_UPDATES )
            {
                // replaceable state: skip this frame when there is no room

                if ( rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 1 ) )
                {
                    uint8_t * p = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 1 );
                    if ( p )
                    {
                        p[0] = (uint8_t) sent;

                        const uint32_t serial = rocketnet_client_finish_message( client, 1 );
                        if ( serial )
                        {
                            serials[sent++] = serial;
                        }
                    }
                }
            }

            // the server's packets carry the acks back to the client

            const int slot = rocketnet_client_index( client );
            if ( rocketnet_server_client_connected( server, slot ) )
            {
                if ( rocketnet_server_can_send_message( server, slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1 ) )
                {
                    uint8_t * p = rocketnet_server_start_message( server, (uint64_t) slot, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1 );
                    if ( p )
                    {
                        p[0] = 0;

                        const uint32_t serial = rocketnet_server_finish_message( server, 1 );
                        (void) serial;
                    }
                }
            }
        }

        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );
        rocketnet_sleep( 0.005 );
    }

    printf( "receipts resolved: %d/%d (acked %d, loss %d)\n", acked + lost, NUM_UPDATES, acked, lost );

    const int result = acked + lost == NUM_UPDATES && acked > 0 && lost > 0 ? 0 : 1;

    rn_cli_client_stop_and_destroy( client, now );
    rn_cli_server_stop_and_destroy( server );

    rn_cli_loopback_hub_destroy( hub );
    const enum rocketnet_status_t term_status = rocketnet_term();
    if ( term_status != ROCKETNET_STATUS_OK )
    {
        printf( "FAIL: term: %s\n", rocketnet_status_name( term_status ) );
        return 1;
    }

    printf( result == 0 ? "PASS\n" : "FAIL: every update resolves, with at least one ACKED and one LOSS\n" );
    return result;
}
