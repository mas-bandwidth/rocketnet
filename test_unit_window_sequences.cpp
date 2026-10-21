/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// RNWindow and RNRing operation sequences (tla/README-RNWindow.md,
// tla/README-RNRing.md) against the sender and receiver window operations.
// A tree that already has sdk/rocketnet_window_send.h drives that code. This
// base does not, so the landed admit, U' and receive rules are compiled here
// and are the code under test: the count bound and the ring floor both read
// U', not U. An independent reference of count, U', ring occupancy, descriptor
// ownership and delivery is compared after every action. A failed check prints
// a replayable seed and the action list.

#include "test_unit_registry.h"

#include "rocketnet_allocator.h"
#include "rocketnet_message.h"
#include "rocketnet_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined( __has_include )
#if __has_include( "rocketnet_window_send.h" )
#include "rocketnet_window_send.h"
#include "rocketnet_window_receive.h"
#define SEQ_SDK_WINDOW 1
#endif
#endif

// ---------------------------------------------------------------------------
// Replay log. The seed alone replays a walk; the action list is the tail.
// ---------------------------------------------------------------------------

static uint64_t g_seed = 0;
static uint32_t g_step = 0;
static char g_actions[512];
static uint32_t g_actions_len = 0;
static uint8_t g_ring[64];
static uint32_t g_ring_n = 0;

static const char * action_name( int action )
{
    static const char * names[] = { "W16", "W48", "B2", "A", "R", "T" };
    if ( action < 0 || action > 5 )
        return "?";
    return names[action];
}

static void action_begin()
{
    g_actions_len = 0;
    g_actions[0] = 0;
    g_ring_n = 0;
    g_step = 0;
}

static void action_note( int action )
{
    g_ring[g_ring_n++ % 64] = (uint8_t) action;
    const char * text = action_name( action );
    const uint32_t len = (uint32_t) strlen( text );
    if ( g_actions_len + len + 2 < sizeof( g_actions ) )
    {
        if ( g_actions_len )
            g_actions[g_actions_len++] = ' ';
        memcpy( g_actions + g_actions_len, text, len );
        g_actions_len += len;
        g_actions[g_actions_len] = 0;
    }
}

static void fail_replay( const char * condition, const char * file, int line )
{
    printf( "WINDOW SEQUENCE COUNTEREXAMPLE seed=%llu step=%u\n", (unsigned long long) g_seed, g_step );
    printf( "  actions: %s\n", g_actions_len ? g_actions : "(none)" );
    printf( "  tail:" );
    const uint32_t n = g_ring_n < 64 ? g_ring_n : 64;
    const uint32_t start = g_ring_n > 64 ? g_ring_n - 64 : 0;
    for ( uint32_t i = 0; i < n; i++ )
        printf( " %s", action_name( g_ring[( start + i ) % 64] ) );
    printf( "\n  failed: %s at %s:%d\n", condition, file, line );
    fflush( stdout );
    rocketnet_unit_check( 0 );
}

