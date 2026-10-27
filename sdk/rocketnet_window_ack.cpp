/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_window_ack.h"
#include "rocketnet_message_object.h"
#include "rocketnet_packet_builder.h"
#include "rocketnet_platform.h"

#include <math.h>
#include <string.h>

#if defined(ROCKETNET_ACK_COST_TEST_HOOKS)
// Unit-library instrumentation only: no production state or per-ACK writes.
static thread_local rn_ack_cost_t rn_ack_cost;
void rn_window_ack_cost_reset() { memset( &rn_ack_cost, 0, sizeof(rn_ack_cost) ); }
rn_ack_cost_t rn_window_ack_cost_read() { return rn_ack_cost; }
#endif

// Layer 3 of the message layer: the sent-packet map, the resend FIFO, RTT,
// RTT estimation, resend delay, failed-send recovery, and reliable timeout (docs/CHANNELS.md
// section 6.4; TLA+ actions Init, WritePacket, WriteOldest, ReceiveAck and
// invariants TablesBounded and EveryReliableAckedAndDelivered in
// tla/RNWindow.tla). Resend links name {message number, descriptor generation,
// schedule epoch} (tla/RNChannelQueues.tla). A compact of acked holes is not
// done on the walk: capacity stays W and walk_visits still counts them.

// tla/RNWindow.tla WritePacket
static void rn_window_ack_count( const rn_region_t * region, int counter )
{
    if ( region && region->counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &region->counters->values[counter], 1 );
}

// ----------------------------------------------------------------
//     Two intrusive resend FIFOs over the one region->resend_fifo
// ----------------------------------------------------------------

// The two FIFOs share the W links: a number is in at most one of them, the
// one its class picks when it is linked (tla/RNResendTiming.tla, one FIFO per
// class). Unlinking needs no class: a slot with no prev is the head of the
// FIFO whose head it is, and a slot with no next the tail of the FIFO whose
// tail it is, so a slot whose descriptor was freed still unlinks exactly.

bool rn_window_ack_slot_is_chunk( const rn_window_ack_t * ack, uint32_t slot_index )
{
    const rn_region_t * region = ack->region;
    const rn_send_number_t * slot = &region->send_numbers[slot_index];
    if ( slot->descriptor == RN_DESCRIPTOR_NONE || slot->descriptor >= region->queue )
        return false;
    const rn_send_descriptor_t * descriptor = &region->send_descriptors[slot->descriptor];
    return descriptor->in_use && descriptor->fragments > 1;
}

static bool rn_resend_fifo_is_linked( const rn_window_ack_t * ack, uint32_t slot )
{
    const rn_resend_link_t * link = &ack->region->resend_fifo[slot];
    return ack->resend_head == slot || ack->chunk_head == slot || link->prev != RN_RESEND_NONE || link->next != RN_RESEND_NONE;
}

static void rn_resend_fifo_unlink( rn_window_ack_t * ack, uint32_t slot )
{
    if ( !rn_resend_fifo_is_linked( ack, slot ) )
        return;

    rn_resend_link_t * fifo = ack->region->resend_fifo;
    const uint32_t prev = fifo[slot].prev;
    const uint32_t next = fifo[slot].next;

    if ( prev != RN_RESEND_NONE )
        fifo[prev].next = next;
    else if ( ack->resend_head == slot )
        ack->resend_head = next;
    else
        ack->chunk_head = next;

    if ( next != RN_RESEND_NONE )
        fifo[next].prev = prev;
    else if ( ack->resend_tail == slot )
        ack->resend_tail = prev;
    else
        ack->chunk_tail = prev;

    fifo[slot].prev = RN_RESEND_NONE;
    fifo[slot].next = RN_RESEND_NONE;
    fifo[slot].generation = 0;
    fifo[slot].owner_epoch = 0;
    rocketnet_assert( ack->resend_count > 0 );
    ack->resend_count--;
}

// tla/RNChannelQueues.tla Resend, Ack and StaleAck. Names the link with the live descriptor's generation and this connection's
// schedule epoch. Generation 0 is never a live name.
static void rn_resend_fifo_stamp( rn_window_ack_t * ack, uint32_t slot )
{
    rn_region_t * region = ack->region;
    rn_resend_link_t * link = &region->resend_fifo[slot];
    link->owner_epoch = region->schedule_epoch;
    link->generation = 0;
    const rn_send_number_t * number = &region->send_numbers[slot];
    if ( number->descriptor == RN_DESCRIPTOR_NONE || number->descriptor >= region->queue )
        return;
    const rn_send_descriptor_t * descriptor = &region->send_descriptors[number->descriptor];
    if ( descriptor->in_use && descriptor->generation != 0 )
        link->generation = descriptor->generation;
}

