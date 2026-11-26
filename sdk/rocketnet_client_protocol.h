/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_CLIENT_PROTOCOL_H
#define ROCKETNET_SDK_ROCKETNET_CLIENT_PROTOCOL_H

#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_ack.h"
#include "rocketnet_link_mtu.h"

// The client state machine, per standard/TRANSPORT.md t11: the machine steps forward or
// fails, never sideways. A client climbs 1, 2, 3, 4 entering each state
// exactly once per attempt; on any failure it dies to a terminal state, and
// recovery is a fresh connect with a fresh token -- the ordinary path, run
// every time. The state is readable from outside at any time and is the
// primary means of diagnosing a connection that is not progressing.

#define ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS             10

// Pointer-only dependency, deliberately incomplete here: the counters
// (rocketnet_counters.h). The implementation file includes the full
// definition it consumes.
struct rocketnet_counters_t;

// The verdict a client I/O thread records for each datagram it worked
// (proofs/tla/RNClientIoDecrypt.tla IoCheck and IoOpen, docs/CLIENT-IO-DECRYPT.md):
// raw (not opened: a handshake packet, or anything while its copy of the open
// key is not live), opened, or the rung that refused it before or at the AEAD.

enum
{
    ROCKETNET_CLIENT_IO_VERDICT_RAW = 0,
    ROCKETNET_CLIENT_IO_VERDICT_OPENED = 1,
    ROCKETNET_CLIENT_IO_VERDICT_ALREADY_RECEIVED = 2,
    ROCKETNET_CLIENT_IO_VERDICT_DID_NOT_OPEN = 3,
    ROCKETNET_CLIENT_IO_VERDICT_ZERO_SEQUENCE = 4,
    ROCKETNET_CLIENT_IO_VERDICT_WRONG_SIZE = 5,
};

struct rocketnet_client_protocol_t;

// The open block hooks of a transport whose I/O thread opens session packets
// (proofs/tla/RNClientIoDecrypt.tla PublishKeys and RetireKeys): publish runs where
// the challenge commits the session keys, retire on every terminal state
// before the keys are zeroed. NULL on every other path.

typedef void ( *rocketnet_client_publish_keys_t )( void * context, const struct rocketnet_client_protocol_t * client );
typedef void ( *rocketnet_client_retire_keys_t )( void * context );

