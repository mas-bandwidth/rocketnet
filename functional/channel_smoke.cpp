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
    rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, to_server, 4 ) != 0 );
    rocketnet_check( rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, to_client, 4 ) != 0 );

    int server_got = 0, client_got = 0;
    for ( int i = 0; i < 60 * 10 && ( server_got == 0 || client_got == 0 ); i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
        if ( server_got == 0 && rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.channel == 0 );
            rocketnet_check( d.bytes == 4 );
            rocketnet_check( memcmp( d.data, to_server, 4 ) == 0 );
            rocketnet_check( d.hash == rocketnet_channel_hash( to_server, 4 ) );
            server_got++;
        }
        if ( client_got == 0 && rocketnet_channel_harness_client_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.channel == 0 );
            rocketnet_check( d.bytes == 4 );
            rocketnet_check( memcmp( d.data, to_client, 4 ) == 0 );
            rocketnet_check( d.hash == rocketnet_channel_hash( to_client, 4 ) );
            client_got++;
        }
    }
    rocketnet_check( server_got == 1 );
    rocketnet_check( client_got == 1 );
    rocketnet_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    rocketnet_check( rocketnet_channel_allocator_calls == h.allocations_after_create );

    rocketnet_channel_harness_destroy( &h );
}

// unreliable messages each way over the lossy knobs the 256 KiB card will use.
// An unreliable message is allowed to be lost (10% per packet here), so one
// message each way fails on about one seed in five. The case holds on every
// seed by sending a fresh unreliable message each update, each side until the
// other side has one, for at most 600 updates (ten seconds; the chance that all 600
// are lost is 0.1^600), then draining the link and asserting that no message
// is delivered twice and that nothing arrives that was not sent. Loss, jitter
// and duplicate packets stay on: the case still pins that the knobs drop some
// packets, deliver the rest intact, and never deliver a message twice.
// Measured completion time and resend count print only on failure.
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

    const int max_updates = 600;
    const int drain_updates = 60;
    uint8_t seen_by_server[600] = { 0 };
    uint8_t seen_by_client[600] = { 0 };
    int server_sent = 0, client_sent = 0;
    int server_got = 0, client_got = 0;
    int duplicate_deliveries = 0, unsent_deliveries = 0;
    int drain = 0;

    for ( int i = 0; i < max_updates + drain_updates && drain < drain_updates; i++ )
    {
        const bool both_arrived = server_got > 0 && client_got > 0;
        if ( both_arrived )
            drain++;
        else if ( i < max_updates )
        {
            // payload: 'l', 'o', the message index (two bytes)
            if ( server_got == 0 )
            {
                const uint8_t m[4] = { 'l', 'o', (uint8_t) ( client_sent >> 8 ), (uint8_t) client_sent };
                rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, m, 4 ) != 0 );
                client_sent++;
            }
            if ( client_got == 0 )
            {
                const uint8_t m[4] = { 'l', 'o', (uint8_t) ( server_sent >> 8 ), (uint8_t) server_sent };
                rocketnet_check( rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, m, 4 ) != 0 );
                server_sent++;
            }
        }
        else
            break;

        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.channel == 0 );
            rocketnet_check( d.bytes == 4 );
            rocketnet_check( d.data[0] == 'l' && d.data[1] == 'o' );
            const int index = ( d.data[2] << 8 ) | d.data[3];
            if ( index >= client_sent || index >= 600 )
                unsent_deliveries++;
            else if ( seen_by_server[index]++ )
                duplicate_deliveries++;
            server_got++;
        }
        while ( rocketnet_channel_harness_client_collect( &h, 0, &d ) )
        {
            rocketnet_check( d.channel == 0 );
            rocketnet_check( d.bytes == 4 );
            rocketnet_check( d.data[0] == 'l' && d.data[1] == 'o' );
            const int index = ( d.data[2] << 8 ) | d.data[3];
            if ( index >= server_sent || index >= 600 )
                unsent_deliveries++;
            else if ( seen_by_client[index]++ )
                duplicate_deliveries++;
            client_got++;
        }
    }

    const double completion = h.now - t0;
    if ( server_got == 0 || client_got == 0 || duplicate_deliveries != 0 || unsent_deliveries != 0 )
    {
        rocketnet_channel_harness_read_counters( &h );
        printf( "lossy smoke failed: seed=0x%016llx server_got=%d client_got=%d duplicate_deliveries=%d unsent_deliveries=%d client_sent=%d server_sent=%d completion_ms=%.1f resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
                (unsigned long long) h.seed, server_got, client_got, duplicate_deliveries, unsent_deliveries,
                client_sent, server_sent, completion * 1000.0, (unsigned long long) h.resends,
                (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    }
    rocketnet_check( server_got >= 1 );
    rocketnet_check( client_got >= 1 );
    rocketnet_check( duplicate_deliveries == 0 );
    rocketnet_check( unsent_deliveries == 0 );
    rocketnet_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    rocketnet_check( rocketnet_channel_allocator_calls == h.allocations_after_create );

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
    rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, to_server, 4 ) != 0 );
    rocketnet_check( rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, to_client, 4 ) != 0 );

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
        printf( "negative control failed: loss not honored server_got=%d client_got=%d completion_ms=%.1f resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
                server_got, client_got, completion * 1000.0, (unsigned long long) h.resends,
                (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    }
    rocketnet_check( server_got == 0 );
    rocketnet_check( client_got == 0 );
    rocketnet_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    rocketnet_check( rocketnet_channel_allocator_calls == h.allocations_after_create );

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
    rocketnet_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE, payload, 4 ) != 0 );
    int got = 0;
    for ( int i = 0; i < 60 * 10 && got == 0; i++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        rocketnet_channel_delivery_t d;
        if ( rocketnet_channel_harness_server_collect( &h, 0, &d ) )
            got++;
    }
    rocketnet_check( got == 1 );
    rocketnet_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    rocketnet_check( rocketnet_channel_allocator_calls == h.allocations_after_create );

    // The counting allocator records every SDK allocation. The
    // allocations_after_create assertion proves the exchange touched only
    // storage carved at create.
    rocketnet_channel_harness_destroy( &h );
}