// Unlinks only the named generation of this number in the current epoch.
// A stale generation or a previous connection's epoch leaves the FIFO as it is.
static bool rn_resend_fifo_unlink_named( rn_window_ack_t * ack, uint32_t slot, uint64_t number, uint32_t generation )
{
    if ( generation == 0 || !rn_resend_fifo_is_linked( ack, slot ) )
        return false;
    rn_region_t * region = ack->region;
    const rn_send_number_t * slot_number = &region->send_numbers[slot];
    const rn_resend_link_t * link = &region->resend_fifo[slot];
    if ( slot_number->number != number || link->generation != generation || link->owner_epoch != region->schedule_epoch )
        return false;
    if ( slot_number->descriptor != RN_DESCRIPTOR_NONE && slot_number->descriptor < region->queue )
    {
        const rn_send_descriptor_t * descriptor = &region->send_descriptors[slot_number->descriptor];
        if ( descriptor->in_use && descriptor->generation != generation )
            return false;
    }
    rn_resend_fifo_unlink( ack, slot );
    return true;
}

bool rn_window_ack_cancel_resend( rn_window_ack_t * ack, uint64_t number, uint32_t generation )
{
    if ( !ack || !ack->region || ack->region->window == 0 )
        return false;
    const uint32_t slot = (uint32_t) ( number & ( ack->region->window - 1 ) );
    return rn_resend_fifo_unlink_named( ack, slot, number, generation );
}

static void rn_resend_fifo_push_tail( rn_window_ack_t * ack, uint32_t slot )
{
    if ( rn_resend_fifo_is_linked( ack, slot ) )
        rn_resend_fifo_unlink( ack, slot );

    const bool chunk = rn_window_ack_slot_is_chunk( ack, slot );
    uint32_t * head = chunk ? &ack->chunk_head : &ack->resend_head;
    uint32_t * tail = chunk ? &ack->chunk_tail : &ack->resend_tail;

    rn_resend_link_t * fifo = ack->region->resend_fifo;
    fifo[slot].prev = *tail;
    fifo[slot].next = RN_RESEND_NONE;

    if ( *tail != RN_RESEND_NONE )
        fifo[*tail].next = slot;
    else
        *head = slot;

    *tail = slot;
    ack->resend_count++;
    rn_resend_fifo_stamp( ack, slot );
}

static void rn_resend_fifo_push_head( rn_window_ack_t * ack, uint32_t slot )
{
    if ( rn_resend_fifo_is_linked( ack, slot ) )
        rn_resend_fifo_unlink( ack, slot );

    const bool chunk = rn_window_ack_slot_is_chunk( ack, slot );
    uint32_t * head = chunk ? &ack->chunk_head : &ack->resend_head;
    uint32_t * tail = chunk ? &ack->chunk_tail : &ack->resend_tail;

    rn_resend_link_t * fifo = ack->region->resend_fifo;
    fifo[slot].prev = RN_RESEND_NONE;
    fifo[slot].next = *head;

    if ( *head != RN_RESEND_NONE )
        fifo[*head].prev = slot;
    else
        *tail = slot;

    *head = slot;
    ack->resend_count++;
    rn_resend_fifo_stamp( ack, slot );
}

void rn_window_ack_park( rn_window_ack_t * ack, uint32_t after, uint32_t slot_index )
{
    rocketnet_assert( ack );
    if ( after == RN_RESEND_NONE )
    {
        rn_resend_fifo_push_head( ack, slot_index );
        return;
    }

    rocketnet_assert( rn_window_ack_slot_is_chunk( ack, after ) == rn_window_ack_slot_is_chunk( ack, slot_index ) );
    if ( rn_resend_fifo_is_linked( ack, slot_index ) )
        rn_resend_fifo_unlink( ack, slot_index );

    const bool chunk = rn_window_ack_slot_is_chunk( ack, slot_index );
    uint32_t * tail = chunk ? &ack->chunk_tail : &ack->resend_tail;
    rn_resend_link_t * fifo = ack->region->resend_fifo;
    const uint32_t next = fifo[after].next;
    fifo[slot_index].prev = after;
    fifo[slot_index].next = next;
    fifo[after].next = slot_index;
    if ( next != RN_RESEND_NONE )
        fifo[next].prev = slot_index;
    else
        *tail = slot_index;
    ack->resend_count++;
    rn_resend_fifo_stamp( ack, slot_index );
}

// ----------------------------------------------------------------
//       Open-addressed sent_index (2P entries, O(1) lookup)
// ----------------------------------------------------------------

static uint32_t rn_sent_index_hash( uint64_t packet_id, uint32_t cap )
{
    uint64_t h = packet_id * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    return (uint32_t) ( h % cap );
}

static bool rn_sent_index_find( const rn_region_t * region, uint64_t packet_id, uint32_t * out_slot )
{
    if ( packet_id == 0 || region->sent_index_entries == 0 )
        return false;

    const uint32_t cap = region->sent_index_entries;
    uint32_t slot = rn_sent_index_hash( packet_id, cap );
    for ( uint32_t step = 0; step < cap; step++ )
    {
        const rn_sent_index_t * entry = &region->sent_index[slot];
        if ( !entry->used )
            return false;
        if ( entry->packet_id == packet_id )
        {
            *out_slot = slot;
            return true;
        }
        slot = ( slot + 1 ) % cap;
    }
    return false;
}

