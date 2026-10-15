/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server message object (docs/CHANNELS.md 10 to 12). It owns one region
// per client slot, reset on slot release; a slot whose region is exhausted is
// disconnected with MESSAGE_MEMORY_EXHAUSTED and reset for the next client
// (R5, RNSlots). The receive and release contract is the client's: views are
// valid until the next update and must be released before it.

#include "rocketnet_message_object.h"

#include <stdlib.h>

struct rocketnet_message_server_t * rocketnet_message_server_create(
    void * context, const char * address,
    const struct rocketnet_transport_config_t * transport_config,
    const struct rocketnet_message_config_t * message_config,
    void * memory, uint64_t memory_bytes, uint32_t per_client_send_kbps )
{
    (void) address;
    (void) transport_config;

    struct rocketnet_message_server_t * server = ( struct rocketnet_message_server_t * ) malloc( sizeof( *server ) );
    if ( !server )
        return NULL;
    memset( server, 0, sizeof( *server ) );
    server->context = context;

    const uint32_t kbps = per_client_send_kbps ? per_client_send_kbps : ROCKETNET_DEFAULT_SERVER_PER_CLIENT_SEND_KBPS;
    rn_message_node_t * n = &server->slots[0];
    n->budgeted = 1;
    if ( memory )
        n->arena = rn_arena_create_in_block( memory, memory_bytes, message_config, 1, &n->counters );
    else
        n->arena = rn_arena_create( NULL, message_config, 1, &n->counters );
    if ( !n->arena )
    {
        free( server );
        return NULL;
    }
    n->region = rn_arena_region( n->arena, 0 );
    if ( n->region )
        rn_region_carve( n->region );
    if ( !rn_message_node_ordered_setup( n, message_config, 4, 1, kbps, 0.0 ) )
    {
        rn_arena_destroy( n->arena );
        free( server );
        return NULL;
    }
    n->next_packet_id = 1;
    server->used[0] = 1;
    server->connected_count = 1;
    return server;
}

void rocketnet_message_server_destroy( struct rocketnet_message_server_t * server )
{
    if ( !server )
        return;
    for ( int i = 0; i < RN_MESSAGE_SERVER_MAX_SLOTS; i++ )
        if ( server->used[i] && server->slots[i].arena )
            rn_arena_destroy( server->slots[i].arena );
    free( server );
}

int rocketnet_message_server_update( struct rocketnet_message_server_t * server, double time )
{
    if ( !server )
        return 0;
    int result = ROCKETNET_MESSAGE_ERROR_NONE;
    for ( int i = 0; i < RN_MESSAGE_SERVER_MAX_SLOTS; i++ )
    {
        if ( !server->used[i] )
            continue;
        rn_message_node_t * n = &server->slots[i];
        n->time = time;
        if ( rn_message_node_any_unreleased( n ) )
        {
            if ( rocketnet_assert_function_pointer )
                rocketnet_assert_function_pointer( "received message not released before update",
                                                  "rocketnet_message_server_update", __FILE__, __LINE__ );
            result = ROCKETNET_ERROR_MESSAGE_NOT_RELEASED;
            continue;
        }
        rn_message_node_decide( n );
        rn_message_node_init_deliveries( n );
        if ( n->disconnect_reason != 0 )
            continue;
        struct rocketnet_message_client_t * client = server->peer[i];
        if ( client && client->connected )
        {
            const int count = rn_message_node_write_packets( n, time, 4, 1 );
            for ( int p = 0; p < count; p++ )
            {
                rn_message_node_open_frame( &client->node, &n->frames[p] );
                rn_ordered_acks( &n->ordered, &n->frames[p].id, 1, time );
                rn_message_node_decide( &client->node );
            }
        }
    }
    return result;
}

void rocketnet_message_server_send_packets( struct rocketnet_message_server_t * server )
{
    (void) server;
}

