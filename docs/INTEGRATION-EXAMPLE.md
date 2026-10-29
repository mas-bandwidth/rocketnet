# Complete integration examples

Use [Getting started](GETTING-STARTED.md) to build and run the examples already
in the repository. This page is for a reader who wants a complete C program to
copy into a separate project. Build and link against the same public header
and library as described in [Building](../BUILDING.md).

## Connect, exchange a message, and shut down

Link this program with the development library (`ROCKETNET_DEV=1`). It creates
a shared loopback hub, connects a client, exchanges `ping` and `pong`, and
waits for shutdown completion before freeing either endpoint. It observes
delivery across updates; even loopback I/O can run on background threads.
The bounded waits are command-line example policy. A game should poll from its
event loop and keep the objects and their callback contexts alive while waiting.

<!-- check_agents_md:example:begin -->
```c
#include "rocketnet.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

int main( void )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    int result = 1;
    double game_time = 1000.0; /* one clock for every update, shutdown included */
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 8, NULL );
    static const uint8_t dev_key[ROCKETNET_BACKEND_PUBLIC_KEY_BYTES] = ROCKETNET_DEV_BACKEND_PUBLIC_KEY;
    const uint64_t protocol_id = 0x1122334455667788ULL;
    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    if ( !hub || rocketnet_server_default_config( &server_config ) != ROCKETNET_STATUS_OK ||
         rocketnet_client_default_config( &client_config ) != ROCKETNET_STATUS_OK )
        goto cleanup;
    server_config.protocol_id = protocol_id;
    server_config.max_clients = 8;
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;
    memcpy( server_config.backend_public_key, dev_key, sizeof( dev_key ) );
    client_config.transport = "loopback";
    client_config.loopback_hub = hub;

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    server = rocketnet_server_create( NULL, "127.0.0.1:40000", NULL, &server_config, &status );
    if ( !server || status != ROCKETNET_STATUS_OK ||
         rocketnet_server_start( server ) != ROCKETNET_STATUS_OK )
        goto cleanup;

    uint8_t public_key[ROCKETNET_SERVER_PUBLIC_KEY_BYTES];
    char address[ROCKETNET_MAX_ADDRESS_STRING_BYTES];
    char token[ROCKETNET_MAX_CONNECT_TOKEN_STRING_BYTES];
    rocketnet_server_public_key( server, public_key );
    if ( !rocketnet_server_ipv4_address( server, address, sizeof( address ) ) )
        goto cleanup;
    const time_t now = time( NULL );
    if ( now == (time_t) -1 || rocketnet_create_dev_connect_token(
             address, NULL, public_key, 60, 15, 42, protocol_id,
             (uint64_t) now, NULL, token ) != ROCKETNET_STATUS_OK )
        goto cleanup;

    client = rocketnet_client_create( NULL, &client_config, &status );
    if ( !client || status != ROCKETNET_STATUS_OK ||
         rocketnet_client_connect( client, token ) != ROCKETNET_STATUS_OK )
        goto cleanup;

    const double dt = 1.0 / 60.0;
    int client_sent = 0;
    int client_received_reply = 0;
    for ( int tick = 0; tick < 300 && !client_received_reply; tick++, game_time += dt )
    {
        rocketnet_client_update( client, game_time );
        rocketnet_server_update( server, game_time );

        const struct rocketnet_message_batch_t * batch = NULL;
        while ( ( batch = rocketnet_client_receive_messages( client ) ) != NULL )
            for ( int i = 0; i < batch->num_messages; i++ )
                if ( batch->bytes[i] == 4 && memcmp( batch->data[i], "pong", 4 ) == 0 )
                    client_received_reply = 1;

        int reply_index = -1;
        while ( ( batch = rocketnet_server_receive_messages( server ) ) != NULL )
            for ( int i = 0; i < batch->num_messages; i++ )
                if ( batch->bytes[i] == 4 && memcmp( batch->data[i], "ping", 4 ) == 0 )
                    reply_index = batch->client_index[i];

        /* Simulate the game here, then queue outgoing messages. */
        if ( reply_index >= 0 )
        {
            if ( !rocketnet_server_can_send_message( server, reply_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) )
                goto cleanup;
            uint8_t * out = rocketnet_server_start_message( server, (uint64_t) reply_index, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
            if ( out )
            {
                uint8_t * p = out;
                *p++ = 'p'; *p++ = 'o'; *p++ = 'n'; *p++ = 'g';
                if ( !rocketnet_server_finish_message( server, (int) ( p - out ) ) )
                    goto cleanup;
            }
            else
            {
                goto cleanup;
            }
        }
        if ( !client_sent && rocketnet_client_state( client ) == ROCKETNET_CLIENT_STATE_CONNECTED )
        {
            if ( !rocketnet_client_can_send_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 ) )
                goto cleanup;
            uint8_t * out = rocketnet_client_start_message( client, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, 4 );
            if ( out )
            {
                uint8_t * p = out;
                *p++ = 'p'; *p++ = 'i'; *p++ = 'n'; *p++ = 'g';
                if ( !rocketnet_client_finish_message( client, (int) ( p - out ) ) )
                    goto cleanup;
                client_sent = 1;
            }
            else
            {
                goto cleanup;
            }
        }
        rocketnet_client_send_packets( client );
        rocketnet_server_send_packets( server );

        /* Wait for the next frame. The I/O threads run in real time. */
        rocketnet_sleep( dt );
    }
    result = client_received_reply ? 0 : 1;

cleanup:
    /* Command-line exit cleanup; a game would poll once per frame instead. */
    if ( client ) rocketnet_client_disconnect( client );
    if ( server ) rocketnet_server_stop( server );
    {
        const double deadline = rocketnet_time() + 10.0;
        while ( ( client && !rocketnet_client_disconnected( client ) ) ||
                ( server && !rocketnet_server_stopped( server ) ) )
        {
            if ( rocketnet_time() >= deadline )
                return 1; /* Fail without freeing storage still owned by I/O. */
            if ( client && !rocketnet_client_disconnected( client ) )
                rocketnet_client_update( client, game_time ); /* never back in time */
            rocketnet_sleep( 0.001 );
        }
    }
    if ( client ) rocketnet_client_destroy( client );
    if ( server ) rocketnet_server_destroy( server );
    /* The hub goes after every endpoint on it, before term. */
    if ( hub && rocketnet_loopback_hub_destroy( hub ) != ROCKETNET_STATUS_OK )
        result = 1;
    if ( rocketnet_term() != ROCKETNET_STATUS_OK )
        return 1;
    return result;
}
```
<!-- check_agents_md:example:end -->