static void rn_sent_index_insert( rn_region_t * region, uint64_t packet_id, uint32_t entry_index )
{
    rocketnet_assert( packet_id != 0 );
    const uint32_t cap = region->sent_index_entries;
    uint32_t slot = rn_sent_index_hash( packet_id, cap );
    for ( uint32_t step = 0; step < cap; step++ )
    {
        rn_sent_index_t * idx = &region->sent_index[slot];
        if ( !idx->used || idx->packet_id == packet_id )
        {
            idx->packet_id = packet_id;
            idx->entry = entry_index;
            idx->used = 1;
            return;
        }
        slot = ( slot + 1 ) % cap;
    }
    rocketnet_assert( false && "sent_index 2P table exhausted" );
}

static void rn_sent_index_remove_at( rn_region_t * region, uint32_t slot )
{
    const uint32_t cap = region->sent_index_entries;
    uint32_t empty = slot;
    uint32_t j = ( empty + 1 ) % cap;
    while ( region->sent_index[j].used )
    {
        const uint32_t home = rn_sent_index_hash( region->sent_index[j].packet_id, cap );
        const bool move = ( empty < j ) ? ( home <= empty || home > j ) : ( home <= empty && home > j );
        if ( move )
        {
            region->sent_index[empty] = region->sent_index[j];
            empty = j;
        }
        j = ( j + 1 ) % cap;
    }
    region->sent_index[empty].packet_id = 0;
    region->sent_index[empty].entry = 0;
    region->sent_index[empty].used = 0;
}

static void rn_sent_index_remove( rn_region_t * region, uint64_t packet_id )
{
    uint32_t slot = 0;
    if ( rn_sent_index_find( region, packet_id, &slot ) )
        rn_sent_index_remove_at( region, slot );
}

// ----------------------------------------------------------------
//      Sent-packet ring reclamation & oldest-unacked tracking
// ----------------------------------------------------------------

// tla/RNReceipts.tla RetireLostPacket: reclaims the retired head of the
// sent-packet ring, dropping a lost packet's references from the map.
static void rn_window_ack_reclaim_head( rn_window_ack_t * ack )
{
    rn_region_t * region = ack->region;
    const uint32_t P = region->packets;
    while ( ack->pkt_head < ack->pkt_tail )
    {
        rn_sent_packet_t * head = &region->sent_packets[ack->pkt_head % P];
        if ( head->packet_id != 0 )
            break;
        ack->ref_head += head->ref_count + head->unreliable_receipts;
        head->ref_first = 0;
        head->ref_count = 0;
        head->unreliable_receipts = 0;
        ack->pkt_head++;
    }
    if ( ack->pkt_head == ack->pkt_tail )
    {
        ack->ref_head = 0;
        ack->ref_tail = 0;
    }
}

// MESSAGES m9.3: the reliable timeout runs from the oldest unacknowledged
// number's own first send. For a chunk that is its own, never its block's:
// a block paced out by the send budget over longer than the timeout keeps
// the connection while each chunk is acked inside it. A number not yet sent
// has no clock: has_oldest stays 0 until rn_window_ack_record_first_send.
static void rn_window_ack_refresh_oldest( rn_window_ack_t * ack )
{
    const rn_window_send_t * send = ack->send;
    const rn_region_t * region = ack->region;
    if ( ack->has_oldest && ack->oldest_number == send->U && send->U < send->next )
        return;

    ack->oldest_number = send->U;
    if ( send->U >= send->next )
    {
        ack->has_oldest = 0;
        ack->oldest_first_send = 0.0;
        return;
    }

    const uint32_t slot_index = (uint32_t) ( send->U & ( region->window - 1 ) );
    if ( rn_send_number_never_sent( region->send_numbers[slot_index].last_send ) )
    {
        ack->has_oldest = 0;
        ack->oldest_first_send = 0.0;
        return;
    }

    ack->has_oldest = 1;
    ack->oldest_first_send = region->send_first_sends[slot_index];
}

static void rn_window_ack_record_first_send( rn_window_ack_t * ack, rn_send_number_t * slot, double now )
{
    rn_region_t * region = ack->region;
    region->send_first_sends[slot->number & ( region->window - 1 )] = now;
    if ( slot->number == ack->send->U && ( !ack->has_oldest || ack->oldest_number != slot->number ) )
    {
        ack->has_oldest = 1;
        ack->oldest_number = slot->number;
        ack->oldest_first_send = now;
    }
}

// ----------------------------------------------------------------
//                        Public operations
// ----------------------------------------------------------------

