/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP fill ring and the XDP worker's pending return
// (rocketnet_pending_frames_t, sdk/rocketnet_pending_frames.h), driven over a
// fake fill ring: a plain struct with the producer and consumer indices, the
// user's cached copies with the same arithmetic as libxdp's
// xsk_ring_prod__reserve and __submit (<xdp/xsk.h>), and the kernel's cached
// copies with the same arithmetic as its fill ring reads
// (xskq_cons_peek_addr_unchecked, net/xdp/xsk_queue.h): the kernel caches a
// batch and publishes its consumer index only when the batch is used up. No
// socket, no kernel object, no clock. Modelled in tla/RNXdpReceiveRing.tla.
//
// The ring of twice the frames (ROCKETNET_XDP_FILL_RING_SIZE) is the design;
// the ring of exactly the frames is the reversed witness, the shape the
// server had when the Linux x64 XDP bench counted "rx fill ring empty" drops on one queue.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"
#include "rocketnet_pending_frames.h"

namespace
{
    const uint32_t FILL_FRAMES = 8;
    const uint32_t FILL_RING_MAX = 2 * FILL_FRAMES;

    struct fake_fill_ring_t
    {
        uint32_t size;              // a power of two, as the kernel's rings are
        uint32_t producer;          // written by the user
        uint32_t consumer;          // written by the kernel, only when its batch is used up
        uint32_t user_cached_prod;
        uint32_t user_cached_cons;  // the user's view of consumer + size
        uint32_t kernel_cached_prod;
        uint32_t kernel_cached_cons;
        uint32_t empty;             // "rx fill ring empty": the kernel wanted a frame and found none
        uint64_t addr[FILL_RING_MAX];
    };

    void fake_fill_ring_init( fake_fill_ring_t * ring, uint32_t size )
    {
        memset( ring, 0, sizeof( *ring ) );
        ring->size = size;
    }

    // xsk_ring_prod__reserve: all of nb or zero, refreshing the cached
    // consumer from the published one when the cached view is short
    uint32_t fake_reserve( fake_fill_ring_t * ring, uint32_t nb, uint32_t * index )
    {
        uint32_t free_entries = ring->user_cached_cons - ring->user_cached_prod;
        if ( free_entries < nb )
        {
            ring->user_cached_cons = ring->consumer + ring->size;
            free_entries = ring->user_cached_cons - ring->user_cached_prod;
        }
        if ( free_entries < nb )
            return 0;
        *index = ring->user_cached_prod;
        ring->user_cached_prod += nb;
        return nb;
    }

    // the kernel takes one frame for a packet: when its cached batch is used
    // up it publishes the consumer and caches what the producer shows now
    bool fake_kernel_take( fake_fill_ring_t * ring, uint64_t * frame )
    {
        if ( ring->kernel_cached_prod == ring->kernel_cached_cons )
        {
            ring->consumer = ring->kernel_cached_cons;
            ring->kernel_cached_prod = ring->producer;
        }
        if ( ring->kernel_cached_prod == ring->kernel_cached_cons )
        {
            ring->empty++;
            return false;
        }
        *frame = ring->addr[ring->kernel_cached_cons & ( ring->size - 1 )];
        ring->kernel_cached_cons++;
        return true;
    }

    bool flush( rocketnet_pending_frames_t<FILL_FRAMES> * pending, fake_fill_ring_t * ring )
    {
        return pending->flush(
            [&]( uint32_t count, uint32_t * index )
            { return fake_reserve( ring, count, index ); },
            [&]( uint32_t index, uint64_t frame )
            { ring->addr[index & ( ring->size - 1 )] = frame; },
            [&]( uint32_t count )
            { ring->producer += count; } );
    }

    // the server's start: every receive frame on the fill ring
    void populate( fake_fill_ring_t * ring )
    {
        uint32_t index = 0;
        rocketnet_unit_check( fake_reserve( ring, FILL_FRAMES, &index ) == FILL_FRAMES );
        for ( uint32_t i = 0; i < FILL_FRAMES; i++ )
            ring->addr[( index + i ) & ( ring->size - 1 )] = 1000 + i;
        ring->producer += FILL_FRAMES;
    }

