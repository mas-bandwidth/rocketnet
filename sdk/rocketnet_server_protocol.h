/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SERVER_PROTOCOL_H
#define ROCKETNET_SDK_ROCKETNET_SERVER_PROTOCOL_H

#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_connect_token.h"
#include "rocketnet_protocol.h"
#include "rocketnet_ack.h"
#include "rocketnet_token_tables.h"
#include "rocketnet_server.h"

// Server-side connection request processing: the ladder from the standard,
// every check in its exact order, each rung rejecting what it can at its own
// cost so the expensive rung is reached only by traffic that already cleared
// the cheap ones. The library does not own sockets yet, so processing returns
// what to send and the caller sends it.

// Pointer-only dependencies, deliberately incomplete here: the encryption
// map (rocketnet_encryption_map.h), the client slots
// (rocketnet_client_slots.h) and the counters (rocketnet_counters.h). The
// implementation file includes the full definitions it consumes. The ACK and
// used-token-history types are pointer-only here too, but their headers stay
// full includes: sdk/rocketnet_server_transport.cpp, the fuzz harness and
// the soak harness consume those full definitions through this header.
struct rocketnet_encryption_map_t;
struct rocketnet_encryption_mapping_t;
struct rocketnet_client_slots_t;
struct rocketnet_counters_t;

#if ROCKETNET_DEV
#define ROCKETNET_SERVER_TEST_FAIL_WRITER_NONE               0
#define ROCKETNET_SERVER_TEST_FAIL_REQUEST_DENIED_WRITER     1
#define ROCKETNET_SERVER_TEST_FAIL_RESPONSE_DENIED_WRITER    2
#endif // #if ROCKETNET_DEV

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

    double handshake_deadline_seconds;                      // default 5
    int handshakes_per_update;                              // the handshake work budget, 1 .. 1024; default 16
    int32_t max_timeout_seconds;                            // clamp; <= 0 means no clamp

    struct rocketnet_encryption_map_t * encryption_map;
    struct rocketnet_used_token_history_t * used_token_history;
    struct rocketnet_client_slots_t * client_slots;
    struct rocketnet_server_acks_t * acks;
    struct rocketnet_counters_t * counters;
    struct rocketnet_signature_verify_budget_t signature_verify_budget;   // per-source failed-verify budget, before the verify

    // the connection ids (standard/MESSAGES.md m1.4 rule 5). connection_ids holds one
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
    // why each slot's last connection ended, a ROCKETNET_DISCONNECT_REASON_*:
    // written by rocketnet_server_connection_reset with the ending path's
    // reason (TIMED_OUT, CLIENT_DISCONNECTED, SERVER_DISCONNECTED, NONE for
    // the integrity limit) and cleared to NONE when the slot is next
    // assigned. NULL, as connection_ids, in a protocol built without it.
    int * end_reasons;
    uint64_t payload_connection_id;
    uint64_t payload_sequence;
    int defer_payload_acks;
#if ROCKETNET_DEV
    // One protocol call consumes this private development fault. It lets the
    // unit suite make a fixed-size writer return an impossible size after a
    // valid packet has reached that writer, without bypassing any parsing,
    // authentication or state checks. Production builds carry no field.
    int test_fail_fixed_writer;
    uint64_t test_payload_writes;                           // writes of a payload's plaintext bytes: the open, and any copy after it
#endif // #if ROCKETNET_DEV
};

#if ROCKETNET_DEV
// The receive path's work since create, for the unit suite: the keyed hashes
// of the server's encryption map and the writes of payload plaintext.
// Defined in sdk/rocketnet_server_transport.cpp.
struct rocketnet_server_transport_t;
void rocketnet_server_transport_test_receive_work( const struct rocketnet_server_transport_t * server_transport,
                                                   uint64_t * map_hashes, uint64_t * payload_writes );
#endif // #if ROCKETNET_DEV

// The connection id held by client_index now (P9a): 0 for a free slot, an
// index out of range, or a protocol without the connection id arrays.

