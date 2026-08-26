/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_SOCKET_H
#define ROCKETNET_SERVER_SOCKET_H

#include "rocketnet_address.h"
#include "rocketnet_packets.h"
#include "rocketnet_constants.h"

struct rocketnet_server_socket_t;

#define ROCKETNET_SERVER_SOCKET_STOPPED    0
#define ROCKETNET_SERVER_SOCKET_RUNNING    1
#define ROCKETNET_SERVER_SOCKET_STOPPING   2

struct rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * server_address, int num_xdp_queues = 7, int num_os_cpus = 1 );

void rocketnet_server_socket_destroy( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_update( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_stop( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_state( struct rocketnet_server_socket_t * server_socket );

uint64_t rocketnet_server_socket_id( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_num_queues( struct rocketnet_server_socket_t * server_socket );

// send packets (zero copy)

uint8_t * rocketnet_server_socket_start_packet( struct rocketnet_server_socket_t * server_socket, const rocketnet_address_t * to, uint64_t * packet_id );

void rocketnet_server_socket_finish_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data, int packet_bytes );

void rocketnet_server_socket_abort_packet( struct rocketnet_server_socket_t * server_socket, uint64_t packet_id, uint8_t * packet_data );

void rocketnet_server_socket_send_packets( struct rocketnet_server_socket_t * server_socket );

// receive packets (zero copy)

void rocketnet_server_socket_receive_packets( struct rocketnet_server_socket_t * server_socket );

struct rocketnet_server_socket_process_packets_t
{
    int num_packets;
    rocketnet_address_t from[ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS];
    int packet_bytes[ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS];
    uint8_t * packet_data[ROCKETNET_SERVER_SOCKET_MAX_PROCESS_PACKETS];
};

struct rocketnet_server_socket_process_packets_t * rocketnet_server_socket_process_packets( struct rocketnet_server_socket_t * server_socket );

#endif // #ifndef ROCKETNET_SERVER_SOCKET_H
