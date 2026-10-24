/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_packet_builder.h"

#include <string.h>

#include "rocketnet_platform.h"
#include "rocketnet_test_seams.h"

// Layer 6's packet builder (docs/CHANNELS.md 9.3). tla/RNChannel.tla's
// WritePacket is the packet, tla/RNSendBudget.tla's Send, Wait and Drop are
// the budget's answers, tla/RNScheduler.tla's Write is the order items are
// taken in; each function names the action it implements.

static void rn_builder_count( rocketnet_counters_t * counters, int counter )
{
    if ( counters )
        rocketnet_platform_atomic_add_u64( (volatile uint64_t*) &counters->values[counter], 1 );
}

// The per-level stride and seed: stride (1 << p) * (p + 1) + p, seed (1 << p) * p + p.
// tla/RNScheduler.tla's Stride and Seed.

static const uint64_t rn_builder_stride[ROCKETNET_PACKET_BUILDER_LEVELS] = { 1, 5, 14, 35 };
static const uint64_t rn_builder_seed[ROCKETNET_PACKET_BUILDER_LEVELS] = { 0, 3, 10, 27 };

// RNScheduler!Init and Tick's reset: with nothing queued the clocks go back to
// the seeds, so the interleave describes a backlog and nothing else.

// tla/RNScheduler.tla, Init and Tick's clock reset
static void rn_builder_reset_clocks( struct rn_packet_builder_t * builder )
{
    for ( int p = 0; p < ROCKETNET_PACKET_BUILDER_LEVELS; p++ )
        builder->level_clock[p] = rn_builder_seed[p];
    builder->clock_base = 0;
}

// tla/RNScheduler.tla, Init and Seeds (round_robin = num_channels - 1 so the first turn is channel 0's)
bool rn_packet_builder_init( struct rn_packet_builder_t * builder, struct rn_region_t * region, struct rn_window_ack_t * ack, struct rocketnet_send_budget_t * budget, int packets_per_update, int payload_capacity )
{
    if ( !builder )
        return false;

    memset( builder, 0, sizeof( *builder ) );

    if ( !region || payload_capacity < 1 || payload_capacity > ROCKETNET_MESSAGE_SINGLE_BYTES )
        return false;

    builder->region = region;
    builder->ack = ack;
    builder->budget = budget;
    builder->payload_capacity = payload_capacity;
    builder->packets_per_update = packets_per_update < 1 ? 1 : ( packets_per_update > ROCKETNET_PACKET_BUILDER_MAX_PACKETS ? ROCKETNET_PACKET_BUILDER_MAX_PACKETS : packets_per_update );
    builder->num_channels = region->num_channels;
    builder->round_robin = builder->num_channels > 0 ? builder->num_channels - 1 : 0;     // the first turn is channel 0's
    rn_builder_reset_clocks( builder );

    return true;
}

// one item as the record the writer takes

static void rn_builder_record( const struct rn_packet_item_t * item, struct rocketnet_message_record_t * record )
{
    memset( record, 0, sizeof( *record ) );
    record->code = item->code;
    record->data = item->data;
    record->bytes = (int) item->bytes;

    // only the fields the record's code carries (13.2): a field the code does
    // not carry is zero, so the record is valid and packs one way.
    switch ( item->code )
    {
        case ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED:
            record->channel = item->channel;
            record->ordering = item->ordering;
            record->sequencing = item->sequencing;
            break;

        case ROCKETNET_MESSAGE_CODE_RELIABLE:
            record->number = item->number == ROCKETNET_PACKET_NUMBER_NONE ? 0 : (uint32_t) ( item->number & ROCKETNET_MESSAGE_U24_MASK );
            record->control = item->control && !item->block ? 1 : 0;   // CONFIG: code 2 with CONTROL set, never a block (10.3, 13.4)
            if ( item->block )
            {
                record->block = 1;
                record->ring = item->ring;
                record->fragment = item->fragment;
                record->fragments = item->fragments;
            }
            break;

        case ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED:
            record->number = item->number == ROCKETNET_PACKET_NUMBER_NONE ? 0 : (uint32_t) ( item->number & ROCKETNET_MESSAGE_U24_MASK );
            record->ring = item->ring;
            record->channel = item->channel;
            record->ordering = item->ordering;
            if ( item->block )
            {
                record->block = 1;                                      // one fragment of a block: its index and the count - 1 (CHANNELS.md 8)
                record->fragment = item->fragment;
                record->fragments = item->fragments;
            }
            break;

        case ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED:
            record->number = item->number == ROCKETNET_PACKET_NUMBER_NONE ? 0 : (uint32_t) ( item->number & ROCKETNET_MESSAGE_U24_MASK );
            record->ring = item->ring;
            record->channel = item->channel;
            record->ordering = item->ordering;
            record->sequencing = item->sequencing;
            if ( item->block )
            {
                record->block = 1;
                record->fragment = item->fragment;
                record->fragments = item->fragments;
            }
            break;

        default:
            break;                                              // code 0 carries no field but its length
    }
}