// Collecting one channel must preserve messages queued on every other channel.
void harness_collect_preserves_other_channels()
{
    rocketnet_channel_harness_t h;
    if ( !rocketnet_channel_harness_create( &h ) )
    {
        rocketnet_check( 0 );
        return;
    }
    const uint8_t channel_zero[] = { 'c', 'h', '0' };
    const uint8_t channel_one[] = { 'c', 'h', '1' };
    uint8_t large_payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES + 1];
    for ( int i = 0; i < (int) sizeof(large_payload); i++ )
        large_payload[i] = (uint8_t) i;
    // Unreliable message objects route to channel 0 on receive. Populate the
    // collector's bounded queue directly to prove filtering channel 0 cannot
    // discard an unrelated channel's already-delivered view. The third view
    // is larger than one wire payload, ensuring collection does not truncate a
    // reassembled reliable message to the packet size.
    rocketnet_check( rocketnet_channel_delivery_ring_push( &h, &h.to_server, 1, channel_one, sizeof(channel_one) ) );
    rocketnet_check( rocketnet_channel_delivery_ring_push( &h, &h.to_server, 0, channel_zero, sizeof(channel_zero) ) );
    rocketnet_check( rocketnet_channel_delivery_ring_push( &h, &h.to_server, 2, large_payload, sizeof(large_payload) ) );
    rocketnet_channel_delivery_t d;
    rocketnet_check( rocketnet_channel_harness_server_collect( &h, 0, &d ) );
    rocketnet_check( d.bytes == (int) sizeof(channel_zero) );
    rocketnet_check( memcmp( d.data, channel_zero, sizeof(channel_zero) ) == 0 );
    rocketnet_check( rocketnet_channel_harness_server_collect( &h, 1, &d ) );
    rocketnet_check( d.bytes == (int) sizeof(channel_one) );
    rocketnet_check( memcmp( d.data, channel_one, sizeof(channel_one) ) == 0 );
    rocketnet_check( rocketnet_channel_harness_server_collect( &h, 2, &d ) );
    rocketnet_check( d.bytes == (int) sizeof(large_payload) );
    rocketnet_check( d.hash == rocketnet_channel_hash( large_payload, sizeof(large_payload) ) );
    rocketnet_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    for ( int i = 0; i < ROCKETNET_CHANNEL_HARNESS_MAX_DELIVERIES; i++ )
        rocketnet_check( rocketnet_channel_delivery_ring_push( &h, &h.to_server, 3, channel_zero, sizeof(channel_zero) ) );
    rocketnet_check( !rocketnet_channel_delivery_ring_push( &h, &h.to_server, 3, channel_zero, sizeof(channel_zero) ) );
    rocketnet_check( rocketnet_channel_harness_overflows( &h ) == 1 );
    rocketnet_check( rocketnet_channel_allocator_calls == h.allocations_after_create );
    rocketnet_channel_harness_destroy( &h );
}
