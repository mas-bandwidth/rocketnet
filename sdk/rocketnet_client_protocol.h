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

struct rocketnet_reassembly_set_t;

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

    // The family race. When the token names both families and the host can
    // use both, the opening handshake (cookie request) goes to both addresses
    // and the client commits to the first family that answers: second_address
    // holds the non-primary server address (type zero when there is none),
    // and race_active is 1 while both families are still in flight awaiting
    // the first valid cookie. The first valid cookie commits the attempt --
    // race_active drops to 0 and the connection request goes only to the
    // committed family, whose address is server_address -- and a late cookie
    // from the other family is dropped and counted.
    struct rocketnet_address_t second_address;
    int race_active;

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
    uint64_t next_fragment_payload_id;                      // per _Fragmentation_: from 1, per fragmented payload sent
    struct rocketnet_replay_protection_t replay_handshake;      // challenge and denied ride here
    struct rocketnet_replay_protection_t replay_session;
    uint64_t integrity_failures;                            // sealed packets that did not open this attempt, across its keys; ROCKETNET_INTEGRITY_LIMIT disconnects

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

    // Fragment reassembly, per _Fragmentation_: one connection's slots,
    // owned by whoever built this protocol object -- the transport creates
    // it from its config -- and carried across connect, which resets its
    // state. A completed payload is reported through the set's completed
    // fields, in arrival position, at the process_packet call that
    // completed it.

    struct rocketnet_reassembly_set_t * reassembly;
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
// why. out_payload_capacity is the true size of out_payload, which must be at
// least ROCKETNET_MAX_RECEIVE_PAYLOAD_BYTES: a smaller capacity refuses the
// call at entry, before the packet is read, counts RECEIVE_CURSOR_TOO_SMALL,
// leaves out_payload and the protocol state untouched, and never overruns.
// An ordinary single-packet payload lands in out_payload with its length in
// out_payload_bytes; every other outcome leaves it zero. A completed
// fragmented payload is never written here -- it can exceed this buffer --
// and is reported through the reassembly set's completed fields instead.

int rocketnet_client_protocol_process_packet( struct rocketnet_client_protocol_t * client,
                                     const uint8_t * packet_data,
                                     int packet_bytes,
                                     double current_time,
                                     uint8_t * out_payload,
                                     int out_payload_capacity,
                                     int * out_payload_bytes );

// The second family's server address, zeroed when the token names one family
// or the host cannot use both; and whether the attempt is still racing both
// families awaiting the first cookie answer. The transport reads these to
// point a second socket at second_address and to stop racing on commit.

const struct rocketnet_address_t * rocketnet_client_protocol_second_address( const struct rocketnet_client_protocol_t * client );
int rocketnet_client_protocol_race_active( const struct rocketnet_client_protocol_t * client );

// Builds a payload packet for the server, and stamps the send -- ordinary
// traffic suppresses the client's own keep-alive. NOT "as on the server":
// the server's rule splits on whether the client has acknowledged a
// keep-alive (#90), because until then it is starving the packet that would
// establish the connection. The client has no such split, and needs none --
// it sends keep-alives only from _connected_, and nothing at the far end
// waits on one, since any sealed packet confirms the slot.
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

// The fragmented send path, per _Fragmentation_. The payload id counter is
// this attempt's, per direction, starting at one; a sequence block is
// reserved with one atomic add so fill threads split concurrently; and
// each fragment is sealed as an ordinary type 9 session packet whose sent
// record names the payload's aggregate -- the payload's one packet id
// surfaces when every fragment's wire packet is acknowledged.

uint32_t rocketnet_client_protocol_next_fragment_payload_id( struct rocketnet_client_protocol_t * client );

int rocketnet_client_protocol_start_fragments( struct rocketnet_client_protocol_t * client, int fragment_count, uint64_t * base_sequence );

// Seals one fragment packet: the ack block, the fragment header, then the
// fragment bytes, at this sequence under the session keys. The block
// truncates to the budget's remaining room -- the stride is chosen against
// the minimum block and never shrinks to make room. aggregate_id is the
// payload's sender-internal aggregate, or zero when no packet id rides
// this payload. Returns the wire size, or 0 when not connected or the
// fragment is out of range.

int rocketnet_client_protocol_seal_fragment( struct rocketnet_client_protocol_t * client,
                                             uint64_t sequence,
                                             uint32_t payload_id,
                                             int fragment_index,
                                             int fragment_count,
                                             const uint8_t * fragment_data,
                                             int fragment_bytes,
                                             uint64_t aggregate_id,
                                             double current_time,
                                             uint8_t * out_packet );

// The whole split in one call, for callers that drive the protocol
// directly: plans the fragments, assigns the payload id, reserves the
// sequences and seals every fragment packet at ROCKETNET_SEND_BUDGET
// stride in out_packets, each wire size in out_packet_bytes. Returns the
// fragment count, or 0 when the payload does not need fragmenting, does
// not fit the fragment cap or max_packets, or the connection is not up.

int rocketnet_client_protocol_write_fragments( struct rocketnet_client_protocol_t * client,
                                               const uint8_t * payload_data,
                                               int payload_bytes,
                                               double current_time,
                                               uint8_t * out_packets,
                                               int * out_packet_bytes,
                                               int max_packets );

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
