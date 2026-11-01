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

static void unreliable_receipts_settle_config( rocketnet_channel_harness_t * h )
{
    const struct rn_endpoint_t * client_endpoint = &h->message_client->object.connections[0];
    const struct rn_endpoint_t * server_endpoint = &h->message_server->object.connections[h->client_slot];

    for ( int i = 0; i < 60 * 10; i++ )
    {
        if ( client_endpoint->ordered.config_received && server_endpoint->ordered.config_received &&
             client_endpoint->ordered.send.U >= client_endpoint->ordered.send.next &&
             server_endpoint->ordered.send.U >= server_endpoint->ordered.send.next )
            return;
        receipt_check( rocketnet_channel_harness_pump( h, 1.0 / 60.0 ) );
    }
}

void unreliable_receipts_over_lossy_link()
{
    enum
    {
        messages = 48,
        steps = 60 * 12
    };

    rocketnet_channel_harness_t h;
    receipt_check( rocketnet_channel_harness_create( &h ) );
    rocketnet_channel_harness_set_link( &h, 0.02, 0.05, 12.0f, 5.0f );
    receipt_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );
    unreliable_receipts_settle_config( &h );
    rocketnet_channel_harness_read_counters( &h );

    const uint64_t resends_at_connect = h.resends;                                 // the connection's reliable CONFIG may be resent while connecting (standard/MESSAGES.md m11)

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
            client_serial[step] = rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, to_server, 3 );
            server_serial[step] = rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, to_client, 3 );
            receipt_check( client_serial[step] != 0 && server_serial[step] != 0 );
        }

        receipt_check( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) );

        rocketnet_channel_delivery_t delivery = {};
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
        receipt_check( count >= 0 && count <= 256 );
        for ( int j = 0; j < count; j++ )
        {
            const int i = receipt_find_serial( client_serial, messages, events[j].serial );
            receipt_check( i >= 0 && !client_outcome[i] );
            receipt_check( events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED || events[j].status == ROCKETNET_RECEIPT_STATUS_LOSS );
            client_outcome[i] = (uint8_t) events[j].status;
            if ( events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED )
            {
                client_acked++;
            }
            else
            {
                client_lost++;
            }
        }

        count = rocketnet_message_server_receipts( h.message_server, h.client_slot, events, 256 );
        receipt_check( count >= 0 && count <= 256 );
        for ( int j = 0; j < count; j++ )
        {
            const int i = receipt_find_serial( server_serial, messages, events[j].serial );
            receipt_check( i >= 0 && !server_outcome[i] );
            receipt_check( events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED || events[j].status == ROCKETNET_RECEIPT_STATUS_LOSS );
            server_outcome[i] = (uint8_t) events[j].status;
            if ( events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED )
            {
                server_acked++;
            }
            else
            {
                server_lost++;
            }
        }

        if ( step >= messages && client_acked + client_lost == messages && server_acked + server_lost == messages )
            break;
    }

    for ( int i = 0; i < messages; i++ )
    {
        receipt_check( client_outcome[i] != 0 && server_outcome[i] != 0 );
        if ( client_outcome[i] == ROCKETNET_RECEIPT_STATUS_ACKED )
        {
            receipt_check( at_server[i] );
        }

        if ( server_outcome[i] == ROCKETNET_RECEIPT_STATUS_ACKED )
        {
            receipt_check( at_client[i] );
        }
    }

    rocketnet_channel_harness_read_counters( &h );
    printf( "receipt lossy: sent=%d each client_acked=%d client_lost=%d server_acked=%d server_lost=%d delivered_server=%d delivered_client=%d elapsed_ms=%.1f resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
            messages, client_acked, client_lost, server_acked, server_lost, client_delivered, server_delivered, ( h.now - start ) * 1000.0,
            (unsigned long long) h.resends, (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    receipt_check( client_acked + client_lost == messages && server_acked + server_lost == messages );
    receipt_check( h.resends == resends_at_connect );
    receipt_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    receipt_check( rocketnet_channel_allocator_calls == h.allocations_after_create );
    rocketnet_channel_harness_destroy( &h );
}

void promoted_receipts_over_lossy_reliable_blocks()
{
    rocketnet_channel_harness_t h;
    receipt_check( rocketnet_channel_harness_create( &h ) );
    rocketnet_channel_harness_set_link( &h, 0.02, 0.05, 20.0f, 5.0f );
    receipt_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) );

    static const int lengths[] = { 1412, 2 * ROCKETNET_MESSAGE_FRAGMENT_BYTES + 17, 8192 };
    uint8_t to_server[3][8192], to_client[3][8192];
    uint32_t client_serial[3], server_serial[3];
    uint8_t client_receipt[3] = {}, server_receipt[3] = {};
    uint8_t server_seen[3] = {}, client_seen[3] = {};

    for ( int i = 0; i < 3; i++ )
    {
        for ( int j = 0; j < lengths[i]; j++ )
        {
            to_server[i][j] = (uint8_t) ( i * 31 + j * 7 + 1 );
            to_client[i][j] = (uint8_t) ( i * 29 + j * 11 + 2 );
        }

        client_serial[i] = rocketnet_channel_harness_client_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, to_server[i], lengths[i] );
        server_serial[i] = rocketnet_channel_harness_server_send( &h, 0, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, to_client[i], lengths[i] );
        receipt_check( client_serial[i] != 0 && server_serial[i] != 0 );
    }

    const double started = h.now;

    // Exercise the promoted reliable-block resend path deterministically.
    // Random loss alone can spare every message packet for a seed, so drop one
    // packet from the client's first flight and deliver the rest.

    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_HOLD_PAYLOADS, ROCKETNET_CHANNEL_WIRE_PASS );
    receipt_check( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) );
    receipt_check( h.held_count > 0 );
    for ( int i = 1; i < h.held_count; i++ )
    {
        receipt_check( rocketnet_channel_harness_deliver_held( &h, &i, 1 ) == 1 );
    }

    rocketnet_channel_harness_clear_held( &h );
    rocketnet_channel_harness_set_wire( &h, ROCKETNET_CHANNEL_WIRE_PASS, ROCKETNET_CHANNEL_WIRE_PASS );

    for ( int step = 0; step < 60 * 30; step++ )
    {
        receipt_check( rocketnet_channel_harness_pump( &h, 1.0 / 60.0 ) );

        rocketnet_channel_delivery_t delivery = {};
        while ( rocketnet_channel_harness_server_collect( &h, 0, &delivery ) )
        {
            int matched = -1;

            for ( int i = 0; i < 3; i++ )
                if ( delivery.bytes == lengths[i] && memcmp( delivery.data, to_server[i], (size_t) lengths[i] ) == 0 )
                {
                    matched = i;
                }

            receipt_check( matched >= 0 && !server_seen[matched] );
            server_seen[matched] = 1;
        }

        while ( rocketnet_channel_harness_client_collect( &h, 0, &delivery ) )
        {
            int matched = -1;

            for ( int i = 0; i < 3; i++ )
                if ( delivery.bytes == lengths[i] && memcmp( delivery.data, to_client[i], (size_t) lengths[i] ) == 0 )
                {
                    matched = i;
                }

            receipt_check( matched >= 0 && !client_seen[matched] );
            client_seen[matched] = 1;
        }

        rocketnet_message_receipt_t events[256];
        int count = rocketnet_message_client_receipts( h.message_client, events, 256 );
        receipt_check( count >= 0 && count <= 256 );
        for ( int j = 0; j < count; j++ )
        {
            const int i = receipt_find_serial( client_serial, 3, events[j].serial );
            receipt_check( i >= 0 && !client_receipt[i] && events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            client_receipt[i] = 1;
        }

        count = rocketnet_message_server_receipts( h.message_server, h.client_slot, events, 256 );
        receipt_check( count >= 0 && count <= 256 );
        for ( int j = 0; j < count; j++ )
        {
            const int i = receipt_find_serial( server_serial, 3, events[j].serial );
            receipt_check( i >= 0 && !server_receipt[i] && events[j].status == ROCKETNET_RECEIPT_STATUS_ACKED );
            server_receipt[i] = 1;
        }

        if ( client_receipt[0] && client_receipt[1] && client_receipt[2] &&
             server_receipt[0] && server_receipt[1] && server_receipt[2] )
            break;
    }

    for ( int i = 0; i < 3; i++ )
    {
        receipt_check( client_receipt[i] && server_receipt[i] && server_seen[i] && client_seen[i] );
    }

    rocketnet_channel_harness_read_counters( &h );
    printf( "promoted receipts: lengths=1412,%d,8192 elapsed_ms=%.1f resends=%llu wire_to_server=%lld wire_to_client=%lld\n",
            lengths[1], ( h.now - started ) * 1000.0, (unsigned long long) h.resends,
            (long long) h.wire_bytes_to_server, (long long) h.wire_bytes_to_client );
    receipt_check( h.resends > 0 );
    receipt_check( rocketnet_channel_harness_overflows( &h ) == 0 );
    receipt_check( rocketnet_channel_allocator_calls == h.allocations_after_create );
    rocketnet_channel_harness_destroy( &h );
}
