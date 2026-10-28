/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The control packets the harness never sent: the disconnect burst
// (type 6), the standalone ack (type 8), the fragment (type 9) and the
// denied (type 1). The fragment case uses the public client and server APIs
// over the portable transport; the other cases use the channel harness.
#include "channel_harness.h"
#include "../sdk/rocketnet_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTROL_TICK              ( 1.0 / 60.0 )
#define CONTROL_MAX_FRAGMENTS     16

static void control_check( bool condition, const char * label )
{
    if ( condition )
        return;
    printf( "control packets failed: %s\n", label );
    fflush( stdout );
    exit( 1 );
}

void control_disconnect_burst_each_end()
{
    rocketnet_channel_harness_t h;
    control_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    control_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );

    // the client disconnects: the burst of type 6 packets goes out through
    // the tee; the server reads them and releases the slot

    rocketnet_channel_endpoint_disconnect( &h.client_endpoint, 0 );

    for ( int i = 0; i < 30; i++ )
    {
        rocketnet_channel_harness_pump( &h, CONTROL_TICK );
    }

    control_check( !rocketnet_client_slots_connected( h.server.client_slots, h.client_slot ), "server slot still connected after client disconnect" );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    // the server disconnects: the same burst the other way, and the client
    // reads the server's type 6

    control_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    control_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );
    rocketnet_channel_endpoint_disconnect( &h.server_endpoint, 0 );

    for ( int i = 0; i < 30; i++ )
    {
        rocketnet_channel_harness_pump( &h, CONTROL_TICK );
    }

    control_check( h.client.state != ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED, "client still connected after server disconnect" );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    printf( "test_control_disconnect_burst_each_end: both ends read the other's type 6\n" );
}

