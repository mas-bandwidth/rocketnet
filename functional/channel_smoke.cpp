/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The smoke cases for the channel harness. They prove the harness itself,
    not any message type: each uses only the unreliable path the message
    objects already deliver.
*/

#include "channel_harness.h"

#include <stdio.h>
#include <stdlib.h>

// the harness's own check: prints and exits on failure, the shape the
// functional suite's rocketnet_check has
static void rocketnet_channel_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_check( condition )                                                                                 \
do                                                                                                              \
{                                                                                                               \
    if ( !(condition) )                                                                                         \
    {                                                                                                           \
        rocketnet_channel_check_handler( #condition, (const char*) __FUNCTION__, (const char*) __FILE__, __LINE__ ); \
    }                                                                                                           \
} while(0)

// one unreliable message each way, lossless loopback
void harness_one_message_each_way_lossless()
{
    rocketnet_channel_harness_t h;
    if ( !rocketnet_channel_harness_create( &h ) )
    {
        rocketnet_check( 0 );
        return;
    }

    if ( !rocketnet_channel_harness_connect( &h, 60 * 10 ) )
    {
        rocketnet_check( 0 );
        rocketnet_channel_harness_destroy( &h );
        return;
    }

    const uint8_t to_server[4] = { 'h', 'i', 'c', 's' };
    const uint8_t to_client[4] = { 'h', 'i', 'c', 'c' };
    rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, to_server, 4 ) != 0 );
    rocketnet_check( rocketnet_channel_harness_server_send( &h, 0, to_client, 4 ) != 0 );

    int server_got = 0, client_got = 0;
    for ( int i = 0; i < 60 * 10 && ( server_got == 0 || client_got == 0 ); i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
        if ( server_got == 0 && rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.bytes == 4 );
            rocketnet_check( memcmp( d.data, to_server, 4 ) == 0 );
            server_got++;
        }
        if ( client_got == 0 && rocketnet_channel_harness_client_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.bytes == 4 );
            rocketnet_check( memcmp( d.data, to_client, 4 ) == 0 );
            client_got++;
        }
    }
    rocketnet_check( server_got == 1 );
    rocketnet_check( client_got == 1 );

    rocketnet_channel_harness_destroy( &h );
}

// one unreliable message each way over the lossy knobs the 256 KiB card will
// use; measured completion time and resend count print only on failure
void harness_one_message_each_way_lossy()
{
    rocketnet_channel_harness_t h;
    if ( !rocketnet_channel_harness_create( &h ) )
    {
        rocketnet_check( 0 );
        return;
    }
    rocketnet_channel_harness_set_link( &h, 0.0, 0.050, 10.0f, 5.0f );

    const double t0 = h.now;
    if ( !rocketnet_channel_harness_connect( &h, 60 * 10 ) )
    {
        rocketnet_check( 0 );
        rocketnet_channel_harness_destroy( &h );
        return;
    }

    const uint8_t to_server[4] = { 'l', 'o', 's', 's' };
    const uint8_t to_client[4] = { 'l', 'o', 's', 's' };
    rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, to_server, 4 ) != 0 );
    rocketnet_check( rocketnet_channel_harness_server_send( &h, 0, to_client, 4 ) != 0 );

    int server_got = 0, client_got = 0;
    for ( int i = 0; i < 60 * 30 && ( server_got == 0 || client_got == 0 ); i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
        if ( server_got == 0 && rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.bytes == 4 );
            server_got++;
        }
        if ( client_got == 0 && rocketnet_channel_harness_client_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.bytes == 4 );
            client_got++;
        }
    }

    const double completion = h.now - t0;
    if ( server_got != 1 || client_got != 1 )
    {
        rocketnet_channel_harness_read_counters( &h );
        printf( "lossy smoke failed: server_got=%d client_got=%d completion_ms=%.1f resends=%llu\n",
                server_got, client_got, completion * 1000.0, (unsigned long long) h.resends );
    }
    rocketnet_check( server_got == 1 );
    rocketnet_check( client_got == 1 );

    rocketnet_channel_harness_destroy( &h );
}

// the negative control: the link knobs are honored. After the connection is
// up, a 100% loss link must deliver nothing; if the loss is ignored the
// exchange completes and this case fails loudly with the measured numbers.
void harness_lossy_100_percent_negative_control()
{
    rocketnet_channel_harness_t h;
    if ( !rocketnet_channel_harness_create( &h ) )
    {
        rocketnet_check( 0 );
        return;
    }
    if ( !rocketnet_channel_harness_connect( &h, 60 * 10 ) )
    {
        rocketnet_check( 0 );
        rocketnet_channel_harness_destroy( &h );
        return;
    }

    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 100.0f, 0.0f );

    const uint8_t to_server[4] = { 'n', 'o', 'p', 'e' };
    const uint8_t to_client[4] = { 'n', 'o', 'p', 'e' };
    rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, to_server, 4 ) != 0 );
    rocketnet_check( rocketnet_channel_harness_server_send( &h, 0, to_client, 4 ) != 0 );

    const double t0 = h.now;
    int server_got = 0, client_got = 0;
    for ( int i = 0; i < 60 * 30; i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
        if ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
            server_got++;
        if ( rocketnet_channel_harness_client_collect( &h, 0, &d ) )
            client_got++;
    }

    const double completion = h.now - t0;
    if ( server_got != 0 || client_got != 0 )
    {
        rocketnet_channel_harness_read_counters( &h );
        printf( "negative control failed: loss not honored server_got=%d client_got=%d completion_ms=%.1f resends=%llu\n",
                server_got, client_got, completion * 1000.0, (unsigned long long) h.resends );
    }
    rocketnet_check( server_got == 0 );
    rocketnet_check( client_got == 0 );

    rocketnet_channel_harness_destroy( &h );
}

// the allocator sees no calls after create
void harness_no_allocation_after_create()
{
    rocketnet_channel_harness_t h;
    if ( !rocketnet_channel_harness_create( &h ) )
    {
        rocketnet_check( 0 );
        return;
    }

    if ( !rocketnet_channel_harness_connect( &h, 60 * 10 ) )
    {
        rocketnet_check( 0 );
        rocketnet_channel_harness_destroy( &h );
        return;
    }

    const uint8_t payload[4] = { 1, 2, 3, 4 };
    rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, payload, 4 ) != 0 );
    int got = 0;
    for ( int i = 0; i < 60 * 10 && got == 0; i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
        if ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
            got++;
    }
    rocketnet_check( got == 1 );

    // both objects were created with caller blocks; the exchange drives no
    // allocation. the assertion is the harness's: everything the exchange
    // touched came from the region carved at create (R2).
    rocketnet_channel_harness_destroy( &h );
}
