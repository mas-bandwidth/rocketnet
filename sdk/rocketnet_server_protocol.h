/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_PROTOCOL_H
#define ROCKETNET_SERVER_PROTOCOL_H

#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_counters.h"
#include "rocketnet_client_slots.h"
#include "rocketnet_ack.h"
#include "rocketnet_server.h"

// Server-side connection request processing: the ladder from the standard,
// every check in its exact order, each rung rejecting what it can at its own
// cost so the expensive rung is reached only by traffic that already cleared
// the cheap ones. The library does not own sockets yet, so processing returns
// what to send and the caller sends it.

struct rocketnet_reassembly_set_t;

#define ROCKETNET_SERVER_PROCESS_IGNORED            0
#define ROCKETNET_SERVER_PROCESS_SEND_COOKIE        1
#define ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE     2
#define ROCKETNET_SERVER_PROCESS_SEND_DENIED        3
#define ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE    4
#define ROCKETNET_SERVER_PROCESS_ACCEPTED           5

struct rocketnet_server_protocol_t
{
    uint64_t protocol_id;
    double send_rate;                                       // keep-alives per second per client, default style: 10
    uint8_t packet_filter[ROCKETNET_FILTER_VALUE_BYTES];

    uint8_t backend_public_key[ROCKETNET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_KEY_BYTES];

    struct rocketnet_address_t advertised_ipv4;             // type NONE when not advertised
    struct rocketnet_address_t advertised_ipv6;

    uint8_t cookie_key[ROCKETNET_COOKIE_KEY_BYTES];
    uint8_t previous_cookie_key[ROCKETNET_COOKIE_KEY_BYTES];
    int has_previous_cookie_key;
    int cookie_lifetime_seconds;                            // default 3
    int cookie_rotation_seconds;                            // default 60; 0 means no rotation
    double cookie_key_rotation_time;                        // when the current key is next replaced
    double previous_cookie_key_expire_time;                 // when the previous key stops being accepted
    int max_token_lifetime_seconds;                         // 0 means no limit

    double handshake_deadline_seconds;                      // default 5
    int32_t max_timeout_seconds;                            // clamp; <= 0 means no clamp

    struct rocketnet_encryption_map_t * encryption_map;
    struct rocketnet_used_token_history_t * used_token_history;
    struct rocketnet_client_slots_t * client_slots;
    struct rocketnet_server_acks_t * acks;
    struct rocketnet_reassembly_set_t * reassembly;         // per-client fragment reassembly, one connection per slot
    struct rocketnet_counters_t * counters;
    struct rocketnet_signature_verify_budget_t signature_verify_budget;   // per-source failed-verify budget, before the verify

    // the connection ids (docs/CHANNELS.md P9a, P9b). connection_ids holds one
    // per client slot: nonzero and unique over the server's life while the
    // slot is held (a counter from 1, taken when the slot is assigned), 0
    // while it is free. acked_connection_ids parallels acks->acked_client_index
    // and payload_connection_id is the tag of the payload process_packet last
    // returned (0 when none): each is written when its entry is produced, so
    // an entry names the connection it was received for even when its slot is
    // released and reassigned later in the same update. A protocol built
    // without the arrays (NULL) tags nothing and reads 0.
    uint64_t next_connection_id;
    uint64_t * connection_ids;
    uint64_t * acked_connection_ids;
    uint64_t payload_connection_id;
};

// The connection id held by client_index now (P9a): 0 for a free slot, an
// index out of range, or a protocol without the connection id arrays.

uint64_t rocketnet_server_protocol_connection_id( const struct rocketnet_server_protocol_t * server, int client_index );

// Processes one connection request packet, full wire bytes including the
// prefilter header, from this source address. current_timestamp is unix
// seconds for token and cookie checks; current_time is the server clock for
// deadlines. On SEND_*, the response packet is in response_packet -- sized at
// least ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES (64), the largest response
// -- with its length in response_bytes; on IGNORED nothing is sent, ever.
// The exact reason is counted, always, along with the arrival.

