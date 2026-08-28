/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CLIENT_PROTOCOL_H
#define ROCKETNET_CLIENT_PROTOCOL_H

#include <stdint.h>
#include "rocketnet.h"
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_keys.h"
#include "rocketnet_protocol.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_counters.h"
#include "rocketnet_ack.h"

// The client state machine, per STANDARD.md: the machine steps forward or
// fails, never sideways. A client climbs 1, 2, 3, 4 entering each state
// exactly once per attempt; on any failure it dies to a terminal state, and
// recovery is a fresh connect with a fresh token -- the ordinary path, run
// every time. The state is readable from outside at any time and is the
// primary means of diagnosing a connection that is not progressing.

#define ROCKETNET_CLIENT_NUM_DISCONNECT_PACKETS             10

struct rocketnet_client_protocol_t
{
    // configuration, set before connect
    uint8_t filter_value[ROCKETNET_FILTER_VALUE_BYTES];
    double send_rate;                                       // packets per second, default 10
    struct rocketnet_counters_t * counters;

    // the current state, readable at any time
    int state;

    // the token, parsed once at connect
    struct rocketnet_connect_token_t token;
    struct rocketnet_signed_connect_data_t signed_fields;
    struct rocketnet_address_t server_address;

    // this attempt: one salt, one ephemeral keypair, one sequence space per
    // phase, never reset within the attempt
    uint8_t connection_salt[ROCKETNET_CONNECTION_SALT_BYTES];
    uint8_t client_ephemeral_public_key[ROCKETNET_KEY_BYTES];
    uint8_t client_ephemeral_secret_key[ROCKETNET_KEY_BYTES];
    uint8_t return_cookie[ROCKETNET_COOKIE_FIELD_BYTES];
    int has_cookie;

    // the keys and the server contribution, committed only after a
    // successful challenge open
    uint8_t session_nonce[ROCKETNET_SESSION_NONCE_BYTES];
    struct rocketnet_connection_keys_t keys;
    uint8_t stored_response[ROCKETNET_CONNECTION_RESPONSE_WIRE_BYTES];

    // the connection, once the first keep-alive arrives
    int client_index;
    int max_clients;

    // sending sequences per phase, and receive replay per phase
    uint64_t send_sequence_handshake;
    uint64_t send_sequence_session;
    struct rocketnet_replay_protection_t replay_handshake;      // challenge and denied ride here
    struct rocketnet_replay_protection_t replay_session;

    // time. the caller supplies both clocks; nothing in here reads a clock.
    double attempt_start_time;
    double last_send_time;
    double last_receive_time;
    uint64_t total_deadline_seconds;                        // expire - create
    int32_t timeout_seconds;                                // from the token

    // quality measurement for the transport's stats. rtt is sampled from the
    // handshake's round trips -- the server answers a request, a challenge
    // provocation and a response immediately, so first-send-in-state to
    // first-answer is the round trip; the estimate keeps the minimum across
    // the three exchanges, which discards any sample inflated by a lost
    // first send. received_session_sequence is the session sequence of the
    // packet the last process_packet call accepted, zero when it accepted
    // none -- the transport feeds its loss and jitter trackers from it.
    double state_first_send_time;                           // -1 until the state's first send
    double rtt_estimate;                                    // seconds; 0 until the first sample
    int rtt_session_sampled;                                // the session round trip is sampled once
    uint64_t received_session_sequence;

    // The general acks: this end's delivery bookkeeping and the per-update
    // batch of newly-acknowledged packet ids the transport surfaces. Every
    // session packet sent carries a block built from replay_session; every
    // session packet received applies its block here. The rtt estimate
    // above seeds from the handshake and the endpoint's first-ack samples
    // refresh it continuously from there.

    struct rocketnet_ack_endpoint_t ack;
    int num_acked;
    uint64_t acked_packet_ids[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
};

void rocketnet_client_protocol_init( struct rocketnet_client_protocol_t * client, struct rocketnet_counters_t * counters );

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

// Processes one received packet. Returns 1 when the packet was accepted and
// advanced or fed the connection, 0 when it was refused -- the counters name
// why. A payload delivered on an established connection lands in out_payload
// (sized at least 1400) with its length in out_payload_bytes; every other
// outcome leaves it zero.

int rocketnet_client_protocol_process_packet( struct rocketnet_client_protocol_t * client,
                                     const uint8_t * packet_data,
                                     int packet_bytes,
                                     double current_time,
                                     uint8_t * out_payload,
                                     int * out_payload_bytes );

// Builds a payload packet for the server, and stamps the send -- ordinary
// traffic suppresses the keep-alive, on the client exactly as on the server.
// Valid only while connected; returns the wire size, or 0 when not connected
// or the payload does not fit.

int rocketnet_client_protocol_start_payload( struct rocketnet_client_protocol_t * client, uint64_t * sequence );

// seals the frame's payload in place, opening the sealed plaintext with the
// ack block, and records the sent packet in the delivery bookkeeping under
// packet_id -- the join key surfaced when the packet is acknowledged

int rocketnet_client_protocol_seal_payload_frame( struct rocketnet_client_protocol_t * client,
                                                  uint64_t sequence,
                                                  uint64_t packet_id,
                                                  uint8_t * wire_start,
                                                  int payload_bytes,
                                                  double current_time );

int rocketnet_client_protocol_write_payload( struct rocketnet_client_protocol_t * client,
                                    const uint8_t * payload_data,
                                    int payload_bytes,
                                    double current_time,
                                    uint8_t * out_packet );

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

#endif // #ifndef ROCKETNET_CLIENT_PROTOCOL_H
