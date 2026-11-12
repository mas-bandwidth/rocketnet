/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Cases for the survivors of the 2026-11-11 mutation run over the reliable
// window (sdk/rocketnet_window_ack.cpp, sdk/rocketnet_window_send.cpp): each
// pins a rule a surviving mutant broke with no test noticing. The resend gap
// record against exact gaps per class; NoLiveEviction under steady acked
// traffic; the reference ring returned on eviction, filled exactly, and reset
// on a drain; the sent index's backward shift across the wrap; a refused
// resend back at its class's mark; the window's would-number refusals and its
// stall re-arm; the receipt sweep at packet id 1; probing's slot per class.
//
// Time is an argument and the clock starts at 1000.0; nothing here reads a
// real clock, opens a socket or sleeps.

#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"
#include "unit_arena.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_window_send.h"
#include "../sdk/rocketnet_window_ack.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_packet_builder.h"

#define check rocketnet_unit_check

// ----------------------------------------------------------------
// A window over one carved region: the sender's numbering and the ack map
// ----------------------------------------------------------------

struct gaps_window_t
{
    rocketnet_message_config_t config;
    struct rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_window_send_t send;
    rn_window_ack_t ack;
};

// The smallest window the config accepts: W = P = 64, so the sent index has
// 2P = 128 cells and the reference ring 16P = 1024 entries.
static void gaps_small_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->reliable_messages_in_flight = 64;
    config->packets_in_flight = 64;
    config->max_message_bytes = 4096;
    config->receive_message_buffer_bytes = 4096;
}

static void gaps_window_open( gaps_window_t * w )
{
    check( rocketnet_message_validate_config( &w->config ) );
    memset( &w->counters, 0, sizeof( w->counters ) );
    w->arena = rn_unit_arena_create( &w->config, 1, &w->counters );
    check( w->arena != NULL );
    w->region = rn_arena_region( w->arena, 0 );
    rn_region_carve( w->region );
    rn_window_send_init( &w->send, w->region, 0 );
    rn_window_ack_init( &w->ack, &w->send, w->region, &w->config );
}

static void gaps_window_close( gaps_window_t * w )
{
    rn_unit_arena_destroy( w->arena );
    w->arena = NULL;
}

// Numbers one code-2 reliable record (no ring bytes, no descriptor).
static uint64_t gaps_number( gaps_window_t * w )
{
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &w->send, ROCKETNET_MESSAGE_CODE_RELIABLE, 16, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    return number;
}

static uint64_t gaps_counter( const gaps_window_t * w, int counter )
{
    return w->counters.values[counter];
}

static uint64_t gaps_refs( const gaps_window_t * w )
{
    return w->ack.ref_tail - w->ack.ref_head;
}

static uint64_t gaps_entries( const gaps_window_t * w )
{
    return w->ack.pkt_tail - w->ack.pkt_head;
}

// ----------------------------------------------------------------
// The resend gap record (development builds only)
// ----------------------------------------------------------------

#if ROCKETNET_DEV

