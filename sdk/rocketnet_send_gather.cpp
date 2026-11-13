/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_send_gather.h"
#include "rocketnet_internal.h"

#include <string.h>

// proofs/tla/RNSendGatherCall.tla Gather(o, r) is one call, as its caller
// sees it: o the order it writes, r the runs it returns.
// proofs/tla/RNSendGatherLayout.tla models the gather at this layout: the
// scan's open-addressed table probed modulo its size, a group per
// destination in order of its first slot, the counts turned into starts, a
// stable placement of the slots first to last, and the emit's runs cut by
// the segment limit, the byte cap and a smaller packet. No slot links to
// another (docs/SEQUENCE-BUFFERS.md). Each step cites the action it is.
// ROCKETNET_SEND_GATHER_PLANT is the trace check's plant (the placement last
// to first, reversing each destination's packets): only
// tests/tla_trace_gather.cpp in the plant build defines it, and its traces
// must fail TraceConsistent.

static int rocketnet_send_gather_buckets( int max_slots )
{
    int buckets = 1;
    while ( buckets < 2 * max_slots )
    {
        buckets *= 2;
    }
    return buckets;
}

size_t rocketnet_send_gather_scratch_bytes( int max_slots )
{
    if ( max_slots < 1 || max_slots > ( 1 << 24 ) )
    {
        return 0;
    }
    return ( (size_t) rocketnet_send_gather_buckets( max_slots ) + 3 * (size_t) max_slots ) * sizeof( int );
}

bool rocketnet_send_gather_scratch_init( struct rocketnet_send_gather_scratch_t * scratch, void * memory, int max_slots )
{
    rocketnet_assert( scratch );

    if ( !memory || max_slots < 1 || max_slots > ( 1 << 24 ) )
    {
        return false;
    }

    int * ints = (int *) memory;
    scratch->max_slots = max_slots;
    scratch->buckets = rocketnet_send_gather_buckets( max_slots );
    scratch->bucket = ints;
    scratch->head = ints + scratch->buckets;
    scratch->group = scratch->head + max_slots;
    scratch->start = scratch->group + max_slots;
    return true;
}

static inline const struct rocketnet_address_t * rocketnet_send_gather_to( const struct rocketnet_address_t * to, size_t to_stride, int slot )
{
    return (const struct rocketnet_address_t *) ( (const uint8_t *) to + (size_t) slot * to_stride );
}

static inline int rocketnet_send_gather_bytes( const int * packet_bytes, size_t bytes_stride, int slot )
{
    return *(const int *) ( (const uint8_t *) packet_bytes + (size_t) slot * bytes_stride );
}

// a destination's home bucket: FNV-1a of its address bytes and port
// (RNSendGatherLayout Hash)
int rocketnet_send_gather_home( const struct rocketnet_address_t * address, int buckets )
{
    uint64_t hash = 0xCBF29CE484222325ULL;
    const uint8_t * bytes = (const uint8_t *) &address->data;
    const int address_bytes = address->type == ROCKETNET_ADDRESS_IPV6 ? 16 : 4;
    for ( int i = 0; i < address_bytes; i++ )
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ULL;
    }
    hash ^= (uint8_t) ( address->port & 0xFF );
    hash *= 0x100000001B3ULL;
    hash ^= (uint8_t) ( address->port >> 8 );
    hash *= 0x100000001B3ULL;
    return (int) ( hash & (uint64_t) ( buckets - 1 ) );
}

int rocketnet_send_gather( const struct rocketnet_address_t * to, size_t to_stride,
                           const int * packet_bytes, size_t bytes_stride, int count,
                           int max_segments, int max_bytes,
                           struct rocketnet_send_gather_scratch_t * scratch,
                           int * order, struct rocketnet_send_gather_run_t * runs )
{
    rocketnet_assert( scratch );

    if ( count < 0 || count > scratch->max_slots || max_segments < 1 || max_bytes < 1 )
    {
        return -1;
    }

    if ( count == 0 )
    {
        return 0;
    }

    rocketnet_assert( to );
    rocketnet_assert( packet_bytes );
    rocketnet_assert( order );
    rocketnet_assert( runs );

    const int buckets = scratch->buckets;
    int * bucket = scratch->bucket;
    int * head = scratch->head;
    int * group = scratch->group;
    int * start = scratch->start;

    for ( int b = 0; b < buckets; b++ )
    {
        bucket[b] = -1;
    }

    // RNSendGatherLayout Scan and Probe: each staged slot finds its
    // destination's group by linear probing modulo the table, or takes a new
    // group, and counts itself into it

    int num_groups = 0;
    for ( int i = 0; i < count; i++ )
    {
        group[i] = -1;
        if ( rocketnet_send_gather_bytes( packet_bytes, bytes_stride, i ) <= 0 )
        {
            continue;
        }

        const struct rocketnet_address_t * address = rocketnet_send_gather_to( to, to_stride, i );
        int k = rocketnet_send_gather_home( address, buckets );
        while ( true )
        {
            const int g = bucket[k];
            if ( g == -1 )
            {
                bucket[k] = num_groups;
                head[num_groups] = i;
                start[num_groups] = 1;
                group[i] = num_groups;
                num_groups++;
                break;
            }
            if ( rocketnet_address_equal( rocketnet_send_gather_to( to, to_stride, head[g] ), address ) )
            {
                start[g]++;
                group[i] = g;
                break;
            }
            k = ( k + 1 ) & ( buckets - 1 );
        }
    }

    // RNSendGatherLayout Prefix: each group's count becomes its start

    int sum = 0;
    for ( int g = 0; g < num_groups; g++ )
    {
        const int n = start[g];
        start[g] = sum;
        sum += n;
    }

    // RNSendGatherLayout Place: each staged slot goes to its group's next
    // position, first to last, so a destination keeps its staged order; each
    // start then ends its group

#if defined( ROCKETNET_SEND_GATHER_PLANT )
    for ( int i = count - 1; i >= 0; i-- )
#else  // #if defined( ROCKETNET_SEND_GATHER_PLANT )
    for ( int i = 0; i < count; i++ )
#endif // #if defined( ROCKETNET_SEND_GATHER_PLANT )
    {
        if ( group[i] >= 0 )
        {
            order[start[group[i]]++] = i;
        }
    }

    // RNSendGatherLayout Emit and Walk: each group's range of order, groups
    // in order, is cut into runs; a packet joins the open run while the run
    // has room under both caps and the packet is no larger than its first,
    // and a smaller packet closes it

    int num_runs = 0;
    int from = 0;
    for ( int g = 0; g < num_groups; g++ )
    {
        bool closed = false;
        int run_total = 0;
        struct rocketnet_send_gather_run_t * run = NULL;

        for ( int p = from; p < start[g]; p++ )
        {
            const int bytes = rocketnet_send_gather_bytes( packet_bytes, bytes_stride, order[p] );

            const bool fits = run && !closed && run->count < max_segments && run_total + bytes <= max_bytes && bytes <= run->segment_bytes;
            if ( fits )
            {
                run->count++;
                run->last_bytes = bytes;
                run_total += bytes;
                closed = bytes < run->segment_bytes;
            }
            else
            {
                run = &runs[num_runs++];
                run->first = p;
                run->count = 1;
                run->segment_bytes = bytes;
                run->last_bytes = bytes;
                run_total = bytes;
                closed = false;
            }
        }

        from = start[g];
    }

    return num_runs;
}