int rocketnet_server_protocol_process_connection_request( struct rocketnet_server_protocol_t * server,
                                                 const struct rocketnet_address_t * from,
                                                 const uint8_t * packet_data,
                                                 int packet_bytes,
                                                 uint64_t current_timestamp,
                                                 double current_time,
                                                 uint8_t * response_packet,
                                                 int * response_bytes );

// The server's receive entry for every packet: the reading order from the
// standard over one function. Runs the prefilter, refuses reserved types and
// wrong-direction types, hands type 0 to the request ladder, and takes the
// sealed types through mapping lookup, sequence reconstruction, replay and
// open, then dispatches. On SEND_* the response is in response_packet with
// its length in response_bytes. A payload that opens for a connected client
// returns ACCEPTED with the bytes in out_payload (sized at least
// ROCKETNET_RECEIVE_PAYLOAD_BYTES) and
// the client index in out_payload_client_index; every other outcome leaves
// out_payload_bytes zero.

int rocketnet_server_protocol_process_packet( struct rocketnet_server_protocol_t * server,
                                     const struct rocketnet_address_t * from,
                                     const uint8_t * packet_data,
                                     int packet_bytes,
                                     uint64_t current_timestamp,
                                     double current_time,
                                     uint8_t * response_packet,
                                     int * response_bytes,
                                     uint8_t * out_payload,
                                     int * out_payload_bytes,
                                     int * out_payload_client_index );

// Rotates the connection cookie key when current_time has reached
// cookie_key_rotation_time. With an on-time rotation the outgoing key is kept
// as previous_cookie_key until cookie_key_rotation_time + period; after a
// stall past the whole window the previous key is zeroed and cleared.

void rocketnet_server_protocol_rotate_cookie_key_if_due( struct rocketnet_server_protocol_t * server,
                                                         double current_time );

// The server's per-frame update: cookie key rotation and the sweep. Call once
// per frame with the mocked or real clocks -- nothing in here reads a clock.
// An unestablished mapping past its handshake deadline is removed; an
// established one idle past its timeout releases its slot -- the client timed
// out -- and its mapping with it. The sweep lives here rather than in the map
// because the decision touches the slots.

void rocketnet_server_protocol_update( struct rocketnet_server_protocol_t * server,
                              double current_time );

// Builds a keep-alive for a connected client at the mapping's next session
// sequence, and stamps the send. The sealed plaintext opens with the ack
// block built from this connection's receive state, so the wire size
// varies: 41 bytes with the minimum block, growing with loss events, never
// past the send budget. Returns the wire size, or 0 for a client index
// that is not connected.

int rocketnet_server_write_keep_alive( struct rocketnet_server_protocol_t * server,
                                       int client_index,
                                       double current_time,
                                       uint8_t * out_packet );

// The standalone ack, type 8: built when the client's ack debt -- eliciting
// packets received since the last block this end sent -- has reached the
// threshold. Sending any block resets the debt, so the connection's own
// traffic normally keeps this at zero; it fires when inbound outruns
// outbound. Returns the wire size, or 0 when not due or not connected.

int rocketnet_server_protocol_write_ack_if_due( struct rocketnet_server_protocol_t * server,
                                                int client_index,
                                                double current_time,
                                                uint8_t * out_packet );

// The paced form, and which clock it reads turns on whether this client has
// ACKNOWLEDGED a keep-alive (#90):
//
//   until it has -- paced off the last keep-alive sent to it, and ordinary
//   traffic MUST NOT suppress it. The keep-alive carries the client index and
//   a client lacking its index cannot re-provoke one, so this is the repair
//   that must not be suppressible.
//
//   once it has -- paced off the last send of anything, so ordinary traffic
//   suppresses it and keep-alives appear only on an otherwise idle
//   connection.
//
// The acknowledgment is the peer's unverifiable claim, not proof; see
// _Keep-alive generation_ in STANDARD.md and the fuller reasoning at the
// definition. The caller loops the client indices each frame. Returns 0 when
// not connected or not due.

int rocketnet_server_protocol_write_keep_alive_if_due( struct rocketnet_server_protocol_t * server,
                                              int client_index,
                                              double current_time,
                                              uint8_t * out_packet );

