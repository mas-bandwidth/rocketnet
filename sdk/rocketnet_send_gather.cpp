/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_send_gather.h"
#include "rocketnet_internal.h"

#include <string.h>

// proofs/tla/RNSendGatherLayout.tla models the gather at this layout: the
// scan's open-addressed table probed modulo its size, each destination's
// chain appended at its tail, and the emit's runs cut by the segment limit,
// the byte cap and a smaller packet. Each step cites the action it is.
// ROCKETNET_SEND_GATHER_PLANT is the trace check's plant (a chain appended
// after its head, reversing a destination's later packets): only
// tla_trace_plant defines it, and its traces must fail TraceConsistent.

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
    return ( (size_t) rocketnet_send_gather_buckets( max_slots ) + 2 * (size_t) max_slots ) * sizeof( int );
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
    scratch->next = ints + scratch->buckets;
    scratch->tail = scratch->next + max_slots;
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
    int * next = scratch->next;
    int * tail = scratch->tail;

    for ( int b = 0; b < buckets; b++ )
    {
        bucket[b] = -1;
    }
    for ( int s = 0; s < count; s++ )
    {
        next[s] = -1;
        tail[s] = -1;
    }

    // RNSendGatherLayout Scan and Probe: each staged slot finds its
    // destination's bucket by linear probing modulo the table, and either
    // becomes the destination's head or joins its chain at the tail

    for ( int i = 0; i < count; i++ )
    {
        if ( rocketnet_send_gather_bytes( packet_bytes, bytes_stride, i ) <= 0 )
        {
            continue;
        }

        const struct rocketnet_address_t * address = rocketnet_send_gather_to( to, to_stride, i );
        int k = rocketnet_send_gather_home( address, buckets );
        while ( true )
        {
            const int h = bucket[k];
            if ( h == -1 )
            {
                bucket[k] = i;
                tail[i] = i;
                break;
            }
            if ( rocketnet_address_equal( rocketnet_send_gather_to( to, to_stride, h ), address ) )
            {
#if defined( ROCKETNET_SEND_GATHER_PLANT )
                next[i] = next[h];
                next[h] = i;
#else  // #if defined( ROCKETNET_SEND_GATHER_PLANT )
                next[tail[h]] = i;
                tail[h] = i;
#endif // #if defined( ROCKETNET_SEND_GATHER_PLANT )
                break;
            }
            k = ( k + 1 ) & ( buckets - 1 );
        }
    }

    // RNSendGatherLayout Emit and Walk: each head's chain, in slot order of
    // the heads, goes to order; a packet joins the open run while the run
    // has room under both caps and the packet is no larger than its first,
    // and a smaller packet closes it

    int num_order = 0;
    int num_runs = 0;

    for ( int i = 0; i < count; i++ )
    {
        if ( tail[i] == -1 )
        {
            continue;
        }

        bool open = false;
        bool closed = false;
        int run_total = 0;
        struct rocketnet_send_gather_run_t * run = NULL;

        for ( int c = i; c != -1; c = next[c] )
        {
            const int bytes = rocketnet_send_gather_bytes( packet_bytes, bytes_stride, c );

            const bool fits = open && !closed && run->count < max_segments && run_total + bytes <= max_bytes && bytes <= run->segment_bytes;
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
                run->first = num_order;
                run->count = 1;
                run->segment_bytes = bytes;
                run->last_bytes = bytes;
                run_total = bytes;
                open = true;
                closed = false;
            }

            order[num_order++] = c;
        }
    }

    return num_runs;
}