// an item the builder may take at all: bytes to write, a priority in range

static bool rn_builder_writable( const struct rn_packet_item_t * item )
{
    return !item->taken && item->bytes >= 1 && item->data && item->priority < ROCKETNET_PACKET_BUILDER_LEVELS;
}

// The items the builder looked at, counted only where the unit seams are
// compiled in (test_packet_builder_visits_linear pins the bound).

static inline void rn_builder_visit( struct rn_packet_builder_t * builder )
{
#if ROCKETNET_TEST_SEAMS
    builder->visits++;
#else // #if ROCKETNET_TEST_SEAMS
    (void) builder;
#endif // #if ROCKETNET_TEST_SEAMS
}

// The hash a channel and class find their list under while the lists are
// linked. 128 slots hold 32 channels times 4 classes with no collision, so the
// default configuration never walks a chain; more channels share slots and
// walk their chain. It lives on the stack of one call: the builder owns no
// memory (R2).

#define RN_BUILDER_LINK_SLOTS                   128

// RNScheduler's IsHead and Boost, made once per call: every writable item of
// a class joins its channel's list, in the caller's order (next_in_channel),
// and records the best priority from itself to the list's end
// (channel_level), so the head of a list is the channel's head (IsHead) and
// its channel_level is the channel's boost (Boost). The heads of each class
// are linked through next_head from class_head[class]. Items are visited in
// reverse, so each is put in front of its list and the lists come out in the
// caller's order. One visit per writable item: linear in the plan.
//
// Returns true when a writable item has a class outside the budget's four,
// so it is never taken and the plan never drains.

// tla/RNScheduler.tla, IsHead and Boost
static bool rn_builder_link( struct rn_packet_builder_t * builder, struct rn_packet_item_t * items, int num_items, int32_t * class_head )
{
    int32_t slot_head[RN_BUILDER_LINK_SLOTS];
    for ( int k = 0; k < RN_BUILDER_LINK_SLOTS; k++ )
        slot_head[k] = -1;

    bool stray = false;

    for ( int i = num_items - 1; i >= 0; i-- )
    {
        rn_builder_visit( builder );

        struct rn_packet_item_t * item = &items[i];
        if ( !rn_builder_writable( item ) )
            continue;

        if ( item->send_class > ROCKETNET_SEND_CLASS_UNRELIABLE )
        {
            stray = true;
            continue;
        }

        // the channel's list so far: its head is the channel's item after this one
        int32_t * link = &slot_head[( (uint32_t) item->channel * 4u + item->send_class ) % RN_BUILDER_LINK_SLOTS];
        while ( *link >= 0 && ( items[*link].channel != item->channel || items[*link].send_class != item->send_class ) )
        {
            rn_builder_visit( builder );
            link = &items[*link].next_head;
        }

        const int32_t next = *link;
        if ( next >= 0 )
        {
            item->next_in_channel = next;
            item->channel_level = items[next].channel_level < item->priority ? items[next].channel_level : item->priority;
            item->next_head = items[next].next_head;
        }
        else
        {
            item->next_in_channel = -1;
            item->channel_level = item->priority;
            item->next_head = -1;
        }
        *link = (int32_t) i;
    }

    // each slot's heads onto their class's list
    for ( int k = 0; k < RN_BUILDER_LINK_SLOTS; k++ )
    {
        int32_t head = slot_head[k];
        while ( head >= 0 )
        {
            const int32_t next = items[head].next_head;
            items[head].next_head = class_head[items[head].send_class];
            class_head[items[head].send_class] = head;
            head = next;
        }
    }

    return stray;
}

