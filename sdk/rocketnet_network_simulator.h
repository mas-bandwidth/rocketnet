/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_NETWORK_SIMULATOR_H
#define ROCKETNET_NETWORK_SIMULATOR_H

#include <stdbool.h>
#include <stdint.h>
#include "rocketnet_address.h"
#include "rocketnet_test_seams.h"

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

#if ROCKETNET_TEST_SEAMS

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

// Deterministic loss and reorder patterns, adjustable at any time.
// drop_every_nth: 0 = disabled, 2..1000000 = drop every Nth send on this link.
// reverse_window: 0 = disabled, 2..64 = buffer every W surviving packets and
// emit them into the wire slots in reversed order (W-1 down to 0). The
// counters start at the call. Returns false, patterns off, when the window
// cannot be allocated.

bool rocketnet_network_simulator_patterns( struct rocketnet_network_simulator_t * simulator,
                                           int drop_every_nth,
                                           int reverse_window );

// Whether either pattern is on.
bool rocketnet_network_simulator_patterns_active( const struct rocketnet_network_simulator_t * simulator );

// Emits the held window reversed, whole; a no-op with nothing held.
void rocketnet_network_simulator_flush_window( struct rocketnet_network_simulator_t * simulator );

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

// Delivers a prepared list of packets in the order given: receive() returns
// packet_data[0] first, then packet_data[1], and so on. Loss, latency, jitter
// and duplicates are not rolled here -- this is the schedule's hand, used to
// script an exact delivery order (drop, duplicate and reorder steps are built
// by composing this list). Every packet becomes due at delivery_time, and the
// slot rule hands them back in list order because they are inserted in list
// order. A packet whose size is outside 1..ROCKETNET_RECEIVE_CEILING is not
// inserted, and a full queue drops the rest exactly as send() does. Returns
// the number of packets handed to the wire.

int rocketnet_network_simulator_deliver_packets( struct rocketnet_network_simulator_t * simulator,
                                                 const struct rocketnet_address_t * from,
                                                 const struct rocketnet_address_t * to,
                                                 const uint8_t * const * packet_data,
                                                 const int * packet_bytes,
                                                 int packet_count,
                                                 double delivery_time );

// Receives the next packet due for this destination at this time, removing
// it from the wire. Returns the packet size, or 0 when nothing is due.
// out_packet must be at least the receive ceiling.

int rocketnet_network_simulator_receive( struct rocketnet_network_simulator_t * simulator,
                                         const struct rocketnet_address_t * to,
                                         double current_time,
                                         struct rocketnet_address_t * out_from,
                                         uint8_t * out_packet );

// Receives the lowest-slot due packet regardless of destination. This is the
// transport-facing drain: it preserves simulator slot order while also
// returning the destination the transport must send to.
int rocketnet_network_simulator_receive_any( struct rocketnet_network_simulator_t * simulator,
                                             double current_time,
                                             struct rocketnet_address_t * out_from,
                                             struct rocketnet_address_t * out_to,
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

// Private DEV-only seams for the index contract. A handle names one physical
// slot generation, so a delayed remove cannot clear a packet that later reused
// the same slot. The counters make the bitmap's bounded word work observable
// without turning timing into a correctness test.

struct rocketnet_network_simulator_slot_handle_t
{
    int slot;
    uint64_t generation;
};

struct rocketnet_network_simulator_index_stats_t
{
    uint64_t membership_word_updates;
    uint64_t max_membership_word_updates;
    uint64_t selection_summary_words;
    uint64_t selection_occupancy_words;
    uint64_t selection_slots;
    uint64_t max_selection_summary_words;
    uint64_t max_selection_occupancy_words;
    uint64_t max_selection_slots;
    uint64_t stale_removals;
};

bool rocketnet_network_simulator_test_handle_for_slot( const struct rocketnet_network_simulator_t * simulator,
                                                        int slot,
                                                        struct rocketnet_network_simulator_slot_handle_t * handle );

bool rocketnet_network_simulator_test_remove_handle( struct rocketnet_network_simulator_t * simulator,
                                                      const struct rocketnet_network_simulator_slot_handle_t * handle );

void rocketnet_network_simulator_test_index_stats( const struct rocketnet_network_simulator_t * simulator,
                                                    struct rocketnet_network_simulator_index_stats_t * stats );

void rocketnet_network_simulator_test_reset_index_stats( struct rocketnet_network_simulator_t * simulator );


#else // #if ROCKETNET_TEST_SEAMS

// A production library has no network simulator: the transports' link
// simulator is refused at config validation before create, so the pointer is
// always NULL there, and these inline stubs let the transport code compile
// unchanged while the simulator itself compiles out.

static inline struct rocketnet_network_simulator_t * rocketnet_network_simulator_create( void *, int, uint64_t ) { return NULL; }
static inline void rocketnet_network_simulator_destroy( struct rocketnet_network_simulator_t * ) {}
static inline void rocketnet_network_simulator_config( struct rocketnet_network_simulator_t *, double, double, float, float ) {}
static inline bool rocketnet_network_simulator_patterns( struct rocketnet_network_simulator_t *, int, int ) { return false; }
static inline bool rocketnet_network_simulator_patterns_active( const struct rocketnet_network_simulator_t * ) { return false; }
static inline void rocketnet_network_simulator_flush_window( struct rocketnet_network_simulator_t * ) {}
static inline void rocketnet_network_simulator_send( struct rocketnet_network_simulator_t *, const struct rocketnet_address_t *, const struct rocketnet_address_t *, const uint8_t *, int, double ) {}
static inline int rocketnet_network_simulator_receive_any( struct rocketnet_network_simulator_t *, double, struct rocketnet_address_t *, struct rocketnet_address_t *, uint8_t * ) { return 0; }

#endif // #if ROCKETNET_TEST_SEAMS

#endif // #ifndef ROCKETNET_NETWORK_SIMULATOR_H