// rn_resend_gaps_t against gaps chosen by hand, every time a multiple of
// 0.25 s so each gap, early and late is exact. The chunk class's delay is
// capped at reliable_timeout / ROCKETNET_MESSAGE_MIN_RESEND_ATTEMPTS = 0.25 s
// while the small class's moves, so the two classes' delays differ, and the
// update before each resend has a different delay from the update at it.
// Every field of both classes is checked exactly: a record that stops
// updating keeps its sentinels (gap_min 1e30, gap_max 0, early_min 1e30,
// late_max -1e30), a class mix-up moves a value to the other class, and a
// delay from the wrong class or the wrong update moves early or late.
static void test_resend_gap_record_measures_each_class_exactly()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    w.config.message_resend_delay_initial_ms = 250;
    w.config.reliable_message_timeout_ms = 1000;
    gaps_window_open( &w );
    check( w.ack.reliable_timeout == 1.0 );
    check( rn_window_ack_chunk_resend_delay( &w.ack ) == 0.25 );

    // g measures the guards; p and s are small records; c0 and c1 the two chunks of one block

    const uint64_t g = gaps_number( &w );
    const uint64_t p = gaps_number( &w );
    const uint64_t s = gaps_number( &w );
    const uint32_t d = rn_region_send_alloc_span( w.region, 2000 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t c0 = 0, ring_start = 0;
    check( rn_window_send_first_write_block( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 2000, d, &c0, &ring_start ) == 2 );
    const uint64_t c1 = c0 + 1;
    check( rn_window_ack_slot_is_chunk( &w.ack, (uint32_t) ( c0 % w.region->window ) ) );
    check( !rn_window_ack_slot_is_chunk( &w.ack, (uint32_t) ( p % w.region->window ) ) );

    const rn_resend_gaps_t * gaps = &w.ack.gaps;
    uint64_t packet_id = 1;
    const uint64_t chunks[2] = { c0, c1 };

    rn_window_ack_commit_packet( &w.ack, packet_id++, &g, 1, 999.0 );               // a first send before any update

    // the first update: no update before it to measure against, so a resend at it is not measured

    rn_window_ack_gaps_update( &w.ack, 1000.0 );
    rn_window_ack_commit_packet( &w.ack, packet_id++, &g, 1, 1000.0 );
    check( gaps->count[0] == 0 && gaps->count[1] == 0 );

    rn_window_ack_commit_packet( &w.ack, packet_id++, chunks, 2, 1000.0 );
    rn_window_ack_commit_packet( &w.ack, packet_id++, &p, 1, 1000.25 );
    rn_window_ack_commit_packet( &w.ack, packet_id++, &s, 1, 1000.5 );
    check( gaps->count[0] == 0 && gaps->count[1] == 0 );                            // first sends are not resends

    // the update at 1001 sees the small class's delay at 2.0; a second call at
    // the same time is the same update and keeps 2.0, not the 1.25 set between

    w.ack.resend_delay = 2.0;
    rn_window_ack_gaps_update( &w.ack, 1001.0 );
    w.ack.resend_delay = 1.25;
    rn_window_ack_gaps_update( &w.ack, 1001.0 );

    // the update at 1002: small delay 1.0 now, 2.0 at the update before; chunks 0.25 at both

    w.ack.resend_delay = 1.0;
    rn_window_ack_gaps_update( &w.ack, 1002.0 );
    rn_window_ack_commit_packet( &w.ack, packet_id++, &p, 1, 1002.0 );              // gap 1.75: early 0.75, late -0.25
    rn_window_ack_commit_packet( &w.ack, packet_id++, &s, 1, 1002.0 );              // gap 1.5: early 0.5, late -0.5
    rn_window_ack_commit_packet( &w.ack, packet_id++, &c0, 1, 1002.0 );             // gap 2.0: early 1.75, late 1.75

    rn_window_ack_gaps_update( &w.ack, 1003.0 );
    rn_window_ack_commit_packet( &w.ack, packet_id++, &c1, 1, 1003.0 );             // gap 3.0: early 2.75, late 2.75

    // a send between updates has no delay of its own update to measure against

    rn_window_ack_commit_packet( &w.ack, packet_id++, &g, 1, 1003.5 );

    check( gaps->count[0] == 2 );
    check( gaps->gap_min[0] == 1.5 && gaps->gap_max[0] == 1.75 );
    check( gaps->early_min[0] == 0.5 );
    check( gaps->late_max[0] == -0.25 );

    check( gaps->count[1] == 2 );
    check( gaps->gap_min[1] == 2.0 && gaps->gap_max[1] == 3.0 );
    check( gaps->early_min[1] == 1.75 );
    check( gaps->late_max[1] == 2.75 );

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_resend_gap_record_measures_each_class_exactly );

#endif // #if ROCKETNET_DEV

// ----------------------------------------------------------------
// The sent map's reference ring (proofs/tla/RNSentMap.tla NoLiveEviction)
// ----------------------------------------------------------------

