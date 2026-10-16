/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "channel_harness.h"

static void reliable_functional_fail( const char * condition, int line )
{
    printf( "reliable functional check failed: %s at line %d\n", condition, line );
    fflush( stdout );
    exit( 1 );
}

#define reliable_check( condition ) do { if ( !( condition ) ) reliable_functional_fail( #condition, __LINE__ ); } while ( 0 )

void reliable_mixed_sizes_over_lossy_link()
{
    rocketnet_channel_harness_t h;
    reliable_check( rocketnet_channel_harness_create( &h ) );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.050, 10.0f, 5.0f );
    reliable_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );

    static const int lengths[] = { 16, 1024, 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17, 8192 };
    uint8_t client_data[4][8192];
    uint8_t server_data[4][8192];
    for ( int i = 0; i < 4; i++ )
        for ( int j = 0; j < lengths[i]; j++ )
        {
            client_data[i][j] = (uint8_t) ( i * 31 + j * 7 + 1 );
            server_data[i][j] = (uint8_t) ( i * 29 + j * 11 + 2 );
        }

    for ( int i = 0; i < 4; i++ )
    {
        reliable_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABLE, client_data[i], lengths[i] ) != 0 );
        reliable_check( rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABLE, server_data[i], lengths[i] ) != 0 );
    }

    uint32_t server_seen = 0, client_seen = 0;
    const double started = h.now;
    for ( int step = 0; step < 60 * 30 && ( server_seen != 15 || client_seen != 15 ); step++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t delivery;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            int match = -1;
            for ( int i = 0; i < 4; i++ )
                if ( delivery.bytes == lengths[i] && delivery.hash == rocketnet_channel_hash( client_data[i], lengths[i] ) &&
                     memcmp( delivery.data, client_data[i], (size_t) lengths[i] ) == 0 )
                    match = i;
            reliable_check( match >= 0 && !( server_seen & ( 1u << match ) ) );
            server_seen |= 1u << match;
        }
        while ( rocketnet_channel_harness_client_collect( &h, 0, &delivery ) )
        {
            int match = -1;
            for ( int i = 0; i < 4; i++ )
                if ( delivery.bytes == lengths[i] && delivery.hash == rocketnet_channel_hash( server_data[i], lengths[i] ) &&
                     memcmp( delivery.data, server_data[i], (size_t) lengths[i] ) == 0 )
                    match = i;
            reliable_check( match >= 0 && !( client_seen & ( 1u << match ) ) );
            client_seen |= 1u << match;
        }
        reliable_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    }

    rocketnet_channel_harness_read_counters( &h );
    printf( "reliable_mixed_sizes_over_lossy_link elapsed=%.3fs resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
            h.now - started, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    reliable_check( server_seen == 15 && client_seen == 15 );
    reliable_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    reliable_check( rocketnet_channel_allocator_calls == h.allocation_snapshot );
    rocketnet_channel_harness_destroy( &h );
}