// Initializes the sent-packet map, resend FIFO, and RTT estimator
// (docs/CHANNELS.md 6.4; tla/RNWindow.tla Init).
void rn_window_ack_init( rn_window_ack_t * ack, rn_window_send_t * send, rn_region_t * region, const rocketnet_message_config_t * config )
{
    rocketnet_assert( ack );
    rocketnet_assert( send );
    rocketnet_assert( region );
    rocketnet_assert( send->region == region );

    rocketnet_message_config_t defaults;
    if ( !config )
    {
        rocketnet_message_default_config( &defaults );
        config = &defaults;
    }

    memset( ack, 0, sizeof(*ack) );
    ack->send = send;
    ack->region = region;
    ack->resend_delay_initial = (double) config->message_resend_delay_initial_ms / 1000.0;
    ack->resend_delay_min = (double) config->message_resend_delay_min_ms / 1000.0;
    ack->resend_delay_max = (double) config->message_resend_delay_max_ms / 1000.0;
    ack->reliable_timeout = (double) config->reliable_message_timeout_ms / 1000.0;

    ack->srtt = 0.0;
    ack->rttvar = 0.0;
    ack->resend_delay = ack->resend_delay_initial;                // tla/RNResendTiming.tla Init: resend_delay = ResendDelayInitial
    ack->has_rtt = 0;
    ack->has_oldest = 0;
    ack->error = ROCKETNET_DISCONNECT_REASON_NONE;

    ack->resend_head = RN_RESEND_NONE;
    ack->resend_tail = RN_RESEND_NONE;
    ack->resend_count = 0;
    ack->chunk_head = RN_RESEND_NONE;
    ack->chunk_tail = RN_RESEND_NONE;
    ack->walk_visits = 0;

    ack->pkt_head = 0;
    ack->pkt_tail = 0;
    ack->ref_head = 0;
    ack->ref_tail = 0;
    ack->receipt_sweep = 0;
    ack->receipt_head = 0;
    ack->receipt_tail = 0;
    ack->receipt_visible_head = 0;
    ack->receipt_visible_tail = 0;
    ack->oldest_number = send->U;
    ack->oldest_first_send = 0.0;

#if ROCKETNET_DEV
    for ( int c = 0; c < 2; c++ )
    {
        ack->gaps.gap_min[c] = 1.0e30;
        ack->gaps.gap_max[c] = 0.0;
        ack->gaps.early_min[c] = 1.0e30;
        ack->gaps.late_max[c] = -1.0e30;
    }
#endif // #if ROCKETNET_DEV
}

#if ROCKETNET_DEV

void rn_window_ack_gaps_update( rn_window_ack_t * ack, double now )
{
    rocketnet_assert( ack );
    rn_resend_gaps_t * gaps = &ack->gaps;
    if ( now == gaps->update_now )
        return;
    gaps->prev_now = gaps->update_now;
    gaps->update_now = now;
    for ( int c = 0; c < 2; c++ )
    {
        gaps->prev_delay[c] = gaps->update_delay[c];
        gaps->update_delay[c] = rn_window_ack_class_delay( ack, c == 1 );
    }
}

// The measure of one resend (struct rn_resend_gaps_t): `last_send` is the
// number's previous send, `now` this one.
static void rn_window_ack_gaps_measure( rn_window_ack_t * ack, uint32_t slot_index, double last_send, double now )
{
    rn_resend_gaps_t * gaps = &ack->gaps;
    if ( gaps->prev_now <= 0.0 || gaps->update_now != now )
        return;                                             // no update before this one to measure against
    const int c = rn_window_ack_slot_is_chunk( ack, slot_index ) ? 1 : 0;
    const double gap = now - last_send;
    const double early = gap - rn_window_ack_class_delay( ack, c == 1 );
    const double late = gap - gaps->prev_delay[c];
    gaps->count[c]++;
    if ( gap < gaps->gap_min[c] ) gaps->gap_min[c] = gap;
    if ( gap > gaps->gap_max[c] ) gaps->gap_max[c] = gap;
    if ( early < gaps->early_min[c] ) gaps->early_min[c] = early;
    if ( late > gaps->late_max[c] ) gaps->late_max[c] = late;
}

#endif // #if ROCKETNET_DEV

// Commits a sent packet and its referenced reliable message numbers into the
// P-entry sent-packet ring, 16P reference ring, 2P open-addressed sent_index,
// and W-entry resend FIFO (docs/CHANNELS.md 6.4 "On send" and "Eviction";
// tla/RNWindow.tla WritePacket, WriteOldest, TablesBounded).
// RNReceipts EmitAck/SweepLoss: an outcome is an accounting event, not a
// message-level ACK. A full ring discards its oldest event, never its history.
// tla/RNReceipts.tla EmitAck
static void rn_window_ack_receipt_emit( rn_window_ack_t * ack, uint32_t serial, int32_t status )
{
    if ( serial == 0 )
        return;
    rn_region_t * region = ack->region;
    rocketnet_assert( status == ROCKETNET_RECEIPT_STATUS_ACKED || status == ROCKETNET_RECEIPT_STATUS_LOSS );
    rn_window_ack_count( region, status == ROCKETNET_RECEIPT_STATUS_ACKED ? ROCKETNET_COUNTER_MESSAGE_RECEIPT_ACKED : ROCKETNET_COUNTER_MESSAGE_RECEIPT_LOSS );
    const uint32_t capacity = region->receipt_entries;
    rocketnet_assert( capacity > 0 );
    if ( ack->receipt_tail - ack->receipt_head == capacity )
    {
        ack->receipt_head++;
        rn_window_ack_count( region, ROCKETNET_COUNTER_MESSAGE_RECEIPT_RING_FULL );
    }
    rn_receipt_t * event = &region->receipts[ack->receipt_tail % capacity];
    event->serial = serial;
    event->status = status;
    ack->receipt_tail++;
}

