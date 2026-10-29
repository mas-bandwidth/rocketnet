/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The server shared object of test_loopback_hub_two_shared_objects: its own
    copy of the development library, a loopback hub and a server on it. Only
    the loopback_hub_server_module_ names are exported
    (functional/loopback_hub_module.h).
*/

#include "loopback_hub_module.h"

#include <string.h>


static struct rocketnet_loopback_hub_t * module_hub = NULL;
static struct rocketnet_server_t * module_server = NULL;

LOOPBACK_HUB_MODULE_EXPORT int loopback_hub_server_module_start( struct rocketnet_loopback_hub_t ** hub, char * address, int address_bytes, uint8_t * public_key );
LOOPBACK_HUB_MODULE_EXPORT int loopback_hub_server_module_update( double time );
LOOPBACK_HUB_MODULE_EXPORT int loopback_hub_server_module_stop( void );

int loopback_hub_server_module_start( struct rocketnet_loopback_hub_t ** hub, char * address, int address_bytes, uint8_t * public_key )
{
    // a second copy of the library in this process: its init succeeds only
    // because nothing it holds is shared with the host's or the client's copy

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 0;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    module_hub = rocketnet_loopback_hub_create( NULL, 1, &status );
    if ( !module_hub || status != ROCKETNET_STATUS_OK )
        return 0;

    struct rocketnet_server_config_t config;
    if ( rocketnet_server_default_config( &config ) != ROCKETNET_STATUS_OK )
        return 0;

    const uint8_t backend_public_key[] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    memcpy( config.backend_public_key, backend_public_key, sizeof( backend_public_key ) );
    config.protocol_id = LOOPBACK_HUB_MODULE_PROTOCOL_ID;
    config.max_clients = 1;
    config.transport = "loopback";
    config.loopback_hub = module_hub;

    module_server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    if ( !module_server || status != ROCKETNET_STATUS_OK )
        return 0;

    if ( rocketnet_server_start( module_server ) != ROCKETNET_STATUS_OK )
        return 0;

    if ( !rocketnet_server_ipv4_address( module_server, address, (size_t) address_bytes ) )
        return 0;

    rocketnet_server_public_key( module_server, public_key );
    *hub = module_hub;

    return 1;
}

int loopback_hub_server_module_update( double time )
{
    rocketnet_server_update( module_server, time );

    int pings = 0;
    int reply_index = -1;
    const struct rocketnet_message_batch_t * batch = NULL;
    while ( ( batch = rocketnet_server_receive_messages( module_server ) ) != NULL )
    {
        for ( int i = 0; i < batch->num_messages; i++ )
        {
            if ( batch->bytes[i] == 4 && memcmp( batch->data[i], "ping", 4 ) == 0 )
            {
                reply_index = batch->client_index[i];
                pings++;
            }
        }
    }

    if ( reply_index >= 0 && rocketnet_server_can_send_message( module_server, reply_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) )
    {
        uint8_t * out = rocketnet_server_start_message( module_server, (uint64_t) reply_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
        if ( out )
        {
            memcpy( out, "pong", 4 );
            rocketnet_server_finish_message( module_server, 4 );
        }
    }

    rocketnet_server_send_packets( module_server );

    return pings;
}

int loopback_hub_server_module_stop( void )
{
    int ok = 1;

    if ( module_server )
    {
        rocketnet_server_stop( module_server );

        const double deadline = rocketnet_time() + 10.0;
        while ( !rocketnet_server_stopped( module_server ) )
        {
            if ( rocketnet_time() >= deadline )
                return 0;           // keep the server: its worker still owns it

            rocketnet_sleep( 0.001 );
        }

        rocketnet_server_destroy( module_server );
        module_server = NULL;
    }

    // after the server and the client are destroyed: no endpoint is registered

    if ( rocketnet_loopback_hub_destroy( module_hub ) != ROCKETNET_STATUS_OK )
        ok = 0;

    module_hub = NULL;

    if ( rocketnet_term() != ROCKETNET_STATUS_OK )
        ok = 0;

    return ok;
}