// Read-only preflight for the update sweep. True when the keep-alive or
// standalone ACK writer may need a frame; the writers retain priority and
// all packet/state changes. Also leaves a missing mapping for the transport's diagnostic.
int rocketnet_server_protocol_maintenance_due( const struct rocketnet_server_protocol_t * server,
                                                int client_index, double current_time );

// Records the same missing-mapping diagnostic as the protocol writers when
// the transport cannot look up an address for an internally allowed -1 slot.
void rocketnet_server_protocol_note_missing_mapping( struct rocketnet_server_protocol_t * server );

#define ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS 10

// The deliberate server-side disconnect of one connected client: fills the
// redundant burst -- ROCKETNET_SERVER_NUM_DISCONNECT_PACKETS sealed
// disconnect packets at consecutive session sequences, each opening with
// the same ack block, at ROCKETNET_SEND_BUDGET stride in out_packets --
// sets out_packet_count and the shared per-packet wire size, then releases
// the slot and destroys the mapping immediately. The reasons live above
// the library: the game decides why. Does nothing (count zero) when the
// client is not connected.

void rocketnet_server_protocol_disconnect_client( struct rocketnet_server_protocol_t * server,
                                         int client_index,
                                         uint8_t * out_packets,
                                         int * out_packet_count,
                                         int * out_packet_bytes );

// Builds a payload packet for a connected client. Returns the wire size, or
// 0 when the client is not connected or the payload does not fit.

int rocketnet_server_protocol_start_payload( struct rocketnet_server_protocol_t * server,
                                             int client_index,
                                             struct rocketnet_address_t * to,
                                             uint64_t * sequence );

// seals the frame's payload in place, opening the sealed plaintext with the
// ack block, records the sent packet in the delivery bookkeeping under
// packet_id -- the join key surfaced when the packet is acknowledged -- and
// stamps the send.

int rocketnet_server_protocol_seal_payload_frame( struct rocketnet_server_protocol_t * server,
                                                  int client_index,
                                                  uint64_t sequence,
                                                  uint64_t packet_id,
                                                  uint8_t * wire_start,
                                                  int payload_bytes,
                                                  double current_time );

int rocketnet_server_protocol_write_payload( struct rocketnet_server_protocol_t * server,
                                    int client_index,
                                    const uint8_t * payload_data,
                                    int payload_bytes,
                                    double current_time,
                                    uint8_t * out_packet );

// the connected client's address, for fill-phase callers that build wire
// frames without burning a sequence. returns 0 when not connected.

int rocketnet_server_protocol_client_address( struct rocketnet_server_protocol_t * server,
                                              int client_index,
                                              struct rocketnet_address_t * out_address );

// The fragmented send path, per _Fragmentation_ -- the client protocol's
// mirror, per connected client: payload ids per direction from one, a
// sequence block reserved with one atomic add, each fragment an ordinary
// sealed type 9 session packet whose sent record names the payload's
// aggregate.

uint32_t rocketnet_server_protocol_next_fragment_payload_id( struct rocketnet_server_protocol_t * server, int client_index );

int rocketnet_server_protocol_start_fragments( struct rocketnet_server_protocol_t * server, int client_index, int fragment_count, uint64_t * base_sequence );

int rocketnet_server_protocol_seal_fragment( struct rocketnet_server_protocol_t * server,
                                             int client_index,
                                             uint64_t sequence,
                                             uint32_t payload_id,
                                             int fragment_index,
                                             int fragment_count,
                                             const uint8_t * fragment_data,
                                             int fragment_bytes,
                                             uint64_t aggregate_id,
                                             double current_time,
                                             uint8_t * out_packet );

int rocketnet_server_protocol_write_fragments( struct rocketnet_server_protocol_t * server,
                                               int client_index,
                                               const uint8_t * payload_data,
                                               int payload_bytes,
                                               double current_time,
                                               uint8_t * out_packets,
                                               int * out_packet_bytes,
                                               int max_packets );

#endif // #ifndef ROCKETNET_SERVER_PROTOCOL_H