void control_standalone_ack_each_end()
{
    rocketnet_channel_harness_t h;
    control_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    control_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );

    // the server side: the client writes more than the threshold of payload
    // packets and sends each; the server is updated (not pumped) so it sends
    // no payload back, and its ack debt reaches the threshold

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];

    // a SINGLE message: three data bytes and the trailing form byte, so the
    // capture's payload records are valid message payloads, not refusals

    uint8_t payload[4] = { 1, 2, 3, ROCKETNET_MESSAGE_FORM_SINGLE };
    const int sends = ROCKETNET_ACK_DEBT_THRESHOLD + 8;
    for ( int i = 0; i < sends; i++ )
    {
        const int size = rocketnet_client_protocol_write_payload( &h.client, payload, 4, h.now, packet );
        control_check( size > 0, "write payload" );
        rocketnet_channel_endpoint_send_control( &h.client_endpoint, packet, size );
    }

    // A payload becomes ACK-eligible only after message processing

    control_check( rocketnet_message_server_update( h.message_server, h.now ) == ROCKETNET_UPDATE_OK, "message update" );

    const uint64_t client_sent_through = h.client.ack.highest_sequence;
    const uint64_t client_ack_accepted_before = h.client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED];
    const uint64_t client_packets_received_before = h.client_counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    int ack_size = rocketnet_server_protocol_write_ack_if_due( &h.server, 0, h.now, packet );
    control_check( ack_size >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES, "standalone ack not due" );
    control_check( packet[0] == ROCKETNET_CONNECTION_ACK_PACKET, "ack packet type" );
    rocketnet_channel_endpoint_send_control( &h.server_endpoint, packet, ack_size );
    rocketnet_channel_endpoint_update( &h.client_endpoint, h.now );
    control_check( h.client_counters.values[ROCKETNET_COUNTER_CLIENT_ACK_ACCEPTED] == client_ack_accepted_before + 1,
                   "client did not accept server standalone ack exactly once" );
    control_check( h.client_counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == client_packets_received_before + 1,
                   "client did not receive exactly one server standalone ack packet" );
    control_check( h.client.ack.largest_acked_sequence == client_sent_through,
                   "server standalone ack did not complete the client's sent sequences" );
    control_check( h.client.ack.ack_debt == 0, "client ACK debt remains after server ack" );

    // Mirror the flow: the server sends enough payloads for the client's
    // standalone ACK threshold, then the production client protocol emits
    // one ACK that the production server protocol accepts. Keep the simulated
    // clock inside the client's normal send-rate window so it selects the
    // standalone-ACK branch rather than the scheduled keep-alive branch.

    const uint64_t server_ack_accepted_before = h.server_counters.values[ROCKETNET_COUNTER_ACK_ACCEPTED];
    const uint64_t server_packets_received_before = h.server_counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED];
    const uint64_t server_sent_through_before = h.server.acks->endpoints[0].highest_sequence;

    for ( int i = 0; i < sends; i++ )
    {
        const int size = rocketnet_server_protocol_write_payload( &h.server, 0, payload, 4, h.now, packet );
        control_check( size > 0, "server write payload" );
        rocketnet_channel_endpoint_send_control( &h.server_endpoint, packet, size );
    }

    control_check( rocketnet_message_client_update( h.message_client, h.now ) == ROCKETNET_UPDATE_OK,
                   "client message update" );
    control_check( h.client.ack.ack_debt >= ROCKETNET_ACK_DEBT_THRESHOLD,
                   "client ACK debt did not reach threshold" );
    rocketnet_platform_atomic_store_double( &h.client.last_send_time, h.now );
    control_check( h.client.last_send_time == h.now, "client send-time fixture baseline" );

    int client_ack_bytes = 0;
    rocketnet_client_protocol_update( &h.client, h.now, packet, &client_ack_bytes );
    control_check( client_ack_bytes >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES,
                   "client standalone ack not written" );
    control_check( packet[0] == ROCKETNET_CONNECTION_ACK_PACKET, "client standalone ack packet type" );
    rocketnet_channel_endpoint_send_control( &h.client_endpoint, packet, client_ack_bytes );
    control_check( h.client.ack.ack_debt == 0, "client did not send standalone ack" );
    rocketnet_channel_endpoint_update( &h.server_endpoint, h.now );
    control_check( h.server_counters.values[ROCKETNET_COUNTER_ACK_ACCEPTED] == server_ack_accepted_before + 1,
                   "server did not accept client standalone ack exactly once" );
    control_check( h.server_counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == server_packets_received_before + 1,
                   "server did not receive exactly one client standalone ack packet" );
    control_check( h.server.acks->endpoints[0].largest_acked_sequence == server_sent_through_before + (uint64_t) sends,
                   "client standalone ack did not complete the server's sent sequences" );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    printf( "test_control_standalone_ack_each_end: both protocols accepted standalone ACKs and completed sent sequences\n" );
}