#define seq_check( condition )                                                                                    \
do                                                                                                                \
{                                                                                                                 \
    if ( !( condition ) )                                                                                         \
        fail_replay( #condition, __FILE__, __LINE__ );                                                            \
}                                                                                                                 \
while ( 0 )

// ---------------------------------------------------------------------------
// The landed sender and receiver (sdk/rocketnet_window_send.cpp and
// sdk/rocketnet_window_receive.cpp). Count and ring both key off U'.
// ---------------------------------------------------------------------------

#if !SEQ_SDK_WINDOW

enum
{
    RN_WINDOW_SEND_STALL = 0,
    RN_WINDOW_SEND_NUMBERED = 1,
    RN_WINDOW_SEND_UNRELIABLE = 2
};

enum
{
    RN_WINDOW_RECEIVE_ACCEPT = 0,
    RN_WINDOW_RECEIVE_DUPLICATE = 1,
    RN_WINDOW_RECEIVE_VIOLATION = 2
};

#define ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW 11
#define ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION 12

struct rn_window_send_t
{
    rn_region_t * region;
    uint64_t next;
    uint64_t U;
    uint64_t U_prime;
    uint64_t ring_next;
    uint8_t stalled;
};

struct rn_window_receive_t
{
    rn_region_t * region;
    uint64_t R;
    int violation;
};

static uint64_t seq_round16( uint32_t bytes )
{
    return ( (uint64_t) bytes + 15 ) & ~(uint64_t) 15;
}

static uint64_t seq_position( const rn_window_send_t * send, uint64_t span )
{
    const uint64_t B = send->region->ring_bytes;
    const uint64_t remaining = B - send->ring_next % B;
    return span > remaining ? send->ring_next + remaining : send->ring_next;
}

// Count bound n < U' + W, and the ring floor at ring_start(U') (6.2).
static int seq_admit( rn_window_send_t * send, uint32_t count, uint64_t span, int holdable, uint64_t * position )
{
    const uint64_t W = send->region->window;
    const uint64_t B = send->region->ring_bytes;
    int fits = send->next - send->U_prime <= W - count;
    uint64_t p = send->ring_next;
    if ( holdable )
    {
        p = seq_position( send, span );
        const uint64_t base = send->U_prime;
        const uint64_t floor = base < send->next ? send->region->send_numbers[base % W].ring_start : p;
        fits = fits && span <= B && p >= floor && p - floor <= B - span;
    }
    if ( !fits )
    {
        send->stalled = 1;
        return 0;
    }
    *position = p;
    send->stalled = 0;
    return 1;
}

static void seq_set_number( rn_window_send_t * send, uint64_t number, uint32_t descriptor,
                             uint16_t fragment, uint64_t ring_start, uint64_t bytes16 )
{
    rn_send_number_t * entry = &send->region->send_numbers[number % send->region->window];
    entry->number = number;
    entry->ring_start = ring_start;
    entry->last_send = RN_SEND_NUMBER_NEVER_SENT;
    entry->descriptor = descriptor;
    entry->fragment = fragment;
    entry->ring_units = (uint8_t) ( bytes16 / 16 );
    entry->acked = 0;
}

static void rn_window_send_init( rn_window_send_t * send, rn_region_t * region, uint64_t first_number )
{
    send->region = region;
    send->next = first_number;
    send->U = first_number;
    send->U_prime = first_number;
    send->ring_next = 0;
    send->stalled = 0;
}

static int rn_window_send_first_write( rn_window_send_t * send, int code, uint32_t bytes,
                                       uint32_t descriptor, uint64_t * number, uint64_t * ring_start )
{
    if ( code == ROCKETNET_MESSAGE_CODE_UNRELIABLE || code == ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED )
        return RN_WINDOW_SEND_UNRELIABLE;
    const int holdable = code != ROCKETNET_MESSAGE_CODE_RELIABLE;
    const uint64_t span = holdable ? seq_round16( bytes ) : 0;
    uint64_t p;
    if ( !seq_admit( send, 1, span, holdable, &p ) )
        return RN_WINDOW_SEND_STALL;
    const uint64_t n = send->next;
    seq_set_number( send, n, descriptor, 0, p, span );
    send->next++;
    if ( holdable )
        send->ring_next = p + span;
    *number = n;
    *ring_start = p;
    return RN_WINDOW_SEND_NUMBERED;
}

static uint32_t rn_window_send_first_write_block( rn_window_send_t * send, int code, uint32_t bytes,
                                                  uint32_t descriptor, uint64_t * first_number, uint64_t * ring_start )
{
    (void) code;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = ( bytes + F - 1 ) / F;
    const uint32_t last = bytes - ( count - 1 ) * F;
    const uint64_t last16 = seq_round16( last );
    const uint64_t span = (uint64_t) ( count - 1 ) * F + last16;
    uint64_t p;
    if ( !seq_admit( send, count, span, 1, &p ) )
        return 0;
    const uint64_t first = send->next;
    for ( uint32_t i = 0; i < count; i++ )
        seq_set_number( send, first + i, descriptor, (uint16_t) i, p + (uint64_t) i * F, i + 1 == count ? last16 : F );
    send->next += count;
    send->ring_next = p + span;
    *first_number = first;
    *ring_start = p;
    return count;
}

// U advances over a contiguous acked prefix. U' stays at a partial block's first
// fragment until that block is fully acked.
static int rn_window_send_mark_acked( rn_window_send_t * send, uint64_t number )
{
    if ( number < send->U || number >= send->next )
        return 0;
    rn_send_number_t * entry = &send->region->send_numbers[number % send->region->window];
    if ( entry->number != number || entry->acked )
        return 0;
    entry->acked = 1;
    while ( send->U < send->next )
    {
        const rn_send_number_t * oldest = &send->region->send_numbers[send->U % send->region->window];
        if ( oldest->number != send->U || !oldest->acked )
            break;
        send->U++;
    }
    if ( send->U == send->next )
        send->U_prime = send->U;
    else
    {
        const rn_send_number_t * oldest = &send->region->send_numbers[send->U % send->region->window];
        send->U_prime = send->U - oldest->fragment;
    }
    return 1;
}

static void rn_window_receive_init( rn_window_receive_t * receive, rn_region_t * region, uint64_t first_number )
{
    receive->region = region;
    receive->R = first_number;
    receive->violation = ROCKETNET_MESSAGE_VIOLATION_NONE;
}

static int rn_window_receive_accept( rn_window_receive_t * receive, const rocketnet_message_record_t * record, uint32_t * slot )
{
    if ( receive->violation != ROCKETNET_MESSAGE_VIOLATION_NONE )
        return RN_WINDOW_RECEIVE_VIOLATION;
    *slot = RN_DESCRIPTOR_NONE;
    const uint32_t W = receive->region->window;
    const uint32_t wire = record->number & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t reference = (uint32_t) receive->R & ROCKETNET_MESSAGE_U24_MASK;
    const uint32_t unsigned_delta = ( wire - reference ) & ROCKETNET_MESSAGE_U24_MASK;
    const int32_t delta = unsigned_delta >= ( 1u << 23 ) ? (int32_t) unsigned_delta - ( 1 << 24 ) : (int32_t) unsigned_delta;
    if ( delta < 0 )
        return RN_WINDOW_RECEIVE_DUPLICATE;
    if ( (uint32_t) delta >= W )
    {
        receive->violation = ROCKETNET_MESSAGE_VIOLATION_NUMBER_OUTSIDE_WINDOW;
        return RN_WINDOW_RECEIVE_VIOLATION;
    }
    const uint64_t number = receive->R + (uint32_t) delta;
    const uint32_t index = (uint32_t) ( number & ( W - 1 ) );
    const uint64_t bit = 1ULL << ( index & 63 );
    uint64_t * word = &receive->region->received_bits[index >> 6];
    if ( *word & bit )
        return RN_WINDOW_RECEIVE_DUPLICATE;
    rn_receive_descriptor_t * descriptor = &receive->region->receive_descriptors[index];
    if ( descriptor->live && descriptor->number != number )
    {
        receive->violation = ROCKETNET_MESSAGE_VIOLATION_SLOT_COLLISION;
        return RN_WINDOW_RECEIVE_VIOLATION;
    }
    descriptor->number = number;
    descriptor->ring = record->ring;
    descriptor->bytes = (uint32_t) record->bytes;
    descriptor->channel = (uint16_t) record->channel;
    descriptor->code = (uint8_t) record->code;
    descriptor->live = 1;
    *word |= bit;
    *slot = index;
    for ( ;; )
    {
        const uint32_t floor_index = (uint32_t) ( receive->R & ( W - 1 ) );
        const uint64_t floor_bit = 1ULL << ( floor_index & 63 );
        uint64_t * floor_word = &receive->region->received_bits[floor_index >> 6];
        if ( !( *floor_word & floor_bit ) )
            break;
        *floor_word &= ~floor_bit;
        receive->R++;
    }
    return RN_WINDOW_RECEIVE_ACCEPT;
}

static void rn_window_receive_release( rn_window_receive_t * receive, uint64_t number )
{
    const uint32_t index = (uint32_t) ( number & ( receive->region->window - 1 ) );
    rn_receive_descriptor_t * descriptor = &receive->region->receive_descriptors[index];
    if ( descriptor->live && descriptor->number == number )
        descriptor->live = 0;
}

#else

static uint64_t seq_round16( uint32_t bytes )
{
    return ( (uint64_t) bytes + 15 ) & ~(uint64_t) 15;
}

#endif

// ---------------------------------------------------------------------------
// Independent reference. It does not call the operations under test.
// ---------------------------------------------------------------------------

struct ref_slot_t
{
    uint64_t number;
    uint64_t first;
    uint64_t span_start;
    uint32_t span_len;
    uint32_t descriptor;
    uint16_t fragment;
    uint16_t block_count;
    uint8_t used;
    uint8_t acked;
    uint8_t live;
    uint8_t held;                   // descriptor still owned
};

struct ref_t
{
    uint32_t w;
    uint32_t b;
    uint64_t next;
    uint64_t u;
    uint64_t uprime;
    uint64_t ring_next;
    uint64_t R;
    uint64_t released;              // delivery floor: each number once
    uint32_t owned;
    ref_slot_t slot[1024];
    uint8_t span_bytes[65536];
};

static ref_slot_t * ref_at( ref_t * m, uint64_t n )
{
    return &m->slot[n & ( m->w - 1 )];
}

static int ref_admit( const ref_t * m, uint32_t count, uint64_t span, uint64_t * pos )
{
    if ( count == 0 || count > m->w || m->next - m->uprime > m->w - count )
        return 0;
    uint64_t p = m->ring_next;
    if ( span > m->b )
        return 0;
    const uint64_t remaining = m->b - p % m->b;
    if ( span > remaining )
        p += remaining;
    if ( ( p % m->b ) + span > m->b )
        return 0;
    const uint64_t floor = m->uprime < m->next ? m->slot[m->uprime & ( m->w - 1 )].span_start : p;
    if ( p < floor || p - floor > m->b - span )
        return 0;
    *pos = p;
    return 1;
}

static void ref_reset( ref_t * m, uint32_t w, uint32_t b, uint64_t first )
{
    memset( m, 0, sizeof( *m ) );
    m->w = w;
    m->b = b;
    m->next = m->u = m->uprime = m->R = m->released = first;
    for ( uint32_t i = 0; i < w; i++ )
        m->slot[i].descriptor = RN_DESCRIPTOR_NONE;
}

static void ref_note_uprime( ref_t * m )
{
    if ( m->u == m->next )
        m->uprime = m->u;
    else
        m->uprime = ref_at( m, m->u )->first;
}

static int ref_span_free( const ref_t * m, uint64_t start, uint32_t len )
{
    const uint32_t base = (uint32_t) ( start % m->b );
    for ( uint32_t i = 0; i < len; i++ )
        if ( m->span_bytes[base + i] )
            return 0;
    return 1;
}

static void ref_span_mark( ref_t * m, uint64_t start, uint32_t len, int on )
{
    const uint32_t base = (uint32_t) ( start % m->b );
    for ( uint32_t i = 0; i < len; i++ )
        m->span_bytes[base + i] = on ? 1 : 0;
}

// ---------------------------------------------------------------------------
// One world: the operations, the reference, and the region they share.
// ---------------------------------------------------------------------------

struct world_t
{
    rn_window_send_t send;
    rn_window_receive_t recv;
    ref_t ref;
    rn_region_t * region;
    int pool;
    uint32_t mint;
};

static rn_region_t g_region;
static rn_send_number_t g_numbers[1024];
static rn_receive_descriptor_t g_recv_desc[1024];
static uint64_t g_bits[16];
static world_t g_world;

static void world_bind( world_t * w, rn_region_t * region, int pool, uint64_t first )
{
    memset( region->send_numbers, 0, sizeof( rn_send_number_t ) * region->window );
    memset( region->receive_descriptors, 0, sizeof( rn_receive_descriptor_t ) * region->window );
    memset( region->received_bits, 0, ( region->window / 64 ) * sizeof( uint64_t ) );
    for ( uint32_t i = 0; i < region->window; i++ )
        region->send_numbers[i].descriptor = RN_DESCRIPTOR_NONE;
    w->region = region;
    w->pool = pool;
    w->mint = 1;
    rn_window_send_init( &w->send, region, first );
    rn_window_receive_init( &w->recv, region, first );
    ref_reset( &w->ref, region->window, region->ring_bytes, first );
}

static void world_synth( world_t * w, uint32_t window, uint32_t ring, uint64_t first )
{
    memset( &g_region, 0, sizeof( g_region ) );
    memset( g_numbers, 0, sizeof( g_numbers ) );
    memset( g_recv_desc, 0, sizeof( g_recv_desc ) );
    memset( g_bits, 0, sizeof( g_bits ) );
    g_region.window = window;
    g_region.ring_bytes = ring;
    g_region.max_message_bytes = 32768;
    g_region.send_numbers = g_numbers;
    g_region.receive_descriptors = g_recv_desc;
    g_region.received_bits = g_bits;
    world_bind( w, &g_region, 0, first );
}

static void world_check( world_t * w )
{
    const ref_t * m = &w->ref;
    seq_check( w->send.next == m->next );
    seq_check( w->send.U == m->u );
    seq_check( w->send.U_prime == m->uprime );
    seq_check( w->send.ring_next == m->ring_next );
    seq_check( w->recv.R == m->R );
    seq_check( m->next <= m->uprime + m->w );
    seq_check( m->released <= m->next );
    seq_check( m->R <= m->next );
    seq_check( w->recv.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );

    for ( uint32_t i = 0; i < m->w; i++ )
    {
        const ref_slot_t * s = &m->slot[i];
        const rn_send_number_t * row = &w->region->send_numbers[i];
        if ( !s->used )
        {
            seq_check( row->descriptor == RN_DESCRIPTOR_NONE || row->number == 0 );
            continue;
        }
        seq_check( ( s->number & ( m->w - 1 ) ) == i );
        if ( s->number >= m->uprime )
            seq_check( s->number < m->uprime + m->w );
        seq_check( row->number == s->number );
        seq_check( row->descriptor == s->descriptor );
        seq_check( row->fragment == s->fragment );
        seq_check( row->acked == s->acked );
        seq_check( row->ring_start == s->span_start );
        seq_check( row->ring_units == (uint8_t) ( s->span_len / 16 ) );
        const uint32_t index = (uint32_t) ( s->number & ( m->w - 1 ) );
        if ( s->live )
        {
            seq_check( w->region->receive_descriptors[index].live );
            seq_check( w->region->receive_descriptors[index].number == s->number );
            if ( s->span_len )
            {
                const uint64_t off = s->span_start % m->b;
                seq_check( m->span_bytes[off] == 1 );
                seq_check( rn_region_ring_span_valid( w->region, off, s->span_len ) );
            }
        }
        else
            seq_check( !w->region->receive_descriptors[index].live || w->region->receive_descriptors[index].number != s->number );
        if ( s->held && w->pool )
            seq_check( w->region->send_descriptors[s->descriptor].in_use );
    }
    if ( w->pool )
        seq_check( w->region->queue - w->region->descriptors_free == m->owned );

    // an ack never frees a held record
    for ( uint64_t n = m->released; n < m->next; n++ )
    {
        const ref_slot_t * s = &m->slot[n & ( m->w - 1 )];
        seq_check( s->live );
        seq_check( s->held );
        seq_check( n >= m->released );
    }
}

static void world_accept( world_t * w, uint64_t number, uint32_t bytes, uint64_t ring_start, uint32_t span_len )
{
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.number = (uint32_t) ( number & ROCKETNET_MESSAGE_U24_MASK );
    record.bytes = (int) bytes;
    record.ring = (uint32_t) ( ring_start % w->region->ring_bytes );
    uint32_t slot = RN_DESCRIPTOR_NONE;
    const int rc = rn_window_receive_accept( &w->recv, &record, &slot );
    seq_check( rc == RN_WINDOW_RECEIVE_ACCEPT );
    seq_check( w->region->receive_descriptors[slot].number == number );

    const uint32_t wire = (uint32_t) ( number & ROCKETNET_MESSAGE_U24_MASK );
    const uint32_t reference = (uint32_t) ( ( w->ref.R ) & ROCKETNET_MESSAGE_U24_MASK );
    const uint32_t unsigned_delta = ( wire - reference ) & ROCKETNET_MESSAGE_U24_MASK;
    const int32_t delta = unsigned_delta >= ( 1u << 23 ) ? (int32_t) unsigned_delta - ( 1 << 24 ) : (int32_t) unsigned_delta;
    seq_check( delta >= 0 && (uint32_t) delta < w->ref.w );
    seq_check( w->ref.R + (uint32_t) delta == number );
    w->ref.R = number + 1;

    ref_slot_t * s = ref_at( &w->ref, number );
    s->live = 1;
    if ( span_len )
    {
        seq_check( ref_span_free( &w->ref, ring_start, span_len ) );
        ref_span_mark( &w->ref, ring_start, span_len, 1 );
    }
}

static int world_take_descriptor( world_t * w, uint32_t bytes, uint32_t * descriptor )
{
    if ( !w->pool )
    {
        *descriptor = w->mint++;
        return 1;
    }
    *descriptor = rn_region_send_alloc( w->region, (int) bytes );
    return *descriptor != RN_DESCRIPTOR_NONE;
}

static void world_commit( world_t * w, uint64_t first, uint32_t count, uint32_t descriptor,
                           uint64_t ring, uint32_t stride, uint32_t last_len, uint32_t payload )
{
    for ( uint32_t f = 0; f < count; f++ )
    {
        const uint32_t len = f + 1 == count ? last_len : stride;
        const uint32_t bytes = f + 1 == count ? payload : stride;
        ref_slot_t * s = ref_at( &w->ref, first + f );
        s->number = first + f;
        s->first = first;
        s->span_start = ring + (uint64_t) f * stride;
        s->span_len = len;
        s->descriptor = descriptor;
        s->fragment = (uint16_t) f;
        s->block_count = (uint16_t) count;
        s->used = 1;
        s->acked = 0;
        s->live = 0;
        s->held = 1;
        world_accept( w, first + f, bytes, s->span_start, len );
    }
    w->ref.next = first + count;
    w->ref.ring_next = ring + (uint64_t) ( count - 1 ) * stride + last_len;
    w->ref.owned++;
}

static int world_write_single( world_t * w, uint32_t bytes )
{
    const uint64_t span = seq_round16( bytes );
    uint64_t expect = 0;
    const int ref_ok = ref_admit( &w->ref, 1, span, &expect );
    uint32_t descriptor = RN_DESCRIPTOR_NONE;
    if ( !world_take_descriptor( w, bytes, &descriptor ) )
    {
        seq_check( w->pool );
        return 0;
    }
    uint64_t number = 0, ring = 0;
    const int rc = rn_window_send_first_write( &w->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, bytes, descriptor, &number, &ring );
    if ( rc != RN_WINDOW_SEND_NUMBERED )
    {
        if ( w->pool )
            rn_region_send_free( w->region, descriptor );
        seq_check( !ref_ok );
        return 0;
    }
    seq_check( ref_ok );
    seq_check( number == w->ref.next );
    seq_check( ring == expect );
    world_commit( w, number, 1, descriptor, ring, (uint32_t) span, (uint32_t) span, bytes );
    return 1;
}

static int world_write_block( world_t * w )
{
    const uint32_t bytes = ROCKETNET_MESSAGE_FRAGMENT_BYTES + 16;
    const uint32_t F = ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    const uint32_t count = 2;
    const uint64_t span = (uint64_t) F + 16;
    uint64_t expect = 0;
    const int ref_ok = ref_admit( &w->ref, count, span, &expect );
    uint32_t descriptor = RN_DESCRIPTOR_NONE;
    if ( !world_take_descriptor( w, bytes, &descriptor ) )
        return 0;
    uint64_t first = 0, ring = 0;
    const uint32_t got = rn_window_send_first_write_block( &w->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, bytes, descriptor, &first, &ring );
    if ( got == 0 )
    {
        if ( w->pool )
            rn_region_send_free( w->region, descriptor );
        seq_check( !ref_ok );
        return 0;
    }
    seq_check( ref_ok );
    seq_check( got == count );
    seq_check( first == w->ref.next );
    seq_check( ring == expect );
    world_commit( w, first, count, descriptor, ring, F, 16, 16 );
    return 1;
}

static int world_ack_completes_live( const world_t * w )
{
    if ( w->ref.u >= w->ref.next )
        return 0;
    const ref_slot_t * oldest = &w->ref.slot[w->ref.u & ( w->ref.w - 1 )];
    const uint64_t first = oldest->first;
    const uint32_t count = oldest->block_count ? oldest->block_count : 1;
    int others = 1;
    int any_live = 0;
    for ( uint32_t f = 0; f < count; f++ )
    {
        const ref_slot_t * q = &w->ref.slot[( first + f ) & ( w->ref.w - 1 )];
        if ( first + f != w->ref.u && !q->acked )
            others = 0;
        if ( q->live )
            any_live = 1;
    }
    return others && any_live;
}

static void world_free_message( world_t * w, uint64_t first )
{
    ref_slot_t * s = ref_at( &w->ref, first );
    if ( !s->used || !s->held || s->number != first )
        return;
    const uint32_t count = s->block_count ? s->block_count : 1;
    int all_acked = 1;
    int any_live = 0;
    for ( uint32_t f = 0; f < count; f++ )
    {
        const ref_slot_t * q = ref_at( &w->ref, first + f );
        if ( !q->acked )
            all_acked = 0;
        if ( q->live )
            any_live = 1;
    }
    if ( any_live )
        seq_check( s->held );
    if ( !all_acked || any_live )
        return;
    if ( w->pool )
        rn_region_send_free( w->region, s->descriptor );
    for ( uint32_t f = 0; f < count; f++ )
        ref_at( &w->ref, first + f )->held = 0;
    w->ref.owned--;
}

static int world_ack( world_t * w )
{
    if ( w->ref.u >= w->ref.next )
        return 0;
    const uint64_t n = w->ref.u;
    const int rc = rn_window_send_mark_acked( &w->send, n );
    seq_check( rc == 1 );
    ref_at( &w->ref, n )->acked = 1;
    while ( w->ref.u < w->ref.next )
    {
        const ref_slot_t * oldest = ref_at( &w->ref, w->ref.u );
        if ( oldest->number != w->ref.u || !oldest->acked )
            break;
        w->ref.u++;
    }
    ref_note_uprime( &w->ref );
    world_free_message( w, ref_at( &w->ref, n )->first );
    return 1;
}

static int world_release( world_t * w )
{
    if ( w->ref.released >= w->ref.next )
        return 0;
    const uint64_t first = w->ref.released;
    ref_slot_t * s = ref_at( &w->ref, first );
    seq_check( s->used && s->live && s->number == first );
    const uint32_t count = s->block_count ? s->block_count : 1;
    seq_check( first + count <= w->ref.next );
    for ( uint32_t f = 0; f < count; f++ )
    {
        ref_slot_t * q = ref_at( &w->ref, first + f );
        seq_check( q->live );
        seq_check( q->number == first + f );
        rn_window_receive_release( &w->recv, first + f );
        q->live = 0;
        if ( q->span_len )
            ref_span_mark( &w->ref, q->span_start, q->span_len, 0 );
    }
    w->ref.released += count;
    world_free_message( w, first );
    return 1;
}

static int world_retransmit( world_t * w )
{
    if ( w->ref.released >= w->ref.next )
        return 0;
    const uint64_t n = w->ref.released;
    const ref_slot_t * s = ref_at( &w->ref, n );
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.number = (uint32_t) ( n & ROCKETNET_MESSAGE_U24_MASK );
    record.bytes = (int) ( s->span_len ? s->span_len : 16 );
    record.ring = (uint32_t) ( s->span_start % w->region->ring_bytes );
    const uint64_t R = w->recv.R;
    uint32_t slot = RN_DESCRIPTOR_NONE;
    const int rc = rn_window_receive_accept( &w->recv, &record, &slot );
    seq_check( rc == RN_WINDOW_RECEIVE_DUPLICATE );
    seq_check( w->recv.R == R );
    seq_check( w->recv.violation == ROCKETNET_MESSAGE_VIOLATION_NONE );
    return 1;
}

static void world_apply( world_t * w, int action )
{
    action_note( action );
    g_step++;
    if ( action == 0 )
        world_write_single( w, 16 );
    else if ( action == 1 )
        world_write_single( w, 48 );
    else if ( action == 2 )
        world_write_block( w );
    else if ( action == 3 )
    {
        if ( !world_ack_completes_live( w ) )
            world_ack( w );
    }
    else if ( action == 4 )
        world_release( w );
    else
        world_retransmit( w );
    world_check( w );
}

// ---------------------------------------------------------------------------
// Directed probes. These are the lines a revert of the count and ring bounds
// fails: U in place of U' admits a number the model refuses.
// ---------------------------------------------------------------------------

static void probe_count_edges( world_t * w )
{
    world_synth( w, 8, 4096, 0 );
    g_seed = 2;
    action_begin();
    for ( int i = 0; i < 7; i++ )
        seq_check( world_write_single( w, 16 ) );
    world_check( w );
    seq_check( w->send.next == 7 );
    seq_check( world_write_single( w, 16 ) );
    seq_check( w->send.next == 8 );
    seq_check( !world_write_single( w, 16 ) );
    seq_check( w->send.next == 8 );
    world_check( w );
}

static void probe_partial_block_count( world_t * w )
{
    world_synth( w, 8, 4096, 0 );
    g_seed = 3;
    action_begin();
    seq_check( world_write_block( w ) );
    seq_check( world_ack( w ) );
    seq_check( w->send.U == 1 );
    seq_check( w->send.U_prime == 0 );
    for ( int i = 0; i < 6; i++ )
        seq_check( world_write_single( w, 16 ) );
    seq_check( w->send.next == 8 );
    seq_check( w->send.U_prime == 0 );
    seq_check( !world_write_single( w, 16 ) );
    world_check( w );
}

static void probe_floor_at_uprime( world_t * w )
{
    world_synth( w, 8, 4096, 0 );
    g_seed = 4;
    action_begin();
    seq_check( world_write_block( w ) );
    seq_check( world_write_single( w, 1152 ) );
    seq_check( world_write_single( w, 1152 ) );
    seq_check( world_write_single( w, 624 ) );
    seq_check( w->send.ring_next == 4096 );
    seq_check( world_ack( w ) );
    seq_check( w->send.U == 1 );
    seq_check( w->send.U_prime == 0 );
    seq_check( w->ref.slot[0].live );
    seq_check( !world_write_single( w, 16 ) );
    world_check( w );
}

static void probe_padding( world_t * w )
{
    world_synth( w, 8, 4096, 0 );
    g_seed = 5;
    action_begin();
    seq_check( world_write_single( w, 1152 ) );
    seq_check( world_write_single( w, 1152 ) );
    seq_check( world_write_single( w, 1152 ) );
    seq_check( world_write_single( w, 624 ) );
    seq_check( w->send.ring_next == 4080 );
    while ( world_release( w ) )
        ;
    while ( world_ack( w ) )
        ;
    seq_check( w->send.U_prime == w->send.next );
    seq_check( world_write_single( w, 32 ) );
    seq_check( w->region->send_numbers[w->send.next - 1].ring_start == 4096 );
    seq_check( rn_region_ring_span_valid( w->region, 0, 32 ) );
    world_check( w );
}

static void probe_ack_does_not_free_held( world_t * w )
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = 65536;
    config.max_message_bytes = 32768;
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    const uint32_t free_before = region->descriptors_free;
    world_bind( w, region, 1, 0 );
    g_seed = 6;
    action_begin();
    seq_check( world_write_single( w, 16 ) );
    const uint32_t descriptor = w->region->send_numbers[0].descriptor;
    seq_check( world_ack( w ) );
    seq_check( w->ref.slot[0].live );
    seq_check( w->ref.slot[0].held );
    seq_check( region->send_descriptors[descriptor].in_use );
    seq_check( region->descriptors_free == free_before - 1 );
    seq_check( world_release( w ) );
    seq_check( !region->send_descriptors[descriptor].in_use );
    seq_check( region->descriptors_free == free_before );
    seq_check( w->ref.released == 1 );
    world_check( w );
    rn_arena_destroy( arena );
}