// tla/RNScheduler.tla, PickLevel's pull-forward: a clock behind clock_base counts as clock_base
static uint64_t rn_builder_effective_clock( const struct rn_packet_builder_t * builder, int level )
{
    const uint64_t clock = builder->level_clock[level];
    return clock > builder->clock_base ? clock : builder->clock_base;
}

// RNScheduler!Pick inside one class. Returns the item's index, or -1 when the
// class is spent; *out_level is the level it was taken at and *out_link the
// link that holds it, for rn_builder_take_head.
//
// 1. Channel FIFO: only a channel's head (its first writable item of this
//    class, in the caller's order, which is the order the application
//    queued) is offered, at the best priority among the channel's writable
//    items of this class: per-send priority boosts the whole channel (17.3
//    item 3, RNScheduler!Boost and IsHead). rn_builder_link made both.
// 2. The level: the smallest clock among the levels offered, a clock behind
//    clock_base counting as clock_base (the pull-forward); a tie goes to
//    the more urgent level (RNScheduler!PickLevel).
// 3. The channel: round robin from the channel after the last one taken, one
//    item per turn (17.3 item 4, RNScheduler!PickIn). Channels the same
//    distance away (a channel at or past the modulus) go in the caller's
//    order.
//
// One visit per channel head of the class, so a take costs the channels
// with something to send (at most ROCKETNET_MAX_CHANNELS, plus any channel
// at or past it), never the plan.

// tla/RNScheduler.tla, PickLevel and PickIn
static int rn_builder_next_in_class( struct rn_packet_builder_t * builder, struct rn_packet_item_t * items, int32_t * class_head, int * out_level, int32_t ** out_link )
{
    const uint32_t channels = builder->num_channels > 0 ? builder->num_channels : 1;

    int best = -1;
    int best_level = 0;
    uint64_t best_clock = 0;
    uint32_t best_distance = 0;
    int32_t * best_link = NULL;

    for ( int32_t * link = class_head; *link >= 0; link = &items[*link].next_head )
    {
        rn_builder_visit( builder );

        const int i = *link;
        const int level = items[i].channel_level;
        const uint64_t clock = rn_builder_effective_clock( builder, level );
        const uint32_t distance = ( items[i].channel % channels + channels - builder->round_robin % channels - 1 ) % channels;

        if ( best < 0 || clock < best_clock || ( clock == best_clock && level < best_level ) ||
             ( clock == best_clock && level == best_level && ( distance < best_distance || ( distance == best_distance && i < best ) ) ) )
        {
            best = i;
            best_level = level;
            best_clock = clock;
            best_distance = distance;
            best_link = link;
        }
    }

    *out_level = best_level;
    *out_link = best_link;
    return best;
}

// A head taken: the channel's next item of the class is its head now, at its
// own channel_level (the boost of what is left), or the channel leaves the
// class's list. Only heads are ever taken, so the lists stay exact.

static void rn_builder_take_head( struct rn_packet_item_t * items, int32_t * link )
{
    const int32_t taken = *link;
    const int32_t next = items[taken].next_in_channel;
    if ( next >= 0 )
    {
        items[next].next_head = items[taken].next_head;
        *link = next;
    }
    else
    {
        *link = items[taken].next_head;
    }
}

// RNScheduler!Write's clock step: the level taken becomes the origin and its
// clock advances by its stride.