void rn_window_ack_receipt_emit_loss( rn_window_ack_t * ack, uint32_t serial )
{
    rocketnet_assert( ack );
    rn_window_ack_receipt_emit( ack, serial, ROCKETNET_RECEIPT_STATUS_LOSS );
}

static void rn_window_ack_receipt_resolve( rn_window_ack_t * ack, rn_sent_packet_t * entry, int32_t status )
{
    const uint32_t max_refs = ack->region->sent_ref_entries;
    for ( uint32_t i = 0; i < entry->unreliable_receipts; i++ )
    {
        uint32_t * serial = &ack->region->sent_refs[( entry->receipt_first + i ) % max_refs];
        rn_window_ack_receipt_emit( ack, *serial, status );
        *serial = 0;                 // RNReceipts ExactlyOneOutcome: a late ACK stutters
    }
}

// tla/RNReceipts.tla WritePacket, EvictMap: evicts the oldest live sent-map entry when a ring needs room, resolving its unreliable receipts as LOSS
// tla/RNSentMap.tla WriteAny: records the packet in the ring in send order, reclaiming the head past holes; the oldest entry is evicted only when the ring spans packets_in_flight
static void rn_window_ack_commit( rn_window_ack_t * ack, uint64_t packet_id, const uint64_t * numbers, uint32_t count,
                                  const uint32_t * serials, uint32_t receipt_count, double now )
{
    rocketnet_assert( ack );
    if ( ( count == 0 && receipt_count == 0 ) || packet_id == 0 )
        return;
    rocketnet_assert( count == 0 || numbers );
    rocketnet_assert( receipt_count == 0 || serials );

    rn_region_t * region = ack->region;
    const uint32_t W = region->window;
    const uint32_t P = region->packets;
    const uint32_t max_refs = region->sent_ref_entries;
    static_assert( RN_MESSAGE_RECEIPT_SERIALS_MAX == ROCKETNET_PACKET_BUILDER_MAX_RECORDS, "the receipt bound is the sent map's record limit" );
    rocketnet_assert( count + receipt_count <= RN_MESSAGE_RECEIPT_SERIALS_MAX ); // one record table holds at most this many records
    const uint32_t total = count + receipt_count;

    // 1. Update each referenced number's last_send (and first_send on its
    //    first send) and link/move it to the tail of the resend FIFO.
    for ( uint32_t i = 0; i < count; i++ )
    {
        const uint64_t n = numbers[i];
        const uint32_t slot_index = (uint32_t) ( n & ( W - 1 ) );
        rn_send_number_t * slot = &region->send_numbers[slot_index];
        rocketnet_assert( slot->number == n );
        if ( rn_send_number_never_sent( slot->last_send ) )
            rn_window_ack_record_first_send( ack, slot, now );
#if ROCKETNET_DEV
        if ( slot->last_send >= 0.0 )                       // a resend: not a first send, not a failed send's retry
            rn_window_ack_gaps_measure( ack, slot_index, slot->last_send, now );
#endif // #if ROCKETNET_DEV
        slot->last_send = now;
        if ( !slot->acked )
            rn_resend_fifo_push_tail( ack, slot_index );
    }

    // 2. Reclaim any already-acked entries at the head of the sent-packet ring,
    //    then evict the oldest live entries while either the P-entry packet ring
    //    or the 16P-entry reference ring needs room (docs/CHANNELS.md 6.4
    //    "Eviction": numbers stay in the resend FIFO, MESSAGE_SENT_MAP_EVICTED
    //    is incremented once per evicted packet entry). The config keeps
    //    P >= W (rocketnet_message_validate_config_ex), so with the resend
    //    delay above the round trip the evicted entry is never one whose ack
    //    is still on its way (tla/RNSentMap.tla NoLiveEviction).
    rn_window_ack_reclaim_head( ack );
    while ( ack->pkt_tail - ack->pkt_head >= P || ( ack->ref_tail - ack->ref_head ) + total > max_refs )
    {
        rocketnet_assert( ack->pkt_head < ack->pkt_tail );
        rn_sent_packet_t * oldest = &region->sent_packets[ack->pkt_head % P];
        if ( oldest->packet_id != 0 )
        {
            rn_window_ack_receipt_resolve( ack, oldest, ROCKETNET_RECEIPT_STATUS_LOSS ); // RNReceipts EvictMap
            rn_sent_index_remove( region, oldest->packet_id );
            oldest->packet_id = 0;
            rn_window_ack_count( region, ROCKETNET_COUNTER_MESSAGE_SENT_MAP_EVICTED );
        }
        ack->ref_head += oldest->ref_count + oldest->unreliable_receipts;
        oldest->ref_first = 0;
        oldest->ref_count = 0;
        oldest->unreliable_receipts = 0;
        ack->pkt_head++;
        rn_window_ack_reclaim_head( ack );
    }

    // 3. Write the new sent-packet entry, its references in sent_refs, and its
    //    lookup slot in sent_index.
    const uint32_t entry_index = (uint32_t) ( ack->pkt_tail % P );
    const uint32_t ref_first = (uint32_t) ( ack->ref_tail % max_refs );
    for ( uint32_t i = 0; i < count; i++ )
        region->sent_refs[( ack->ref_tail + i ) % max_refs] = (uint32_t) numbers[i];
    for ( uint32_t i = 0; i < receipt_count; i++ )
        region->sent_refs[( ack->ref_tail + count + i ) % max_refs] = serials[i];
    ack->ref_tail += total;

    rn_sent_packet_t * entry = &region->sent_packets[entry_index];
    entry->packet_id = packet_id;
    entry->send_time = now;
    entry->ref_first = ref_first;
    entry->ref_count = (uint16_t) count;
    entry->unreliable_receipts = (uint16_t) receipt_count;
    entry->receipt_first = ( ref_first + count ) % max_refs;
    entry->flags = 0;
    ack->pkt_tail++;

    rn_sent_index_insert( region, packet_id, entry_index );
}

