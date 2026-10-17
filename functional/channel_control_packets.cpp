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

    printf( "control_disconnect_burst_each_end: both ends read the other's type 6\n" );
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
    uint8_t payload[4] = { 1, 2, 3, 4 };
    const int sends = ROCKETNET_ACK_DEBT_THRESHOLD + 8;
    for ( int i = 0; i < sends; i++ )
    {
        const int size = rocketnet_client_protocol_write_payload( &h.client, payload, 4, h.now, packet );
        control_check( size > 0, "write payload" );
        rocketnet_channel_endpoint_send_control( &h.client_endpoint, packet, size );
    }
    rocketnet_channel_endpoint_update( &h.server_endpoint, h.now );

    int ack_size = rocketnet_server_protocol_write_ack_if_due( &h.server, 0, h.now, packet );
    control_check( ack_size >= ROCKETNET_CONNECTION_ACK_MIN_WIRE_BYTES, "standalone ack not due" );
    control_check( packet[0] == ROCKETNET_CONNECTION_ACK_PACKET, "ack packet type" );
    rocketnet_channel_endpoint_send_control( &h.server_endpoint, packet, ack_size );
    rocketnet_channel_endpoint_update( &h.client_endpoint, h.now );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    printf( "control_standalone_ack_each_end: server sent its standalone ack, the client read it\n" );
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
    rocketnet_channel_harness_destroy( &h );

    printf( "control_fragment_packets_each_end: both ends reassembled one fragment payload\n" );
}

void control_denied_when_server_full()
{
    rocketnet_channel_harness_t h;
    control_check( rocketnet_channel_harness_create( &h ) != 0, "create" );
    rocketnet_channel_harness_set_link( &h, 0.0, 0.0, 0.0f, 0.0f );

    // fill all four server slots with distinct client ids before the client
    // connects, so the server has none free for the request
    uint8_t user_data[4] = { 9, 9, 9, 9 };
    for ( int i = 0; i < 4; i++ )
    {
        const int slot = rocketnet_client_slots_assign( h.server.client_slots, (uint64_t) ( 1000 + i ), i, user_data );
        control_check( slot >= 0, "slot assign" );
    }
    const uint64_t denied_before = h.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS];
    for ( int i = 0; i < 60 * 5; i++ )
        rocketnet_channel_harness_pump( &h, CONTROL_TICK );
    control_check( h.client.state == ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTION_DENIED, "client not denied" );
    control_check( h.server_counters.values[ROCKETNET_COUNTER_CONNECTION_REQUEST_NO_SLOTS] > denied_before, "no-slots counter did not rise" );
    control_check( rocketnet_channel_harness_overflows( &h ) == 0, "delivery ring overflow" );
    rocketnet_channel_harness_destroy( &h );

    printf( "control_denied_when_server_full: client denied, no-slots counter rose\n" );
}
