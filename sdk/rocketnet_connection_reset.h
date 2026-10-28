/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CONNECTION_RESET_H
#define ROCKETNET_SDK_ROCKETNET_CONNECTION_RESET_H

#include <stddef.h>
#include "rocketnet_test_seams.h"

struct rocketnet_client_protocol_t;
struct rocketnet_server_protocol_t;
struct rocketnet_client_transport_t;
struct rocketnet_server_transport_t;
struct rn_message_object_t;

#if ROCKETNET_DEV

/*
    The inventory is deliberately beside the reset declarations. A change to
    either inventoried layout must make an explicit RESET or KEEP decision here
    and in docs/CONNECTION-RESET.md.

    X receives (decision, field). Array fields are one entry because reset and
    comparison always cover sizeof(field) bytes.
*/

#define ROCKETNET_CLIENT_PROTOCOL_CONNECTION_FIELDS( X ) \
    X( KEEP, packet_filter ) X( KEEP, send_rate ) X( KEEP, counters ) \
    X( KEEP, state ) X( KEEP, defer_payload_acks ) \
    X( RESET, token ) X( RESET, signed_fields ) X( RESET, server_address ) \
    X( RESET, connection_salt ) X( RESET, client_ephemeral_public_key ) \
    X( RESET, client_ephemeral_secret_key ) X( RESET, return_cookie ) X( RESET, has_cookie ) \
    X( RESET, latest_cookie ) X( RESET, has_latest_cookie ) X( RESET, send_latest_cookie ) \
    X( RESET, session_nonce ) X( RESET, keys ) X( RESET, stored_response ) \
    X( RESET, client_index ) X( RESET, max_clients ) \
    X( RESET, send_sequence_handshake ) X( RESET, send_sequence_session ) \
    X( RESET, next_fragment_payload_id ) X( RESET, handshake_highest_opened ) X( RESET, replay_session ) X( RESET, accepted_session ) \
    X( RESET, integrity_failures ) X( RESET, attempt_start_time ) X( RESET, last_send_time ) \
    X( RESET, last_receive_time ) X( RESET, total_deadline_seconds ) X( RESET, timeout_seconds ) \
    X( RESET, state_first_send_time ) X( RESET, rtt_estimate ) X( RESET, rtt_session_sampled ) \
    X( RESET, received_session_sequence ) X( RESET, ack ) X( RESET, num_acked ) \
    X( RESET, acked_packet_ids ) \
    X( KEEP, reassembly ) X( KEEP, publish_keys ) X( KEEP, retire_keys ) X( KEEP, keys_context ) \
    X( RESET, record_verdict ) X( RESET, record_sequence )

#define ROCKETNET_CLIENT_SLOTS_OWNER_FIELDS( X ) \
    X( KEEP, context ) X( KEEP, max_clients ) X( KEEP, num_connected ) X( KEEP, hash_key ) \
    X( KEEP, index_table ) X( KEEP, slot_generation ) X( KEEP, num_buckets ) X( KEEP, free_stack ) \
    X( KEEP, free_stack_count ) X( KEEP, client_id ) X( KEEP, connected ) \
    X( KEEP, mapping_index ) X( KEEP, confirmed ) X( KEEP, user_data )

#define ROCKETNET_SERVER_REPLAY_MARK_FIELDS( X ) \
    X( RESET, has_mark ) X( RESET, marked_value )

#define ROCKETNET_CLIENT_TRANSPORT_CONNECTION_FIELDS( X ) \
    X( RESET, packet_leases ) X( KEEP, context ) X( KEEP, bound_port ) X( KEEP, transport ) \
    X( KEEP, io ) X( KEEP, thread ) X( KEEP, mutex ) X( KEEP, quit ) \
    X( KEEP, stop_requested ) X( KEEP, stopped ) X( KEEP, stop_burst ) \
    X( KEEP, stop_burst_count ) X( KEEP, stop_burst_pending ) X( KEEP, stop_burst_bytes ) X( KEEP, stop_address ) \
    X( KEEP, receive_buffer_index ) X( RESET, received_packets ) X( KEEP, receive_buffer ) \
    X( KEEP, time ) X( KEEP, raw_time ) X( KEEP, time_offset ) X( KEEP, frame ) X( KEEP, last_state ) X( KEEP, config ) \
    X( KEEP, max_payload_bytes ) X( KEEP, send_arena ) X( KEEP, reassembly ) \
    X( RESET, payload_ring ) X( KEEP, io_thread ) X( KEEP, step_driven ) X( KEEP, step_in_update ) X( KEEP, client_io ) \
    X( RESET, stats_counted ) X( KEEP, time_callback ) X( KEEP, link_simulator ) X( KEEP, link_simulator_from ) \
    X( KEEP, counters ) X( RESET, protocol ) \
    X( KEEP, client_stats ) X( KEEP, stats_frames ) X( KEEP, current_stats_index )

#define ROCKETNET_ENCRYPTION_MAPPING_CONNECTION_FIELDS( X ) \
    X( KEEP, control ) X( RESET, keys ) X( RESET, connection_salt ) X( RESET, session_nonce ) \
    X( RESET, server_ephemeral_public_key ) X( RESET, client_ephemeral_public_key ) \
    X( RESET, challenge_packet ) X( RESET, client_id ) X( RESET, user_data ) \
    X( RESET, token_identifier ) X( RESET, token_expire_timestamp ) \
    X( RESET, last_send_time ) X( RESET, last_keep_alive_send_time ) \
    X( RESET, send_sequence_handshake ) X( RESET, send_sequence_session ) X( RESET, send_fragment_payload_id ) \
    X( RESET, handshake_highest_opened ) X( RESET, replay_session ) X( RESET, accepted_session ) X( RESET, integrity_failures )

