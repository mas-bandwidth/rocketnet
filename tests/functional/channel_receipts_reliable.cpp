/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNReceipts over the deterministic lossy, duplicating and reordering link.

#include "channel_harness.h"

static void reliable_receipt_check( int condition, const char * what, int line )
{
    if ( condition )
        return;
    printf( "reliable receipts failed at line %d: %s\n", line, what );
    fflush( stdout );
    exit( 1 );
}

#define check_receipt( condition ) reliable_receipt_check( !!(condition), #condition, __LINE__ )

// RNReceipts BlockAckedOnlyWhenAllFragmentsAcked, seen from the receiver: a receipt
// is read only after the receiver holds the whole message. Each collect runs
// before the receipts of the same update, and the ACK of a message's last
// fragment reaches the sender at least one update after the receiver took that
// fragment, so a type-6 message is already collected when its receipt is
// read. A type-7 message is collected by then too, unless the ordered message
// before it is still missing (the receiver holds it in order). An early receipt
// (one sent on the ACK of a first fragment) breaks this.
static int receipt_receiver_has_whole( int matched, const uint8_t * seen )
{
    if ( matched < 3 )
        return seen[matched];
    return seen[matched] || ( matched > 3 && !seen[matched - 1] );
}

void reliable_receipts_over_lossy_link()
{
    enum
    {
        messages = 6,
        max_bytes = 8192
    };
    static const int lengths[messages] = { 3, 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17, 8192,
                                           4, 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 31, 8191 };
    static const int types[messages] = { ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT,
                                         ROCKETNET_RELIABILITY_RELIABLE_WITH_ACK_RECEIPT, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT,
                                         ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT, ROCKETNET_RELIABILITY_RELIABLE_ORDERED_WITH_ACK_RECEIPT };

    rocketnet_channel_harness_t h;
    check_receipt( rocketnet_channel_harness_create( &h ) );
    rocketnet_channel_harness_set_link( &h, 0.02, 0.05, 20.0f, 5.0f );
    check_receipt( rocketnet_channel_harness_connect( &h, 60 * 10 ) );

    uint8_t to_server[messages][max_bytes], to_client[messages][max_bytes];
    uint32_t client_serial[messages], server_serial[messages];
    uint8_t server_seen[messages] = {}, client_seen[messages] = {};
    uint8_t client_receipt[messages] = {}, server_receipt[messages] = {};
    int server_ordered = 2, client_ordered = 2;

    for ( int i = 0; i < messages; i++ )
    {
        for ( int j = 0; j < lengths[i]; j++ )
        {
            to_server[i][j] = (uint8_t) ( i * 31 + j * 7 + 1 );
            to_client[i][j] = (uint8_t) ( i * 29 + j * 11 + 2 );
        }

        client_serial[i] = rocketnet_channel_harness_client_send( &h, 0, types[i], to_server[i], lengths[i] );
        server_serial[i] = rocketnet_channel_harness_server_send( &h, 0, types[i], to_client[i], lengths[i] );
        check_receipt( client_serial[i] != 0 && server_serial[i] != 0 );
    }

    const double started = h.now;

    for ( int step = 0; step < 60 * 30; step++ )
    {
        // Random loss can discard every retry until the real reliable timeout.
        // This receipt-completion case needs a recovery phase: preserve latency,
        // jitter and duplicates, but stop dropping after five simulated seconds.
        // The default ten-second protocol timeout remains unchanged.
        if ( step == 60 * 5 )
            rocketnet_channel_harness_set_link( &h, 0.02, 0.05, 0.0f, 5.0f );

        check_receipt( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) );

        rocketnet_channel_delivery_t delivery = {};
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            int matched = -1;

            for ( int i = 0; i < messages; i++ )
                if ( delivery.bytes == lengths[i] && memcmp( delivery.data, to_server[i], (size_t) lengths[i] ) == 0 )
                {
                    matched = i;
                }

            check_receipt( matched >= 0 && !server_seen[matched] );
            if ( matched >= 3 )
            {
                check_receipt( matched == server_ordered + 1 );
                server_ordered = matched;
            }

            server_seen[matched] = 1;
        }

        while ( rocketnet_channel_harness_client_collect( &h, 0, &delivery ) )
        {
            int matched = -1;

            for ( int i = 0; i < messages; i++ )
                if ( delivery.bytes == lengths[i] && memcmp( delivery.data, to_client[i], (size_t) lengths[i] ) == 0 )
                {
                    matched = i;
                }

            check_receipt( matched >= 0 && !client_seen[matched] );
            if ( matched >= 3 )
            {
                check_receipt( matched == client_ordered + 1 );
                client_ordered = matched;
            }

            client_seen[matched] = 1;
        }

        rocketnet_message_receipt_t events[256];
        int count = rocketnet_message_client_receipts( h.message_client, events, 256 );
        check_receipt( count >= 0 && count <= 256 );
        for ( int j = 0; j < count; j++ )
        {
            int matched = -1;

            for ( int i = 0; i < messages; i++ )
                if ( events[j].serial == client_serial[i] )
                {
                    matched = i;
                }

            check_receipt( matched >= 0 && !client_receipt[matched] && events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            check_receipt( receipt_receiver_has_whole( matched, server_seen ) );
            client_receipt[matched] = 1;
        }

        count = rocketnet_message_server_receipts( h.message_server, h.client_slot, events, 256 );
        check_receipt( count >= 0 && count <= 256 );
        for ( int j = 0; j < count; j++ )
        {
            int matched = -1;

            for ( int i = 0; i < messages; i++ )
                if ( events[j].serial == server_serial[i] )
                {
                    matched = i;
                }

            check_receipt( matched >= 0 && !server_receipt[matched] && events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            check_receipt( receipt_receiver_has_whole( matched, client_seen ) );
            server_receipt[matched] = 1;
        }

        int complete = 0;

        for ( int i = 0; i < messages; i++ )
        {
            complete += client_receipt[i] && server_receipt[i] && server_seen[i] && client_seen[i];
        }

        if ( complete == messages )
            break;
    }

    for ( int i = 0; i < messages; i++ )
    {
        check_receipt( client_receipt[i] && server_receipt[i] && server_seen[i] && client_seen[i] );
    }

    // each receipt fires once: none follows the last

    for ( int step = 0; step < 60 * 3; step++ )
    {
        check_receipt( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) );

        rocketnet_message_receipt_t events[16];
        check_receipt( rocketnet_message_client_receipts( h.message_client, events, 16 ) == 0 );
        check_receipt( rocketnet_message_server_receipts( h.message_server, h.client_slot, events, 16 ) == 0 );

        rocketnet_channel_delivery_t delivery = {};
        check_receipt( !rocketnet_channel_harness_server_collect( &h, 0, &delivery ) );
        check_receipt( !rocketnet_channel_harness_client_collect( &h, 0, &delivery ) );
    }

    rocketnet_channel_harness_read_counters( &h );
    printf( "reliable receipts: each=%d elapsed_ms=%.1f resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
            messages, ( h.now - started ) * 1000.0, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    check_receipt( h.resends > 0 );
    check_receipt( rocketnet_channel_harness_overflows( &h ) == 0 );
    check_receipt( rocketnet_channel_allocator_calls == h.allocations_after_create );
    rocketnet_channel_harness_destroy( &h );
}
