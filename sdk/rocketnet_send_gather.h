/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_SEND_GATHER_H
#define ROCKETNET_SDK_ROCKETNET_SEND_GATHER_H

// The transport-neutral send gather (X2): a send worker hands it one bank of
// staged slots and gets back the slots grouped by destination, in the order
// they were staged within each destination, cut into runs a segmented send
// can carry. A run is consecutive packets to one destination, at most
// max_segments of them and at most max_bytes in all, every packet the size of
// the first but the last, which may be shorter; a shorter packet ends its
// run. An aborted slot (packet_bytes 0) is in no run. The transport turns each
// run into one send over scattered buffers: an iovec array with UDP_SEGMENT
// on Linux, a WSABUF array with USO on Windows.
// proofs/tla/RNSendGatherLayout.tla models it at this layout.
//
// Pure: no allocation, no platform. The caller owns every array.

#include <stddef.h>
#include <stdint.h>
#include "rocketnet_address.h"

struct rocketnet_send_gather_run_t
{
    int first;                  // index into order of the run's first slot
    int count;                  // packets in the run, 1..max_segments
    int segment_bytes;          // the size of every packet but the last
    int last_bytes;             // the size of the last packet (<= segment_bytes)
};

// the scratch a gather of at most max_slots slots needs: the caller allocates
// it once per worker (rocketnet_send_gather_scratch_bytes) and reuses it

struct rocketnet_send_gather_scratch_t
{
    int max_slots;
    int buckets;                // a power of two, at least 2 * max_slots
    int * bucket;               // [buckets]: a destination's group in this gather, or -1
    int * head;                 // [max_slots]: each group's first slot
    int * group;                // [max_slots]: each slot's group, -1 for an aborted slot
    int * start;                // [max_slots]: each group's count, then start, then end in order
};

size_t rocketnet_send_gather_scratch_bytes( int max_slots );

// lays the scratch out in memory of rocketnet_send_gather_scratch_bytes(
// max_slots ) bytes, aligned for int. false when max_slots < 1.
bool rocketnet_send_gather_scratch_init( struct rocketnet_send_gather_scratch_t * scratch, void * memory, int max_slots );

// Groups count slots. Slot i's destination is *(const rocketnet_address_t *)
// ( (const uint8_t *) to + i * to_stride ) and its size is *(const int *)
// ( (const uint8_t *) packet_bytes + i * bytes_stride ), so a transport passes
// its own slot records without copying. Writes the slot indices to order
// (count entries at most: destinations in the order of their first slot,
// each destination's slots in staged order) and the runs to runs (count
// entries at most), and returns the number of runs. Refuses (-1) when count
// exceeds the scratch's max_slots, max_segments < 1 or max_bytes < 1; a
// packet larger than max_bytes is a run of its own. The grouping is a stable
// counting placement, no links: O(count) with the table at most half full.
int rocketnet_send_gather( const struct rocketnet_address_t * to, size_t to_stride,
                           const int * packet_bytes, size_t bytes_stride, int count,
                           int max_segments, int max_bytes,
                           struct rocketnet_send_gather_scratch_t * scratch,
                           int * order, struct rocketnet_send_gather_run_t * runs );

// a destination's home bucket in a table of buckets entries (a power of two):
// the trace harness reads it to give the model the same table
int rocketnet_send_gather_home( const struct rocketnet_address_t * address, int buckets );

#endif // ROCKETNET_SDK_ROCKETNET_SEND_GATHER_H