## Create-only lifecycle smoke check

This smaller program works with the production or development library. It
creates two stopped endpoints sharing a loopback hub and destroys them. It
does **not** start a server, connect a client, or exchange a message. Use it to
check basic linking and object creation, not to verify networking. Its title
packet filter also permits create in a production build; no token is minted.

<!-- check_readme:example:begin -->
```c
#include "rocketnet.h"

int main( void )
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;

    int result = 1;
    struct rocketnet_server_t * server = NULL;
    struct rocketnet_client_t * client = NULL;
    struct rocketnet_loopback_hub_t * hub = rocketnet_loopback_hub_create( NULL, 1, NULL );
    struct rocketnet_server_config_t server_config;
    struct rocketnet_client_config_t client_config;
    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    if ( !hub || rocketnet_server_default_config( &server_config ) != ROCKETNET_STATUS_OK ||
         rocketnet_client_default_config( &client_config ) != ROCKETNET_STATUS_OK )
        goto cleanup;
    /* A title filter is required even for create in a production build. */
    const uint8_t packet_filter[ROCKETNET_PACKET_FILTER_BYTES] = { 'R', 'N', 'E', 'X' };
    for ( int i = 0; i < ROCKETNET_PACKET_FILTER_BYTES; i++ )
        server_config.packet_filter[i] = client_config.packet_filter[i] = packet_filter[i];
    server_config.max_clients = 1;
    server_config.transport = "loopback";
    server_config.loopback_hub = hub;
    client_config.transport = "loopback";
    client_config.loopback_hub = hub;

    server = rocketnet_server_create( NULL, "127.0.0.1:0", NULL, &server_config, &status );
    if ( !server || status != ROCKETNET_STATUS_OK )
        goto cleanup;
    client = rocketnet_client_create( NULL, &client_config, &status );
    if ( !client || status != ROCKETNET_STATUS_OK )
        goto cleanup;

    /* Neither endpoint has run, so no I/O worker needs to finish. */
    result = rocketnet_client_disconnected( client ) && rocketnet_server_stopped( server ) ? 0 : 1;

cleanup:
    if ( client ) rocketnet_client_destroy( client );
    if ( server ) rocketnet_server_destroy( server );
    if ( hub && rocketnet_loopback_hub_destroy( hub ) != ROCKETNET_STATUS_OK )
        result = 1;
    if ( rocketnet_term() != ROCKETNET_STATUS_OK )
        result = 1;
    return result;
}
```
<!-- check_readme:example:end -->

## Checking these programs

From the repository root, after building the `rocketnet` and `rocketnet_dev`
libraries under `build/`, run:

```sh
sh tools/docs/check_agents_md.sh all
```

The checker compiles and runs both marked programs. The walkthrough's excerpts
are checked separately by `python3 tools/docs/check_getting_started.py`.
