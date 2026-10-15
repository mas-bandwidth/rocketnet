/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The client message object (docs/CHANNELS.md 10 to 12). It wraps the
// transport, the connection's region and the channel path: the application
// starts a message, writes it in place into the connection's region, finishes
// it with its size (receipt serial nonzero), and the object delivers the
// region record's bytes to the transport frame. The reliability values are
// ROCKETNET_UNRELIABLE and ROCKETNET_RELIABLE_ORDERED; the other six are
// refused until their cards enable them. Exhaustion kicks (R5); no OS
// allocation happens on the send/receive/ack/retransmit/deliver path (R2).

#include "rocketnet_message_object.h"

#include <stdlib.h>

// The app writes the payload once into its region record (start, finish); each
// packet write copies the record's bytes into the frame the transport carries
// (rn_ordered_write, copy (a)), because a reliable record must outlive the
// frame for resends and the transport encrypts the frame in place.

struct rocketnet_message_client_t * rocketnet_message_client_create(
    void * context, const uint8_t * connect_token,
    const struct rocketnet_transport_config_t * transport_config,
    const struct rocketnet_message_config_t * message_config,
    void * memory, uint64_t memory_bytes, uint32_t send_kbps )
{
    (void) context;
    (void) connect_token;
    (void) transport_config;

    struct rocketnet_message_client_t * client = ( struct rocketnet_message_client_t * ) malloc( sizeof( *client ) );
    if ( !client )
        return NULL;
    memset( client, 0, sizeof( *client ) );

    const uint32_t kbps = send_kbps ? send_kbps : ROCKETNET_DEFAULT_CLIENT_SEND_KBPS;
    client->node.budgeted = 1;

    if ( memory )
        client->node.arena = rn_arena_create_in_block( memory, memory_bytes, message_config, 1, &client->node.counters );
    else
        client->node.arena = rn_arena_create( NULL, message_config, 1, &client->node.counters );
    if ( !client->node.arena )
    {
        free( client );
        return NULL;
    }
    client->node.region = rn_arena_region( client->node.arena, 0 );
    if ( client->node.region )
        rn_region_carve( client->node.region );
    if ( !rn_message_node_ordered_setup( &client->node, message_config, 4, 1, kbps, 0.0 ) )
    {
        rn_arena_destroy( client->node.arena );
        free( client );
        return NULL;
    }
    client->node.next_packet_id = 1;
    client->connected = 1;
    return client;
}

void rocketnet_message_client_destroy( struct rocketnet_message_client_t * client )
{
    if ( !client )
        return;
    rn_arena_destroy( client->node.arena );
    free( client );
}

int rocketnet_message_client_update( struct rocketnet_message_client_t * client, double time )
{
    if ( !client || !client->connected )
        return ROCKETNET_MESSAGE_ERROR_NONE;
    rn_message_node_t * n = &client->node;
    n->time = time;

    // The release rule: every received message is released before the next
    // update. Anything held is a misuse: asserts in debug, a named error in
    // release builds.
    if ( rn_message_node_any_unreleased( n ) )
    {
        if ( rocketnet_assert_function_pointer )
            rocketnet_assert_function_pointer( "received message not released before update",
                                              "rocketnet_message_client_update", __FILE__, __LINE__ );
        return ROCKETNET_ERROR_MESSAGE_NOT_RELEASED;
    }

    rn_message_node_decide( n );
    rn_message_node_init_deliveries( n );

    if ( n->disconnect_reason != 0 )
        return ROCKETNET_MESSAGE_ERROR_NONE;

    // Send the update's packets and deliver them to the peer transport.
    struct rocketnet_message_server_t * server = client->peer;
    if ( server && server->used[0] )
    {
        const int count = rn_message_node_write_packets( n, time, 4, 1 );
        for ( int p = 0; p < count; p++ )
        {
            rn_message_node_t * sn = &server->slots[0];
            rn_message_node_open_frame( sn, &n->frames[p] );
            rn_ordered_acks( &n->ordered, &n->frames[p].id, 1, time );
            rn_message_node_decide( sn );
        }
    }
    return ROCKETNET_MESSAGE_ERROR_NONE;
}

void rocketnet_message_client_send_packets( struct rocketnet_message_client_t * client )
{
    // The in-memory pair delivers during update; a socket transport would seal
    // and flush here. Nothing to do for the unit suite.
    (void) client;
}

static int reliability_to_channel_type( int reliability )
{
    if ( reliability == ROCKETNET_UNRELIABLE )
        return ROCKETNET_CHANNEL_TYPE_UNRELIABLE;
    if ( reliability == ROCKETNET_RELIABLE_ORDERED )
        return ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED;
    return -1;
}

uint8_t * rocketnet_message_client_start( struct rocketnet_message_client_t * client, int channel, int reliability, int bytes )
{
    if ( !client || !client->connected || bytes < 0 )
        return NULL;
    const int type = reliability_to_channel_type( reliability );
    if ( type < 0 )
        return NULL;
    uint8_t * p = rn_channels_start( &client->node.ordered.channels, (uint32_t) channel, type, (uint32_t) bytes );
    if ( !p )
    {
        // The region is exhausted: kick (R5) and return NULL.
        client->node.disconnect_reason = MESSAGE_MEMORY_EXHAUSTED;
        client->connected = 0;
    }
    return p;
}

int rocketnet_message_client_finish( struct rocketnet_message_client_t * client, int bytes_written )
{
    if ( !client || !client->connected )
        return 0;
    // A nonzero receipt serial for every send (CHANNELS.md 7.4).
    rn_channels_finish( &client->node.ordered.channels, bytes_written );
    return (int) client->node.next_packet_id;
}

void rocketnet_message_client_abort( struct rocketnet_message_client_t * client )
{
    if ( !client )
        return;
    if ( client->node.pending_unreliable )
    {
        rn_send_store_free( &client->node.region->store, client->node.upending_chunk );
        client->node.pending_unreliable = 0;
    }
    else
    {
        rn_channels_cancel( &client->node.ordered.channels );
    }
}

int rocketnet_message_client_receive( struct rocketnet_message_client_t * client, int channel, const uint8_t ** data, int * bytes )
{
    if ( !client || !data || !bytes )
        return 0;
    rn_message_node_t * n = &client->node;
    for ( uint32_t i = 0; i < n->deliveries_count; i++ )
    {
        rn_message_delivery_t & d = n->deliveries[i];
        if ( !d.consumed && d.channel == channel )
        {
            d.consumed = 1;
            *data = d.data;
            *bytes = d.bytes;
            return 1;
        }
    }
    return 0;
}

void rocketnet_message_client_release( struct rocketnet_message_client_t * client, const uint8_t * data )
{
    if ( client )
        rn_message_node_release( &client->node, data );
}

int rocketnet_message_client_disconnect_reason( const struct rocketnet_message_client_t * client )
{
    if ( !client )
        return 0;
    return client->node.disconnect_reason;
}

void rocketnet_message_client_set_send_kbps( struct rocketnet_message_client_t * client, uint32_t kbps )
{
    if ( client && client->node.budgeted )
        rocketnet_send_budget_set_kbps( &client->node.budget, kbps, client->node.time );
}