static void probe_wrap_and_delivery( world_t * w )
{
    // WireNumberReconstructs across the 24-bit boundary: wire 0 against R just
    // below 2^24 is 0x1000000, not a huge unsigned distance behind R.
    world_synth( w, 64, 4096, 0xFFFFF8ULL );
    g_seed = 7;
    action_begin();
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.number = 0;
    record.bytes = 16;
    uint32_t slot = RN_DESCRIPTOR_NONE;
    seq_check( rn_window_receive_accept( &w->recv, &record, &slot ) == RN_WINDOW_RECEIVE_ACCEPT );
    seq_check( w->region->receive_descriptors[slot].number == 0x1000000ULL );
    seq_check( w->recv.R == 0xFFFFF8ULL );
    record.number = 0xFFFFF0u;
    seq_check( rn_window_receive_accept( &w->recv, &record, &slot ) == RN_WINDOW_RECEIVE_DUPLICATE );

    world_synth( w, 64, 4096, 0xFFFFF0ULL );
    action_begin();
    for ( int i = 0; i < 32; i++ )
    {
        seq_check( world_write_single( w, 16 ) );
        seq_check( world_release( w ) );
        seq_check( w->ref.released == w->send.next );
    }
    seq_check( w->send.next > 0x1000000ULL );
    seq_check( w->recv.R > 0x1000000ULL );
    world_check( w );
}

