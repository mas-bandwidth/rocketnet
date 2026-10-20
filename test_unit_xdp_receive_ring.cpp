/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The AF_XDP receive ring's peek, cancel and release, over a fake consumer
// ring. No socket, no kernel object, no clock. Peek adds the returned count
// to cached_cons, cancel subtracts from cached_cons, and release adds to the
// published consumer index. That is the bookkeeping in
// rocketnet_server_socket_xdp_thread_function. The pass peeks more
// descriptors than the staging buffer can take and cancels the tail it
// does not consume.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"

struct fake_xdp_cons_ring
{
    uint32_t producer;
    uint32_t consumer;
    uint32_t cached_prod;
    uint32_t cached_cons;
    uint32_t mask;
    uint64_t desc[8];
};

// The pass cancels an unconsumed peek before release, as the receive
// loop does. Peek has already added the whole count to cached_cons.

static uint32_t cons_peek( fake_xdp_cons_ring * ring, uint32_t nb, uint32_t * idx )
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
        *idx = ring->cached_cons;
        ring->cached_cons += entries;
    }
    return entries;
}

static void cons_cancel( fake_xdp_cons_ring * ring, uint32_t nb )
{
    ring->cached_cons -= nb;
}

static void cons_release( fake_xdp_cons_ring * ring, uint32_t nb )
{
    ring->consumer += nb;
}

static void ring_place( fake_xdp_cons_ring * ring, uint32_t consumer, const uint64_t * frames, uint32_t n )
{
    memset( ring, 0, sizeof( *ring ) );
    ring->mask = 7;
    ring->consumer = consumer;
    ring->cached_cons = consumer;
    ring->cached_prod = consumer;
    ring->producer = consumer + n;
    for ( uint32_t i = 0; i < n; i++ )
        ring->desc[( consumer + i ) & ring->mask] = frames[i];
}

// One receive pass. The peek asks for more descriptors than the staging
// buffer holds. The loop stops when the staging buffer is full. Cancel
// returns the unconsumed tail before release.
static void receive_pass( fake_xdp_cons_ring * ring, uint32_t staging_count, uint32_t staging_cap,
                          uint32_t * peeked_out, uint32_t * consumed_out, uint32_t * idx_out )
{
    uint32_t idx = 0;
    const uint32_t peeked = cons_peek( ring, 32, &idx );
    uint32_t consumed = 0;
    for ( uint32_t i = 0; i < peeked; i++ )
    {
        if ( staging_count + consumed >= staging_cap )
            break;
        consumed++;
    }
    if ( peeked > consumed )
        cons_cancel( ring, peeked - consumed );
    cons_release( ring, consumed );
    *peeked_out = peeked;
    *consumed_out = consumed;
    *idx_out = idx;
}

static void xdp_receive_ring_returns_unconsumed_when_staging_is_full()
{
    const uint64_t frames[4] = { 0x1000, 0x2000, 0x3000, 0x4000 };
    fake_xdp_cons_ring ring;
    ring_place( &ring, 0, frames, 4 );

    uint32_t peeked = 0;
    uint32_t consumed = 0;
    uint32_t idx = 0;
    receive_pass( &ring, 1, 1, &peeked, &consumed, &idx );
    rocketnet_unit_check( peeked == 4 );
    rocketnet_unit_check( consumed == 0 );
    rocketnet_unit_check( ring.consumer == 0 );

    const uint32_t lag = ring.cached_cons - ring.consumer;
    uint32_t idx2 = 0;
    const uint32_t again = cons_peek( &ring, 32, &idx2 );
    rocketnet_unit_check( lag == 0 && again == peeked );
    rocketnet_unit_check( ( idx2 & ring.mask ) == 0 );
    rocketnet_unit_check( ring.desc[idx2 & ring.mask] == frames[0] );
    rocketnet_unit_check( ring.desc[( idx2 + 3 ) & ring.mask] == frames[3] );
}

static void xdp_receive_ring_next_peek_sees_the_skipped_tail()
{
    const uint64_t frames[4] = { 0x1000, 0x2000, 0x3000, 0x4000 };
    fake_xdp_cons_ring ring;
    ring_place( &ring, 0, frames, 4 );

    uint32_t peeked = 0;
    uint32_t consumed = 0;
    uint32_t idx = 0;
    receive_pass( &ring, 0, 1, &peeked, &consumed, &idx );
    rocketnet_unit_check( peeked == 4 );
    rocketnet_unit_check( consumed == 1 );
    rocketnet_unit_check( ring.consumer == 1 );

    const uint32_t lag = ring.cached_cons - ring.consumer;
    uint32_t idx2 = 0;
    const uint32_t again = cons_peek( &ring, 32, &idx2 );
    rocketnet_unit_check( lag == 0 && again == peeked - consumed );
    rocketnet_unit_check( ring.desc[idx2 & ring.mask] == frames[1] );
    rocketnet_unit_check( ring.desc[( idx2 + 1 ) & ring.mask] == frames[2] );
    rocketnet_unit_check( ring.desc[( idx2 + 2 ) & ring.mask] == frames[3] );
}

static void xdp_receive_ring_returns_a_wrapped_tail()
{
    const uint64_t frames[4] = { 0xA, 0xB, 0xC, 0xD };
    fake_xdp_cons_ring ring;
    ring_place( &ring, 6, frames, 4 );

    uint32_t peeked = 0;
    uint32_t consumed = 0;
    uint32_t idx = 0;
    receive_pass( &ring, 0, 1, &peeked, &consumed, &idx );
    rocketnet_unit_check( peeked == 4 );
    rocketnet_unit_check( consumed == 1 );
    rocketnet_unit_check( ring.consumer == 7 );

    const uint32_t lag = ring.cached_cons - ring.consumer;
    uint32_t idx2 = 0;
    const uint32_t again = cons_peek( &ring, 32, &idx2 );
    rocketnet_unit_check( lag == 0 && again == peeked - consumed );
    rocketnet_unit_check( ring.desc[idx2 & ring.mask] == frames[1] );
    rocketnet_unit_check( ring.desc[( idx2 + 1 ) & ring.mask] == frames[2] );
    rocketnet_unit_check( ring.desc[( idx2 + 2 ) & ring.mask] == frames[3] );
}

REGISTER_UNIT_TEST( xdp_receive_ring_returns_unconsumed_when_staging_is_full );
REGISTER_UNIT_TEST( xdp_receive_ring_next_peek_sees_the_skipped_tail );
REGISTER_UNIT_TEST( xdp_receive_ring_returns_a_wrapped_tail );