// tla/RNReceipts.tla WritePacket
void rn_window_ack_commit_packet( rn_window_ack_t * ack, uint64_t packet_id, const uint64_t * numbers, uint32_t count, double now )
{
    rn_window_ack_commit( ack, packet_id, numbers, count, NULL, 0, now );
}

void rn_window_ack_commit_receipt_packet( rn_window_ack_t * ack, uint64_t packet_id, const uint32_t * serials, uint32_t count, double now )
{
    rn_window_ack_commit( ack, packet_id, NULL, 0, serials, count, now );
}

// Processes one acked transport sequence `packet_id` at time `now`
// (docs/CHANNELS.md 6.4 "Ack" and "resend delay"; tla/RNWindow.tla ReceiveAck,
// tla/RNResendTiming.tla Sample).
// tla/RNReceipts.tla ReceiveAck: marks the acked packet's numbers acked, removes its sent-map entry and emits the receipts it completes.
// tla/RNChannelTypes.tla AckAny: one acked packet id retires that packet's reliable send.
// tla/RNSentMap.tla Ack: an id the ring still holds marks its numbers acked and leaves a hole; an id it no longer holds is ignored.
int rn_window_ack_on_ack( rn_window_ack_t * ack, uint64_t packet_id, double now )
{
    rocketnet_assert( ack );
    rn_region_t * region = ack->region;
    rn_window_send_t * send = ack->send;

    uint32_t index_slot = 0;
    if ( !rn_sent_index_find( region, packet_id, &index_slot ) )
        return 0;

    const uint32_t entry_index = region->sent_index[index_slot].entry;
    rocketnet_assert( entry_index < region->packets );
    rn_sent_packet_t * entry = &region->sent_packets[entry_index];
    rocketnet_assert( entry->packet_id == packet_id );

    rn_window_ack_receipt_resolve( ack, entry, ROCKETNET_RECEIPT_STATUS_ACKED ); // RNReceipts ReceiveAck, EmitAck

    // 1. One unambiguous RTT sample per acked packet_id.
    const double rtt = now >= entry->send_time ? ( now - entry->send_time ) : 0.0;
    if ( !ack->has_rtt )
    {
        ack->srtt = rtt;
        ack->rttvar = rtt;
        ack->has_rtt = 1;
    }
    else
    {
        const double diff = fabs( ack->srtt - rtt );
        ack->rttvar = 0.95 * ack->rttvar + 0.05 * diff;
        ack->srtt = 0.95 * ack->srtt + 0.05 * rtt;
    }
    // The resend delay is min(2*RTT + 4*deviation + 30 ms, configured max).
    // The first deviation is the first sample; the optional configured floor applies
    // (standard/MESSAGES.md m9.2).
    ack->resend_delay = rn_window_ack_compute_resend_delay( ack->srtt, ack->rttvar, ack->resend_delay_min, ack->resend_delay_max );

    // 2. For each referenced message number n that is still unacked (n >= U):
    //    unlink from resend FIFO, free its chunk in S (and its descriptor in Q
    //    when its last fragment is acked), and mark n acked to advance U and U'.
    const uint32_t W = region->window;
    const uint32_t max_refs = region->sent_ref_entries;
    const uint64_t u_before = send->U;

    for ( uint32_t i = 0; i < entry->ref_count; i++ )
    {
        const uint32_t ref_u32 = region->sent_refs[( entry->ref_first + i ) % max_refs];
        const uint32_t slot_index = ref_u32 & ( W - 1 );
        rn_send_number_t * slot = &region->send_numbers[slot_index];

        if ( slot->acked || slot->number < send->U || slot->number >= send->next || (uint32_t) slot->number != ref_u32 )
            continue;

        if ( rn_resend_fifo_is_linked( ack, slot_index ) )
        {
            const rn_resend_link_t * link = &region->resend_fifo[slot_index];
            // A named generation unlinks only that generation. Generation 0 means
            // the link was stamped with no in-use descriptor (tla/RNChannelQueues.tla
            // Ack). That link still leaves on this ack while the epoch matches and
            // no descriptor has been attached since the stamp.
            if ( link->generation == 0 )
            {
                const bool descriptor_live = slot->descriptor != RN_DESCRIPTOR_NONE &&
                    slot->descriptor < region->queue &&
                    region->send_descriptors[slot->descriptor].in_use;
                if ( link->owner_epoch != region->schedule_epoch || descriptor_live )
                    continue;
                rn_resend_fifo_unlink( ack, slot_index );
            }
            else if ( !rn_resend_fifo_unlink_named( ack, slot_index, slot->number, link->generation ) )
                continue;
        }

#if defined(ROCKETNET_ACK_COST_TEST_HOOKS)
        rn_ack_cost.completed_numbers++;
#endif

        if ( slot->descriptor != RN_DESCRIPTOR_NONE && slot->descriptor < region->queue )
        {
            const uint32_t d = slot->descriptor;
            const rn_send_descriptor_t * descriptor = &region->send_descriptors[d];
            uint32_t chain_index = 0;
            // Spans release on the final distinct ACK and never use a chain
            // index. Only legacy chunk-backed blocks need the live rank.
            if ( descriptor->in_use && descriptor->fragments > 1 && !( descriptor->flags & RN_SEND_FLAG_SPAN ) )
            {
                for ( uint16_t j = 0; j < slot->fragment; j++ )
                {
#if defined(ROCKETNET_ACK_COST_TEST_HOOKS)
                    rn_ack_cost.predecessor_visits++;
#endif
                    const uint64_t prev_num = slot->number - (uint64_t) ( slot->fragment - j );
                    const rn_send_number_t * prev_slot = &region->send_numbers[prev_num & ( W - 1 )];
                    if ( prev_slot->descriptor == d && prev_slot->number == prev_num && !prev_slot->acked )
                        chain_index++;
                }
            }
            // RNReceipts EmitAck/BlockAckedOnlyWhenAllFragmentsAcked: only the
            // final distinct transport ACK completes a public 6/7 message or
            // a promoted type-5 block.
            // Read the descriptor before its last fragment can free it.
            if ( descriptor->in_use && ( descriptor->flags & ( RN_SEND_FLAG_RECEIPT_PROMOTED | RN_SEND_FLAG_RECEIPT_RELIABLE ) ) &&
                 descriptor->fragments_acked + 1 == descriptor->fragments )
                rn_window_ack_receipt_emit( ack, descriptor->serial, ROCKETNET_RECEIPT_STATUS_ACKED );
            rn_region_send_free_fragment( region, d, chain_index );
        }

        rn_window_send_mark_acked( send, slot->number );
    }

    // 3. Remove the entry from sent_index and reclaim the head of sent_packets.
    rn_sent_index_remove_at( region, index_slot );
    entry->packet_id = 0;
    rn_window_ack_reclaim_head( ack );

    if ( send->U != u_before )
        rn_window_ack_refresh_oldest( ack );

    return 1;
}