// A connection that always has a packet in flight and has every packet
// acknowledged one write later never drains its sent map, so the reference
// ring is returned only as acknowledged entries are reclaimed from its head.
// Past 16P references, a ring that kept them would look full and evict the
// live entry on every write: nothing may be evicted, and the ring holds only
// the two packets in flight (one reliable number, one type-6 receipt serial).
static void test_sent_map_steady_acked_traffic_evicts_nothing()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    gaps_window_open( &w );

    const uint32_t max_refs = w.region->sent_ref_entries;
    check( max_refs == 16 * w.region->packets );

    const uint32_t writes = 3 * max_refs;
    for ( uint32_t k = 0; k < writes; k++ )
    {
        const double now = 1000.0 + (double) k / 64.0;
        const uint64_t number = gaps_number( &w );
        const uint32_t serial = k + 1;
        rn_window_ack_commit_packet( &w.ack, 2 * (uint64_t) k + 1, &number, 1, now );
        rn_window_ack_commit_receipt_packet( &w.ack, 2 * (uint64_t) k + 2, &serial, 1, now );

        if ( k > 0 )
        {
            const uint64_t previous[2] = { 2 * (uint64_t) k - 1, 2 * (uint64_t) k };
            check( rn_window_ack_on_acks( &w.ack, previous, 2, now + 1.0 / 128.0 ) == 2 );
        }

        check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 0 );
        check( gaps_entries( &w ) == 2 );                                           // never drained
        check( gaps_refs( &w ) == 2 );
        check( w.send.U == number );                                                // every earlier number acknowledged
    }

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_sent_map_steady_acked_traffic_evicts_nothing );

// An entry evicted from the packet ring gives its references back: with no
// acks, P packets of one reference each keep exactly P references, however
// many are written after them. And a ring filled exactly to 16P references
// evicts nothing until a write needs more.
static void test_sent_map_eviction_returns_references()
{
    // the packet ring evicts its oldest entry on every write past P

    {
        gaps_window_t w;
        gaps_small_config( &w.config );
        gaps_window_open( &w );

        const uint32_t P = w.region->packets;
        const uint32_t W = w.region->window;
        uint64_t numbers[64];
        check( W == 64 );
        for ( uint32_t i = 0; i < W; i++ )
        {
            numbers[i] = gaps_number( &w );
        }

        const uint32_t writes = 4 * w.region->sent_ref_entries;
        for ( uint32_t k = 0; k < writes; k++ )
        {
            rn_window_ack_commit_packet( &w.ack, (uint64_t) k + 1, &numbers[k % W], 1, 1000.0 + (double) k / 64.0 );

            const uint64_t live = k + 1 < P ? k + 1 : P;
            check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == k + 1 - live );
            check( gaps_entries( &w ) == live );
            check( gaps_refs( &w ) == live );
        }

        gaps_window_close( &w );
    }

    // P packets of 16 references fill the 16P ring exactly: no eviction; the next write evicts one

    {
        gaps_window_t w;
        gaps_small_config( &w.config );
        gaps_window_open( &w );

        const uint32_t P = w.region->packets;
        const uint32_t W = w.region->window;
        uint64_t numbers[64];
        check( W == 64 );
        for ( uint32_t i = 0; i < W; i++ )
        {
            numbers[i] = gaps_number( &w );
        }

        uint64_t carried[16];
        for ( uint32_t k = 0; k <= P; k++ )
        {
            for ( uint32_t i = 0; i < 16; i++ )
            {
                carried[i] = numbers[( 16 * k + i ) % W];
            }
            rn_window_ack_commit_packet( &w.ack, (uint64_t) k + 1, carried, 16, 1000.0 + (double) k / 64.0 );

            if ( k < P )
            {
                check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 0 );
                check( gaps_refs( &w ) == 16 * (uint64_t) ( k + 1 ) );
            }
        }

        check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 1 );
        check( gaps_refs( &w ) == w.region->sent_ref_entries );
        check( gaps_entries( &w ) == P );

        gaps_window_close( &w );
    }
}

REGISTER_UNIT_TEST( test_sent_map_eviction_returns_references );

