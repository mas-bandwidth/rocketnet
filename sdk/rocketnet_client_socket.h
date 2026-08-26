/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_CLIENT_SOCKET_H
#define ROCKETNET_CLIENT_SOCKET_H

#include "rocketnet_address.h"
#include "rocketnet_packets.h"

struct rocketnet_client_socket_t;

#define ROCKETNET_CLIENT_SOCKET_CONNECTION_TIMED_OUT    -2
#define ROCKETNET_CLIENT_SOCKET_INIT_TIMED_OUT          -1
#define ROCKETNET_CLIENT_SOCKET_DISCONNECTED             0
#define ROCKETNET_CLIENT_SOCKET_INITIALIZING             1
#define ROCKETNET_CLIENT_SOCKET_CONNECTING               2
#define ROCKETNET_CLIENT_SOCKET_CONNECTED                3

rocketnet_client_socket_t * rocketnet_client_socket_create( void * context, const char * connect_address );

void rocketnet_client_socket_destroy( rocketnet_client_socket_t * client_socket );

void rocketnet_client_socket_update( rocketnet_client_socket_t * client_socket );

void rocketnet_client_socket_send_packet( rocketnet_client_socket_t * client_socket, const uint8_t * packet_data, int packet_bytes );

int rocketnet_client_socket_receive_packet( rocketnet_client_socket_t * client_socket, uint8_t * packet_data );

void rocketnet_client_socket_disconnect( rocketnet_client_socket_t * client_socket );

int rocketnet_client_socket_state( rocketnet_client_socket_t * client_socket );

uint64_t rocketnet_client_socket_flow_id( rocketnet_client_socket_t * client_socket );

uint64_t rocketnet_client_socket_server_id( rocketnet_client_socket_t * client_socket );

#endif // #ifndef ROCKETNET_CLIENT_SOCKET_H
