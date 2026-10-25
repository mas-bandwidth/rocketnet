/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP receive pass over its RX ring (rocketnet_xdp_receive_ring_finish,
// sdk/rocketnet_server_socket_linux.cpp), driven over a fake consumer ring: a
// plain struct with the producer, consumer and cached indices and the same
// index arithmetic as libxdp's xsk_ring_cons__peek, __cancel and __release
// (<xdp/xsk.h>). No socket, no kernel object, no clock. The frame ownership
// these cases check is modelled in tla/RNXdpReceiveRing.tla.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"

// the AF_XDP code is Linux only and compiled out of a build without XDP, so
// its tests are built only where it is
#if defined( __linux__ ) && ROCKETNET_XDP

void rocketnet_xdp_receive_ring_finish( uint32_t num_packets, uint32_t num_consumed,
                                        void ( *cancel )( void * ring, uint32_t count ),
                                        void ( *release )( void * ring, uint32_t count ),
                                        void * ring );

namespace
{
    const uint32_t FAKE_RING_SIZE = 8;          // a power of two, as the kernel's rings are
    const uint32_t FAKE_UMEM_FRAMES = 16;

    // The user side reads producer and writes consumer; the kernel side the
    // reverse. cached_prod and cached_cons are the user's private copies, as
    // in struct xsk_ring_cons.
    struct fake_rx_ring_t
    {
        uint32_t producer;
        uint32_t consumer;
        uint32_t cached_prod;
        uint32_t cached_cons;
        uint64_t addr[FAKE_RING_SIZE];
    };

    // xsk_ring_cons__peek: refresh cached_prod when the cached view is empty,
    // hand out at most nb from cached_cons and advance cached_cons past them.
    uint32_t fake_peek( fake_rx_ring_t * ring, uint32_t nb, uint32_t * index )
    {
        uint32_t entries = ring->cached_prod - ring->cached_cons;
        if ( entries == 0 )
        {
            ring->cached_prod = ring->producer;
            entries = ring->cached_prod - ring->cached_cons;
        }
        if ( entries > nb )
            entries = nb;
        if ( entries > 0 )
        {
            *index = ring->cached_cons;
            ring->cached_cons += entries;
        }
        return entries;
    }

    // xsk_ring_cons__cancel
    void fake_cancel( void * r, uint32_t nb ) { ( (fake_rx_ring_t *) r )->cached_cons -= nb; }

    // xsk_ring_cons__release
    void fake_release( void * r, uint32_t nb ) { ( (fake_rx_ring_t *) r )->consumer += nb; }

    // the kernel: post one received frame if the ring has a free slot, as it
    // sees the ring (producer - consumer < size)
    bool fake_kernel_post( fake_rx_ring_t * ring, uint64_t addr )
    {
        if ( ring->producer - ring->consumer >= FAKE_RING_SIZE )
            return false;
        ring->addr[ring->producer & ( FAKE_RING_SIZE - 1 )] = addr;
        ring->producer++;
        return true;
    }

    // one receive pass as the XDP worker runs it: peek, take while the
    // staging buffer has room, cancel unconsumed and release what was taken
    uint32_t receive_pass( fake_rx_ring_t * ring, uint32_t peek_max, uint32_t staging_room, uint64_t * taken_addr )
    {
        uint32_t index = 0;
        const uint32_t peeked = fake_peek( ring, peek_max, &index );
        if ( peeked == 0 )
            return 0;
        uint32_t taken = 0;
        for ( uint32_t i = 0; i < peeked; i++ )
        {
            if ( taken >= staging_room )
                break;
            taken_addr[taken] = ring->addr[( index + i ) & ( FAKE_RING_SIZE - 1 )];
            taken++;
        }
        rocketnet_xdp_receive_ring_finish( peeked, taken, fake_cancel, fake_release, ring );
        return taken;
    }
}

// The staging buffer has room for 3 of the 8 descriptors a peek returns.
// The 5 not taken go back to the ring: the consumer index does not lag the
// cached one, and the next peek returns exactly those 5, in order.
static void test_xdp_transport_receive_ring_returns_descriptors_the_staging_buffer_cannot_take()
{
    fake_rx_ring_t ring;
    memset( &ring, 0, sizeof( ring ) );
    for ( uint32_t i = 0; i < FAKE_RING_SIZE; i++ )
        rocketnet_unit_check( fake_kernel_post( &ring, 100 + i ) );

    uint64_t taken[FAKE_RING_SIZE];
    memset( taken, 0, sizeof( taken ) );
    const uint32_t first = receive_pass( &ring, FAKE_RING_SIZE, 3, taken );
    rocketnet_unit_check( first == 3 );
    rocketnet_unit_check( taken[0] == 100 && taken[1] == 101 && taken[2] == 102 );

    // every peeked descriptor is consumed or returned: the user's cached
    // consumer is the consumer the kernel reads
    rocketnet_unit_check( ring.cached_cons == ring.consumer );
    rocketnet_unit_check( ring.producer - ring.consumer == FAKE_RING_SIZE - first );

    // the next peek sees the skipped descriptors first
    uint32_t index = 0;
    const uint32_t again = fake_peek( &ring, FAKE_RING_SIZE, &index );
    rocketnet_unit_check( again == FAKE_RING_SIZE - first );
    rocketnet_unit_check( index == first );
    for ( uint32_t i = 0; i < again; i++ )
        rocketnet_unit_check( ring.addr[( index + i ) & ( FAKE_RING_SIZE - 1 )] == 103 + i );
}

// A full staging buffer takes nothing: every peeked descriptor goes back,
// and the ring is exactly as it was before the peek.
static void test_xdp_transport_receive_ring_full_staging_returns_every_descriptor()
{
    fake_rx_ring_t ring;
    memset( &ring, 0, sizeof( ring ) );
    for ( uint32_t i = 0; i < 4; i++ )
        rocketnet_unit_check( fake_kernel_post( &ring, 200 + i ) );

    uint64_t taken[FAKE_RING_SIZE];
    rocketnet_unit_check( receive_pass( &ring, FAKE_RING_SIZE, 0, taken ) == 0 );
    rocketnet_unit_check( ring.cached_cons == 0 && ring.consumer == 0 );

    rocketnet_unit_check( receive_pass( &ring, FAKE_RING_SIZE, FAKE_RING_SIZE, taken ) == 4 );
    for ( uint32_t i = 0; i < 4; i++ )
        rocketnet_unit_check( taken[i] == 200 + i );
    rocketnet_unit_check( ring.cached_cons == ring.consumer && ring.consumer == 4 );
}

// Frame conservation over many passes: a UMEM of 16 frames, a fill queue the
// kernel takes frames from, the RX ring, and a staging buffer of 3 that the
// application drains every other pass. A taken frame goes straight back to
// the fill queue (the worker's pending return). Every frame is in exactly
// one of the fill queue, the RX ring or the worker's hands, and receive
// never stalls while frames are free: every pass with staging room takes.
static void test_xdp_transport_receive_ring_conserves_frames_and_never_stalls()
{
    fake_rx_ring_t ring;
    memset( &ring, 0, sizeof( ring ) );

    uint64_t fill[FAKE_UMEM_FRAMES];
    uint32_t fill_count = 0;
    for ( uint32_t i = 0; i < FAKE_UMEM_FRAMES; i++ )
        fill[fill_count++] = i;

    const uint32_t staging_cap = 3;
    uint32_t staged = 0;
    uint64_t received = 0;

    for ( int pass = 0; pass < 64; pass++ )
    {
        // the kernel fills the RX ring from the fill queue
        while ( fill_count > 0 && fake_kernel_post( &ring, fill[fill_count - 1] ) )
            fill_count--;

        // the application drains the staging buffer every other pass
        if ( pass % 2 == 1 )
            staged = 0;

        uint32_t index = 0;
        const uint32_t peeked = fake_peek( &ring, FAKE_RING_SIZE, &index );
        uint64_t taken[FAKE_RING_SIZE];
        const uint32_t room = staging_cap - staged;
        uint32_t took = 0;
        for ( uint32_t i = 0; i < peeked; i++ )
        {
            if ( staged >= staging_cap )
                break;
            taken[took] = ring.addr[( index + i ) & ( FAKE_RING_SIZE - 1 )];
            took++;
            staged++;
        }
        rocketnet_xdp_receive_ring_finish( peeked, took, fake_cancel, fake_release, &ring );
        for ( uint32_t i = 0; i < took; i++ )
            fill[fill_count++] = taken[i];
        received += took;

        // every frame has exactly one owner: the fill queue or the RX ring
        // as the kernel sees it (producer - consumer); none is lost
        rocketnet_unit_check( ring.cached_cons == ring.consumer );
        rocketnet_unit_check( fill_count + ( ring.producer - ring.consumer ) == FAKE_UMEM_FRAMES );

        // liveness: a pass with staging room takes while the ring holds frames
        rocketnet_unit_check( took == ( room < peeked ? room : peeked ) );
        rocketnet_unit_check( peeked > 0 || room == 0 );
    }

    rocketnet_unit_check( received >= 64 );
}

REGISTER_UNIT_TEST( test_xdp_transport_receive_ring_returns_descriptors_the_staging_buffer_cannot_take );
REGISTER_UNIT_TEST( test_xdp_transport_receive_ring_full_staging_returns_every_descriptor );
REGISTER_UNIT_TEST( test_xdp_transport_receive_ring_conserves_frames_and_never_stalls );

#endif // #if defined(__linux__) && ROCKETNET_XDP
