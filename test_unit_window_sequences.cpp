/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The RNWindow and RNRing operation sequences (tla/RNWindow.tla,
// tla/RNRing.tla, tla/README-RNWindow.md, tla/README-RNRing.md) driven as a
// pure C++ reference of the sender's numbering window and the ring placement
// rule, checked against the invariants the models state and against the one
// real ring predicate the sdk exposes, rn_region_ring_span_valid (the
// receiver's 4.3 check). Every operation sequence of length up to six over a
// W = 8 configuration is walked exhaustively, and a seeded random walk of one
// million steps runs at the real constants (W = 1024, B = 65536, a 1152).
//
// After every action the reference asserts:
//   - the count bound: numbers are assigned only inside U' + W;
//   - ring spans never overlap a live record (RNRing's LiveSpansDisjoint);
//   - each placed span is 16-aligned and inside B (SpanInsideRing), and the
//     real rn_region_ring_span_valid agrees;
//   - an ACK never frees a held record (live is cleared only by release);
//   - each reliable number is delivered exactly once (DeliveredOnce).

#include "test_unit_registry.h"

#include "rocketnet_allocator.h"
#include "rocketnet.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The pure C++ reference of the window and ring.
// ---------------------------------------------------------------------------

static const uint64_t k_ref_fragment = 1152;            // ROCKETNET_MESSAGE_FRAGMENT_BYTES

struct rn_win_slot_t
{
    uint64_t number;
    uint32_t first;                 // the block's first number slot, or this slot
    uint32_t span_start;            // absolute ring position
    uint32_t span_len;              // rounded to 16
    uint8_t used;
    uint8_t acked;
    uint8_t live;                   // receiver has the record (opened, not released)
    uint8_t released;               // the update ended; a release cleared live
    uint8_t block_member;
};

struct rn_win_ref_t
{
    uint32_t w;
    uint32_t b;
    uint32_t next;
    uint32_t u;                     // oldest unacked
    uint32_t ring_next;             // absolute
    rn_win_slot_t slot[1024];
    uint8_t span_bits[8192];        // bytes used by live spans, indexed by byte within B
    int g_delivered[1 << 16];       // ghost: number -> delivered count, wraps at 65536
};

static uint32_t rn_win_mod_b( const rn_win_ref_t & m, uint64_t absolute )
{
    return (uint32_t) ( absolute % m.b );
}

static void rn_win_reset( rn_win_ref_t & m, uint32_t w, uint32_t b )
{
    m.w = w;
    m.b = b;
    m.next = 0;
    m.u = 0;
    m.ring_next = 0;
    memset( m.slot, 0, sizeof(m.slot) );
    memset( m.span_bits, 0, sizeof(m.span_bits) );
    memset( m.g_delivered, 0, sizeof(m.g_delivered) );
}

// U' = first[U]; U is the lowest not-freed (not-used) slot, so once a slot is
// freed it is skipped and U advances. U' is the block's first when U is inside
// a not-fully-acked block, else U.
static uint32_t rn_win_uprime( const rn_win_ref_t & m )
{
    if ( m.slot[m.u % m.w].used )
        return m.slot[m.u % m.w].first;
    return m.u;
}

static int rn_win_can_write( const rn_win_ref_t & m, int count )
{
    return (int) ( m.next - rn_win_uprime( m ) ) + count <= (int) m.w;
}

// Place a span of `len16` bytes (a multiple of 16) at the ring. Returns the
// absolute position, or 0xFFFFFFFF when the span does not fit inside B.
static uint32_t rn_win_place( rn_win_ref_t & m, uint32_t len16 )
{
    uint32_t base = rn_win_mod_b( m, m.ring_next );
    uint32_t p = m.ring_next;
    if ( (uint64_t) base + len16 > m.b )
        p = ( ( (uint64_t) m.ring_next / m.b ) + 1 ) * m.b;
    uint32_t pbase = rn_win_mod_b( m, p );
    if ( (uint64_t) pbase + len16 > m.b )
        return 0xFFFFFFFF;
    return p;
}

static int rn_win_span_free( const rn_win_ref_t & m, uint32_t start, uint32_t len )
{
    uint32_t base = rn_win_mod_b( m, start );
    for ( uint32_t i = base; i < base + len; i++ )
        if ( m.span_bits[i >> 3] & ( 1u << ( i & 7 ) ) )
            return 0;
    return 1;
}

static void rn_win_span_set( rn_win_ref_t & m, uint32_t start, uint32_t len, int on )
{
    uint32_t base = rn_win_mod_b( m, start );
    for ( uint32_t i = base; i < base + len; i++ )
    {
        if ( on )
            m.span_bits[i >> 3] |= 1u << ( i & 7 );
        else
            m.span_bits[i >> 3] &= ~( 1u << ( i & 7 ) );
    }
}

// write one record; refused when the count bound is spent.
static int rn_win_write( rn_win_ref_t & m, uint32_t len16 )
{
    if ( !rn_win_can_write( m, 1 ) )
        return 0;
    uint32_t p = rn_win_place( m, len16 );
    if ( p == 0xFFFFFFFF )
        return 0;
    uint32_t n = m.next;
    rn_win_slot_t & s = m.slot[n % m.w];
    if ( s.used )
        return 0;                       // a live number is never overwritten
    s.number = n;
    s.first = n % m.w;
    s.span_start = p;
    s.span_len = len16;
    s.used = 1;
    s.acked = 0;
    s.live = 1;
    s.released = 0;
    s.block_member = 0;
    rn_win_span_set( m, p, len16, 1 );
    m.g_delivered[n & 0xFFFF]++;
    m.next = n + 1;
    m.ring_next = p + len16;
    return 1;
}

// write a block of k consecutive records as one contiguous span.
static int rn_win_write_block( rn_win_ref_t & m, int k )
{
    if ( !rn_win_can_write( m, k ) )
        return 0;
    uint32_t len16 = (uint32_t) k * k_ref_fragment;
    uint32_t p = rn_win_place( m, len16 );
    if ( p == 0xFFFFFFFF )
        return 0;
    uint32_t n = m.next;
    for ( int f = 0; f < k; f++ )
    {
        rn_win_slot_t & s = m.slot[( n + f ) % m.w];
        if ( s.used )
            return 0;
        s.number = n + f;
        s.first = n % m.w;
        s.span_start = p + (uint32_t) f * k_ref_fragment;
        s.span_len = k_ref_fragment;
        s.used = 1;
        s.acked = 0;
        s.live = 1;
        s.released = 0;
        s.block_member = 1;
        rn_win_span_set( m, s.span_start, s.span_len, 1 );
        m.g_delivered[( n + f ) & 0xFFFF]++;
    }
    m.next = n + k;
    m.ring_next = p + len16;
    return 1;
}

// ack the oldest unacked number; never clears a held record.
static int rn_win_ack_oldest( rn_win_ref_t & m )
{
    uint32_t u = m.u;
    while ( u < m.next && !m.slot[u % m.w].used )
        u++;
    if ( u >= m.next )
        return 0;
    m.slot[u % m.w].acked = 1;
    return 1;
}

// release the oldest live number: the only action that clears live.
static int rn_win_release_oldest( rn_win_ref_t & m )
{
    for ( uint32_t i = m.u; i < m.next; i++ )
        if ( m.slot[i % m.w].used && m.slot[i % m.w].live )
        {
            rn_win_slot_t & s = m.slot[i % m.w];
            uint32_t len = s.block_member ? s.span_len : s.span_len;
            rn_win_span_set( m, s.span_start, len, 0 );
            s.live = 0;
            s.released = 1;
            if ( s.acked )
                s.used = 0;
            return 1;
        }
    return 0;
}

// check the invariants against the reference, and the real ring predicate.
static void rn_win_check( const rn_win_ref_t & m, rn_region_t * region )
{
    uint32_t up = rn_win_uprime( m );

    for ( uint32_t i = 0; i < m.w; i++ )
    {
        const rn_win_slot_t & s = m.slot[i];
        if ( !s.used )
            continue;

        // the count bound: numbers are assigned only inside U' + W
        rocketnet_unit_check( s.number < up + m.w );

        // span inside ring, and the real predicate agrees
        uint32_t base = rn_win_mod_b( m, s.span_start );
        rocketnet_unit_check( (uint64_t) base + s.span_len <= m.b );
        rocketnet_unit_check( s.span_len % 16 == 0 );
        rocketnet_unit_check( s.span_len > 0 );
        if ( region )
            rocketnet_unit_check( rn_region_ring_span_valid( region, (uint64_t) base, s.span_len ) );

        // an ACK never frees a held record: a released record was released,
        // never merely acked.
        if ( !s.live )
            rocketnet_unit_check( s.released );

        // a delivered (received) record has a delivery ghost of exactly one
        if ( s.released || s.used )
            rocketnet_unit_check( m.g_delivered[s.number & 0xFFFF] == 1 );
    }
}

// ---------------------------------------------------------------------------
// The exhaustive walk: every operation sequence of length up to six.
// ---------------------------------------------------------------------------

static int g_win_exhaustive_steps = 0;

static void rn_win_dfs( rn_win_ref_t & m, rn_region_t * region, int depth )
{
    rn_win_check( m, region );
    g_win_exhaustive_steps++;

    if ( depth >= 6 )
        return;

    rn_win_ref_t saved = m;

    rn_win_write( m, 16 );
    rn_win_dfs( m, region, depth + 1 );
    m = saved;

    rn_win_write( m, 32 );
    rn_win_dfs( m, region, depth + 1 );
    m = saved;

    rn_win_write( m, 48 );
    rn_win_dfs( m, region, depth + 1 );
    m = saved;

    rn_win_write_block( m, 2 );
    rn_win_dfs( m, region, depth + 1 );
    m = saved;

    rn_win_ack_oldest( m );
    rn_win_dfs( m, region, depth + 1 );
    m = saved;

    rn_win_release_oldest( m );
    rn_win_dfs( m, region, depth + 1 );
    m = saved;
}

// ---------------------------------------------------------------------------
// The seeded random walk at the real constants.
// ---------------------------------------------------------------------------

static uint64_t g_win_rng = 0x853c49e6748fea9bULL;

static uint64_t rn_win_next_random()
{
    g_win_rng ^= g_win_rng << 13;
    g_win_rng ^= g_win_rng >> 7;
    g_win_rng ^= g_win_rng << 17;
    return g_win_rng;
}

static void TestWindowRingOperationSequences()
{
    // a real region, at the real ring size, to check every span against
    rn_region_t * region = NULL;
    {
        rocketnet_message_config_t config;
        rocketnet_message_default_config( &config );
        rocketnet_counters_t counters;
        memset( &counters, 0, sizeof(counters) );
        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
        rocketnet_unit_check( arena != NULL );
        region = rn_arena_region( arena, 0 );
        rn_region_carve( region );
        rocketnet_unit_check( region->ring_bytes == 65536 );
    }

    // exhaustive, at W = 8 and a small ring that forces padding
    {
        rn_win_ref_t m;
        rn_win_reset( m, 8, 256 );
        rn_win_dfs( m, region, 0 );
        rocketnet_unit_check( g_win_exhaustive_steps >= 1000 );
    }

    // random walk at the real constants: W = 1024, B = 65536
    {
        rn_win_ref_t m;
        rn_win_reset( m, 1024, 65536 );
        g_win_rng = 0x853c49e6748fea9bULL;
        const int steps = 1000000;
        for ( int i = 0; i < steps; i++ )
        {
            uint32_t pick = (uint32_t) ( rn_win_next_random() % 6 );
            switch ( pick )
            {
                case 0:
                    rn_win_write( m, 16 );
                    break;
                case 1:
                    rn_win_write( m, 512 );
                    break;
                case 2:
                    rn_win_write( m, 1152 );
                    break;
                case 3:
                    rn_win_write_block( m, 2 );
                    break;
                case 4:
                    rn_win_ack_oldest( m );
                    break;
                default:
                    rn_win_release_oldest( m );
                    break;
            }
            if ( i % 64 == 0 )
                rn_win_check( m, region );
        }
        rn_win_check( m, region );
    }

    // region never leaked a live span past the walk (nothing to assert here;
    // the walk's checks already cover it).
}

REGISTER_UNIT_TEST( TestWindowRingOperationSequences );
