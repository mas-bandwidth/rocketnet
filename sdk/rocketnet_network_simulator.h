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
//
// The delivery rule, stated because it is the instrument's behaviour rather
// than an implementation detail: receive() delivers the packet in the LOWEST
// OCCUPIED SLOT INDEX that is due and addressed to the caller, and send()
// takes the LOWEST FREE SLOT INDEX. That is not delivery-time order and not
// FIFO; it is what the array walk this file was built around produced, and
// every soak and functional test has run against it. #99 replaced the walk
// with an index over the same slots in the same order -- the cost changed,
// the delivered sequence did not -- and the audit below is what holds that
// claim up.

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

// The audit: the instrument's failure state. With it on, every receive() also
// runs the original full-array scan as a reference and compares which slot the
// two selections chose, then checks the whole index against the packet arrays.
// Any divergence prints what diverged and ABORTS -- it does not log, warn or
// return an error, because a simulator that has caught itself delivering a
// different packet sequence must not go on feeding a soak. It fires under
// NDEBUG, where rocketnet_assert does not, which is the build the soaks run.
//
// Deliberately slow: it is O(capacity) per receive by construction, since
// checking the fast path means running the slow one. Proof mode, not soak
// mode. soak_sim turns it on with ROCKETNET_SOAK_SIM_AUDIT=1.

void rocketnet_network_simulator_audit( struct rocketnet_network_simulator_t * simulator, int enabled );

// The audit's own negative control: unlink one occupied packet from the index
// while leaving it occupied. Nothing crashes and nothing logs -- the packet is
// simply never delivered again, which is exactly the silent mis-measurement an
// index can introduce. With the audit on, this must go red immediately; if it
// does not, the audit is decoration. scripts/soak-sim-selftest runs both
// legs and is the receipt.

void rocketnet_network_simulator_inject_index_fault( struct rocketnet_network_simulator_t * simulator );

#endif // #ifndef ROCKETNET_NETWORK_SIMULATOR_H