    // a burst of packets lands, then one worker pass takes every one of them
    // from the RX ring and hands their frames back through the pending return.
    // returns whether the pass handed every frame back
    bool burst_then_pass( fake_fill_ring_t * ring, rocketnet_pending_frames_t<FILL_FRAMES> * pending, uint32_t burst, uint32_t * received )
    {
        uint64_t rx[FILL_FRAMES];
        uint32_t rx_count = 0;
        for ( uint32_t i = 0; i < burst; i++ )
        {
            uint64_t frame = 0;
            if ( fake_kernel_take( ring, &frame ) )
            {
                rocketnet_unit_check( rx_count < FILL_FRAMES );
                rx[rx_count++] = frame;
            }
        }
        *received += rx_count;
        rocketnet_unit_check( pending->append( rx, rx_count ) );
        flush( pending, ring );
        return pending->count == 0;
    }

    // every frame has one owner: the fill ring's entries the kernel has not
    // taken, or the worker's pending return (each pass empties the RX ring)
    void check_conservation( const fake_fill_ring_t * ring, const rocketnet_pending_frames_t<FILL_FRAMES> * pending )
    {
        rocketnet_unit_check( ( ring->producer - ring->kernel_cached_cons ) + pending->count == FILL_FRAMES );
        rocketnet_unit_check( ring->producer - ring->consumer <= ring->size );
    }
}

// A ring of twice the frames: whatever bursts land, each pass hands every
// frame back at once, and the kernel never finds the fill ring empty while a
// burst is no larger than the frames.
static void test_xdp_transport_fill_ring_of_twice_the_frames_never_runs_empty()
{
    fake_fill_ring_t ring;
    fake_fill_ring_init( &ring, 2 * FILL_FRAMES );
    populate( &ring );

    rocketnet_pending_frames_t<FILL_FRAMES> pending;
    uint32_t received = 0;
    uint32_t offered = 0;
    for ( uint32_t step = 0; step < 4096; step++ )
    {
        // every burst size from 1 to all the frames, in a scrambled order
        const uint32_t burst = 1 + ( step * 5 + step / 7 ) % FILL_FRAMES;
        offered += burst;
        rocketnet_unit_check( burst_then_pass( &ring, &pending, burst, &received ) );
        check_conservation( &ring, &pending );
    }

    rocketnet_unit_check( ring.empty == 0 );
    rocketnet_unit_check( received == offered );
}

// Reversed witness: a ring of exactly the frames. A frame the kernel took
// keeps its fill ring slot until the kernel publishes at the end of its
// batch, so the worker cannot hand back a single frame before then; when the
// kernel ends the batch it finds nothing new and drops the packet, while the
// worker holds every frame.
static void test_xdp_transport_fill_ring_of_the_frames_runs_empty_holding_every_frame()
{
    fake_fill_ring_t ring;
    fake_fill_ring_init( &ring, FILL_FRAMES );
    populate( &ring );

    rocketnet_pending_frames_t<FILL_FRAMES> pending;
    uint32_t received = 0;

    rocketnet_unit_check( !burst_then_pass( &ring, &pending, 3, &received ) );
    rocketnet_unit_check( pending.count == 3 );
    rocketnet_unit_check( !burst_then_pass( &ring, &pending, FILL_FRAMES - 3, &received ) );
    rocketnet_unit_check( pending.count == FILL_FRAMES );
    check_conservation( &ring, &pending );

    // the next packet ends the kernel's batch: the consumer is published, the
    // ring holds no frame, and the packet is dropped
    rocketnet_unit_check( burst_then_pass( &ring, &pending, 1, &received ) );
    rocketnet_unit_check( ring.empty == 1 );
    rocketnet_unit_check( received == FILL_FRAMES );
    check_conservation( &ring, &pending );
}

REGISTER_UNIT_TEST( test_xdp_transport_fill_ring_of_twice_the_frames_never_runs_empty );
REGISTER_UNIT_TEST( test_xdp_transport_fill_ring_of_the_frames_runs_empty_holding_every_frame );
