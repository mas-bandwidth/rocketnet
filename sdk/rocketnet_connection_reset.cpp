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
#include <string.h>

void rocketnet_client_connection_reset( struct rocketnet_client_protocol_t * client )
{
    rocketnet_assert( client );

    const int terminal_state = client->state;
    uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES];
    memcpy( filter_value, client->filter_value, sizeof(filter_value) );
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
    sodium_memzero( &client->keys, sizeof(client->keys) );
    sodium_memzero( &client->token, sizeof(client->token) );
    sodium_memzero( client->client_ephemeral_secret_key, sizeof(client->client_ephemeral_secret_key) );
    sodium_memzero( client->session_nonce, sizeof(client->session_nonce) );
    memset( client, 0, sizeof(*client) );

    memcpy( client->filter_value, filter_value, sizeof(filter_value) );
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
        Remove first: rocketnet_encryption_map_remove wipes the session keys,
        nonce, challenge and token record while the slot still identifies the
        mapping. The slot identity and user data are cleared only afterwards.
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
        their contents become fresh with the region.
    */
    rn_endpoint_reset( connection, object->time );
    memset( connection->views, 0, connection->region->delivery_entries );
    memset( connection->cursors, 0,
            sizeof(uint32_t) * (size_t) connection->region->num_channels );
    connection->held = 0;
    connection->connection_id = 0;
    connection->state = RN_MESSAGE_CONNECTION_FREE;
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
