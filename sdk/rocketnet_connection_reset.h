/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CONNECTION_RESET_H
#define ROCKETNET_CONNECTION_RESET_H

#include <stddef.h>

struct rocketnet_client_protocol_t;
struct rocketnet_server_protocol_t;
struct rocketnet_client_transport_t;
struct rn_message_object_t;

/*
    The inventory is deliberately beside the reset declarations. A change to
    either inventoried layout must make an explicit RESET or KEEP decision here
    and in docs/CONNECTION-RESET.md.

    X receives (decision, field). Array fields are one entry because reset and
    comparison always cover sizeof(field) bytes.
*/

#define ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS( X ) \
    X( KEEP, filter_value ) X( KEEP, send_rate ) X( KEEP, counters ) \
    X( KEEP, state ) \
    X( RESET, token ) X( RESET, signed_fields ) X( RESET, server_address ) \
    X( RESET, connection_salt ) X( RESET, client_ephemeral_public_key ) \
    X( RESET, client_ephemeral_secret_key ) X( RESET, return_cookie ) X( RESET, has_cookie ) \
    X( RESET, session_nonce ) X( RESET, keys ) X( RESET, stored_response ) \
    X( RESET, client_index ) X( RESET, max_clients ) \
    X( RESET, send_sequence_handshake ) X( RESET, send_sequence_session ) \
    X( RESET, next_fragment_payload_id ) X( RESET, replay_handshake ) X( RESET, replay_session ) \
    X( RESET, integrity_failures ) X( RESET, attempt_start_time ) X( RESET, last_send_time ) \
    X( RESET, last_receive_time ) X( RESET, total_deadline_seconds ) X( RESET, timeout_seconds ) \
    X( RESET, state_first_send_time ) X( RESET, rtt_estimate ) X( RESET, rtt_session_sampled ) \
    X( RESET, received_session_sequence ) X( RESET, ack ) X( RESET, num_acked ) \
    X( RESET, acked_packet_ids ) \
    X( KEEP, reassembly ) X( KEEP, publish_keys ) X( KEEP, retire_keys ) X( KEEP, keys_context ) \
    X( RESET, record_verdict ) X( RESET, record_sequence )

#define ROCKETNET_MESSAGE_CONNECTION_FIELDS( X ) \
    X( RESET, endpoint ) X( RESET, connection_id ) X( RESET, state ) \
    X( KEEP, disconnect_reason ) X( KEEP, views ) X( KEEP, cursors ) X( RESET, held )

/*
    Reset order is part of the contract:
      1. retire the client I/O thread's published key copy;
      2. erase protocol key/token/session state and reset ack/reassembly;
      3. on the server, erase the encryption mapping before releasing the slot,
         then reset the slot's ack and reassembly endpoints;
      4. release message views and reset the endpoint arena last.

    Redundant disconnect packets are completely written before these functions
    are called. They are the only per-connection bytes permitted to outlive the
    reset.

    The protocol overloads implement tla/ClientSession.tla End and
    tla/ServerSession.tla EndClient. The message-object overloads implement the
    corresponding message/channel cleanup when the transport connection id
    changes. All declarations are internal and are intentionally absent from
    sdk/rocketnet.h.
*/

void rocketnet_client_connection_reset( struct rocketnet_client_protocol_t * client );
void rocketnet_client_connection_reset( struct rocketnet_client_transport_t * client );
void rocketnet_server_connection_reset( struct rocketnet_server_protocol_t * server, int client_index );

void rocketnet_client_connection_reset( struct rn_message_object_t * client );
void rocketnet_server_connection_reset( struct rn_message_object_t * server, int client_index );

#endif // #ifndef ROCKETNET_CONNECTION_RESET_H