// tla/RNReceipts.tla TakeReceipt
void rn_window_ack_receipt_begin_update( rn_window_ack_t * ack )
{
    rocketnet_assert( ack );
    // RNReceipts TakeReceipt: the prior update's list expires; losses made by
    // send_packets since then remain queued for this update.
    if ( ack->receipt_head < ack->receipt_visible_tail )
        ack->receipt_head = ack->receipt_visible_tail;
    ack->receipt_visible_head = ack->receipt_head;
    ack->receipt_visible_tail = ack->receipt_head;
}

// tla/RNReceipts.tla TakeReceipt
void rn_window_ack_receipt_end_update( rn_window_ack_t * ack )
{
    rocketnet_assert( ack );
    ack->receipt_visible_head = ack->receipt_head;
    ack->receipt_visible_tail = ack->receipt_tail;
}

void rn_window_ack_receipt_sweep( rn_window_ack_t * ack, double now )
{
    rocketnet_assert( ack );
    // tla/RNReceipts.tla SweepLoss, LossOnlyAfterDeadline: a type-5 receipt is
    // LOSS only once its packet's receipt deadline has passed. The deadline is
    // the fixed maximum resend delay (message_resend_delay_max_ms), never the
    // adaptive resend delay, so an ACK that arrives after the RTO estimate but
    // inside the deadline still reports ACKED. Sent order and a fixed deadline
    // make the first unexpired entry the stopping point. Reliable references
    // remain in the map for late ACKs.
    if ( ack->receipt_sweep < ack->pkt_head )
        ack->receipt_sweep = ack->pkt_head;
    while ( ack->receipt_sweep < ack->pkt_tail )
    {
        rn_sent_packet_t * entry = &ack->region->sent_packets[ack->receipt_sweep % ack->region->packets];
        if ( entry->packet_id != 0 && now < entry->send_time + ack->resend_delay_max )
            break;
        if ( entry->packet_id != 0 )
            rn_window_ack_receipt_resolve( ack, entry, ROCKETNET_RECEIPT_STATUS_LOSS );
        ack->receipt_sweep++;
    }
}