// tla/RNScheduler.tla, Write's clock step (the level taken becomes the origin and advances by its stride)
static void rn_builder_take_level( struct rn_packet_builder_t * builder, int level )
{
    const uint64_t clock = rn_builder_effective_clock( builder, level );
    builder->clock_base = clock;
    builder->level_clock[level] = clock + rn_builder_stride[level];
}

// tla/RNScheduler.tla, Write (class order, clock step, rr') and Tick's clock reset (every item taken)
int rn_packet_builder_write( struct rn_packet_builder_t * builder, struct rn_packet_item_t * items, int num_items, uint8_t ** payloads, int num_payloads )
{
    if ( !builder || builder->packets_per_update == 0 )
        return 0;

    builder->packets = 0;
    builder->unreliable_dropped = 0;
    builder->visits = 0;

    if ( !items || num_items < 1 || !payloads || num_payloads < 1 )
        return 0;

    // each class's channel heads, linked once for the whole call: an item
    // leaves its list only when it is taken, and a refused packet ends the call
    int32_t class_head[ROCKETNET_SEND_CLASS_UNRELIABLE + 1];
    for ( int c = 0; c <= ROCKETNET_SEND_CLASS_UNRELIABLE; c++ )
        class_head[c] = -1;
    const bool stray = rn_builder_link( builder, items, num_items, class_head );
    bool refused = false;

    const int limit = builder->packets_per_update < num_payloads ? builder->packets_per_update : num_payloads;

    for ( int p = 0; p < limit; p++ )
    {
        if ( !payloads[p] )
            break;

        struct rocketnet_message_writer_t writer;
        rocketnet_message_writer_begin( &writer, payloads[p], builder->payload_capacity );

        // the scheduler's state at the packet's start: a packet the budget
        // refuses is not written, so its takes are undone
        uint64_t saved_level_clock[ROCKETNET_PACKET_BUILDER_LEVELS];
        for ( int l = 0; l < ROCKETNET_PACKET_BUILDER_LEVELS; l++ )
            saved_level_clock[l] = builder->level_clock[l];
        const uint64_t saved_clock_base = builder->clock_base;
        const uint32_t saved_round_robin = builder->round_robin;

        uint64_t numbers[ROCKETNET_PACKET_BUILDER_MAX_NUMBERS];
        uint32_t count = 0;
        uint32_t records = 0;
        int packet_class = -1;
        int taken[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];

        // 9.3 and RNScheduler!Write: fill in the budget's class order, the
        // outer rule (RNScheduler ClassOrderBeforePriority). Within a class,
        // the four priority levels by their clocks; within a level,
        // channels take turns one item each, so one chatty channel cannot
        // starve the others; within a channel, FIFO.
        for ( int send_class = ROCKETNET_SEND_CLASS_RETRANSMIT; send_class <= ROCKETNET_SEND_CLASS_UNRELIABLE; send_class++ )
        {
            for ( ;; )
            {
                int level = 0;
                int32_t * link = NULL;
                const int i = rn_builder_next_in_class( builder, items, &class_head[send_class], &level, &link );
                if ( i < 0 )
                    break;

                // a packet tracks at most MAX_RECORDS records, and at most
                // MAX_NUMBERS reliable numbers, so every number it carries is
                // committed; the next record starts the next packet.
                if ( records >= ROCKETNET_PACKET_BUILDER_MAX_RECORDS ||
                     ( items[i].number != ROCKETNET_PACKET_NUMBER_NONE && count >= ROCKETNET_PACKET_BUILDER_MAX_NUMBERS ) )
                    goto packet_done;

                // an item that travels alone starts a packet and ends it
                // (tla/RNChannel.tla: a chunk travels alone)
                if ( items[i].alone && records > 0 )
                    goto packet_done;

                struct rocketnet_message_record_t record;
                rn_builder_record( &items[i], &record );

                // a record that does not fit ends this packet; it is never
                // split across packets (9.3).
                if ( !rocketnet_message_writer_add( &writer, &record ) )
                {
                    items[i].taken = 0;
                    goto packet_done;
                }

                items[i].taken = 1;
                rn_builder_take_head( items, link );
                taken[records] = i;
                records++;

                if ( packet_class < 0 )
                    packet_class = send_class;

                // I3: an unreliable record never takes a reliable number.
                if ( items[i].number != ROCKETNET_PACKET_NUMBER_NONE && count < ROCKETNET_PACKET_BUILDER_MAX_NUMBERS )
                    numbers[count++] = items[i].number;

                builder->round_robin = items[i].channel;
                rn_builder_take_level( builder, level );

                if ( items[i].alone )
                    goto packet_done;
            }
        }

    packet_done:

        const int payload_bytes = rocketnet_message_writer_finish( &writer );
        if ( payload_bytes <= 0 )
            break;                                              // an empty plan writes nothing (9.3)

        // R6 and RNSendBudget: the packet is admitted at the class of the
        // first item it carries, at the most wire bytes its payload can take
        // once the transport adds its ack block. settle refunds the rest.
        const int admitted = rocketnet_send_budget_max_wire_bytes( payload_bytes );
        int decision = ROCKETNET_SEND_BUDGET_SEND;

        if ( builder->budget && packet_class >= 0 )
            decision = rocketnet_send_budget_admit( builder->budget, packet_class, admitted );

        if ( decision != ROCKETNET_SEND_BUDGET_SEND )
        {
            // tla/RNChannel.tla DropUnreliableForBudget: an unreliable packet that does not fit the budget is dropped and counted.
            // The packet is not written. Its items stay as they were: a
            // reliable one waits for the next update and is never dropped, an
            // unreliable one is dropped and counted. Either way nothing after
            // it spends the bucket this update.
            for ( uint32_t r = 0; r < records && r < ROCKETNET_PACKET_BUILDER_MAX_RECORDS; r++ )
                items[taken[r]].taken = 0;

            for ( int l = 0; l < ROCKETNET_PACKET_BUILDER_LEVELS; l++ )
                builder->level_clock[l] = saved_level_clock[l];
            builder->clock_base = saved_clock_base;
            builder->round_robin = saved_round_robin;

            if ( packet_class == ROCKETNET_SEND_CLASS_UNRELIABLE )
            {
                for ( uint32_t r = 0; r < records && r < ROCKETNET_PACKET_BUILDER_MAX_RECORDS; r++ )
                {
                    builder->unreliable_dropped++;
                    rn_builder_count( builder->region->counters, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
                }
            }

            refused = true;
            break;
        }

        auto * slot = &builder->packet[p];

        slot->count = count;
        slot->resends = 0;
        for ( uint32_t r = 0; r < records && r < ROCKETNET_PACKET_BUILDER_MAX_RECORDS; r++ )
        {
            if ( items[taken[r]].taken && items[taken[r]].resend )
                slot->resends++;
        }
        for ( uint32_t n = 0; n < count; n++ )
            slot->numbers[n] = numbers[n];
        slot->records = records;
        slot->payload_bytes = (uint32_t) payload_bytes;
        slot->admitted_wire_bytes = builder->budget ? admitted : 0;

        builder->packets++;
    }

    // RNScheduler!Tick's reset: every item taken, nothing queued, the clocks
    // go back to the seeds. Every class list empty is every writable item
    // taken, unless a refused packet gave its items back or an item's class
    // is none of the four.
    bool drained = !refused && !stray;
    for ( int c = 0; c <= ROCKETNET_SEND_CLASS_UNRELIABLE && drained; c++ )
        if ( class_head[c] >= 0 )
            drained = false;
    if ( drained )
        rn_builder_reset_clocks( builder );

    return (int) builder->packets;
}

static void rn_builder_clear_pending_resend( struct rn_packet_builder_t * builder, uint64_t number );

void rn_packet_builder_commit( struct rn_packet_builder_t * builder, int packet_index, uint64_t packet_id, double now )
{
    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets || !builder->ack )
        return;

    const uint32_t count = builder->packet[packet_index].count;
    if ( count != 0 )
    {
        // RNChannel!WritePacket's smap': keyed by the transport's own packet id.
        rn_window_ack_commit_packet( builder->ack, packet_id, builder->packet[packet_index].numbers, count, now );
    }

    // a resend is counted when the packet carrying it is sent (6.4, RNResendTiming)
    for ( uint32_t i = 0; i < builder->packet[packet_index].resends; i++ )
        rn_builder_count( builder->region->counters, ROCKETNET_COUNTER_MESSAGE_RESENT );

    for ( uint32_t n = 0; n < count; n++ )
        rn_builder_clear_pending_resend( builder, builder->packet[packet_index].numbers[n] );
}

