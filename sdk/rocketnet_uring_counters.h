/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_URING_COUNTERS_H
#define ROCKETNET_SDK_ROCKETNET_URING_COUNTERS_H

// How one io_uring queue's module counts become the transport's public
// ROCKETNET_COUNTER_URING_* rows (rocketnet_server_uring_counters sums every
// queue through this). The module arrays are accumulators of those rows, one
// writer each, read here by index; no module count is readable any other
// way. Pure: unit tested with a distinct value in every input
// (tests/test_unit_uring_counters.cpp).

#include <stdint.h>

#include "rocketnet_counters.h"
#include "rocketnet_uring_queue_send.h"
#include "rocketnet_uring_queue_worker.h"
#include "rocketnet_uring_receive_queue.h"

// adds one queue's counts to counters, an array of ROCKETNET_NUM_COUNTERS
// values indexed by public counter: receive and worker are the module
// arrays, sent, dropped and abandoned the send module's counts

static inline void rocketnet_uring_counters_add_queue( uint64_t * counters, const uint64_t * receive, const uint64_t * worker, uint64_t sent, uint64_t dropped, uint64_t abandoned )
{
    counters[ROCKETNET_COUNTER_URING_PACKETS_SENT] += sent;
    counters[ROCKETNET_COUNTER_URING_PACKETS_RECEIVED] += receive[ROCKETNET_URING_RECEIVE_COUNTER_BANKED];
    counters[ROCKETNET_COUNTER_URING_SUBMIT_SYSCALLS] += worker[ROCKETNET_URING_WORKER_COUNTER_SUBMITS];
    counters[ROCKETNET_COUNTER_URING_SEND_DROPPED] += dropped + abandoned;
    counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SIZE] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SIZE];
    counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_OVERFLOW] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_OVERFLOW];
    counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_SOURCE] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_SOURCE];
    counters[ROCKETNET_COUNTER_URING_RECEIVE_NO_BUFFERS] += receive[ROCKETNET_URING_RECEIVE_COUNTER_NO_BUFFERS];
    counters[ROCKETNET_COUNTER_URING_RECEIVE_REARMS] += receive[ROCKETNET_URING_RECEIVE_COUNTER_REARMS];
    counters[ROCKETNET_COUNTER_URING_REAP_DEFERRED] += worker[ROCKETNET_URING_WORKER_COUNTER_REAP_DEFERRED];
    counters[ROCKETNET_COUNTER_URING_SEND_DEFERRED] += worker[ROCKETNET_URING_WORKER_COUNTER_SOCKET_FULL];
    counters[ROCKETNET_COUNTER_URING_RECEIVE_DROPPED_NO_BUFFER] += receive[ROCKETNET_URING_RECEIVE_COUNTER_DROPPED_NO_BUFFER];
    counters[ROCKETNET_COUNTER_URING_RECEIVE_REARM_FAILED] += receive[ROCKETNET_URING_RECEIVE_COUNTER_REARM_FAILED];
    counters[ROCKETNET_COUNTER_URING_GSO_SENDS] += worker[ROCKETNET_URING_WORKER_COUNTER_GSO_SENDS];
    counters[ROCKETNET_COUNTER_URING_GSO_FALLBACKS] += worker[ROCKETNET_URING_WORKER_COUNTER_GSO_FALLBACKS];
    counters[ROCKETNET_COUNTER_URING_GRO_BATCHES] += receive[ROCKETNET_URING_RECEIVE_COUNTER_GRO_BATCHES];
    counters[ROCKETNET_COUNTER_URING_GRO_CMSG_FAULTS] += receive[ROCKETNET_URING_RECEIVE_COUNTER_GRO_CMSG_FAULTS];
}

// the finish_packet calls the send banks refused (a pointer naming no slot of
// the producer bank): each lost one packet, so each is a send drop

static inline void rocketnet_uring_counters_add_finish_refused( uint64_t * counters, uint64_t refused )
{
    counters[ROCKETNET_COUNTER_URING_SEND_DROPPED] += refused;
}

#endif // ROCKETNET_SDK_ROCKETNET_URING_COUNTERS_H
