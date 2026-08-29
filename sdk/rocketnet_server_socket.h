/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.  
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SERVER_SOCKET_H
#define ROCKETNET_SERVER_SOCKET_H

#include "rocketnet_address.h"
#include "rocketnet_constants.h"
#include "rocketnet_wire.h"

struct rocketnet_server_socket_t;

#define ROCKETNET_SERVER_SOCKET_STOPPED    0
#define ROCKETNET_SERVER_SOCKET_RUNNING    1
#define ROCKETNET_SERVER_SOCKET_STOPPING   2

// The non-mutating pre-flight (#106, ruled on the #110 review): checks the
// machine IS provisioned for the XDP data plane and refuses by name when it
// is not -- each unmet condition logs the knob, the wanted value, and
// scripts/rocketnet-provision, whose header carries the authoritative
// condition list both this check and that script derive from. Read-only by
// contract: it may create and close probe descriptors (an AF_XDP socket
// never bound, a BPF program never attached) but never changes machine
// state. create() runs it first and refuses without it; the driver ladder
// and the test suite call it directly so a refusal is named without ever
// touching the committing path.

bool rocketnet_server_socket_preflight( const char * public_address_string, int num_queues );

// The filter value configures the kernel prefilter: only packets carrying it
// reach the AF_XDP rings. It is the same value the protocol object checks, at the same epoch.
//
// create is VERIFY AND BIND only: the pre-flight above, then the library's
// own per-socket acts -- building and loading its BPF program, attaching
// it, AF_XDP binds, the memlock rlimit. Machine provisioning (xdp-loader
// clearing, SMT, napi tuning, ethtool -L queue counts, isolcpus guidance)
// lives in scripts/rocketnet-provision, run deliberately by the operator;
// create never mutates machine state.

struct rocketnet_server_socket_t * rocketnet_server_socket_create( void * context, const char * server_address, const uint8_t * filter_value, int num_xdp_queues = 7, int num_os_cpus = 1 );

void rocketnet_server_socket_destroy( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_update( struct rocketnet_server_socket_t * server_socket );

void rocketnet_server_socket_stop( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_state( struct rocketnet_server_socket_t * server_socket );

uint64_t rocketnet_server_socket_id( struct rocketnet_server_socket_t * server_socket );

int rocketnet_server_socket_num_queues( struct rocketnet_server_socket_t * server_socket );

// The kernel prefilter's counters, summed across CPUs into an array of
// ROCKETNET_SERVER_XDP_NUM_COUNTERS values -- what the XDP program dropped
// and why, and what it redirected, before userspace ever saw a byte.

void rocketnet_server_socket_xdp_counters( struct rocketnet_server_socket_t * server_socket, uint64_t * counters );

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
