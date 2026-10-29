/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The client shared object of test_loopback_hub_two_shared_objects: its own
    copy of the development library and a loopback client on the hub the
    server module created. Only the loopback_hub_client_module_ names are
    exported (functional/loopback_hub_module.h).
*/

#include "loopback_hub_module.h"

#include <string.h>
#include <time.h>


static struct rocketnet_client_t * module_client = NULL;
static int module_sent = 0;
static int module_received = 0;

LOOPBACK_HUB_MODULE_EXPORT int loopback_hub_client_module_start( struct rocketnet_loopback_hub_t * hub, const char * address, const uint8_t * public_key );
LOOPBACK_HUB_MODULE_EXPORT int loopback_hub_client_module_update( double time );
LOOPBACK_HUB_MODULE_EXPORT enum rocketnet_status_t loopback_hub_client_module_destroy_hub( struct rocketnet_loopback_hub_t * hub );
LOOPBACK_HUB_MODULE_EXPORT int loopback_hub_client_module_stop( double time );

int loopback_hub_client_module_start( struct rocketnet_loopback_hub_t * hub, const char * address, const uint8_t * public_key )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 0;

    const time_t now = time( NULL );
    if ( now == (time_t) -1 )
        return 0;

    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    if ( rocketnet_create_dev_connect_token( address, NULL, public_key, 60, 15, LOOPBACK_HUB_MODULE_CLIENT_ID, LOOPBACK_HUB_MODULE_PROTOCOL_ID, (uint64_t) now, NULL, token ) != ROCKETNET_STATUS_OK )
        return 0;

    struct rocketnet_client_config_t config;
    if ( rocketnet_client_default_config( &config ) != ROCKETNET_STATUS_OK )
        return 0;

    // the hub another copy of the library created: same build, so its magic,
    // version and layout id are this copy's too

    config.transport = "loopback";
    config.loopback_hub = hub;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    module_client = rocketnet_client_create( NULL, &config, &status );
    if ( !module_client || status != ROCKETNET_STATUS_OK )
        return 0;

    return rocketnet_client_connect( module_client, token ) == ROCKETNET_STATUS_OK ? 1 : 0;
}

int loopback_hub_client_module_update( double time )
{
    rocketnet_client_update( module_client, time );

    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_client_receive_messages( module_client ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            if ( batch->bytes[i] == 4 && memcmp( batch->data[i], "pong", 4 ) == 0 )
                module_received = 1;
        }
    }

    const enum rocketnet_client_state_t state = rocketnet_client_state( module_client );

    if ( !module_sent && state == ROCKETNET_CLIENT_STATE_CONNECTED &&
         rocketnet_client_can_send_message( module_client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) )
    {
        uint8_t * out = rocketnet_client_start_message( module_client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        if ( out )
        {
            memcpy( out, "ping", 4 );
            module_sent = rocketnet_client_finish_message( module_client, 4 ) != 0;
        }
    }

    rocketnet_client_send_packets( module_client );

    if ( module_received )
        return 1;

    return (int) state < 0 ? -1 : 0;
}

enum rocketnet_status_t loopback_hub_client_module_destroy_hub( struct rocketnet_loopback_hub_t * hub )
{
    return rocketnet_loopback_hub_destroy( hub );
}

int loopback_hub_client_module_stop( double time )
{
    if ( module_client )
    {
        rocketnet_client_disconnect( module_client );

        const double deadline = rocketnet_time() + 10.0;
        while ( !rocketnet_client_disconnected( module_client ) )
        {
            if ( rocketnet_time() >= deadline )
                return 0;           // keep the client: its worker still owns it

            time += 1.0 / 60.0;
            rocketnet_client_update( module_client, time );
            rocketnet_sleep( 0.001 );
        }

        rocketnet_client_destroy( module_client );
        module_client = NULL;
    }

    return rocketnet_term() == ROCKETNET_STATUS_OK ? 1 : 0;
}