// A drained sent map (every entry acknowledged and reclaimed) starts its
// reference ring again at 0, both ends. Bursts of four packets, each burst
// acknowledged whole, pass 3 x 16P references through the ring: the loop is
// bounded, each burst checks the ring is empty after its drain and that its
// numbers were acknowledged, so a ring end left behind fails at the first
// drain rather than reaching the commit's eviction loop with an empty map.
static void test_sent_map_drain_resets_the_reference_ring()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    gaps_window_open( &w );

    const uint32_t max_refs = w.region->sent_ref_entries;
    const uint32_t per_burst = 4 * 4;
    const uint32_t bursts = 3 * max_refs / per_burst;
    check( bursts * per_burst >= 16 * w.region->window );

    uint64_t packet_id = 1;
    for ( uint32_t b = 0; b < bursts; b++ )
    {
        const double now = 1000.0 + (double) b / 16.0;
        const uint64_t first_packet = packet_id;
        for ( int p = 0; p < 4; p++ )
        {
            uint64_t numbers[4];
            for ( int i = 0; i < 4; i++ )
            {
                numbers[i] = gaps_number( &w );
            }
            rn_window_ack_commit_packet( &w.ack, packet_id++, numbers, 4, now );
        }
        check( gaps_refs( &w ) == per_burst );

        const uint64_t ids[4] = { first_packet, first_packet + 1, first_packet + 2, first_packet + 3 };
        check( rn_window_ack_on_acks( &w.ack, ids, 4, now + 1.0 / 32.0 ) == 4 );

        check( w.ack.pkt_head == w.ack.pkt_tail );                                  // drained
        check( w.ack.ref_head == 0 && w.ack.ref_tail == 0 );
        check( w.send.U == w.send.next );                                           // progress: the burst's numbers acknowledged
        check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED ) == 0 );
    }

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_sent_map_drain_resets_the_reference_ring );

// ----------------------------------------------------------------
// The sent index's backward shift across the wrap
// (proofs/tla/RNOpenAddress.tla EraseStep, ClearLast TRUE)
// ----------------------------------------------------------------

// The index's home cell for a packet id: the same arithmetic as
// rn_sent_index_hash, kept here as an independent oracle. The layout checks
// below fail if the two ever disagree.
static uint32_t gaps_sent_index_home( uint64_t packet_id, uint32_t cap )
{
    uint64_t h = packet_id * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    return (uint32_t) ( h % cap );
}

static uint64_t gaps_id_with_home( uint32_t home, uint32_t cap, uint64_t after )
{
    for ( uint64_t id = after + 1; id < after + 1000000; id++ )
    {
        if ( gaps_sent_index_home( id, cap ) == home )
        {
            return id;
        }
    }
    check( false && "no packet id with that home" );
    return 0;
}

