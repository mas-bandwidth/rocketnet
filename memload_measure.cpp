// Throwaway: prints every byte figure of docs/MEMORY.md and docs/LOAD.md
// Tables 1-3 from the compiled library. Build once against librocketnet_dev.a
// with -DROCKETNET_DEV=1 and once against librocketnet.a with -DROCKETNET_DEV=0.

#include "rocketnet.h"
#if ROCKETNET_DEV
#include "rocketnet_config.h"
#include "rocketnet_layers.h"
#include "rocketnet_driver_loopback.h"
#endif

#include <stdio.h>
#include <string.h>

static struct rocketnet_loopback_hub_t * hub = NULL;

static void arena_server( struct rocketnet_server_config_t * c )
{
    c->max_clients = 64;
    c->num_channels = 4;
    c->max_message_bytes = 16384;
    c->receive_message_buffer_bytes = 65536;
    c->send_message_buffer_bytes = 65536;
    c->send_queue_messages = 256;
    c->reliable_messages_in_flight = 64;
    c->packets_in_flight = 128;
    c->packets_per_update = 64;
    c->unreliable_messages_per_update = 64;
    c->sequenced_messages_held = 8;
    c->max_message_receipts = 256;
    c->per_client_send_kbps = 1000;
    c->reliable_message_timeout_ms = 0;
}

static void arena_client( struct rocketnet_client_config_t * c )
{
    c->num_channels = 4;
    c->max_message_bytes = 16384;
    c->receive_message_buffer_bytes = 65536;
    c->send_message_buffer_bytes = 65536;
    c->send_queue_messages = 256;
    c->reliable_messages_in_flight = 64;
    c->packets_in_flight = 128;
    c->packets_per_update = 64;
    c->unreliable_messages_per_update = 64;
    c->sequenced_messages_held = 8;
    c->max_message_receipts = 256;
    c->send_kbps = 1000;
    c->reliable_message_timeout_ms = 0;
}

static void server_config( const char * profile, struct rocketnet_server_config_t * c )
{
    if ( strcmp( profile, "light" ) == 0 )
    {
        rocketnet_server_light_config( c );
    }
    else
    {
        rocketnet_server_default_config( c );
        if ( strcmp( profile, "arena-64" ) == 0 )
        {
            arena_server( c );
        }
    }
}

static void client_config( const char * profile, struct rocketnet_client_config_t * c )
{
    if ( strcmp( profile, "light" ) == 0 )
    {
        rocketnet_client_light_config( c );
    }
    else
    {
        rocketnet_client_default_config( c );
        if ( strcmp( profile, "arena-64" ) == 0 )
        {
            arena_client( c );
        }
    }
}

static void server_cell( const char * profile, int max_clients, const char * transport, int zero_filter )
{
    struct rocketnet_server_config_t c;
    server_config( profile, &c );
    c.max_clients = max_clients;
    c.transport = transport;
    if ( zero_filter )
    {
        memset( c.packet_filter, 0, sizeof( c.packet_filter ) );
    }
    if ( transport && strcmp( transport, "loopback" ) == 0 )
    {
        c.loopback_hub = hub;
    }
    printf( "server profile=%s filter=%s max_clients=%d transport=%s required=%zu\n", profile, zero_filter ? "zero" : "default", max_clients,
            transport ? transport : "auto", rocketnet_server_required_memory( &c ) );
}

static void client_cell( const char * profile, const char * transport, int zero_filter )
{
    struct rocketnet_client_config_t c;
    client_config( profile, &c );
    c.transport = transport;
    if ( zero_filter )
    {
        memset( c.packet_filter, 0, sizeof( c.packet_filter ) );
    }
    if ( transport && strcmp( transport, "loopback" ) == 0 )
    {
        c.loopback_hub = hub;
    }
    printf( "client profile=%s filter=%s transport=%s required=%zu\n", profile, zero_filter ? "zero" : "default", transport ? transport : "auto",
            rocketnet_client_required_memory( &c ) );
}

