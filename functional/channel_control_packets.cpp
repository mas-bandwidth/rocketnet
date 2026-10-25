/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The four control packets the harness never sent: the disconnect burst
// (type 6), the standalone ack (type 8), the fragment (type 9) and the
// denied (type 1). Each is driven through the harness's own send path so
// the capture tee records it on interface 0, and each is delivered through
// rocketnet_channel_endpoint_update so the receiving end's reader runs and
// the tee records interface 1.
#include "channel_harness.h"

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
        rocketnet_channel_harness_pump( &h, CONTROL_TICK );
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
        rocketnet_channel_harness_pump( &h, CONTROL_TICK );
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
    // A payload becomes ACK-eligible only after message processing.
    control_check( rocketnet_message_server_update( h.message_server, h.now ) == ROCKETNET_UPDATE_OK, "message update" );

    int ack_size = rocketnet_server_protocol_write_ack_if_due( &h.server, 0, h.now, packet );
    control_check( ack_size >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES, "standalone ack not due" );
    control_check( packet[0] == ROCKETNET_CONNECTION_ACK_PACKET, "ack packet type" );
    rocketnet_channel_endpoint_send_control( &h.server_endpoint, packet, ack_size );
    rocketnet_channel_endpoint_update( &h.client_endpoint, h.now );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    printf( "test_control_standalone_ack_each_end: server sent its standalone ack, the client read it\n" );
}

void control_fragment_packets_each_end()
{
    rocketnet_channel_harness_t h;
    control_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    control_check( rocketnet_channel_harness_connect( &h, 60 * 10 ) != 0, "connect" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );

    uint8_t payload[3000];
    for ( int i = 0; i < 3000; i++ )
        payload[i] = (uint8_t) ( i * 7 );
    uint8_t packets[CONTROL_MAX_FRAGMENTS * ROCKETNET_SEND_BUDGET];
    int packet_bytes[CONTROL_MAX_FRAGMENTS];
    if ( !h.client.reassembly )
        h.client.reassembly = rocketnet_reassembly_set_create( NULL, 1, 4, 16384, 4 );

    // client to server: fragments arrive, the server reassembles one payload
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

    // server to client: fragments arrive, the client reassembles one payload
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

    printf( "test_control_fragment_packets_each_end: both ends reassembled one fragment payload\n" );
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
        const int slot = rocketnet_client_slots_assign( h.server.client_slots, mapping->client_id, mapping_index, mapping->user_data );
        control_check( slot >= 0, "slot assign" );
        mapping->control->client_index = slot;
        mapping->control->established = 1;
    }
    const uint64_t denied_before = h.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS];
    for ( int i = 0; i < 60 * 5; i++ )
        rocketnet_channel_harness_pump( &h, CONTROL_TICK );
    control_check( h.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED, "client not denied" );
    control_check( h.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] > denied_before, "no-slots counter did not rise" );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    printf( "test_control_denied_when_server_full: client denied, no-slots counter rose\n" );
}
