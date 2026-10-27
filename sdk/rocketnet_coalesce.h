/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The UDP coalescing rules, pure: no syscall, no allocation, no global state,
// no platform socket header. Two rules live here, each as the Linux GSO transport
// (sdk/rocketnet_server_gso_linux.cpp) applies it, so every transport that
// coalesces (Linux UDP_GRO / UDP_SEGMENT, Windows URO / USO) splits and builds
// runs the same way:
//
//   receive split: one coalesced receive of message_bytes cut at the segment
//   stride the kernel reports, the last segment shorter when the message is
//   not a multiple of the stride.
//
//   send run: the longest prefix of a send queue that goes out as one
//   segmented send: the same destination, every packet the size of the first
//   except at most one shorter tail, which closes the run.

#pragma once

#ifndef ROCKETNET_SDK_ROCKETNET_COALESCE_H
#define ROCKETNET_SDK_ROCKETNET_COALESCE_H

#include <stdint.h>

#include "rocketnet_address.h"

// Splits a coalesced receive of message_bytes at segment_bytes. A
// segment_bytes <= 0 (no stride reported) is one segment of the whole
// message; otherwise segments of segment_bytes from offset 0, the last one
// shorter when the message is not a multiple. Writes offsets[i] and
// lengths[i] for each segment and returns the count. Returns 0 for
// message_bytes <= 0, and -1 with nothing written when the count would pass
// max_segments. Size bounds on each segment stay with the caller.

static inline int rocketnet_coalesce_split( int message_bytes, int segment_bytes, int * offsets, int * lengths, int max_segments )
{
    if ( message_bytes <= 0 )
        return 0;

    if ( segment_bytes <= 0 )
        segment_bytes = message_bytes;

    const int count = message_bytes / segment_bytes + ( message_bytes % segment_bytes != 0 ? 1 : 0 );
    if ( count > max_segments )
        return -1;

    int n = 0;

    for ( int offset = 0; offset < message_bytes; offset += segment_bytes )
    {
        const int remaining = message_bytes - offset;

        offsets[n] = offset;
        lengths[n] = remaining < segment_bytes ? remaining : segment_bytes;
        n++;
    }

    return n;
}

// The length of the send run starting at index 0 of a queue of count packets.
// packet_bytes[0] sets the segment size; the run extends over the following
// packets while each is not larger than the first, has positive bytes (zero
// marks an aborted slot), fits max_super_bytes in total, goes to the same
// destination as packet 0, and the run is below max_segments. A packet
// shorter than the first is taken into the run and closes it. Returns at
// least 1; 0 when count is 0 or packet_bytes[0] <= 0.

static inline int rocketnet_coalesce_send_run( const int * packet_bytes, const struct rocketnet_address_t * to, int count, int max_segments, int max_super_bytes )
{
    if ( count <= 0 || packet_bytes[0] <= 0 )
        return 0;

    const int segment_bytes = packet_bytes[0];
    int run_count = 1;
    int run_bytes = segment_bytes;

    while ( run_count < count && run_count < max_segments )
    {
        const int next_bytes = packet_bytes[run_count];

        if ( next_bytes <= 0 )
            break;
        if ( next_bytes > segment_bytes )
            break;
        if ( run_bytes + next_bytes > max_super_bytes )
            break;
        if ( !rocketnet_address_equal( &to[run_count], &to[0] ) )
            break;

        run_count++;
        run_bytes += next_bytes;

        if ( next_bytes < segment_bytes )
            break;                                  // the shorter tail closes the run
    }

    return run_count;
}

#endif // ROCKETNET_SDK_ROCKETNET_COALESCE_H