uint8_t * rocketnet_message_server_start( struct rocketnet_message_server_t * server, int client_index, int channel, int reliability, int bytes )
{
    if ( !server || client_index < 0 || client_index >= RN_MESSAGE_SERVER_MAX_SLOTS || !server->used[client_index] || bytes < 0 )
        return NULL;
    int type;
    if ( reliability == ROCKETNET_UNRELIABLE )
        type = ROCKETNET_CHANNEL_TYPE_UNRELIABLE;
    else if ( reliability == ROCKETNET_RELIABLE_ORDERED )
        type = ROCKETNET_CHANNEL_TYPE_RELIABLE_ORDERED;
    else
        return NULL;
    uint8_t * p = rn_channels_start( &server->slots[client_index].ordered.channels, (uint32_t) channel, type, (uint32_t) bytes );
    if ( !p )
    {
        server->slots[client_index].disconnect_reason = MESSAGE_MEMORY_EXHAUSTED;
        server->used[client_index] = 0;
    }
    return p;
}

int rocketnet_message_server_finish( struct rocketnet_message_server_t * server, int client_index, int bytes_written )
{
    if ( !server || client_index < 0 || client_index >= RN_MESSAGE_SERVER_MAX_SLOTS || !server->used[client_index] )
        return 0;
    rn_channels_finish( &server->slots[client_index].ordered.channels, bytes_written );
    return (int) server->slots[client_index].next_packet_id;
}

void rocketnet_message_server_abort( struct rocketnet_message_server_t * server, int client_index )
{
    (void) server;
    (void) client_index;
}

int rocketnet_message_server_receive( struct rocketnet_message_server_t * server, int channel, const uint8_t ** data, int * bytes )
{
    if ( !server || !data || !bytes )
        return 0;
    for ( int i = 0; i < RN_MESSAGE_SERVER_MAX_SLOTS; i++ )
    {
        if ( !server->used[i] )
            continue;
        rn_message_node_t * n = &server->slots[i];
        for ( uint32_t k = 0; k < n->deliveries_count; k++ )
        {
            rn_message_delivery_t & d = n->deliveries[k];
            if ( !d.consumed && d.channel == channel )
            {
                d.consumed = 1;
                *data = d.data;
                *bytes = d.bytes;
                return 1;
            }
        }
    }
    return 0;
}

int rocketnet_message_server_receive_any( struct rocketnet_message_server_t * server, int * channel, const uint8_t ** data, int * bytes )
{
    if ( !server || !channel || !data || !bytes )
        return 0;
    for ( int i = 0; i < RN_MESSAGE_SERVER_MAX_SLOTS; i++ )
    {
        if ( !server->used[i] )
            continue;
        rn_message_node_t * n = &server->slots[i];
        for ( uint32_t k = 0; k < n->deliveries_count; k++ )
        {
            rn_message_delivery_t & d = n->deliveries[k];
            if ( !d.consumed )
            {
                d.consumed = 1;
                *channel = d.channel;
                *data = d.data;
                *bytes = d.bytes;
                return 1;
            }
        }
    }
    return 0;
}

void rocketnet_message_server_release( struct rocketnet_message_server_t * server, const uint8_t * data )
{
    if ( !server )
        return;
    for ( int i = 0; i < RN_MESSAGE_SERVER_MAX_SLOTS; i++ )
        if ( server->used[i] )
            rn_message_node_release( &server->slots[i], data );
}

int rocketnet_message_server_disconnect_reason( const struct rocketnet_message_server_t * server, int client_index )
{
    if ( !server || client_index < 0 || client_index >= RN_MESSAGE_SERVER_MAX_SLOTS )
        return 0;
    return server->slots[client_index].disconnect_reason;
}

void rocketnet_message_server_set_send_kbps( struct rocketnet_message_server_t * server, int client_index, uint32_t kbps )
{
    if ( server && client_index >= 0 && client_index < RN_MESSAGE_SERVER_MAX_SLOTS && server->used[client_index]
         && server->slots[client_index].budgeted )
        rocketnet_send_budget_set_kbps( &server->slots[client_index].budget, kbps, server->slots[client_index].time );
}
