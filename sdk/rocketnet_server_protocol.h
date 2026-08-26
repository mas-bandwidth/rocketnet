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

// Server-side connection request processing: the ladder from the standard,
// every check in its exact order, each rung rejecting what it can at its own
// cost so the expensive rung is reached only by traffic that already cleared
// the cheap ones. The library does not own sockets yet, so processing returns
// what to send and the caller sends it.

#define ROCKETNET_SERVER_PROCESS_IGNORED            0
#define ROCKETNET_SERVER_PROCESS_SEND_COOKIE        1
#define ROCKETNET_SERVER_PROCESS_SEND_CHALLENGE     2
#define ROCKETNET_SERVER_PROCESS_SEND_DENIED        3

// The slot view: the ladder needs three answers about client slots, which
// live above this aspect. The interface is three functions and a context, in
// the library's hook style. All three may be NULL while no slot system
// exists, which reads as: nobody connected, slots available.

struct rocketnet_server_slot_view_t
{
    void * context;
    int (*address_connected)( void * context, const struct rocketnet_address_t * address );
    int (*client_id_connected)( void * context, uint64_t client_id );
    int (*num_free_slots)( void * context );
};

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
    struct rocketnet_server_slot_view_t slots;
    struct rocketnet_counters_t * counters;
};

// Processes one connection request packet, full wire bytes including the
// prefilter header, from this source address. current_timestamp is unix
// seconds for token and cookie checks; current_time is the server clock for
// deadlines. On SEND_*, the response packet is in response_packet with its
// length in response_bytes; on IGNORED nothing is sent, ever. The exact
// reason is counted, always, along with the arrival.

int rocketnet_server_process_connection_request( struct rocketnet_server_protocol_t * server,
                                                 const struct rocketnet_address_t * from,
                                                 const uint8_t * packet_data,
                                                 int packet_bytes,
                                                 uint64_t current_timestamp,
                                                 double current_time,
                                                 uint8_t * response_packet,
                                                 int * response_bytes );

#endif // #ifndef ROCKETNET_SERVER_PROTOCOL_H
