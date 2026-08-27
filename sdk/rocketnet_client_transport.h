/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CLIENT_TRANSPORT_H
#define ROCKETNET_CLIENT_TRANSPORT_H

#include <stdint.h>
#include "rocketnet_client_protocol.h"

// The client transport: flow's socket IO -- dual-stack bind, platform
// preferred ports, a receive thread double-buffering against the update
// thread -- carrying the connection protocol. The transport owns the real
// clocks and the sockets; the protocol underneath is the one proven on
// mocked time. Create is the out-of-the-box path: it mints a development
// connect token for the dialed address with the published development keys
// and starts connecting; production token intake lands with the production
// surface.

struct rocketnet_client_transport_t;

struct rocketnet_client_transport_t * rocketnet_client_transport_create( void * context, const char * connect_address_string );

// IMPORTANT: call disconnect and pump a few updates before destroying, so
// the redundant disconnect burst reaches the wire

void rocketnet_client_transport_destroy( struct rocketnet_client_transport_t * client_transport );

// pumps receiving, protocol sending, and deadlines. call every frame.

void rocketnet_client_transport_update( struct rocketnet_client_transport_t * client_transport );

// sends one payload to the server. valid only while connected.

void rocketnet_client_transport_send_packet( struct rocketnet_client_transport_t * client_transport, const uint8_t * packet_data, int packet_bytes );

// receives the next delivered payload, or 0 when none remain this update

int rocketnet_client_transport_receive_packet( struct rocketnet_client_transport_t * client_transport, uint8_t * packet_data );

// the protocol state -- ROCKETNET_CLIENT_STATE_*, readable at any time, the
// primary diagnostic

int rocketnet_client_transport_state( struct rocketnet_client_transport_t * client_transport );

int rocketnet_client_transport_client_index( struct rocketnet_client_transport_t * client_transport );

// sends the redundant disconnect burst and resets to disconnected

void rocketnet_client_transport_disconnect( struct rocketnet_client_transport_t * client_transport );

const struct rocketnet_counters_t * rocketnet_client_transport_counters( struct rocketnet_client_transport_t * client_transport );

#endif // #ifndef ROCKETNET_CLIENT_TRANSPORT_H
