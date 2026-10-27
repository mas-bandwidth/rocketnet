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
        rocketnet_platform_atomic_add_u64( (volatile uint64_t *) &counters->values[counter], 1 );
}

// The per-level stride and seed: stride (1 << p) * (p + 1) + p, seed (1 << p) * p + p.
// tla/RNScheduler.tla's Stride and Seed.

static const uint64_t rn_builder_stride[ROCKETNET_PACKET_BUILDER_LEVELS] = { 1, 5, 14, 35 };
static const uint64_t rn_builder_seed[ROCKETNET_PACKET_BUILDER_LEVELS] = { 0, 3, 10, 27 };

#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
static thread_local struct rn_packet_builder_plan_t rn_builder_last_plan;

const struct rn_packet_builder_plan_t * rn_packet_builder_test_last_plan( void )
{
    return &rn_builder_last_plan;
}
#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS

static void rn_builder_reset_clocks( struct rn_packet_builder_t * builder )
{
    // RNScheduler!Init and Tick's reset: with nothing queued the clocks go back to
    // the seeds, so the interleave describes a backlog and nothing else.

    // tla/RNScheduler.tla, Init and Tick's clock reset

    for ( int p = 0; p < ROCKETNET_PACKET_BUILDER_LEVELS; p++ )
        builder->level_clock[p] = rn_builder_seed[p];

    builder->clock_base = 0;
}