uint64_t rocketnet_server_protocol_connection_id( const struct rocketnet_server_protocol_t * server, int client_index );
int rocketnet_server_protocol_end_reason( const struct rocketnet_server_protocol_t * server, int client_index );

// The reported round-trip time of the connection in client_index, in seconds
// (standard/TRANSPORT.md t15.5): the minimum retained first-ack sample, 0
// while it has none, for a free slot, or for an index out of range. The
// message layer takes it as a connection's first RTT sample
// (standard/MESSAGES.md m9.2).

double rocketnet_server_protocol_client_rtt( const struct rocketnet_server_protocol_t * server, int client_index );

// Processes one connection request packet, full wire bytes including the
// prefilter header, from this source address. current_timestamp is unix
// seconds for token and cookie checks; current_time is the server clock for
// deadlines. On SEND_*, the response packet is in response_packet -- sized at
// least ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES (66), the largest response
// -- with its length in response_bytes; on IGNORED nothing is sent, ever.
// The exact reason is counted, always, along with the arrival. It looks the
// source up itself; process_received takes the caller's lookup instead.

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

// The receive entry the server transport calls, with nothing copied twice.
// known_mapping_index is the caller's encryption-map lookup for from (-1 for
// none), made with nothing changing the map since, or
// ROCKETNET_SERVER_MAPPING_NOT_LOOKED_UP and the protocol looks it up: a
// sealed packet and a connection request each pay one keyed hash, the
// request's existing-mapping rung taking the same answer. The packet opens into opened, the caller's
// ROCKETNET_RECEIVE_CEILING bytes; an accepted payload is delivered where it
// opened, *out_payload pointing at its bytes inside opened (NULL otherwise),
// and one above out_payload_capacity is refused whole as
// RECEIVE_CURSOR_TOO_SMALL. Every counter is the one process_packet counts.

#define ROCKETNET_SERVER_MAPPING_NOT_LOOKED_UP -2

int rocketnet_server_protocol_process_received( struct rocketnet_server_protocol_t * server,
                                                const struct rocketnet_address_t * from,
                                                int known_mapping_index,
                                                const uint8_t * packet_data,
                                                int packet_bytes,
                                                uint64_t current_timestamp,
                                                double current_time,
                                                uint8_t * response_packet,
                                                int * response_bytes,
                                                uint8_t * opened,
                                                int out_payload_capacity,
                                                const uint8_t ** out_payload,
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

// The reference an inbound ack block's 32-bit largest is reconstructed
// against: this end's highest session sequence actually sealed and recorded
// sent on the connection, never the next sequence less one, which counts
// sequences a fill has taken but not yet sealed.

uint64_t rocketnet_server_protocol_highest_sent_session( struct rocketnet_server_protocol_t * server,
                                                         int client_index,
                                                         struct rocketnet_encryption_mapping_t * mapping );

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
// ACKNOWLEDGED a keep-alive:
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
// The acknowledgment is the client's unverifiable claim, not proof; see
// _Keep-alive generation_ in standard/RATIONALE.md. The caller loops the client indices each frame. Returns 0 when
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

#if ROCKETNET_DEV
int rocketnet_server_protocol_write_payload( struct rocketnet_server_protocol_t * server,
                                             int client_index,
                                             const uint8_t * payload_data,
                                             int payload_bytes,
                                             double current_time,
                                             uint8_t * out_packet );
#endif // #if ROCKETNET_DEV

// the connected client's address, for fill-phase callers that build wire
// frames without burning a sequence. returns 0 when not connected.

int rocketnet_server_protocol_client_address( struct rocketnet_server_protocol_t * server,
                                              int client_index,
                                              struct rocketnet_address_t * out_address );

// Commit only after the message layer accepted the entire payload.
void rocketnet_server_protocol_accept_payload( struct rocketnet_server_protocol_t * server, int client_index, uint64_t connection_id, uint64_t sequence );

#endif // ROCKETNET_SDK_ROCKETNET_SERVER_PROTOCOL_H