#if ROCKETNET_DEV
static void sizing_server( const char * profile )
{
    struct rocketnet_server_config_t c;
    server_config( profile, &c );
    struct rocketnet_server_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rocketnet_server_config_resolve( &c, false, &transport, &message, &kbps ) )
    {
        printf( "sizing server %s: resolve refused\n", profile );
        return;
    }
    message.client_send_kbps = message.server_send_kbps;
    char text[8192];
    rocketnet_message_connection_sizing( &message, 1, text, (int) sizeof( text ) );
    printf( "sizing server profile=%s connection_bytes=%llu\n%s", profile, (unsigned long long) rocketnet_message_connection_bytes( &message ), text );
}

static void sizing_client( const char * profile )
{
    struct rocketnet_client_config_t c;
    client_config( profile, &c );
    struct rocketnet_client_transport_config_t transport;
    struct rocketnet_message_config_t message;
    uint32_t kbps = 0;
    if ( !rocketnet_client_config_resolve( &c, false, &transport, &message, &kbps ) )
    {
        printf( "sizing client %s: resolve refused\n", profile );
        return;
    }
    message.server_send_kbps = message.client_send_kbps;
    char text[8192];
    rocketnet_message_connection_sizing( &message, 1, text, (int) sizeof( text ) );
    printf( "sizing client profile=%s connection_bytes=%llu\n%s", profile, (unsigned long long) rocketnet_message_connection_bytes( &message ), text );
}
#endif

int main( void )
{
    printf( "build ROCKETNET_DEV=%d\n", (int) ROCKETNET_DEV );

    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        printf( "init failed\n" );
        return 1;
    }

    enum rocketnet_status_t status = ROCKETNET_STATUS_OK;
    hub = rocketnet_loopback_hub_create( NULL, 1, &status );
    printf( "hub status=%s\n", rocketnet_status_name( status ) );

    // the MEMORY.md example program, as written there (default packet filter)
    if ( ROCKETNET_DEV )
    {
        server_cell( "default", 1000, NULL, 0 );
        server_cell( "default", 1, NULL, 0 );
        server_cell( "default", 2, NULL, 0 );
        server_cell( "light", 1000, NULL, 0 );
        client_cell( "light", NULL, 0 );
        client_cell( "default", NULL, 0 );
    }

    // Table 1
    const char * profiles[3] = { "default", "light", "arena-64" };
    const char * client_transports[3] = { NULL, "portable", "loopback" };
    for ( int p = 0; p < 3; p++ )
    {
        for ( int t = 0; t < 3; t++ )
        {
            client_cell( profiles[p], client_transports[t], 1 );
        }
        client_cell( profiles[p], "gso", 1 );
    }

    // Table 2
    const char * server_transports[6] = { NULL, "xdp", "io_uring", "gso", "portable", "loopback" };
    const int counts[7] = { 1, 2, 64, 100, 250, 500, 1000 };
    for ( int p = 0; p < 3; p++ )
    {
        for ( int n = 0; n < 7; n++ )
        {
            if ( p == 2 && counts[n] != 64 )
            {
                continue;
            }
            for ( int t = 0; t < 6; t++ )
            {
                server_cell( profiles[p], counts[n], server_transports[t], 1 );
            }
        }
    }

#if ROCKETNET_DEV
    // Table 3
    sizing_server( "default" );
    sizing_server( "light" );
    sizing_server( "arena-64" );
    sizing_client( "default" );
    sizing_client( "light" );
    sizing_client( "arena-64" );

    printf( "loopback_hub_bytes 1000=%llu 64=%llu 1=%llu\n", (unsigned long long) rocketnet_loopback_hub_bytes( 1000 ),
            (unsigned long long) rocketnet_loopback_hub_bytes( 64 ), (unsigned long long) rocketnet_loopback_hub_bytes( 1 ) );
#endif

    if ( hub )
    {
        rocketnet_loopback_hub_destroy( hub );
    }
    rocketnet_term();
    return 0;
}
