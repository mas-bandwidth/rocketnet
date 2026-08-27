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

// Server-side connection request processing: the ladder from the standard,
// every check in its exact order, each rung rejecting what it can at its own
// cost so the expensive rung is reached only by traffic that already cleared
// the cheap ones. The library does not own sockets yet, so processing returns
// what to send and the caller sends it.

#define ROCKETNET_SERVER_PROCESS_IGNORED            0
#define ROCKETNET_SERVER_PROCESS_SEND_COOKIE        1
#define ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE     2
#define ROCKETNET_SERVER_PROCESS_SEND_DENIED        3
#define ROCKETNET_SERVER_PROCESS_SEND_KEEP_ALIVE    4
#define ROCKETNET_SERVER_PROCESS_ACCEPTED           5

struct rocketnet_server_protocol_t
{
    uint64_t protocol_id;
    uint8_t title_tag[ROCKETNET_TITLE_TAG_BYTES];

    uint8_t backend_public_key[ROCKETNET_KEY_BYTES];
    uint8_t server_public_key[ROCKETNET_KEY_BYTES];
    uint8_t server_secret_key[ROCKETNET_KEY_BYTES];

    struct rocketnet_address_t advertised_ipv4;             // type NONE when not advertised
    struct rocketnet_address_t advertised_ipv6;

    uint8_t cookie_key[ROCKETNET_COOKIE_KEY_BYTES];
    uint8_t previous_cookie_key[ROCKETNET_COOKIE_KEY_BYTES];
    int has_previous_cookie_key;
    int cookie_lifetime_seconds;                            // default 3

    double handshake_deadline_seconds;                      // default 5
    int32_t max_timeout_seconds;                            // clamp; <= 0 means no clamp
    int allow_negative_timeout;                             // dev only

    struct rocketnet_encryption_map_t * encryption_map;
    struct rocketnet_used_token_history_t * used_token_history;
    struct rocketnet_admitted_token_set_t * admitted_token_set;
    struct rocketnet_client_slots_t * client_slots;
    struct rocketnet_counters_t * counters;
};

// Processes one connection request packet, full wire bytes including the
// prefilter header, from this source address. current_timestamp is unix
// seconds for token and cookie checks; current_time is the server clock for
// deadlines. On SEND_*, the response packet is in response_packet -- sized at
// least ROCKETNET_CONNECTION_CHALLENGE_WIRE_BYTES (64), the largest response
// -- with its length in response_bytes; on IGNORED nothing is sent, ever.
// The exact reason is counted, always, along with the arrival.

int rocketnet_server_process_connection_request( struct rocketnet_server_protocol_t * server,
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
// returns ACCEPTED with the bytes in out_payload (sized at least 1376) and
// the client index in out_payload_client_index; every other outcome leaves
// out_payload_bytes zero.

int rocketnet_server_process_packet( struct rocketnet_server_protocol_t * server,
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

// Builds a keep-alive for a connected client at the mapping's next session
// sequence. The caller paces these at the send rate per client -- a rate in
// time, never per update -- and they are what repair a lost first keep-alive.
// Returns the wire size, or 0 for a client index that is not connected.

int rocketnet_server_write_keep_alive( struct rocketnet_server_protocol_t * server,
                                       int client_index,
                                       uint8_t * out_packet );

// Builds a payload packet for a connected client. Returns the wire size, or
// 0 when the client is not connected or the payload does not fit.

int rocketnet_server_write_payload( struct rocketnet_server_protocol_t * server,
                                    int client_index,
                                    const uint8_t * payload_data,
                                    int payload_bytes,
                                    uint8_t * out_packet );

#endif // #ifndef ROCKETNET_SERVER_PROTOCOL_H