// The witness of MCRNOpenAddressBrokenNoWrapTest.cfg on the sent index: a
// cluster that crosses the last cell. k1 and k2 both have home N - 1, so k2
// sits in cell 0; k3 has home 0 and sits in cell 1. Erasing k1 leaves the
// hole at N - 1 with the scan at cell 0 after the wrap: k2's home N - 1 is not
// cyclically in (hole, scan] = (N - 1, 0], so k2 moves back into the hole, then
// k3 into cell 0. Left in place, k2 is behind an empty home cell: its ack is
// ignored and its number resent. Run at N = 5 (MCRNOpenAddressClearLast.cfg),
// 8, and the carved 2P = 128 of the smallest config; the smaller indexes use
// the carved one's first cells.
static void test_sent_index_backward_shift_across_the_wrap()
{
    const uint32_t caps[3] = { 5, 8, 0 };
    for ( int c = 0; c < 3; c++ )
    {
        gaps_window_t w;
        gaps_small_config( &w.config );
        gaps_window_open( &w );

        rn_region_t * region = w.region;
        check( region->sent_index_entries == 128 );
        if ( caps[c] != 0 )
        {
            region->sent_index_entries = caps[c];
        }
        const uint32_t N = region->sent_index_entries;

        const uint64_t k1 = gaps_id_with_home( N - 1, N, 0 );
        const uint64_t k2 = gaps_id_with_home( N - 1, N, k1 );
        const uint64_t k3 = gaps_id_with_home( 0, N, k2 );

        const uint64_t n1 = gaps_number( &w );
        const uint64_t n2 = gaps_number( &w );
        const uint64_t n3 = gaps_number( &w );
        rn_window_ack_commit_packet( &w.ack, k1, &n1, 1, 1000.0 );
        rn_window_ack_commit_packet( &w.ack, k2, &n2, 1, 1000.0 );
        rn_window_ack_commit_packet( &w.ack, k3, &n3, 1, 1000.0 );

        check( region->sent_index[N - 1].used && region->sent_index[N - 1].packet_id == k1 );
        check( region->sent_index[0].used && region->sent_index[0].packet_id == k2 );
        check( region->sent_index[1].used && region->sent_index[1].packet_id == k3 );

        check( rn_window_ack_on_ack( &w.ack, k1, 1000.125 ) == 1 );

        check( region->sent_index[N - 1].used && region->sent_index[N - 1].packet_id == k2 );
        check( region->sent_index[0].used && region->sent_index[0].packet_id == k3 );
        check( !region->sent_index[1].used );

        check( rn_window_ack_on_ack( &w.ack, k2, 1000.125 ) == 1 );
        check( rn_window_ack_on_ack( &w.ack, k3, 1000.125 ) == 1 );
        check( w.send.U == w.send.next );
        check( w.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
        for ( uint32_t i = 0; i < N; i++ )
        {
            check( !region->sent_index[i].used );
        }

        gaps_window_close( &w );
    }
}

REGISTER_UNIT_TEST( test_sent_index_backward_shift_across_the_wrap );

// ----------------------------------------------------------------
// The receipt sweep and probing's slot
// ----------------------------------------------------------------

// A transport's first packet id is 1: a live type-5 receipt entry under it
// stays unresolved until its deadline (resend_delay_max after its send), and
// resolves LOSS at it.
static void test_receipt_sweep_resolves_packet_id_one_at_its_deadline()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    gaps_window_open( &w );

    const uint32_t serials[2] = { 7, 8 };
    rn_window_ack_commit_receipt_packet( &w.ack, 1, &serials[0], 1, 1000.0 );
    rn_window_ack_commit_receipt_packet( &w.ack, 2, &serials[1], 1, 1000.0 );
    const double deadline = 1000.0 + w.ack.resend_delay_max;

    rn_window_ack_receipt_sweep( &w.ack, deadline - 0.5 );
    check( w.ack.receipt_tail == 0 );
    check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ) == 0 );

    rn_window_ack_receipt_sweep( &w.ack, deadline );
    check( w.ack.receipt_tail == 2 );
    check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS ) == 2 );

    rn_window_ack_receipt_begin_update( &w.ack );
    rn_window_ack_receipt_end_update( &w.ack );
    rocketnet_message_receipt_t out[4];
    check( rn_window_ack_receipts( &w.ack, out, 4 ) == 2 );
    check( out[0].serial == 7 && out[0].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    check( out[1].serial == 8 && out[1].status == ROCKETNET_RECEIPT_STATUS_LOSS );

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_receipt_sweep_resolves_packet_id_one_at_its_deadline );

// The receipt ring full between the update that made its events visible and
// the read: events emitted after end_update (send_packets' losses) push the
// oldest visible ones out, and the read returns only the live ones, oldest
// first, never a slot already written over.
static void test_receipt_read_skips_events_the_full_ring_dropped()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    gaps_window_open( &w );

    const uint32_t capacity = w.ack.region->receipt_entries;
    check( capacity >= 4 && capacity <= 1024 );
    rn_window_ack_receipt_begin_update( &w.ack );
    for ( uint32_t s = 1; s <= capacity; s++ )
    {
        rn_window_ack_receipt_emit_loss( &w.ack, s );
    }
    rn_window_ack_receipt_end_update( &w.ack );

    const uint32_t late = 3;
    for ( uint32_t s = capacity + 1; s <= capacity + late; s++ )
    {
        rn_window_ack_receipt_emit_loss( &w.ack, s );
    }
    check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL ) == late );

    static rocketnet_message_receipt_t out[1024 + 8];
    const int n = rn_window_ack_receipts( &w.ack, out, (int) capacity + 8 );
    check( n == (int) ( capacity - late ) );
    for ( int i = 0; i < n && i < (int) capacity; i++ )
    {
        check( out[i].serial == (uint32_t) i + late + 1 );
        check( out[i].status == ROCKETNET_RECEIPT_STATUS_LOSS );
    }

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_receipt_read_skips_events_the_full_ring_dropped );

