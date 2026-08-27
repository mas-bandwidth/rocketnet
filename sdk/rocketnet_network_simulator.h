/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_NETWORK_SIMULATOR_H
#define ROCKETNET_NETWORK_SIMULATOR_H

#include <stdint.h>
#include "rocketnet_address.h"

// An in-memory wire for tests: latency, jitter, packet loss and duplicates
// under mocked time, so connection logic is tested with no sockets and no
// real clocks anywhere. A debug and test instrument, deliberately in its
// simplest correct form -- it is not a production network emulator, and the
// perf baselines measure the protocol on a direct wire, never through this.
//
// Deterministic: all randomness comes from a seed given at create, so a
// failing run replays exactly.

struct rocketnet_network_simulator_t;

struct rocketnet_network_simulator_t * rocketnet_network_simulator_create( void * context,
                                                                           int capacity,
                                                                           uint64_t seed );

void rocketnet_network_simulator_destroy( struct rocketnet_network_simulator_t * simulator );

// configuration, adjustable at any time. latency and jitter in seconds;
// loss and duplicate as percentages 0..100.

void rocketnet_network_simulator_config( struct rocketnet_network_simulator_t * simulator,
                                         double latency,
                                         double jitter,
                                         float packet_loss_percent,
                                         float duplicate_percent );

// Sends a packet into the wire. Loss is rolled here; delivery time is
// current_time + latency + random jitter; a duplicate may follow with its
// own delivery time. A full queue drops the packet -- the wire is lossy by
// nature and the capacity is the caller's statement about burst size.

void rocketnet_network_simulator_send( struct rocketnet_network_simulator_t * simulator,
                                       const struct rocketnet_address_t * from,
                                       const struct rocketnet_address_t * to,
                                       const uint8_t * packet_data,
                                       int packet_bytes,
                                       double current_time );

// Receives the next packet due for this destination at this time, removing
// it from the wire. Returns the packet size, or 0 when nothing is due.
// out_packet must be at least the receive ceiling.

int rocketnet_network_simulator_receive( struct rocketnet_network_simulator_t * simulator,
                                         const struct rocketnet_address_t * to,
                                         double current_time,
                                         struct rocketnet_address_t * out_from,
                                         uint8_t * out_packet );

#endif // #ifndef ROCKETNET_NETWORK_SIMULATOR_H