/*
    The declaration manifest is deliberately independent of the RESET/KEEP
    inventory in rocketnet_connection_reset.h. Tests compare the two by name,
    offset and size. Adding a member here without an inventory decision names
    that exact member in test_connection_reset_inventory_covers_every_field.
*/
#define ROCKETNET_CLIENT_PROTOCOL_LAYOUT_FIELDS( X ) \
    X( packet_filter, uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES] ) \
    X( send_rate, double send_rate ) \
    X( counters, struct rocketnet_counters_t * counters ) \
    X( state, int state ) \
    X( token, struct rocketnet_connect_token_t token ) \
    X( signed_fields, struct rocketnet_signed_connect_data_t signed_fields ) \
    X( server_address, struct rocketnet_address_t server_address ) \
    X( connection_salt, uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES] ) \
    X( client_ephemeral_public_key, uint8_t client_ephemeral_public_key[ROCKETNET_KEY_BYTES] ) \
    X( client_ephemeral_secret_key, uint8_t client_ephemeral_secret_key[ROCKETNET_KEY_BYTES] ) \
    X( return_cookie, uint8_t return_cookie[ROCKETNET_COOKIE_FIELD_BYTES] ) \
    X( has_cookie, int has_cookie ) \
    X( latest_cookie, uint8_t latest_cookie[ROCKETNET_COOKIE_FIELD_BYTES] ) \
    X( has_latest_cookie, int has_latest_cookie ) \
    X( send_latest_cookie, int send_latest_cookie ) \
    X( keys, struct rocketnet_connection_keys_t keys ) \
    X( stored_response, uint8_t stored_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES] ) \
    X( client_index, int client_index ) \
    X( max_clients, int max_clients ) \
    X( send_sequence_handshake, uint64_t send_sequence_handshake ) \
    X( send_sequence_session, uint64_t send_sequence_session ) \
    X( handshake_highest_opened, uint64_t handshake_highest_opened ) \
    X( replay_session, struct rocketnet_replay_protection_t replay_session ) \
    X( accepted_session, struct rocketnet_replay_protection_t accepted_session ) \
    X( defer_payload_acks, int defer_payload_acks ) \
    X( handshake_early_send, int handshake_early_send ) \
    X( integrity_failures, uint64_t integrity_failures ) \
    X( attempt_start_time, double attempt_start_time ) \
    X( last_send_time, double last_send_time ) \
    X( last_receive_time, double last_receive_time ) \
    X( total_deadline_seconds, uint64_t total_deadline_seconds ) \
    X( timeout_seconds, int32_t timeout_seconds ) \
    X( state_first_send_time, double state_first_send_time ) \
    X( rtt_estimate, double rtt_estimate ) \
    X( rtt_session_sampled, int rtt_session_sampled ) \
    X( received_session_sequence, uint64_t received_session_sequence ) \
    X( ack, struct rocketnet_ack_endpoint_t ack ) \
    X( num_acked, int num_acked ) \
    X( acked_packet_ids, uint64_t acked_packet_ids[ROCKETNET_SENT_PACKET_BUFFER_SIZE] ) \
    X( publish_keys, rocketnet_client_publish_keys_t publish_keys ) \
    X( retire_keys, rocketnet_client_retire_keys_t retire_keys ) \
    X( keys_context, void * keys_context ) \
    X( record_verdict, uint8_t record_verdict ) \
    X( record_sequence, uint64_t record_sequence ) \
    X( link_mtus, int link_mtus[ROCKETNET_LINK_MTU_MAX_ENTRIES] ) \
    X( num_link_mtus, int num_link_mtus ) \
    X( probe_candidates, int probe_candidates[ROCKETNET_LINK_MTU_MAX_ENTRIES] ) \
    X( num_probe_candidates, int num_probe_candidates ) \
    X( probe_index, int probe_index ) \
    X( probe_strikes, int probe_strikes ) \
    X( probe_pair, uint32_t probe_pair ) \
    X( probe_pair_small, int probe_pair_small ) \
    X( probe_pair_large, int probe_pair_large ) \
    X( probe_pair_out, int probe_pair_out ) \
    X( probe_pair_spaced, int probe_pair_spaced ) \
    X( probe_next_time, double probe_next_time ) \
    X( probe_interval, double probe_interval ) \
    X( probe_pair_time, double probe_pair_time ) \
    X( probe_strike_time, double probe_strike_time ) \
    X( probing, int probing ) \
    X( confirm_bytes, int confirm_bytes ) \
    X( confirm_next_time, double confirm_next_time ) \
    X( packet_bytes, int packet_bytes )

struct rocketnet_client_protocol_t
{
#define ROCKETNET_CLIENT_PROTOCOL_DECLARE_FIELD( name, declaration ) declaration;
    ROCKETNET_CLIENT_PROTOCOL_LAYOUT_FIELDS( ROCKETNET_CLIENT_PROTOCOL_DECLARE_FIELD )
#undef ROCKETNET_CLIENT_PROTOCOL_DECLARE_FIELD
};

void rocketnet_client_protocol_init( struct rocketnet_client_protocol_t * client, struct rocketnet_counters_t * counters );

// The connection's packet size P (sdk/rocketnet_link_mtu.h): the smallest
// candidate of the link MTU list, which the response carries, until the path
// probe raises it; it never decreases. ROCKETNET_MAX_PACKET_BYTES before the
// response is built and on the default link MTU list.
int rocketnet_client_protocol_packet_bytes( const struct rocketnet_client_protocol_t * client );

// The path probe's sends of this update, after rocketnet_client_protocol_update
// (standard/TRANSPORT.md t11.4; proofs/tla/RNPathProbe.tla ClientSendPair and
// ClientSendConfirm): while connected and probing, a pair at connect and
// then at the probe's paced interval -- the small probe into
// out_small, the large one at the candidate into out_large -- or a confirm
// into out_small. Each buffer holds ROCKETNET_RECEIVE_CEILING bytes; a zero
// size is no packet. May end the connection in mtu probe failed (-14).
void rocketnet_client_protocol_probe_update( struct rocketnet_client_protocol_t * client,
                                             double current_time,
                                             uint8_t * out_small,
                                             int * out_small_bytes,
                                             uint8_t * out_large,
                                             int * out_large_bytes );

