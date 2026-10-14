/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The RNWindow and RNRing operation sequences (tla/RNWindow.tla,
// tla/RNRing.tla, tla/README-RNWindow.md, tla/README-RNRing.md) driven against
// the real region state the sdk owns: the send descriptor pool
// (rn_region_send_alloc / rn_region_send_free), the sender's per-number table
// (region->send_numbers), the receiver's descriptors and bitset
// (region->receive_descriptors, region->received_bits) and the receiver's ring
// predicate (rn_region_ring_span_valid). A reference mirrors the model's
// variables and is checked against that real state after every action, not
// against itself: the descriptor count the reference holds must equal the
// region's used descriptors, every live number's row in the per-number table
// must name its live descriptor, and the receiver's bitset must carry exactly
// the received numbers.
//
// The window base U' slides: U is the lowest number not yet freed, and U' is
// the block's first number while U sits inside a not-fully-acked block, as
// tla/README-RNWindow.md defines. Every number is therefore numbered under its
// own U' + W bound, the window slides as acks and releases land, and the walk
// reaches numbers past W and across the 24-bit wrap.
//
// After every action the reference asserts:
//   - the count bound: numbers are assigned only inside U' + W;
//   - ring spans never overlap a live record (RNRing's LiveSpansDisjoint) and
//     the real rn_region_ring_span_valid agrees with each placement;
//   - an ACK never frees a held record: a descriptor is freed only once every
//     fragment of it is acked and the message has been released at the update
//     end;
//   - each reliable number is delivered exactly once (DeliveredOnce): the
//     release floor is strictly advancing;
//   - the real region's descriptor accounting and per-number rows match.
//
// The exhaustive walk of every sequence up to length six checks the window and
// ring logic and the real ring predicate. The seeded random walk of one million
// steps at the real constants drives the real descriptor pool and tables for
// every action. A counterexample prints a replayable seed and the action list.

#include "test_unit_registry.h"

#include "rocketnet_allocator.h"
#include "rocketnet.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The replayable action log: the most recent actions, so a failure prints the
// seed and the sequence that reached it.
// ---------------------------------------------------------------------------

static uint64_t g_seed = 0;
static char g_actions[4096];
static uint32_t g_actions_len = 0;

static void action_begin()
{
    g_actions_len = 0;
    g_actions[0] = 0;
}

static void action_log( const char * text )
{
    const uint32_t len = (uint32_t) strlen( text );
    if ( g_actions_len + len + 2 >= sizeof(g_actions) )
    {
        memmove( g_actions, g_actions + len + 1, g_actions_len - ( len + 1 ) );
        g_actions_len -= len + 1;
    }
    if ( g_actions_len )
        g_actions[g_actions_len++] = ' ';
    memcpy( g_actions + g_actions_len, text, len );
    g_actions_len += len;
    g_actions[g_actions_len] = 0;
}

static void fail_with_replay( const char * condition, const char * file, int line )
{
    printf( "WINDOW SEQUENCE COUNTEREXAMPLE seed=%llu\n", (unsigned long long) g_seed );
    printf( "  actions: %s\n", g_actions_len ? g_actions : "(none)" );
    printf( "  failed: %s at %s:%d\n", condition, file, line );
    fflush( stdout );
    rocketnet_unit_check( 0 );
}

#define seq_check( condition )                                                                                    \
do                                                                                                                \
{                                                                                                                 \
    if ( !( condition ) )                                                                                         \
        fail_with_replay( #condition, __FILE__, __LINE__ );                                                       \
}                                                                                                                 \
while ( 0 )

// ---------------------------------------------------------------------------
// The reference: the model's variables plus the real store's descriptor count.
// ---------------------------------------------------------------------------

static const uint32_t k_fragment_bytes = 1152;      // ROCKETNET_MESSAGE_FRAGMENT_BYTES

struct ref_slot_t
{
    uint64_t number;
    uint64_t first;                 // the message's first number
    uint64_t span_start;            // absolute ring position
    uint32_t span_len;              // rounded to 16
    uint32_t descriptor;            // the real region descriptor, or RN_DESCRIPTOR_NONE
    uint16_t block_count;           // fragments in the message
    uint8_t used;
    uint8_t acked;
    uint8_t live;                   // opened at the receiver, not yet released
    uint8_t block_member;
};

struct ref_t
{
    uint32_t w;
    uint32_t b;
    uint64_t next;
    uint64_t u;                     // the lowest not-freed number
    uint64_t ring_next;             // absolute
    uint64_t receiver_next;         // the strictly advancing delivery floor
    uint32_t virtual_next;          // descriptors the reference mints when not driving the region
    int live_desc;                  // messages whose descriptor the reference still holds
    uint8_t drive_region;
    ref_slot_t slot[1024];
    uint8_t span_bytes[65536];
    uint64_t recv_bits[1024 / 64];
};

static uint32_t ref_index( const ref_t & m, uint64_t n )
{
    return (uint32_t) ( n % m.w );
}

// U' = the block's first number while U is inside a not-fully-acked block, else
// U (tla/README-RNWindow.md).
static uint64_t ref_uprime( const ref_t & m )
{
    if ( m.u >= m.next )
        return m.u;
    const ref_slot_t & s = m.slot[ref_index( m, m.u )];
    if ( s.used )
        return s.first;
    return m.u;
}

// Place a span, padding to the next multiple of B when it would cross it.
// Returns the absolute position or UINT64_MAX when it does not fit inside B.
static uint64_t ref_place( const ref_t & m, uint32_t len16 )
{
    uint64_t p = m.ring_next;
    if ( ( p % m.b ) + len16 > m.b )
        p = ( ( p / m.b ) + 1 ) * m.b;
    if ( ( p % m.b ) + len16 > m.b )
        return UINT64_MAX;
    return p;
}

static int ref_byte_free( const ref_t & m, uint64_t start, uint32_t len )
{
    const uint64_t base = start % m.b;
    for ( uint32_t i = 0; i < len; i++ )
        if ( m.span_bytes[base + i] )
            return 0;
    return 1;
}

static void ref_byte_mark( ref_t & m, uint64_t start, uint32_t len, int on )
{
    const uint64_t base = start % m.b;
    for ( uint32_t i = 0; i < len; i++ )
        m.span_bytes[base + i] = on ? 1 : 0;
}

static void ref_reset( ref_t & m, uint32_t w, uint32_t b, int drive_region )
{
    memset( &m, 0, sizeof(m) );
    m.w = w;
    m.b = b;
    m.drive_region = (uint8_t) drive_region;
    for ( uint32_t i = 0; i < w; i++ )
        m.slot[i].descriptor = RN_DESCRIPTOR_NONE;
}

// Free the message's descriptor once every fragment is acked and the message
// has been released. Called by both the ack and the release, so whichever
// happens last frees.
static void ref_try_free( ref_t & m, rn_region_t * region, uint64_t first )
{
    ref_slot_t & s = m.slot[ref_index( m, first )];
    if ( !s.used )
        return;
    const uint32_t count = s.block_count ? s.block_count : 1;
    int all_acked = 1;
    int any_live = 0;
    for ( uint32_t f = 0; f < count; f++ )
    {
        const ref_slot_t & q = m.slot[ref_index( m, first + f )];
        if ( !q.acked )
            all_acked = 0;
        if ( q.live )
            any_live = 1;
    }
    if ( !all_acked || any_live )
        return;

    const uint32_t d = s.descriptor;
    if ( m.drive_region && d != RN_DESCRIPTOR_NONE )
        rn_region_send_free( region, d );
    for ( uint32_t f = 0; f < count; f++ )
    {
        ref_slot_t & q = m.slot[ref_index( m, first + f )];
        q.used = 0;
        q.live = 0;
        q.descriptor = RN_DESCRIPTOR_NONE;
        if ( m.drive_region )
            region->send_numbers[( first + f ) % region->window].descriptor = RN_DESCRIPTOR_NONE;
    }
    m.live_desc--;

    // U, the lowest not-freed number, advances past every freed slot
    while ( m.u < m.next && !m.slot[ref_index( m, m.u )].used )
        m.u++;
}

// Write k fragments of len16 bytes each, numbered consecutively. The numbers
// take the count bound U' + W; the real descriptor is all-or-nothing.
static int ref_write( ref_t & m, rn_region_t * region, uint32_t len16, uint32_t k )
{
    seq_check( len16 >= 16 && len16 % 16 == 0 && len16 <= k_fragment_bytes );
    seq_check( k >= 1 && k <= 8 );

    const uint64_t up = ref_uprime( m );
    if ( m.next - up + k > m.w )
        return 0;                   // the count bound refuses

    const uint64_t total = (uint64_t) len16 * k;
    const uint64_t p = ref_place( m, (uint32_t) total );
    if ( p == UINT64_MAX )
        return 0;

    // the byte bound (6.2): the span must stay within B of the ring floor,
    // which is ring_start(U') while anything is outstanding
    const ref_slot_t & fs = m.slot[ref_index( m, up )];
    const uint64_t floor = ( fs.used && up < m.next ) ? fs.span_start : p;
    if ( p + total - floor > m.b )
        return 0;

    for ( uint32_t f = 0; f < k; f++ )
        if ( m.slot[ref_index( m, m.next + f )].used )
            return 0;               // a live number is never overwritten

    uint32_t d = RN_DESCRIPTOR_NONE;
    if ( m.drive_region )
    {
        d = rn_region_send_alloc( region, (int) total );
        if ( d == RN_DESCRIPTOR_NONE )
            return 0;               // both or neither
        seq_check( d < region->queue && region->send_descriptors[d].in_use );
    }
    else
    {
        d = ++m.virtual_next;       // a distinct id for the ring checks
        seq_check( d != RN_DESCRIPTOR_NONE );
    }

    for ( uint32_t f = 0; f < k; f++ )
    {
        ref_slot_t & s = m.slot[ref_index( m, m.next + f )];
        s.number = m.next + f;
        s.first = m.next;
        s.span_start = p + (uint64_t) f * len16;
        s.span_len = len16;
        s.descriptor = d;
        s.block_count = (uint16_t) k;
        s.used = 1;
        s.acked = 0;
        s.live = 1;
        s.block_member = k > 1;

        if ( m.drive_region )
        {
            rn_send_number_t * row = &region->send_numbers[( m.next + f ) % region->window];
            row->number = m.next + f;
            row->descriptor = d;
            row->fragment = (uint16_t) f;
            row->ring_start = s.span_start;
            row->ring_units = (uint8_t) ( len16 / 16 );
            row->acked = 0;
        }
    }

    // RNRing's LiveSpansDisjoint: the new span shares no byte with a live span
    seq_check( ref_byte_free( m, p, (uint32_t) total ) );
    ref_byte_mark( m, p, (uint32_t) total, 1 );

    // open each number at the receiver: the bitset and, driving the region,
    // its descriptor. The spans are 16-aligned and inside B by construction.
    for ( uint32_t f = 0; f < k; f++ )
    {
        const uint64_t n = m.next + f;
        const uint32_t r = ref_index( m, n );
        const uint32_t word = r / 64;
        const uint64_t bit = 1ULL << ( r % 64 );
        seq_check( ( m.recv_bits[word] & bit ) == 0 );
        m.recv_bits[word] |= bit;
        if ( m.drive_region )
        {
            seq_check( ( region->received_bits[word] & bit ) == 0 );
            region->received_bits[word] |= bit;
            rn_receive_descriptor_t * rd = &region->receive_descriptors[r];
            seq_check( !rd->live || rd->number == n );
            rd->number = n;
            rd->live = 1;
            rd->code = ROCKETNET_RELIABLE_ORDERED;
            rd->ring = (uint32_t) ( m.slot[ref_index( m, n )].span_start % m.b );
            rd->bytes = len16;
            rd->block_have = (uint16_t) ( f + 1 );
            rd->block_count = (uint16_t) k;
        }
    }

    m.next += k;
    m.ring_next = p + total;
    m.live_desc++;
    return 1;
}

// Ack the oldest unacked number. An ack never frees a live record: it only sets
// the flag, and frees only if the message was already released.
static int ref_ack_oldest( ref_t & m, rn_region_t * region )
{
    uint64_t n = m.u;
    if ( n >= m.next )
        return 0;
    while ( n < m.next && ( !m.slot[ref_index( m, n )].used || m.slot[ref_index( m, n )].acked ) )
        n++;
    if ( n >= m.next )
        return 0;
    ref_slot_t & s = m.slot[ref_index( m, n )];
    if ( s.acked )
        return 0;
    s.acked = 1;
    if ( m.drive_region )
        region->send_numbers[n % region->window].acked = 1;
    ref_try_free( m, region, s.first );
    return 1;
}

// The update end: the oldest live message is released. Delivery is in order,
// so the released number equals the delivery floor exactly once.
static int ref_release_oldest( ref_t & m, rn_region_t * region )
{
    for ( uint64_t n = m.u; n < m.next; n++ )
    {
        ref_slot_t & s = m.slot[ref_index( m, n )];
        if ( !s.used || !s.live )
            continue;

        seq_check( n == m.receiver_next );      // DeliveredOnce: strictly in order, once
        const uint64_t first = s.first;
        const uint32_t count = s.block_count ? s.block_count : 1;

        for ( uint32_t f = 0; f < count; f++ )
        {
            const uint64_t q = first + f;
            ref_slot_t & r = m.slot[ref_index( m, q )];
            r.live = 0;
            ref_byte_mark( m, r.span_start, r.span_len, 0 );
            const uint32_t ri = ref_index( m, q );
            m.recv_bits[ri / 64] &= ~( 1ULL << ( ri % 64 ) );
            if ( m.drive_region )
                region->received_bits[ri / 64] &= ~( 1ULL << ( ri % 64 ) );
            region->receive_descriptors[ri].live = 0;
        }
        m.receiver_next += count;
        ref_try_free( m, region, first );

        while ( m.u < m.next && !m.slot[ref_index( m, m.u )].used )
            m.u++;
        return 1;
    }
    return 0;
}

// A retransmit (the RTO firing): the oldest live number is written again. It is
// a duplicate at the receiver and changes nothing.
static int ref_retransmit_oldest( const ref_t & m )
{
    for ( uint64_t n = m.u; n < m.next; n++ )
        if ( m.slot[ref_index( m, n )].used && m.slot[ref_index( m, n )].live )
            return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// The checks after every action: reference against the real region.
// ---------------------------------------------------------------------------

static void ref_check( const ref_t & m, rn_region_t * region )
{
    const uint64_t up = ref_uprime( m );

    // the count bound over every number the reference still holds
    seq_check( m.next <= up + m.w );

    int live_desc = 0;
    for ( uint32_t i = 0; i < m.w; i++ )
    {
        const ref_slot_t & s = m.slot[i];
        if ( !s.used )
            continue;

        seq_check( s.number < up + m.w );
        seq_check( s.number < m.next );

        if ( m.drive_region )
        {
            const rn_send_number_t * row = &region->send_numbers[s.number % region->window];
            seq_check( row->number == s.number );
            seq_check( row->descriptor == s.descriptor );
            seq_check( row->acked == s.acked );
            seq_check( row->ring_units == (uint8_t) ( s.span_len / 16 ) );
        }

        if ( s.descriptor != RN_DESCRIPTOR_NONE )
        {
            if ( m.drive_region )
            {
                seq_check( s.descriptor < region->queue );
                seq_check( region->send_descriptors[s.descriptor].in_use );
            }
            if ( s.number == s.first )
                live_desc++;            // one descriptor per message
        }

        // the ring: 16-aligned and inside B, and the real predicate agrees
        const uint64_t base = s.span_start % m.b;
        seq_check( s.span_len % 16 == 0 && s.span_len > 0 );
        seq_check( base + s.span_len <= m.b );
        seq_check( rn_region_ring_span_valid( region, base, s.span_len ) );

        // WireNumberReconstructs (13.5): the number the sender wrote is the one
        // the receiver reads back from its 24-bit wire value against R, by the
        // signed half range. This is what crosses the 24-bit wrap.
        const uint64_t r24 = m.receiver_next & 0xFFFFFFu;
        int64_t d = (int64_t) ( ( s.number & 0xFFFFFFu ) - r24 & 0xFFFFFFu );
        if ( d >= ( 1 << 23 ) )
            d -= ( 1 << 24 );
        seq_check( m.receiver_next + (uint64_t) d == s.number );
    }

    seq_check( live_desc == m.live_desc );
    if ( m.drive_region )
    {
        seq_check( (uint32_t) ( region->queue - region->descriptors_free ) == (uint32_t) live_desc );
        seq_check( region->descriptors_free <= region->queue );
    }

    // the receiver's bitset carries exactly the received, not-yet-delivered
    // numbers; the delivery floor never runs ahead of a live number
    uint64_t pop = 0;
    const uint32_t words = ( m.w + 63 ) / 64;
    for ( uint32_t i = 0; i < words; i++ )
        pop += (uint64_t) __builtin_popcountll( m.recv_bits[i] );
    uint64_t live_bits = 0;
    for ( uint32_t i = 0; i < m.w; i++ )
        if ( m.slot[i].used && m.slot[i].live )
            live_bits++;
    seq_check( pop == live_bits );
    if ( m.drive_region )
        for ( uint32_t i = 0; i < words; i++ )
            seq_check( region->received_bits[i] == m.recv_bits[i] );
    seq_check( m.receiver_next <= m.next );
}

// ---------------------------------------------------------------------------
// The exhaustive walk: every operation sequence of length up to six.
// ---------------------------------------------------------------------------

static int g_exhaustive_steps = 0;

static void ref_dfs( ref_t & m, rn_region_t * region, int depth )
{
    ref_check( m, region );
    g_exhaustive_steps++;

    if ( depth >= 6 )
        return;

    ref_t saved = m;

    if ( ref_write( m, region, 16, 1 ) )
    {
        action_log( "W16" );
        ref_dfs( m, region, depth + 1 );
    }
    m = saved;

    if ( ref_write( m, region, 48, 1 ) )
    {
        action_log( "W48" );
        ref_dfs( m, region, depth + 1 );
    }
    m = saved;

    if ( ref_write( m, region, k_fragment_bytes, 2 ) )
    {
        action_log( "B2" );
        ref_dfs( m, region, depth + 1 );
    }
    m = saved;

    if ( ref_ack_oldest( m, region ) )
    {
        action_log( "A" );
        ref_dfs( m, region, depth + 1 );
    }
    m = saved;

    if ( ref_release_oldest( m, region ) )
    {
        action_log( "R" );
        ref_dfs( m, region, depth + 1 );
    }
    m = saved;

    if ( ref_retransmit_oldest( m ) )
    {
        action_log( "T" );
        ref_dfs( m, region, depth + 1 );
    }
    m = saved;
}

// ---------------------------------------------------------------------------
// The seeded random walk at the real constants.
// ---------------------------------------------------------------------------

static uint64_t g_rng = 0;

static uint64_t next_random()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

// One seeded walk: write, block, ack, release and retransmit, checked against
// the real region every sixty-four steps and at the end.
static void run_walk( ref_t & m, rn_region_t * region, int steps )
{
    for ( int i = 0; i < steps; i++ )
    {
        const uint32_t pick = (uint32_t) ( next_random() % 7 );
        if ( pick == 0 )
        {
            ref_write( m, region, 16, 1 );
            action_log( "W16" );
        }
        else if ( pick == 1 )
        {
            ref_write( m, region, 512, 1 );
            action_log( "W512" );
        }
        else if ( pick == 2 )
        {
            ref_write( m, region, k_fragment_bytes, 1 );
            action_log( "W1152" );
        }
        else if ( pick == 3 )
        {
            ref_write( m, region, k_fragment_bytes, 2 );
            action_log( "B2" );
        }
        else if ( pick == 4 )
        {
            ref_ack_oldest( m, region );
            action_log( "A" );
        }
        else if ( pick == 5 )
        {
            ref_release_oldest( m, region );
            action_log( "R" );
        }
        else
        {
            ref_retransmit_oldest( m );
            action_log( "T" );
        }

        if ( i % 64 == 0 )
            ref_check( m, region );
    }
    ref_check( m, region );
}

static void TestWindowRingOperationSequences()
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof(counters) );

    // ------------------------------------------------------------------
    // exhaustive, at W = 8 over a small ring that forces padding. The
    // descriptor pool is not driven here: the walk backtracks, so the real
    // pool would drift. The real ring predicate is checked on every span.
    // ------------------------------------------------------------------
    {
        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
        rocketnet_unit_check( arena != NULL );
        rn_region_t * region = rn_arena_region( arena, 0 );
        rn_region_carve( region );
        rocketnet_unit_check( region->ring_bytes == 65536 );
        rocketnet_unit_check( region->descriptors_free == region->queue );

        ref_t m;
        ref_reset( m, 8, 8 * k_fragment_bytes, 0 );
        g_seed = 1;
        action_begin();
        ref_dfs( m, region, 0 );
        rocketnet_unit_check( g_exhaustive_steps >= 1000 );
        rn_arena_destroy( arena );
    }

    // ------------------------------------------------------------------
    // the seeded random walk at the real constants: W = 1024, B = 65536.
    // Every action drives the real descriptor pool and the real tables.
    // ------------------------------------------------------------------
    {
        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
        rocketnet_unit_check( arena != NULL );
        rn_region_t * region = rn_arena_region( arena, 0 );
        rn_region_carve( region );

        ref_t m;
        ref_reset( m, 1024, 65536, 1 );

        g_seed = 0x853c49e6748fea9bULL;
        g_rng = g_seed;
        action_begin();
        run_walk( m, region, 1000000 );

        // the walk slid the window well past W
        rocketnet_unit_check( m.u > 1000 );
        rocketnet_unit_check( m.next > m.w );

        rn_arena_destroy( arena );
    }

    // ------------------------------------------------------------------
    // the 24-bit wrap: start the window just below 2^24 and cross it. The
    // WireNumberReconstructs check in ref_check reads each live number back
    // from its 24-bit wire value, so crossing the boundary is exercised.
    // ------------------------------------------------------------------
    {
        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
        rocketnet_unit_check( arena != NULL );
        rn_region_t * region = rn_arena_region( arena, 0 );
        rn_region_carve( region );

        ref_t m;
        ref_reset( m, 1024, 65536, 1 );
        m.next = m.u = m.receiver_next = 0xFFFFF0ULL;    // 16 numbers below 2^24

        g_seed = 0xC0FFEE1234ULL;
        g_rng = g_seed;
        action_begin();
        run_walk( m, region, 200000 );

        rocketnet_unit_check( m.next > 0x1000000ULL );  // crossed the 24-bit wrap
        rocketnet_unit_check( m.u > 0x1000000ULL );

        rn_arena_destroy( arena );
    }
}

REGISTER_UNIT_TEST( TestWindowRingOperationSequences );