void control_fragment_packets_each_end()
{
    // Keep the deterministic control capture for the pinned wire corpus while
    // the public API exercise below proves that both real endpoints deliver
    // the complete message to an application consumer.

    rocketnet_channel_harness_t h;
    control_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    control_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );

    uint8_t payload[3000];

    for ( int i = 0; i < 3000; i++ )
    {
        payload[i] = (uint8_t) ( i * 7 );
    }

    uint8_t packets[CONTROL_MAX_FRAGMENTS * ROCKETNET_SEND_BUDGET];
    int packet_bytes[CONTROL_MAX_FRAGMENTS];

    if ( !h.client.reassembly )
    {
        h.client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, 4 );
    }

    const uint64_t server_reassembled_before = h.server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED];

    const int client_count = rocketnet_client_protocol_write_fragments( &h.client, payload, 3000, h.now,
                                                                        packets, packet_bytes, CONTROL_MAX_FRAGMENTS );
    control_check( client_count >= 2, "client write fragments" );
    for ( int i = 0; i < client_count; i++ )
    {
        rocketnet_channel_endpoint_send_control( &h.client_endpoint, packets + (size_t) i * ROCKETNET_SEND_BUDGET, packet_bytes[i] );
        rocketnet_channel_endpoint_update( &h.server_endpoint, h.now );
    }

    control_check( h.server_counters.values[ROCKETNET_COUNTER_FRAGMENT_PAYLOAD_REASSEMBLED] == server_reassembled_before + 1,
                   "server reassembled one fragment payload" );

    const uint64_t client_reassembled_before = h.client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED];

    const int server_count = rocketnet_server_protocol_write_fragments( &h.server, 0, payload, 3000, h.now,
                                                                        packets, packet_bytes, CONTROL_MAX_FRAGMENTS );
    control_check( server_count >= 2, "server write fragments" );
    for ( int i = 0; i < server_count; i++ )
    {
        rocketnet_channel_endpoint_send_control( &h.server_endpoint, packets + (size_t) i * ROCKETNET_SEND_BUDGET, packet_bytes[i] );
        rocketnet_channel_endpoint_update( &h.client_endpoint, h.now );
    }

    control_check( h.client_counters.values[ROCKETNET_COUNTER_CLIENT_FRAGMENT_PAYLOAD_REASSEMBLED] == client_reassembled_before + 1,
                   "client reassembled one fragment payload" );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    if ( h.client.reassembly )
    {
        rocketnet_reassembly_set_destroy( h.client.reassembly );
        h.client.reassembly = NULL;
    }

    rocketnet_channel_harness_destroy( &h );

    extern void rocketnet_test_public_fragment_delivery();
    rocketnet_test_public_fragment_delivery();
}

void control_denied_when_server_full()
{
    rocketnet_channel_harness_t h;
    control_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );

    // fill all four server slots with distinct client ids before the client
    // connects, so the server has none free for the request. each filler is
    // a connected client as the server sees one: a live, established
    // encryption mapping at its own address, assigned the slot the way the
    // connect path assigns it, with no timeout so it holds the slot for the
    // whole case, as the connect path gives every slot it assigns a live
    // mapping behind it.

    uint8_t user_data[ROCKETNET_USER_DATA_BYTES];
    memset( user_data, 9, sizeof( user_data ) );

    for ( int i = 0; i < 4; i++ )
    {
        char filler_address_string[64];
        snprintf( filler_address_string, sizeof( filler_address_string ), "198.51.100.%d:5000", 20 + i );

        struct rocketnet_address_t filler_address;
        control_check( rocketnet_address_parse( &filler_address, filler_address_string ), "filler address" );

        const int mapping_index = rocketnet_encryption_map_add( h.server.encryption_map, &filler_address, h.now + h.server.handshake_deadline_seconds );
        control_check( mapping_index >= 0, "filler mapping add" );

        struct rocketnet_encryption_mapping_t * mapping = rocketnet_encryption_map_entry( h.server.encryption_map, mapping_index );
        mapping->client_id = (uint64_t) ( 1000 + i );
        memcpy( mapping->user_data, user_data, ROCKETNET_USER_DATA_BYTES );
        mapping->control->timeout_seconds = -1;
        mapping->control->last_receive_time = h.now;
        mapping->last_send_time = h.now;
        mapping->last_keep_alive_send_time = h.now;
        const uint64_t mapping_generation =
            rocketnet_encryption_map_owner_generation( h.server.encryption_map, mapping_index );
        const int slot = rocketnet_client_slots_assign( h.server.client_slots, mapping->client_id, mapping_index,
                                                        mapping_generation, mapping->user_data );
        control_check( slot >= 0, "slot assign" );
        mapping->control->client_index = slot;
        mapping->control->established = 1;
    }

    const uint64_t denied_before = h.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS];

    for ( int i = 0; i < 60 * 5; i++ )
    {
        rocketnet_channel_harness_pump( &h, CONTROL_TICK );
    }

    control_check( h.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED, "client not denied" );
    control_check( h.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] > denied_before, "no-slots counter did not rise" );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    printf( "test_control_denied_when_server_full: client denied, no-slots counter rose\n" );
}
