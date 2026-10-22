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
        reliable_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE, client_data[i], lengths[i] ) != 0 );
        reliable_check( rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE, server_data[i], lengths[i] ) != 0 );
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
    printf( "test_reliable_mixed_sizes_over_lossy_link elapsed=%.3fs resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
            h.now - started, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    reliable_check( server_seen == 15 && client_seen == 15 );
    reliable_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    reliable_check( rocketnet_channel_allocator_calls == h.allocations_after_create );
    rocketnet_channel_harness_destroy( &h );
}

// RNWindow Accept: a reliable record that arrives again while it is inside the
// window and above a gap (so R has not passed it) is a duplicate, and it is
// delivered once. Without the received-bit check the second copy is accepted
// as new and the user reads the message twice. The transport's replay window
// drops a byte-identical packet first, so the second copy here is the
// client's own resend in a new packet: the scripted order holds the client's
// payload packets and drops every packet to the client, so no ack lands.
// The large message A takes number n and fills its packet; the small message
// B takes n + 1. B's first packet arrives, then B's resend, then A's resend.
void reliable_duplicate_after_gap_delivered_once()
{
    enum { a_bytes = ROCKETNET_MESSAGE_SINGLE_BYTES - 8, b_bytes = 4 };
    rocketnet_channel_harness_t h;
    reliable_check( rocketnet_channel_harness_create( &h ) );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );
    reliable_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );
    for ( int i = 0; i < 30; i++ )
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );

    static uint8_t a[a_bytes];
    for ( int i = 0; i < a_bytes; i++ )
        a[i] = (uint8_t) ( i * 13 + 7 );
    const uint8_t b[b_bytes] = { 0xB0, 0xB1, 0xB2, 0xB3 };
    const uint64_t duplicates_before = h.message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE];

    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS, ROCKETNET_CHANNEL_WIRE_DROP );
    reliable_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE, a, a_bytes ) != 0 );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
    const int a_end = h.held_count;
    reliable_check( rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_RELIABLE, b, b_bytes ) != 0 );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
    const int b_end = h.held_count;
    reliable_check( a_end >= 1 && b_end > a_end );
    for ( int i = 0; i < a_end; i++ )
        reliable_check( h.held_bytes[i] > a_bytes );
    for ( int i = a_end; i < b_end; i++ )
        reliable_check( h.held_bytes[i] < a_bytes );

    int order[ROCKETNET_CHANNEL_HARNESS_MAX_HELD];
    int count = 0;
    for ( int i = a_end; i < b_end; i++ )
        order[count++] = i;
    reliable_check( rocketnet_channel_harness_deliver_held( &h, order, count ) == count );
    rocketnet_channel_harness_clear_held( &h );

    int a_seen = 0, b_seen = 0;
    rocketnet_channel_delivery_t delivery;
    // Wait for the client's resends: A's fills a packet of its own and B's
    // rides a small one. The resends are paced, so wait for both.
    int a_resend = 0, b_resend = 0;
    for ( int step = 0; step < 60 * 5 && !( a_resend && b_resend ); step++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
            b_seen++;
        a_resend = b_resend = 0;
        for ( int i = 0; i < h.held_count; i++ )
        {
            a_resend += h.held_bytes[i] > a_bytes;
            b_resend += h.held_bytes[i] < a_bytes;
        }
    }
    reliable_check( b_seen == 1 );
    reliable_check( a_resend > 0 && b_resend > 0 );

    // B's resend (every small held packet) first, then A's resend.
    count = 0;
    int b_resends = 0;
    for ( int i = 0; i < h.held_count; i++ )
        if ( h.held_bytes[i] < a_bytes )
            order[count++] = i;
    b_resends = count;
    reliable_check( rocketnet_channel_harness_deliver_held( &h, order, count ) == count );
    rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
    while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
    {
        reliable_check( delivery.bytes == b_bytes && memcmp( delivery.data, b, b_bytes ) == 0 );
        b_seen++;
    }
    const uint64_t duplicates = h.message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_DUPLICATE] - duplicates_before;
    printf( "test_reliable_duplicate_after_gap_delivered_once: b_first_packets=%d held=%d b_resend_packets=%d duplicates=%llu b_seen=%d\n",
            b_end - a_end, h.held_count, b_resends, (unsigned long long) duplicates, b_seen );
    reliable_check( b_seen == 1 );
    reliable_check( duplicates >= 1 );

    count = 0;
    for ( int i = 0; i < h.held_count; i++ )
        if ( h.held_bytes[i] > a_bytes )
            order[count++] = i;
    reliable_check( rocketnet_channel_harness_deliver_held( &h, order, count ) == count );
    rocketnet_channel_harness_clear_held( &h );
    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_PASS, ROCKETNET_CHANNEL_WIRE_PASS );
    for ( int step = 0; step < 60 * 3; step++ )
    {
        rocketnet_channel_harness_pump( &h, 1.0 / 60.0 );
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            if ( delivery.bytes == a_bytes && memcmp( delivery.data, a, a_bytes ) == 0 )
                a_seen++;
            else
            {
                reliable_check( delivery.bytes == b_bytes && memcmp( delivery.data, b, b_bytes ) == 0 );
                b_seen++;
            }
        }
    }
    reliable_check( a_seen == 1 && b_seen == 1 );
    reliable_check( h.held_overflow == 0 );
    reliable_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    reliable_check( rocketnet_channel_allocator_calls == h.allocations_after_create );
    rocketnet_channel_harness_destroy( &h );
}
