/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_TRANSPORT_H
#define ROCKETNET_SERVER_TRANSPORT_H

#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_counters.h"

// The server transport: flow's server socket IO carrying the connection
// protocol. This is the portable implementation, plain sockets on every
// platform; the XDP transport is its high-performance twin and follows the
// same surface. Transport selection is compile-time for now; runtime
// configuration and preference ordering is a named follow-on.
//
// The shape is flow's: received payloads come back as a list of pointers,
// zero copy from the frames they were decrypted in; sends are reserved as
// frames, written in place, and flushed together. The optimization effort
// concentrates here rather than the client, because the server carries a
// thousand players' packets.
//
// Create is the out-of-the-box path: development keys, so a first server is
// create then update in a loop. Table sizing per the standard: encryption
// map 2x max clients, used-token history 8x, admitted-token set 2x.

struct rocketnet_server_transport_t;

struct rocketnet_server_transport_t * rocketnet_server_transport_create( void * context,
                                                                         const char * public_address_string,
                                                                         int max_clients );

// IMPORTANT: stop the server and wait for the stopped state before destroying

void rocketnet_server_transport_destroy( struct rocketnet_server_transport_t * server_transport );

#define ROCKETNET_SERVER_TRANSPORT_RUNNING      0
#define ROCKETNET_SERVER_TRANSPORT_STOPPING     1
#define ROCKETNET_SERVER_TRANSPORT_STOPPED      2

void rocketnet_server_transport_stop( struct rocketnet_server_transport_t * server_transport );

int rocketnet_server_transport_state( struct rocketnet_server_transport_t * server_transport );

// Pumps everything: receives, runs every packet through the protocol, sends
// the responses, runs the sweep, sends the paced keep-alives, and flushes
// the send frames. Call every frame. After update, the delivered payloads
// are in the payloads list until the next update.

void rocketnet_server_transport_update( struct rocketnet_server_transport_t * server_transport );

// the payloads delivered by the last update: pointers into transport
// storage, valid until the next update, each tagged with the client index

struct rocketnet_server_transport_payloads_t
{
    int num_payloads;
    int client_index[256];
    uint8_t * payload_data[256];
    int payload_bytes[256];
};

struct rocketnet_server_transport_payloads_t * rocketnet_server_transport_payloads( struct rocketnet_server_transport_t * server_transport );

// sends one payload to a connected client, sealed in place into a send
// frame and flushed with the next update

void rocketnet_server_transport_send_packet( struct rocketnet_server_transport_t * server_transport,
                                             int client_index,
                                             const uint8_t * payload_data,
                                             int payload_bytes );

int rocketnet_server_transport_num_connected_clients( struct rocketnet_server_transport_t * server_transport );

int rocketnet_server_transport_is_client_connected( struct rocketnet_server_transport_t * server_transport, int client_index );

uint64_t rocketnet_server_transport_client_id( struct rocketnet_server_transport_t * server_transport, int client_index );

const struct rocketnet_counters_t * rocketnet_server_transport_counters( struct rocketnet_server_transport_t * server_transport );

#endif // #ifndef ROCKETNET_SERVER_TRANSPORT_H
