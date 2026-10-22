/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_connection_reset.h"

#include "rocketnet_client_protocol.h"
#include "rocketnet_server_protocol.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_fragment.h"
#include "rocketnet_message_object.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>

void rocketnet_client_protocol_wipe_secrets( struct rocketnet_client_protocol_t * client )
{
    rocketnet_assert( client );
    sodium_memzero( &client->keys, sizeof(client->keys) );
    sodium_memzero( &client->token, sizeof(client->token) );
    sodium_memzero( client->client_ephemeral_secret_key, sizeof(client->client_ephemeral_secret_key) );
    sodium_memzero( client->session_nonce, sizeof(client->session_nonce) );
}

void rocketnet_client_connection_reset( struct rocketnet_client_protocol_t * client )
{
    rocketnet_assert( client );

    const int terminal_state = client->state;
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES];
    memcpy( packet_filter, client->packet_filter, sizeof(packet_filter) );
    const double send_rate = client->send_rate;
    struct rocketnet_counters_t * counters = client->counters;
    struct rocketnet_reassembly_set_t * reassembly = client->reassembly;
    const rocketnet_client_publish_keys_t publish_keys = client->publish_keys;
    const rocketnet_client_retire_keys_t retire_keys = client->retire_keys;
    void * keys_context = client->keys_context;

    if ( retire_keys )
        retire_keys( keys_context );

    // Key and token erasure must not be optimized away. The following memset
    // establishes the fresh non-secret fields after these secure wipes.
    rocketnet_client_protocol_wipe_secrets( client );
    rocketnet_client_protocol_zero_state( client );

    memcpy( client->packet_filter, packet_filter, sizeof(packet_filter) );
    client->send_rate = send_rate;
    client->counters = counters;
    client->state = terminal_state;
    client->client_index = -1;
    client->state_first_send_time = -1.0;
    client->next_fragment_payload_id = 1;
    client->reassembly = reassembly;
    client->publish_keys = publish_keys;
    client->retire_keys = retire_keys;
    client->keys_context = keys_context;

    rocketnet_ack_endpoint_reset( &client->ack );
    if ( reassembly )
        rocketnet_reassembly_set_reset_connection( reassembly, 0 );
}

void rocketnet_server_connection_reset( struct rocketnet_server_protocol_t * server, int client_index )
{
    rocketnet_assert( server );

    if ( !server->client_slots || client_index < 0 ||
         client_index >= rocketnet_client_slots_max_clients( server->client_slots ) )
        return;

    int mapping_index = -1;
    if ( rocketnet_client_slots_connected( server->client_slots, client_index ) )
        mapping_index = rocketnet_client_slots_mapping_index( server->client_slots, client_index );

    /*
        Remove first: rocketnet_encryption_map_remove wipes the session keys
        and nonce, and takes the entry out of live, while the slot still
        identifies the mapping. The slot identity and user data are cleared only afterwards.
    */
    if ( mapping_index >= 0 && server->encryption_map )
        rocketnet_encryption_map_remove( server->encryption_map, mapping_index );

    if ( rocketnet_client_slots_connected( server->client_slots, client_index ) )
        rocketnet_client_slots_release( server->client_slots, client_index );

    if ( server->connection_ids )
        server->connection_ids[client_index] = 0;

    if ( server->acks )
        rocketnet_server_acks_reset_slot( server->acks, client_index );

    if ( server->reassembly )
        rocketnet_reassembly_set_reset_connection( server->reassembly, client_index );

}

static void rocketnet_message_connection_reset( struct rn_message_object_t * object, int client_index )
{
    rocketnet_assert( object );
    rocketnet_assert( client_index >= 0 );
    rocketnet_assert( client_index < object->num_connections );

    struct rn_message_connection_t * connection = &object->connections[client_index];
    const int disconnect_reason = connection->disconnect_reason;

    /*
        rn_endpoint_reset releases queued channel state and resets its region.
        views and cursors are allocations inside that region and remain stable;
        their contents become fresh with the region. A start pending on this
        connection is cancelled by the update before it calls this reset
        (sdk/rocketnet_message_server.cpp, rn_message_connection_cancel_at_reset).
    */
    rn_endpoint_reset( connection, object->time );
    memset( connection->hot->views, 0, connection->region->delivery_entries );
    memset( connection->hot->cursors, 0,
            sizeof(uint32_t) * (size_t) connection->region->num_channels );
    connection->hot->held = 0;
    connection->hot->connection_id = 0;
    connection->hot->state = RN_MESSAGE_CONNECTION_FREE;
    connection->disconnect_reason = disconnect_reason;
}

void rocketnet_client_connection_reset( struct rn_message_object_t * client )
{
    rocketnet_assert( client );
    rocketnet_assert( !client->is_server );
    rocketnet_message_connection_reset( client, 0 );
}

void rocketnet_server_connection_reset( struct rn_message_object_t * server, int client_index )
{
    rocketnet_assert( server );
    rocketnet_assert( server->is_server );
    rocketnet_message_connection_reset( server, client_index );
}

static size_t rocketnet_reset_align_up( size_t value, size_t alignment )
{
    return ( value + alignment - 1 ) & ~( alignment - 1 );
}

bool rocketnet_reset_inventory_matches( const char * structure,
                                        const struct rocketnet_reset_member_t * members, size_t num_members,
                                        const struct rocketnet_reset_member_t * decisions, size_t num_decisions,
                                        size_t first_offset, size_t struct_bytes, size_t struct_alignment,
                                        char * failure, size_t failure_bytes )
{
    rocketnet_assert( structure && members && decisions && failure && failure_bytes > 0 );
    failure[0] = '\0';

    for ( size_t i = 0; i < num_members; i++ )
    {
        int matches = 0;
        for ( size_t j = 0; j < num_decisions; j++ )
        {
            if ( strcmp( members[i].name, decisions[j].name ) != 0 )
                continue;
            matches++;
            if ( decisions[j].offset != members[i].offset || decisions[j].bytes != members[i].bytes )
            {
                snprintf( failure, failure_bytes, "%s.%s: the inventory entry does not cover the member", structure, members[i].name );
                return false;
            }
        }
        if ( matches != 1 )
        {
            snprintf( failure, failure_bytes, "%s.%s: %s", structure, members[i].name,
                      matches == 0 ? "no RESET or KEEP decision" : "more than one decision" );
            return false;
        }
    }

    for ( size_t j = 0; j < num_decisions; j++ )
    {
        size_t matches = 0;
        for ( size_t i = 0; i < num_members; i++ )
            matches += strcmp( members[i].name, decisions[j].name ) == 0;
        if ( matches == 0 )
        {
            snprintf( failure, failure_bytes, "%s.%s: decided, but not a member", structure, decisions[j].name );
            return false;
        }
    }

    size_t end = first_offset;
    for ( size_t i = 0; i < num_members; i++ )
    {
        if ( members[i].bytes == 0 || members[i].alignment == 0 ||
             members[i].offset != rocketnet_reset_align_up( end, members[i].alignment ) ||
             members[i].offset + members[i].bytes > struct_bytes )
        {
            snprintf( failure, failure_bytes, "%s.%s: bytes before it are covered by no member", structure, members[i].name );
            return false;
        }
        end = members[i].offset + members[i].bytes;
    }
    if ( rocketnet_reset_align_up( end, struct_alignment ) != struct_bytes )
    {
        snprintf( failure, failure_bytes, "%s.%s: bytes after it are covered by no member", structure,
                  num_members ? members[num_members - 1].name : "(none)" );
        return false;
    }
    return true;
}