// Probing's slot is U's only in the resend sequence U is linked in: asked for
// the other class, or once U is acknowledged, there is none.
static void test_probe_slot_is_u_in_its_own_class_only()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    gaps_window_open( &w );

    check( rn_window_ack_probe_slot( &w.ack, false ) == NULL );                    // nothing numbered

    const uint64_t u = gaps_number( &w );
    rn_window_ack_commit_packet( &w.ack, 1, &u, 1, 1000.0 );
    const rn_send_number_t * slot = &w.region->send_numbers[u % w.region->window];

    check( rn_window_ack_probe_slot( &w.ack, false ) == slot );
    check( rn_window_ack_probe_slot( &w.ack, true ) == NULL );

    check( rn_window_ack_on_ack( &w.ack, 1, 1000.125 ) == 1 );
    check( w.send.U == w.send.next );
    check( rn_window_ack_probe_slot( &w.ack, false ) == NULL );
    check( rn_window_ack_probe_slot( &w.ack, true ) == NULL );

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_probe_slot_is_u_in_its_own_class_only );

// ----------------------------------------------------------------
// The window's admit test (proofs/tla/RNWindow.tla WouldNumber)
// ----------------------------------------------------------------

// rn_window_send_would_number refuses exactly what the first write would,
// for each reason, and changes nothing: a full window refuses every reliable
// code and never an unreliable one; a block asks for all its numbers, so one
// free number refuses a 2-fragment block; a full ring refuses the holdable
// codes (singles and blocks) and never code 2, which holds no ring bytes.
// rn_window_send_stall_unless_numbered answers the same, marks a refusal and
// counts it once.
static void test_would_number_refuses_each_reason()
{
    const int reliable[3] = { ROCKETNET_MESSAGE_CODE_RELIABLE, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED };

    // the window: one number free, then none

    {
        gaps_window_t w;
        gaps_small_config( &w.config );
        gaps_window_open( &w );

        const uint32_t W = w.region->window;
        for ( uint32_t i = 0; i + 1 < W; i++ )
        {
            gaps_number( &w );
        }

        for ( int c = 0; c < 3; c++ )
        {
            check( rn_window_send_would_number( &w.send, reliable[c], 16, 1 ) == 1 );
            check( rn_window_send_would_number( &w.send, reliable[c], 1300, 2 ) == 0 );     // two numbers, one free
        }

        gaps_number( &w );
        check( w.send.next - w.send.U_prime == W );

        for ( int c = 0; c < 3; c++ )
        {
            check( rn_window_send_would_number( &w.send, reliable[c], 16, 1 ) == 0 );
        }
        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 16, 1 ) == 1 );
        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED, 16, 1 ) == 1 );
        check( w.send.stalled == 0 && w.send.next == W );
        check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) == 0 );

        check( rn_window_send_stall_unless_numbered( &w.send, ROCKETNET_MESSAGE_CODE_UNRELIABLE, 16, 1 ) == 1 );
        check( w.send.stalled == 0 );
        check( rn_window_send_stall_unless_numbered( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 16, 1 ) == 0 );
        check( rn_window_send_stall_unless_numbered( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, 1 ) == 0 );
        check( w.send.stalled == 1 );
        check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) == 1 );

        gaps_window_close( &w );
    }

    // the ring: B = 4096 bytes; two 1415-byte singles take 2 x 1424, leaving 1248 before the end

    {
        gaps_window_t w;
        gaps_small_config( &w.config );
        gaps_window_open( &w );
        check( w.region->ring_bytes == 4096 );

        uint64_t number = 0, ring_start = 0;
        for ( int i = 0; i < 2; i++ )
        {
            check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
        }
        check( w.send.ring_next == 2 * 1424 );

        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, 1 ) == 0 );
        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED, ROCKETNET_MESSAGE_SINGLE_BYTES, 1 ) == 0 );
        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE, ROCKETNET_MESSAGE_SINGLE_BYTES, 1 ) == 1 );
        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1300, 2 ) == 0 );   // span 1312 > 1248
        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1200, 2 ) == 1 );   // span 1200 fits
        check( rn_window_send_would_number( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1200, 1 ) == 1 );   // one number, 1200 bytes fits

        // the answers match the first writes they stand for

        check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_STALL );
        check( rn_window_send_first_write_block( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1300, RN_DESCRIPTOR_NONE, &number, &ring_start ) == 0 );
        check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) == 1 );
        check( w.send.next == 2 );

        gaps_window_close( &w );
    }
}

