/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNReceipts over the deterministic lossy, duplicating, reordering link.

#include "channel_harness.h"

static void receipt_functional_check( int condition, const char * what, int line )
{
    if ( condition )
        return;
    printf( "channel receipts failed at line %d: %s\n", line, what );
    fflush( stdout );
    exit( 1 );
}

#define receipt_check( condition ) receipt_functional_check( !!(condition), #condition, __LINE__ )

static int receipt_find_serial( const uint32_t * serials, int count, uint32_t serial )
{
    for ( int i = 0; i < count; i++ )
        if ( serials[i] == serial )
            return i;
    return -1;
}

void unreliable_receipts_over_lossy_link()
{
    enum { messages = 48, steps = 60 * 12 };
    rocketnet_channel_harness_t h;
    receipt_check( rocketnet_channel_harness_create( &h ) );
    rocketnet_channel_harness_set_link( &h, 0.02, 0.05, 12.0f, 5.0f );
    receipt_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );

    uint32_t client_serial[messages] = {}, server_serial[messages] = {};
    uint8_t client_outcome[messages] = {}, server_outcome[messages] = {};
    uint8_t at_server[messages] = {}, at_client[messages] = {};
    int client_acked = 0, client_lost = 0, server_acked = 0, server_lost = 0;
    int client_delivered = 0, server_delivered = 0;
    const double start = h.now;

    for ( int step = 0; step < steps; step++ )
    {
        if ( step < messages )
        {
            const uint8_t to_server[3] = { 0xC1, (uint8_t) step, 0x5A };
            const uint8_t to_client[3] = { 0xC2, (uint8_t) step, 0xA5 };
            client_serial[step] = rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT, to_server, 3 );
            server_serial[step] = rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_UNRELIABLE_WITH_ACK_RECEIPT, to_client, 3 );
            receipt_check( client_serial[step] != 0 && server_serial[step] != 0 );
        }

        receipt_check( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) );
        rocketnet_channel_delivery_t delivery;
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            receipt_check( delivery.bytes == 3 && delivery.data[0] == 0xC1 && delivery.data[2] == 0x5A );
            const int i = delivery.data[1];
            receipt_check( i >= 0 && i < messages && !at_server[i] );
            at_server[i] = 1;
            client_delivered++;
        }
        while ( rocketnet_channel_harness_client_collect( &h, 0, &delivery ) )
        {
            receipt_check( delivery.bytes == 3 && delivery.data[0] == 0xC2 && delivery.data[2] == 0xA5 );
            const int i = delivery.data[1];
            receipt_check( i >= 0 && i < messages && !at_client[i] );
            at_client[i] = 1;
            server_delivered++;
        }

        rocketnet_message_receipt_t events[256];
        int count = rocketnet_message_client_receipts( h.message_client, events, 256 );
        for ( int j = 0; j < count; j++ )
        {
            const int i = receipt_find_serial( client_serial, messages, events[j].serial );
            receipt_check( i >= 0 && !client_outcome[i] );
            receipt_check( events[j].status == ROCKETNET_RECEIPT_ACKED || events[j].status == ROCKETNET_RECEIPT_LOSS );
            client_outcome[i] = (uint8_t) events[j].status;
            if ( events[j].status == ROCKETNET_RECEIPT_ACKED ) client_acked++; else client_lost++;
        }
        count = rocketnet_message_server_receipts( h.message_server, h.client_slot, events, 256 );
        for ( int j = 0; j < count; j++ )
        {
            const int i = receipt_find_serial( server_serial, messages, events[j].serial );
            receipt_check( i >= 0 && !server_outcome[i] );
            receipt_check( events[j].status == ROCKETNET_RECEIPT_ACKED || events[j].status == ROCKETNET_RECEIPT_LOSS );
            server_outcome[i] = (uint8_t) events[j].status;
            if ( events[j].status == ROCKETNET_RECEIPT_ACKED ) server_acked++; else server_lost++;
        }
        if ( step >= messages && client_acked + client_lost == messages && server_acked + server_lost == messages )
            break;
    }

    for ( int i = 0; i < messages; i++ )
    {
        receipt_check( client_outcome[i] != 0 && server_outcome[i] != 0 );
        if ( client_outcome[i] == ROCKETNET_RECEIPT_ACKED ) receipt_check( at_server[i] );
        if ( server_outcome[i] == ROCKETNET_RECEIPT_ACKED ) receipt_check( at_client[i] );
    }
    rocketnet_channel_harness_read_counters( &h );
    printf( "receipt lossy: sent=%d each client_acked=%d client_lost=%d server_acked=%d server_lost=%d delivered_server=%d delivered_client=%d elapsed_ms=%.1f resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
            messages, client_acked, client_lost, server_acked, server_lost, client_delivered, server_delivered, ( h.now - start ) * 1000.0,
            (unsigned long long) h.resends, (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    receipt_check( client_acked + client_lost == messages && server_acked + server_lost == messages );
    receipt_check( h.resends == 0 );
    receipt_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    receipt_check( rocketnet_channel_allocator_calls == h.allocation_snapshot );
    rocketnet_channel_harness_destroy( &h );
}
