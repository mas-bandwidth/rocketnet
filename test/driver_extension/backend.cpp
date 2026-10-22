/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "rocketnet_driver.h"
#include "rocketnet_platform.h"

// A deliberately small native extension. The test selects it by its new name
// through the public transport API; it has no transport implementation access.
int external_test_creates = 0;
int external_test_destroys = 0;
int external_test_replay_forgets = 0;
int external_test_replay_marks = 0;
uint32_t external_test_replay_max_high_water = 0;
rocketnet_address_t external_test_replay_last_address;
rocketnet_address_t external_test_replay_forgotten_address;
bool external_test_replay_has_address = false;
bool external_test_replay_has_forgotten = false;

static void * socket_create( void * context, rocketnet_address_t * bind )
{
    void * socket = rocketnet_platform_socket_create_with_pump(context, bind,
        ROCKETNET_PLATFORM_SOCKET_NON_BLOCKING, 0, ROCKETNET_SOCKET_SEND_BUFFER_SIZE,
        ROCKETNET_SOCKET_RECEIVE_BUFFER_SIZE, "portable");
    if (socket) ++external_test_creates;
    return socket;
}
static void * server_create( void * context, const rocketnet_server_transport_io_config_t *, rocketnet_address_t * bind )
{ return socket_create(context, bind); }
static void * client_create( void * context, const rocketnet_client_transport_io_config_t *, rocketnet_address_t * bind )
{ return socket_create(context, bind); }
static void destroy( void * socket )
{
    rocketnet_platform_socket_destroy((rocketnet_platform_socket_t*)socket);
    ++external_test_destroys;
}
static void stop( void * ) {}
static bool stopped( void * ) { return true; }
static void send_batch( void * socket, const rocketnet_platform_send_t * packets, int count )
{ rocketnet_platform_socket_send_packets((rocketnet_platform_socket_t*)socket, packets, count); }
static int receive_batch( void * socket, rocketnet_address_t * from, uint8_t * data, int stride, int * bytes, int count )
{ return rocketnet_platform_socket_receive_packets((rocketnet_platform_socket_t*)socket, from, data, stride, bytes, count); }
static void send( void * socket, const rocketnet_address_t * to, const uint8_t * data, int bytes )
{ rocketnet_platform_socket_send_packet((rocketnet_platform_socket_t*)socket, to, data, bytes); }
static int receive( void * socket, rocketnet_address_t * from, uint8_t * data, int bytes )
{ return rocketnet_platform_socket_receive_packet((rocketnet_platform_socket_t*)socket, from, data, bytes); }

static void replay_forget( void *, const rocketnet_address_t * address )
{
    ++external_test_replay_forgets;
    external_test_replay_forgotten_address = *address;
    external_test_replay_has_forgotten = true;
}
static void replay_mark( void *, const rocketnet_address_t * address, uint32_t high_water )
{
    external_test_replay_last_address = *address;
    if ( high_water > external_test_replay_max_high_water )
        external_test_replay_max_high_water = high_water;
    external_test_replay_has_address = true;
    ++external_test_replay_marks;
}
static void replay_commit( void * ) {}

static const rocketnet_server_transport_io_operations_t server_operations =
{
    ROCKETNET_TRANSPORT_FLUSH_ON_UPDATE, server_create, destroy, stop, stopped,
    NULL, NULL, NULL, NULL, NULL, send_batch, receive_batch,
    NULL, replay_forget, replay_mark, replay_commit, NULL, NULL, NULL
};
// the optional batch, wait/wake and send-failure members are named NULL: the
// strict build refuses an initializer that leaves members out
static const rocketnet_client_transport_io_operations_t client_operations =
{
    client_create, destroy, send, receive,
    NULL, NULL, NULL, NULL, NULL
};
extern const rocketnet_transport_backend_t external_test_transport = { &server_operations, &client_operations };