#define ROCKETNET_ENCRYPTION_MAPPING_CONTROL_CONNECTION_FIELDS( X ) \
    X( RESET, handshake_deadline ) X( RESET, last_receive_time ) \
    X( RESET, timeout_seconds ) X( RESET, client_index ) X( RESET, established )

#define ROCKETNET_ENDPOINT_CONNECTION_FIELDS( X ) \
    X( KEEP, region ) X( RESET, ordered ) X( RESET, unreliable_builder ) X( RESET, budget ) \
    X( KEEP, config ) X( KEEP, default_kbps ) X( RESET, send_budget_enabled ) \
    X( KEEP, unreliable_limit ) X( KEEP, max_payload_bytes ) X( RESET, send_error ) \
    X( RESET, config_wait_start ) X( RESET, config_error ) X( RESET, serial ) \
    X( RESET, pending_type ) X( RESET, pending_descriptor ) X( RESET, pending_bytes ) \
    X( RESET, pending_in_place ) X( RESET, pending_channel ) X( RESET, pending_header ) \
    X( RESET, in_place_serial ) X( RESET, in_place_receipt ) X( RESET, in_place_admitted ) \
    X( RESET, unreliable_head ) X( RESET, unreliable_tail ) X( RESET, unreliable_count ) \
    X( RESET, write_phase ) X( RESET, written ) X( RESET, packet_kind ) X( RESET, packet_written ) \
    X( RESET, large_bytes ) X( RESET, large_admitted ) X( RESET, large_descriptor ) \
    X( RESET, packet_records ) X( RESET, receipt_count ) X( RESET, receipt_serials )

#define ROCKETNET_MESSAGE_CONNECTION_FIELDS( X ) \
    X( KEEP, hot ) X( KEEP, disconnect_reason ) X( RESET, unreliable_build )

#define ROCKETNET_MESSAGE_CONNECTION_HOT_FIELDS( X ) \
    X( RESET, connection_id ) X( KEEP, views ) X( RESET, receive_cursor ) \
    X( RESET, held ) X( RESET, state )

#endif // #if ROCKETNET_DEV

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

// the client protocol's secrets, and only those: the session keys, the
// token (its private connect data holds the keys), the ephemeral secret and
// the session nonce. Every terminal state and the transport's destroy run it.
void rocketnet_client_protocol_wipe_secrets( struct rocketnet_client_protocol_t * client );
void rocketnet_client_connection_reset( struct rocketnet_client_transport_t * client );
void rocketnet_server_connection_reset( struct rocketnet_server_protocol_t * server, int client_index );

void rocketnet_client_connection_reset( struct rn_message_object_t * client );
void rocketnet_server_connection_reset( struct rn_message_object_t * server, int client_index );

#if ROCKETNET_DEV

/*
    The field walk. members is a struct's declaration manifest (its
    *_LAYOUT_FIELDS list, from which the struct is declared), decisions its
    RESET/KEEP inventory above. The walk passes when every member has exactly
    one decision of the same offset and size, every decision names a member,
    and the members tile the struct from first_offset to its end with nothing
    but alignment padding between them. Otherwise it writes the first failure,
    naming "<structure>.<member>", into failure and returns false.
*/
struct rocketnet_reset_member_t
{
    const char * name;
    size_t offset;
    size_t bytes;
    size_t alignment;
    int reset;                                  // 1 RESET, 0 KEEP, -1 a manifest entry
};

#define ROCKETNET_RESET_MEMBER( type, field, reset ) \
    { #field, offsetof( type, field ), sizeof( ((type *) 0)->field ), alignof( decltype( ((type *) 0)->field ) ), reset }

bool rocketnet_reset_inventory_matches( const char * structure,
                                        const struct rocketnet_reset_member_t * members, size_t num_members,
                                        const struct rocketnet_reset_member_t * decisions, size_t num_decisions,
                                        size_t first_offset, size_t struct_bytes, size_t struct_alignment,
                                        char * failure, size_t failure_bytes );

// Unit-test witnesses for the private structs. The inventory witnesses run the
// field walk over a struct only its own file can see; the reset witness
// poisons the client transport's RESET fields and runs its reset; the clean
// queries read the state an ended connection must have left, without
// resetting anything, so a disconnect path that skips the reset is seen.
bool rocketnet_client_transport_inventory_matches_for_test( char * failure, size_t failure_bytes );
bool rocketnet_client_slots_inventory_matches_for_test( char * failure, size_t failure_bytes );
bool rocketnet_server_replay_marks_inventory_matches_for_test( char * failure, size_t failure_bytes );
bool rocketnet_client_transport_reset_witness_for_test( struct rocketnet_client_transport_t * client );
void rocketnet_client_transport_mark_connection_state_for_test( struct rocketnet_client_transport_t * client );
bool rocketnet_client_transport_connection_state_clean_for_test( struct rocketnet_client_transport_t * client,
                                                                 char * failure, size_t failure_bytes );
void rocketnet_server_transport_poison_slot_for_test( struct rocketnet_server_transport_t * server, int client_index );
bool rocketnet_server_transport_connection_state_clean_for_test( struct rocketnet_server_transport_t * server );
bool rocketnet_server_transport_slot_clean_for_test( struct rocketnet_server_transport_t * server, int client_index,
                                                     char * failure, size_t failure_bytes );

#endif // #if ROCKETNET_DEV

#endif // ROCKETNET_SDK_ROCKETNET_CONNECTION_RESET_H