bool rn_packet_builder_init( struct rn_packet_builder_t * builder, struct rn_region_t * region, struct rn_window_ack_t * ack, struct rocketnet_send_budget_t * budget, int packets_per_update, int payload_capacity )
{
    // tla/RNScheduler.tla, Init and Seeds (round_robin = num_channels - 1 so the first turn is channel 0's)

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

static void rn_builder_record( const struct rn_packet_item_t * item, struct rocketnet_message_record_t * record )
{
    // one item as the record the writer takes

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

static bool rn_builder_writable( const struct rn_packet_item_t * item )
{
    // an item the builder may take at all: bytes to write, a priority in range

    return !item->taken && item->bytes >= 1 && item->data && item->priority < ROCKETNET_PACKET_BUILDER_LEVELS;
}

static inline void rn_builder_plan_operation( struct rn_packet_builder_plan_t * plan )
{
#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
    plan->plan_operations++;
#else // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
    (void) plan;
#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
}

static inline void rn_builder_selection_operation( struct rn_packet_builder_plan_t * plan )
{
#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
    plan->selection_operations++;
#else // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
    (void) plan;
#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
}

static uint8_t rn_builder_key_byte( const struct rn_packet_item_t * items, uint32_t item_index, int pass )
{
    // one byte of the stable radix key: class in the most significant pass and
    // the full 16-bit channel in the first two. Stable passes preserve caller
    // order inside an equal channel/class key. The 256-entry count table is the
    // only call stack scratch: it is bounded independently of the configured
    // maximum of 65536 channels.

    if ( pass == 0 )
        return (uint8_t) items[item_index].channel;

    if ( pass == 1 )
        return (uint8_t) ( items[item_index].channel >> 8 );

    return items[item_index].send_class;
}

static void rn_builder_radix_pass( struct rn_packet_builder_plan_t * plan, const struct rn_packet_item_t * items, const uint8_t * input, uint8_t * output, uint32_t count, int pass )
{
    uint8_t buckets[256];
    memset( buckets, 0, sizeof( buckets ) );

    for ( uint32_t i = 0; i < count; i++ )
    {
        buckets[rn_builder_key_byte( items, input[i], pass )]++;
        rn_builder_plan_operation( plan );
    }

    uint32_t offset = 0;
    for ( uint32_t b = 0; b < 256; b++ )
    {
        const uint32_t n = buckets[b];
        buckets[b] = (uint8_t) offset;
        offset += n;
    }

    for ( uint32_t i = 0; i < count; i++ )
    {
        const uint8_t item_index = input[i];
        output[buckets[rn_builder_key_byte( items, item_index, pass )]++] = item_index;
        rn_builder_plan_operation( plan );
    }
}

static bool rn_builder_plan( struct rn_packet_builder_t * builder, struct rn_packet_builder_plan_t * plan, struct rn_packet_item_t * items, int num_items )
{
    // RNScheduler's IsHead and Boost, made once per call. Writable item indices
    // are stably radix-grouped into contiguous class/channel ranges. A reverse
    // pass over each range stores its suffix priority minimum, so cursor is the
    // channel head (IsHead) and sequence[cursor].suffix_level is Boost. No item
    // or group link is built and no grouping work repeats per packet.
    //
    // Returns true when a writable item has a class outside the budget's four,
    // so it is never taken and the plan never drains.

    // tla/RNScheduler.tla, IsHead and Boost

    uint8_t alternate[ROCKETNET_PACKET_BUILDER_MAX_ITEMS];
    uint32_t count = 0;
    bool stray = false;

    for ( int i = 0; i < num_items; i++ )
    {
        rn_builder_plan_operation( plan );

        if ( !rn_builder_writable( &items[i] ) )
            continue;

        if ( items[i].send_class > ROCKETNET_SEND_CLASS_UNRELIABLE )
        {
            stray = true;
            continue;
        }

        plan->sort_indices[count++] = (uint8_t) i;
    }

    rn_builder_radix_pass( plan, items, plan->sort_indices, alternate, count, 0 );
    rn_builder_radix_pass( plan, items, alternate, plan->sort_indices, count, 1 );
    rn_builder_radix_pass( plan, items, plan->sort_indices, alternate, count, 2 );

    plan->group_count = 0;

    for ( int c = 0; c <= ROCKETNET_SEND_CLASS_UNRELIABLE; c++ )
    {
        plan->class_group_begin[c] = 0;
        plan->class_group_end[c] = 0;
    }

    uint32_t position = 0;
    while ( position < count )
    {
        const uint32_t first = alternate[position];
        const uint8_t send_class = items[first].send_class;
        const uint16_t channel = items[first].channel;
        const uint8_t group_index = plan->group_count++;
        struct rn_packet_builder_group_t * group = &plan->groups[group_index];
        group->begin = (uint8_t) position;
        group->cursor = (uint8_t) position;
        group->channel = channel;
        group->send_class = send_class;

        do
        {
            struct rn_packet_builder_sequence_t * entry = &plan->sequence[position];
            entry->call_epoch = builder->call_epoch;
            entry->item_index = alternate[position];
            entry->group_index = (uint16_t) group_index;
            position++;
            rn_builder_plan_operation( plan );
        } while ( position < count && items[alternate[position]].send_class == send_class && items[alternate[position]].channel == channel );

        group->end = (uint8_t) position;

        uint8_t suffix = ROCKETNET_PACKET_BUILDER_LEVELS;
        for ( uint32_t p = group->end; p > group->begin; p-- )
        {
            struct rn_packet_builder_sequence_t * entry = &plan->sequence[p - 1];
            const uint8_t priority = items[entry->item_index].priority;
            suffix = priority < suffix ? priority : suffix;
            entry->suffix_level = suffix;
            rn_builder_plan_operation( plan );
        }
    }

    uint32_t next_group = 0;
    for ( int c = 0; c <= ROCKETNET_SEND_CLASS_UNRELIABLE; c++ )
    {
        plan->class_group_begin[c] = (uint8_t) next_group;

        while ( next_group < plan->group_count && plan->groups[next_group].send_class == (uint8_t) c )
            next_group++;

        plan->class_group_end[c] = (uint8_t) next_group;
    }

    return stray;
}

static uint64_t rn_builder_effective_clock( const struct rn_packet_builder_t * builder, int level )
{
    // tla/RNScheduler.tla, PickLevel's pull-forward: a clock behind clock_base counts as clock_base

    const uint64_t clock = builder->level_clock[level];
    return clock > builder->clock_base ? clock : builder->clock_base;
}

static int rn_builder_next_in_class( struct rn_packet_builder_t * builder, struct rn_packet_builder_plan_t * plan, struct rn_packet_item_t * items, int num_items, int send_class, int * out_level, uint32_t * out_group )
{
    // RNScheduler!Pick inside one class. Returns the item's index, or -1 when the
    // class is spent; *out_level is the level it was taken at and *out_group the
    // contiguous range that holds it, for rn_builder_take_head.
    //
    // 1. Channel FIFO: only a channel's head (its first writable item of this
    //    class, in the caller's order, which is the order the application
    //    queued) is offered, at the best priority among the channel's writable
    //    items of this class: per-send priority boosts the whole channel (17.3
    //    item 3, RNScheduler!Boost and IsHead). rn_builder_plan made both.
    // 2. The level: the smallest clock among the levels offered, a clock behind
    //    clock_base counting as clock_base (the pull-forward); a tie goes to
    //    the more urgent level (RNScheduler!PickLevel).
    // 3. The channel: round robin from the channel after the last one taken, one
    //    item per turn (17.3 item 4, RNScheduler!PickIn). Channels the same
    //    distance away (a channel at or past the modulus) go in the caller's
    //    order.
    //
    // One selection operation per channel head of the class, so a take costs the channels
    // with something to send (at most ROCKETNET_MAX_CHANNELS, plus any channel
    // at or past it), never the plan.

    // tla/RNScheduler.tla, PickLevel and PickIn

    const uint32_t channels = builder->num_channels > 0 ? builder->num_channels : 1;

    int best = -1;
    int best_level = 0;
    uint64_t best_clock = 0;
    uint32_t best_distance = 0;
    uint32_t best_group = 0;

    for ( uint32_t g = plan->class_group_begin[send_class]; g < plan->class_group_end[send_class]; g++ )
    {
        rn_builder_selection_operation( plan );

        const struct rn_packet_builder_group_t * group = &plan->groups[g];
        if ( group->cursor >= group->end )
            continue;

        const struct rn_packet_builder_sequence_t * entry = &plan->sequence[group->cursor];
        if ( entry->call_epoch != builder->call_epoch || entry->item_index >= (uint32_t) num_items || entry->group_index != g )
            continue;                                           // a stale or damaged plan slot is never an item

        const int i = (int) entry->item_index;
        const int level = entry->suffix_level;
        const uint64_t clock = rn_builder_effective_clock( builder, level );
        const uint32_t distance = ( items[i].channel % channels + channels - builder->round_robin % channels - 1 ) % channels;

        if ( best < 0 || clock < best_clock || ( clock == best_clock && level < best_level ) ||
             ( clock == best_clock && level == best_level && ( distance < best_distance || ( distance == best_distance && i < best ) ) ) )
        {
            best = i;
            best_level = level;
            best_clock = clock;
            best_distance = distance;
            best_group = g;
        }
    }

    *out_level = best_level;
    *out_group = best_group;

    return best;
}

static uint32_t rn_builder_take_head( struct rn_packet_builder_plan_t * plan, uint32_t group_index )
{
    // a head taken: advance its contiguous range. The next sequence entry already
    // carries the suffix priority of what remains.

    struct rn_packet_builder_group_t * group = &plan->groups[group_index];
    const uint32_t position = group->cursor;
    group->cursor++;
    return position;
}

static void rn_builder_take_level( struct rn_packet_builder_t * builder, int level )
{
    // RNScheduler!Write's clock step: the level taken becomes the origin and its
    // clock advances by its stride.

    // tla/RNScheduler.tla, Write's clock step (the level taken becomes the origin and advances by its stride)

    const uint64_t clock = rn_builder_effective_clock( builder, level );
    builder->clock_base = clock;
    builder->level_clock[level] = clock + rn_builder_stride[level];
}

int rn_packet_builder_write( struct rn_packet_builder_t * builder, struct rn_packet_item_t * items, int num_items, uint8_t ** payloads, int num_payloads )
{
    // tla/RNScheduler.tla, Write (class order, clock step, rr') and Tick's clock reset (every item taken)

    if ( !builder || builder->packets_per_update == 0 )
        return 0;

    builder->packets = 0;
    builder->unreliable_dropped = 0;

    if ( !items || num_items < 1 || !payloads || num_payloads < 1 )
        return 0;

    // the production producer's plan is capped at the same 96 items. Refuse
    // a larger direct/internal call without touching its items, rather than
    // truncate it into a different scheduler trace. A full-width epoch never
    // wraps back into the validity domain of an earlier plan.

    if ( num_items > ROCKETNET_PACKET_BUILDER_MAX_ITEMS || builder->call_epoch == UINT64_MAX )
        return 0;

    builder->call_epoch++;

    // each class's channel groups are built once for the whole call. An item
    // advances its group's cursor only when taken; a refused packet restores
    // the positions it took. This bounded transient plan stays on the call's
    // stack instead of adding persistent bytes to every connection.

    struct rn_packet_builder_plan_t plan;
    memset( &plan, 0, sizeof( plan ) );
    const bool stray = rn_builder_plan( builder, &plan, items, num_items );
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
        uint32_t taken_positions[ROCKETNET_PACKET_BUILDER_MAX_RECORDS];

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
                uint32_t group_index = 0;
                const int i = rn_builder_next_in_class( builder, &plan, items, num_items, send_class, &level, &group_index );
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
                taken_positions[records] = rn_builder_take_head( &plan, group_index );
                records++;

                if ( packet_class < 0 )
                    packet_class = send_class;

                // I3: an unreliable record never takes a reliable number

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

            for ( uint32_t r = records; r > 0; r-- )
            {
                const struct rn_packet_builder_sequence_t * entry = &plan.sequence[taken_positions[r - 1]];
                struct rn_packet_builder_group_t * group = &plan.groups[entry->group_index];
                rocketnet_assert( group->cursor > group->begin );
                group->cursor--;
                items[entry->item_index].taken = 0;
            }

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

        for ( uint32_t n = 0; n < count; n++ )
            slot->numbers[n] = numbers[n];

        slot->records = records;
        slot->payload_bytes = (uint32_t) payload_bytes;
        slot->admitted_wire_bytes = builder->budget ? admitted : 0;

        builder->packets++;
    }

    // RNScheduler!Tick's reset: every item taken, nothing queued, the clocks
    // go back to the seeds. Every group range spent is every writable item
    // taken, unless a refused packet gave its items back or an item's class
    // is none of the four.

    bool drained = !refused && !stray;
    for ( uint32_t g = 0; g < plan.group_count && drained; g++ )
        if ( plan.groups[g].cursor < plan.groups[g].end )
            drained = false;

    if ( drained )
        rn_builder_reset_clocks( builder );

#if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS
    rn_builder_last_plan = plan;
#endif // #if ROCKETNET_DEV && ROCKETNET_TEST_SEAMS

    return (int) builder->packets;
}

static double rn_builder_refused_resend( const struct rn_window_ack_t * ack )
{
    // a refused resend's send time: a second due-now sentinel, below the one
    // rn_window_ack_packet_failed writes (-(resend_delay_max + 1)), so the number
    // is due now like any failed send (rn_send_number_unsent_due) and still known
    // to have gone out before. Neither sentinel is a real send time.

    return -( ack->resend_delay_max + 2.0 );
}

static bool rn_builder_sent_before( const struct rn_packet_builder_t * builder, uint64_t number )
{
    // a number that went out before: a real send time, or a refused resend

    const rn_region_t * region = builder->region;
    const double last_send = region->send_numbers[(uint32_t) ( number & ( region->window - 1 ) )].last_send;
    if ( last_send >= 0.0 )
        return true;

    return !rn_send_number_never_sent( last_send ) && last_send < -( builder->ack->resend_delay_max + 1.5 );
}

void rn_packet_builder_commit( struct rn_packet_builder_t * builder, int packet_index, uint64_t packet_id, double now )
{
    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets || !builder->ack )
        return;

    const uint32_t count = builder->packet[packet_index].count;
    if ( count == 0 )
        return;                                                 // a pure-unreliable packet takes no sent-map entry

    // a resend is counted when the packet carrying it is sent (6.4,
    // RNResendTiming): a number this packet carries that went out before is a
    // resend. Read before the commit below stamps its send time.

    const uint64_t * numbers = builder->packet[packet_index].numbers;
    for ( uint32_t i = 0; i < count; i++ )
    {
        if ( rn_builder_sent_before( builder, numbers[i] ) )
            rn_builder_count( builder->region->counters, ROCKETNET_COUNTER_MESSAGE_RESENT );
    }

    // RNChannel!WritePacket's smap': keyed by the transport's own packet id

    rn_window_ack_commit_packet( builder->ack, packet_id, numbers, count, now );
}

void rn_packet_builder_packet_failed( struct rn_packet_builder_t * builder, int packet_index, double now )
{
    if ( !builder || packet_index < 0 || packet_index >= (int) builder->packets || !builder->ack )
        return;

    const uint32_t count = builder->packet[packet_index].count;
    if ( count == 0 )
        return;

    // which numbers went out before this refused finish: the recovery below
    // makes every number due now, and these keep the mark of a resend.

    const uint64_t * numbers = builder->packet[packet_index].numbers;
    bool sent_before[ROCKETNET_PACKET_BUILDER_MAX_NUMBERS];
    for ( uint32_t i = 0; i < count; i++ )
        sent_before[i] = rn_builder_sent_before( builder, numbers[i] );

    rn_window_ack_packet_failed( builder->ack, numbers, count, now );

    rn_region_t * region = builder->region;
    for ( uint32_t i = 0; i < count; i++ )
    {
        if ( sent_before[i] )
            region->send_numbers[(uint32_t) ( numbers[i] & ( region->window - 1 ) )].last_send = rn_builder_refused_resend( builder->ack );
    }
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