static void probe_exhaustive( world_t * w )
{
    g_seed = 1;
    int acts[6];
    uint32_t sequences = 0;
    memset( acts, 0, sizeof( acts ) );
    for ( ;; )
    {
        world_synth( w, 8, 4096, 0 );
        action_begin();
        for ( int i = 0; i < 6; i++ )
            world_apply( w, acts[i] );
        sequences++;
        int k = 5;
        while ( k >= 0 && ++acts[k] == 6 )
        {
            acts[k] = 0;
            k--;
        }
        if ( k < 0 )
            break;
    }
    rocketnet_unit_check( sequences == 46656 );
}

static uint64_t g_rng = 0;

static uint64_t next_random()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void probe_walk( world_t * w, uint64_t seed, uint64_t first, int steps, int expect_slide )
{
    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.receive_message_buffer_bytes = 65536;          // the walk's reference model is this ring's, not the default's
    config.max_message_bytes = 32768;
    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    rocketnet_unit_check( arena != NULL );
    rn_region_t * region = rn_arena_region( arena, 0 );
    rocketnet_unit_check( region->window == 1024 );
    rocketnet_unit_check( region->ring_bytes == 65536 );
    world_bind( w, region, 1, first );
    g_seed = seed;
    g_rng = seed;
    action_begin();
    for ( int i = 0; i < steps; i++ )
    {
        const uint32_t pick = (uint32_t) ( next_random() % 10 );
        int action;
        if ( pick < 3 )
            action = 0;
        else if ( pick == 3 )
            action = 1;
        else if ( pick == 4 )
            action = 2;
        else if ( pick < 8 )
            action = 3;
        else if ( pick == 8 )
            action = 4;
        else
            action = 5;
        world_apply( w, action );
        // An exhausted region refuses every further send until reset (sticky,
        // tla/RNConnectionMemory.tla ExhaustionOnlyLeadsToDisconnect). Reset it
        // (the model's Reuse) so the walk keeps exercising live window state;
        // without this the guard stops all writes for the rest of the walk.
        if ( rn_region_exhausted( region ) )
        {
            const uint64_t cont = w->ref.next;
            rn_region_carve( region );
            world_bind( w, region, 1, cont );
            action_begin();
        }
    }
    world_check( w );
    if ( expect_slide )
    {
        seq_check( w->ref.u > 1000 );
        seq_check( w->send.next > w->region->window );
    }
    rn_arena_destroy( arena );
}

static void test_window_ring_operation_sequences()
{
    probe_count_edges( &g_world );
    probe_partial_block_count( &g_world );
    probe_floor_at_uprime( &g_world );
    probe_padding( &g_world );
    probe_ack_does_not_free_held( &g_world );
    probe_wrap_and_delivery( &g_world );
    probe_exhaustive( &g_world );
    probe_walk( &g_world, 0x853c49e6748fea9bULL, 0, 200000, 1 );
}

REGISTER_UNIT_TEST( test_window_ring_operation_sequences );
