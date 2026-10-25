/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_message_object.h"

// Handles that never started own no worker and are immediately destroyable.
static void test_shutdown_new_endpoints_are_complete()
{
    rocketnet_status_t status = ROCKETNET_STATUS_OK;
    rocketnet_client_t * client = rocketnet_client_create( NULL, NULL, &status );
    rocketnet_unit_check( client != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_client_disconnected( client ) );
    rocketnet_client_disconnect( client );
    rocketnet_client_disconnect( client );
    rocketnet_unit_check( rocketnet_client_disconnected( client ) );
    rocketnet_client_destroy( client );

    rocketnet_server_config_t config;
    rocketnet_unit_check( rocketnet_server_default_config( &config ) == ROCKETNET_STATUS_OK );
    config.max_clients = 1;
    rocketnet_server_t * server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &config, &status );
    rocketnet_unit_check( server != NULL && status == ROCKETNET_STATUS_OK );
    rocketnet_unit_check( rocketnet_server_stopped( server ) );
    rocketnet_server_stop( server );
    rocketnet_server_stop( server );
    rocketnet_unit_check( rocketnet_server_stopped( server ) );
    rocketnet_server_destroy( server );
}
REGISTER_UNIT_TEST( test_shutdown_new_endpoints_are_complete );

static int shutdown_borrowed_completion_checks;
static bool shutdown_borrowed_stopped( const void * )
{
    ++shutdown_borrowed_completion_checks;
    return false;
}

// A message object borrows a link when destroy is NULL. Releasing message
// storage must not require stopping the independently owned transport.
static void test_shutdown_borrowed_message_link_does_not_require_stop()
{
    rn_message_link_t link = {};
    link.stopped = shutdown_borrowed_stopped;
    shutdown_borrowed_completion_checks = 0;
    rn_message_object_t * object = rn_message_object_create( NULL, &link, 0, 1,
                                                             NULL, NULL, 0, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 1000.0 );
    rocketnet_unit_check( object != NULL );
    rn_message_object_destroy( object );
    rocketnet_unit_check( shutdown_borrowed_completion_checks == 0 );
}
REGISTER_UNIT_TEST( test_shutdown_borrowed_message_link_does_not_require_stop );