// The three steps of a due pair inside rocketnet_client_protocol_probe_update,
// in its order, each a model action of proofs/tla/RNPathProbe.tla, so the
// trace check (tests/tla_trace_path_probe.cpp) reads the state after each.
// rocketnet_client_protocol_probe_resolve: the outstanding pair resolves
// (ClientResolvePair), returning 1 when it did, 0 when no pair was out.
// rocketnet_client_protocol_probe_rule_out: the candidate's counted strikes
// rule it out (ClientRuleOut, returning 1) or, at P0 itself, end the
// connection in mtu probe failed (ClientFail, returning -1); 0 otherwise.
// rocketnet_client_protocol_probe_send_pair: the next pair (ClientSendPair).
// None is called but by rocketnet_client_protocol_probe_update and the trace
// walk, and only while connected, probing and the pair is due.
int rocketnet_client_protocol_probe_resolve( struct rocketnet_client_protocol_t * client );
int rocketnet_client_protocol_probe_rule_out( struct rocketnet_client_protocol_t * client );
void rocketnet_client_protocol_probe_send_pair( struct rocketnet_client_protocol_t * client,
                                                double current_time,
                                                uint8_t * out_small,
                                                int * out_small_bytes,
                                                uint8_t * out_large,
                                                int * out_large_bytes );

// Every field zeroed but the ack history's record rings (guarded by its
// written bits, rocketnet_ack_endpoint_reset) and the acked packet ids
// (read only below num_acked): 295 KB of the protocol's 300 KB are never
// zeroed. The caller resets the ack endpoint and sets the nonzero fields.
void rocketnet_client_protocol_zero_state( struct rocketnet_client_protocol_t * client );

// Begins a connection attempt: parses the token, runs the pre-send checks --
// address flags valid, create not after expire, not already expired --
// generates the salt and the ephemeral keypair, picks the server address
// (IPv6 preferred when both families are present), and enters _requesting
// cookie_. On a bad token the state is the terminal that names why, and 0
// is returned.

int rocketnet_client_protocol_connect( struct rocketnet_client_protocol_t * client,
                                       const uint8_t * connect_token,
                                       double current_time,
                                       uint64_t current_timestamp );

// Drives sending and deadlines. Call every frame with the current clocks; at
// most one packet comes back per call, at the send rate -- a rate in time,
// never per update. out_packet_bytes is zero when there is nothing to send.
// Deadlines fire in here: each waiting state N dies to -(N+1) after the
// token's timeout with nothing received, and the whole handshake dies to
// _connect token expired_ once the token's lifetime has elapsed since the
// attempt began.

void rocketnet_client_protocol_update( struct rocketnet_client_protocol_t * client,
                                       double current_time,
                                       uint8_t * out_packet,
                                       int * out_packet_bytes );

// Whether the next update sends its state's packet: the early send a
// handshake advance armed, or the send interval elapsed. The update reads
// its send decision here and nowhere else (proofs/tla/RNPathProbe.tla
// maySend, which the TLA+ trace check reads through this call).

bool rocketnet_client_protocol_send_ready( const struct rocketnet_client_protocol_t * client, double current_time );

// Processes one received packet. Returns 1 when the packet was accepted and
// advanced or fed the connection, 0 when it was refused -- the counters name
// why. out_payload_capacity is the true size of out_payload, which must be at
// least ROCKETNET_RECEIVE_PAYLOAD_BYTES: a smaller capacity refuses the
// call at entry, before the packet is read, counts RECEIVE_CURSOR_TOO_SMALL,
// leaves out_payload and the protocol state untouched, and never overruns.
// A payload lands in out_payload with its length in out_payload_bytes;
// every other outcome leaves it zero.