static void rn_builder_note_refused_resend( struct rn_packet_builder_t * builder, uint64_t number )
{
    if ( rn_packet_builder_resend_pending( builder, number ) )
        return;
    if ( builder->pending_resend_count >= ROCKETNET_PACKET_BUILDER_PENDING_RESENDS )
        return;
    builder->pending_resend[builder->pending_resend_count++] = number;
}

static void rn_builder_clear_pending_resend( struct rn_packet_builder_t * builder, uint64_t number )
{
    uint32_t kept = 0;
    for ( uint32_t i = 0; i < builder->pending_resend_count; i++ )
    {
        if ( builder->pending_resend[i] == number )
            continue;
        builder->pending_resend[kept++] = builder->pending_resend[i];
    }
    builder->pending_resend_count = kept;
}

int rn_packet_builder_resend_pending( const struct rn_packet_builder_t * builder, uint64_t number )
{
    if ( !builder )
        return 0;
    for ( uint32_t i = 0; i < builder->pending_resend_count; i++ )
        if ( builder->pending_resend[i] == number )
            return 1;
    return 0;
}

void rn_packet_builder_packet_failed( struct rn_packet_builder_t * builder, int packet_index, double now )
{
    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets || !builder->ack )
        return;

    const uint32_t count = builder->packet[packet_index].count;
    if ( count == 0 )
        return;

    // A number that has already been sent stays a resend when this finish is
    // refused. A first send that never committed stays a first write.
    rn_region_t * region = builder->region;
    const uint32_t W = region->window;
    const uint64_t * numbers = builder->packet[packet_index].numbers;
    uint8_t refused_resend[ROCKETNET_PACKET_BUILDER_MAX_NUMBERS];
    const uint32_t noted = count < ROCKETNET_PACKET_BUILDER_MAX_NUMBERS ? count : ROCKETNET_PACKET_BUILDER_MAX_NUMBERS;
    for ( uint32_t i = 0; i < noted; i++ )
    {
        const uint32_t slot_index = (uint32_t) ( numbers[i] & ( W - 1 ) );
        refused_resend[i] = region->send_numbers[slot_index].last_send >= 0.0;
    }

    rn_window_ack_packet_failed( builder->ack, numbers, count, now );

    for ( uint32_t i = 0; i < noted; i++ )
        if ( refused_resend[i] )
            rn_builder_note_refused_resend( builder, numbers[i] );
}

bool rn_packet_builder_settle( struct rn_packet_builder_t * builder, int packet_index, int actual_wire_bytes )
{
    if ( !builder || !builder->budget || packet_index < 0 || packet_index >= (int) builder->packets )
        return false;

    const int admitted = builder->packet[packet_index].admitted_wire_bytes;
    if ( admitted <= 0 )
        return false;

    return rocketnet_send_budget_settle( builder->budget, admitted, actual_wire_bytes );
}

const uint64_t * rn_packet_builder_numbers( const struct rn_packet_builder_t * builder, int packet_index, uint32_t * out_count )
{
    if ( out_count )
        *out_count = 0;

    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets )
        return NULL;

    if ( out_count )
        *out_count = builder->packet[packet_index].count;

    return builder->packet[packet_index].numbers;
}