REGISTER_UNIT_TEST( test_would_number_refuses_each_reason );

// "A failed first write counts once until U' moves or one succeeds": a stall
// is counted, a later first write that succeeds (a single, then a block)
// re-arms it, and the next refusal is counted again. U' never moves here.
static void test_window_stall_rearmed_by_a_first_write()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    gaps_window_open( &w );

    uint64_t number = 0, ring_start = 0;
    for ( int i = 0; i < 2; i++ )
    {
        check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    }
    const uint64_t u_prime = w.send.U_prime;

    // the ring has 1248 bytes before its end: a 1415-byte single does not fit
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_STALL );
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_STALL );
    check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) == 1 );       // once per stall

    // a code-2 single takes no ring bytes: it succeeds and re-arms the count
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE, 16, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    check( w.send.stalled == 0 );
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_STALL );
    check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) == 2 );

    // a 1200-byte block (span 1200) fits the 1248: it succeeds and re-arms the count
    check( rn_window_send_first_write_block( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 1200, RN_DESCRIPTOR_NONE, &number, &ring_start ) == 2 );
    check( w.send.stalled == 0 );
    check( rn_window_send_first_write( &w.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, ROCKETNET_MESSAGE_SINGLE_BYTES, RN_DESCRIPTOR_NONE, &number, &ring_start ) == RN_WINDOW_SEND_STALL );
    check( gaps_counter( &w, ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL ) == 3 );
    check( w.send.U_prime == u_prime );

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_window_stall_rearmed_by_a_first_write );

// An ack can only name a number the window numbered: before the first write,
// number 0 (which the carve's zeroed slot 0 also reads as) is refused.
static void test_mark_acked_refuses_a_number_not_yet_numbered()
{
    gaps_window_t w;
    gaps_small_config( &w.config );
    gaps_window_open( &w );

    check( w.send.next == 0 && w.region->send_numbers[0].number == 0 );
    check( rn_window_send_mark_acked( &w.send, 0 ) == 0 );
    check( w.region->send_numbers[0].acked == 0 );
    check( w.send.U == 0 && w.send.U_prime == 0 );

    const uint64_t n = gaps_number( &w );
    check( rn_window_send_mark_acked( &w.send, n + 1 ) == 0 );
    check( rn_window_send_mark_acked( &w.send, n ) == 1 );
    check( w.send.U == n + 1 );

    gaps_window_close( &w );
}

REGISTER_UNIT_TEST( test_mark_acked_refuses_a_number_not_yet_numbered );

// ----------------------------------------------------------------
// A refused resend goes back to its class's mark
// (proofs/tla/RNResendFifoLayout.tla PushMark, RNResendTiming.tla WriteRefused).
// Development builds only: the pass reads each packet's numbers through
// rn_packet_builder_numbers.
// ----------------------------------------------------------------

#if ROCKETNET_DEV

struct gaps_ordered_t
{
    rocketnet_message_config_t config;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    uint64_t next_packet_id;
};

static void gaps_ordered_open( gaps_ordered_t * f )
{
    rocketnet_message_default_config( &f->config );
    check( rocketnet_message_validate_config( &f->config ) );
    f->arena = rn_unit_arena_create( &f->config, 1, NULL );
    check( f->arena != NULL );
    f->region = rn_arena_region( f->arena, 0 );
    rn_region_carve( f->region );
    check( rn_ordered_init( &f->ordered, f->region, &f->config, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, ROCKETNET_MESSAGE_SINGLE_BYTES ) );
    f->next_packet_id = 1;
}