int rocketnet_client_protocol_process_packet( struct rocketnet_client_protocol_t * client,
                                              const uint8_t * packet_data,
                                              int packet_bytes,
                                              double current_time,
                                              uint8_t * out_payload,
                                              int out_payload_capacity,
                                              int * out_payload_bytes );

// proofs/tla/RNClientIoDecrypt.tla UpdateConsume: processes one record of a receive
// buffer a client I/O thread filled, in arrival order. A raw record is
// rocketnet_client_protocol_process_packet unchanged. An opened record carries
// the plaintext behind its wire header and the I/O thread's sequence: the
// state check, update's own reconstruction (a disagreement is counted as did
// not open, with no integrity failure), update's own replay check and advance,
// then the rest of the session path as today, with no AEAD. A refused record
// is counted with today's counter for its rung in its arrival position; a
// did-not-open record also runs the integrity failure there.

int rocketnet_client_protocol_process_record( struct rocketnet_client_protocol_t * client,
                                              const uint8_t * packet_data,
                                              int packet_bytes,
                                              uint8_t verdict,
                                              uint64_t sequence,
                                              double current_time,
                                              uint8_t * out_payload,
                                              int out_payload_capacity,
                                              int * out_payload_bytes );

// Starts a payload packet for the server: takes the next session send
// sequence into *sequence. Valid only while connected; returns 1, or 0 when
// not connected. The seal (rocketnet_client_protocol_seal_payload_frame)
// stamps the send -- ordinary traffic suppresses the client's own keep-alive.
// NOT "as on the server": the server's rule splits on whether the client has
// acknowledged a keep-alive, because until then it is starving the packet
// that would establish the connection. The client has no such split, and
// needs none -- it sends keep-alives only from _connected_, and nothing at the
// far end waits on one, since any sealed packet confirms the slot.

int rocketnet_client_protocol_start_payload( struct rocketnet_client_protocol_t * client, uint64_t * sequence );

// The reference an inbound ack block's 32-bit largest is reconstructed
// against: the highest session sequence this client actually sealed and
// recorded sent, never the next sequence less one.

uint64_t rocketnet_client_protocol_highest_sent_session( struct rocketnet_client_protocol_t * client );

// seals the frame's payload in place, opening the sealed plaintext with the
// ack block, and records the sent packet in the delivery bookkeeping under
// packet_id -- the join key surfaced when the packet is acknowledged

int rocketnet_client_protocol_seal_payload_frame( struct rocketnet_client_protocol_t * client,
                                                  uint64_t sequence,
                                                  uint64_t packet_id,
                                                  uint8_t * wire_start,
                                                  int payload_bytes,
                                                  double current_time );

#if ROCKETNET_DEV
int rocketnet_client_protocol_write_payload( struct rocketnet_client_protocol_t * client,
                                             const uint8_t * payload_data,
                                             int payload_bytes,
                                             double current_time,
                                             uint8_t * out_packet );
#endif // #if ROCKETNET_DEV

// The deliberate disconnect: fills the redundant burst -- ten sealed
// disconnect packets at consecutive session sequences, each opening with
// the same ack block, at ROCKETNET_SEND_BUDGET stride in out_packets --
// sets out_packet_count and the shared per-packet wire size, and enters
// _disconnected_ immediately without waiting for anything. Before the
// session keys exist there is nothing to seal, so the burst is empty and
// the state simply resets.

void rocketnet_client_protocol_disconnect( struct rocketnet_client_protocol_t * client,
                                           uint8_t * out_packets,
                                           int * out_packet_count,
                                           int * out_packet_bytes );

// Commit only after the message layer accepted the entire payload.
void rocketnet_client_protocol_accept_payload( struct rocketnet_client_protocol_t * client, uint64_t sequence );

// The reported round-trip time of the client's connection, in seconds
// (standard/TRANSPORT.md t15.5): the minimum retained first-ack sample, 0
// while it has none. The message layer takes it as the connection's first
// RTT sample (standard/MESSAGES.md m9.2).
double rocketnet_client_protocol_rtt( const struct rocketnet_client_protocol_t * client );

#endif // ROCKETNET_SDK_ROCKETNET_CLIENT_PROTOCOL_H