// tla/RNReceipts.tla TakeReceipt
int rn_window_ack_receipts( const rn_window_ack_t * ack, rocketnet_message_receipt_t * out, int capacity )
{
    if ( !ack || capacity < 0 || ( !out && capacity > 0 ) )
        return 0;
    uint64_t first = ack->receipt_visible_head;
    if ( first < ack->receipt_head )
        first = ack->receipt_head;
    const uint64_t available = ack->receipt_visible_tail > first ? ack->receipt_visible_tail - first : 0;
    const uint64_t copied = available < (uint64_t) capacity ? available : (uint64_t) capacity;
    for ( uint64_t i = 0; i < copied; i++ )
    {
        const rn_receipt_t * event = &ack->region->receipts[( first + i ) % ack->region->receipt_entries];
        out[i].serial = event->serial;
        out[i].status = event->status;
    }
    return (int) available;
}

uint32_t rn_window_ack_on_acks( rn_window_ack_t * ack, const uint64_t * packet_ids, uint32_t count, double now )
{
    rocketnet_assert( ack );
    if ( count == 0 || !packet_ids )
        return 0;

    uint32_t matched = 0;
    for ( uint32_t i = 0; i < count; i++ )
    {
        if ( rn_window_ack_on_ack( ack, packet_ids[i], now ) )
            matched++;
    }
    return matched;
}

// Recovers reliable numbers from a packet refused by finish_packet
// (docs/CHANNELS.md 6.4 "Failed send").
void rn_window_ack_packet_failed( rn_window_ack_t * ack, const uint64_t * numbers, uint32_t count, double now )
{
    rocketnet_assert( ack );
    if ( count == 0 || !numbers )
        return;

    rn_region_t * region = ack->region;
    const uint32_t W = region->window;
    // Negative sentinel: guarantees now >= last_send + rn_window_ack_resend_delay
    // for all now >= 0.0, while marking a failed-send retry apart from a resend.
    const double due_now_sentinel = -( ack->resend_delay_max + 1.0 );

    for ( uint32_t i = count; i > 0; i-- )
    {
        const uint64_t n = numbers[i - 1];
        const uint32_t slot_index = (uint32_t) ( n & ( W - 1 ) );
        rn_send_number_t * slot = &region->send_numbers[slot_index];
        rocketnet_assert( slot->number == n );
        if ( rn_send_number_never_sent( slot->last_send ) )
            rn_window_ack_record_first_send( ack, slot, now );
        slot->last_send = due_now_sentinel;
        if ( !slot->acked )
            rn_resend_fifo_push_head( ack, slot_index );
    }
}

// Checks the per-connection reliable timeout (docs/CHANNELS.md 6.4 "Giving up").
// tla/RNReceipts.tla Disconnect: ends the connection when the oldest unacked number reaches the reliable timeout
int rn_window_ack_check_timeout( rn_window_ack_t * ack, double now )
{
    rocketnet_assert( ack );
    if ( ack->error != ROCKETNET_DISCONNECT_REASON_NONE )
        return 1;

    const rn_window_send_t * send = ack->send;
    if ( send->U >= send->next )
        return 0;

    rn_window_ack_refresh_oldest( ack );
    if ( ack->has_oldest && ( now - ack->oldest_first_send ) >= ack->reliable_timeout )
    {
        ack->error = ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT;
        return 1;
    }
    return 0;
}

uint32_t rn_window_ack_entries_in_flight( const rn_window_ack_t * ack )
{
    rocketnet_assert( ack );
    const rn_region_t * region = ack->region;
    const uint32_t P = region->packets;
    uint32_t live = 0;
    for ( uint64_t p = ack->pkt_head; p < ack->pkt_tail; p++ )
    {
        if ( region->sent_packets[p % P].packet_id != 0 )
            live++;
    }
    return live;
}

uint32_t rn_window_ack_refs_in_flight( const rn_window_ack_t * ack )
{
    rocketnet_assert( ack );
    return (uint32_t) ( ack->ref_tail - ack->ref_head );
}