// Numbers one 16-byte reliable-ordered record and sends it in its own packet.
static uint64_t gaps_ordered_send( gaps_ordered_t * f, double now )
{
    const uint32_t d = rn_region_send_alloc( f->region, 16 );
    check( d != RN_DESCRIPTOR_NONE );
    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &f->ordered.send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );
    rn_window_ack_commit_packet( &f->ordered.ack, f->next_packet_id++, &number, 1, now );
    return number;
}

// One write pass at now through the product walk; each packet it writes is
// sent (committed) or refused by the transport (failed). Returns the numbers.
static uint32_t gaps_ordered_pass( gaps_ordered_t * f, double now, bool refuse, uint64_t * out, uint32_t max_numbers )
{
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * payloads[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int i = 0; i < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; i++ )
    {
        payloads[i] = frames[i];
    }

    const int written = rn_ordered_write( &f->ordered, now, payloads, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    uint32_t total = 0;
    for ( int p = 0; p < written; p++ )
    {
        uint32_t count = 0;
        const uint64_t * numbers = rn_packet_builder_numbers( &f->ordered.builder, p, &count );
        check( numbers != NULL || count == 0 );
        check( total + count <= max_numbers );
        for ( uint32_t i = 0; i < count; i++ )
        {
            out[total++] = numbers[i];
        }

        if ( refuse )
        {
            rn_ordered_packet_failed( &f->ordered, p, now );
        }
        else
        {
            rn_ordered_commit( &f->ordered, p, f->next_packet_id++, now );
        }
    }

    return total;
}

// While probing holds every number but U, U is resent alone and goes to the
// tail of its class, behind numbers held on the wire. When the transport
// refuses U's next resend, U goes back on the wire as its class's mark, ahead
// of them, so the next pass resends it even after the resend delay rises past
// the held numbers' last sends (an RTT sample between the passes): the walk of
// the numbers on the wire stops at the first one not due, and U is first.
static void test_refused_resend_returns_to_its_class_mark()
{
    gaps_ordered_t f;
    gaps_ordered_open( &f );
    rn_window_ack_t * ack = &f.ordered.ack;
    check( ack->reliable_timeout == 0.0 );                                         // probe pacing on
    check( rn_window_ack_seed_rtt( ack, 0.050 ) == 1 );                            // resend delay 330 ms

    const double t0 = 1000.0;
    const uint64_t r0 = gaps_ordered_send( &f, t0 );
    const uint64_t r1 = gaps_ordered_send( &f, t0 );
    const uint64_t r2 = gaps_ordered_send( &f, t0 );
    check( f.ordered.send.U == r0 );
    const uint32_t W = f.region->window;

    uint64_t out[16];
    check( gaps_ordered_pass( &f, t0 + 0.34, false, out, 16 ) == 3 );             // all three resent, in order

    // probing from t0 + 1.0: only U = r0 is resent, and it goes to the tail

    check( rn_window_ack_probing( ack, t0 + 1.02 ) );
    check( gaps_ordered_pass( &f, t0 + 1.02, false, out, 16 ) == 1 && out[0] == r0 );
    check( rn_window_ack_resend_sent( ack, false ) == (uint32_t) ( r1 % W ) );

    // U's next resend is refused: U becomes the mark, ahead of r1 and r2

    check( gaps_ordered_pass( &f, t0 + 1.36, true, out, 16 ) == 1 && out[0] == r0 );
    check( rn_window_ack_resend_sent( ack, false ) == (uint32_t) ( r0 % W ) );
    check( rn_window_ack_resend_next( ack, false, (uint32_t) ( r0 % W ) ) == (uint32_t) ( r1 % W ) );
    check( rn_window_ack_resend_next( ack, false, (uint32_t) ( r1 % W ) ) == (uint32_t) ( r2 % W ) );

    // the delay rises past r1's and r2's last send (t0 + 0.34): they are not due, U is

    ack->resend_delay = 1.5;
    check( gaps_ordered_pass( &f, t0 + 1.37, false, out, 16 ) == 1 );
    check( out[0] == r0 );
    check( ack->error == ROCKETNET_DISCONNECT_REASON_NONE );

    rn_unit_arena_destroy( f.arena );
}

REGISTER_UNIT_TEST( test_refused_resend_returns_to_its_class_mark );

#endif // #if ROCKETNET_DEV
