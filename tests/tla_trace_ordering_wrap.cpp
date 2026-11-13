/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the ordered receive's delivery reservation
// (proofs/tla/README-TraceCheck.md, proofs/tla/RNOrderingWrap.tla,
// proofs/tla/TraceRNOrderingWrap.tla).
//
// The code under test is rn_ordered_receive (sdk/rocketnet_channel.cpp) on
// one connection's region of the code's smallest window, W = 64, with an
// unreliable allowance Du of OW_DU: the reservation
// (rn_receive_delivery_reservation, after rn_window_receive_duplicate), the
// deferral, the window's accept, the ordered dispatch and its drain, the
// sequenced hold's dispatch and release (sdk/rocketnet_channel_sequenced.cpp),
// and rn_ordered_end_update. The walk is the sender: each step hands
// rn_ordered_receive one payload of one record on channel 0, written by the
// library's own writer, or ends the update. Each step is one call:
//
//   receive_ordered    a fresh code-3 record at ordering o, in turn or ahead
//                      of the read index by less than W and not held:
//                      Receive(o, d)
//   receive_completing_fragment
//                      the last missing fragment of a code-3 block at the
//                      read index: Receive(read, d)
//   receive_fragment   a fragment of a code-3 block at the read index that
//                      leaves the block short: ReceivePartial(d)
//   receive_duplicate  a code-3 record whose number the window has taken
//                      already, at an ordering o in turn or ahead:
//                      ReceiveDuplicate(o, d)
//   receive_unreliable a code-0 record, while the update's unreliable
//                      deliveries are below Du and the table has room:
//                      ReceiveUnreliable
//   receive_sequenced  a code-1 ("u") or code-4 ("r") record ahead of the
//                      read index, while the channel holds none:
//                      HoldSequenced(k)
//   end_update         rn_ordered_end_update: EndUpdate
//
// d is what rn_ordered_receive returned: TRUE for
// ROCKETNET_CHANNEL_RECEIVE_DEFERRED; it is the only part of a record's
// action the call's result gives. The kind of record is the walk's input
// and the op names it (whether a fragment completes its block is the walk's
// record of the fragments it sent), and OpActs gives each op one action; the
// model then fixes what the call may do with it, so a duplicate the code
// delivered, a fragment it delivered short, or a deferral it made with room
// is rejected at its record.
//
// What the reservation can decide here. A number stays live while it is
// held or delivered in the update (rn_channels_release), so the delivery
// table always has room for every live descriptor and the unreliable
// allowance left: below a full table a record that takes a free descriptor
// never defers, whatever the reservation counts for its held run, its
// sequenced hold or its in-turn compare. The trace therefore binds the
// duplicate's reservation (a duplicate takes no descriptor), every deferral
// at a full table, and the dispatch, the drain, the read index and the held
// set across the wire's wrap; the in-turn run count, the masked in-turn
// compare and the sequenced term never decide a deferral in a state the
// code reaches, and are bound by RNOrderingWrap's own cases only.
//
// Every state field is read from the code after the call:
//
//   read        region->channel_receive[0].ordered_read
//   held        the wire orderings of the held index's complete entries on
//               channel 0 (region->held_index, used 1: RN_HELD_COMPLETE of
//               sdk/rocketnet_channel.cpp, which the clean binary cannot see)
//   deliveries  channels->deliveries
//   exhausted   rn_region_exhausted( region )
//   uDelivered  channels->unreliable_delivered
//   seqHold     region->channel_receive[0].held_sequenced: "none" for
//               RN_DESCRIPTOR_NONE, "r" with RN_SEQUENCED_RELIABLE_HOLD set,
//               "u" otherwise
//
// The row's ghosts, disagree and wasted, are the model's history for
// CompareAgrees and NeverDeferWithRoom; the code holds no word for either.
//
// Before the first record the channel's read index is set OW_BEFORE_WRAP
// short of 2^24, so the walk's deliveries pass the wire's wrap: the state
// 2^24 - OW_BEFORE_WRAP deliveries leave, not a step of this model
// (ReadInit). The read counter's own wrap at 2^32 is past TLC's 32-bit
// integers: CounterMod is written as the largest multiple of 2^24 TLC holds,
// which the walk's read index never reaches, so its modulus never acts in a
// trace; MCRNOrderingWrap.cfg checks the counter's wrap at its small size.
//
// The walk is honest where the model is: a fresh record takes a number the
// window can accept into a free descriptor (or the table is full and it is
// deferred before the accept), its ordering is the read index or less than
// W ahead and not held, a block collects only at the read index, code 0 is
// sent only while it would be delivered, and a sequenced record only into
// an empty hold, code 4 only while the table has room. A free update picks
// calls at random. A fill update delivers in turn until only r descriptors
// are free, holds the run read+1 .. read+r, spends the unreliable allowance,
// and then, its tail, takes a code-1 hold and sends duplicates and fragments
// at the read index and fresh records into the table, so the deferrals
// happen. A sender's numbers are consecutive, so only a fill that starts with
// no descriptor live uses all W: update 1 of a seed is a fill with r = 0
// (the table full), update 2 a fill with r of one to three (the plant's
// case), update 3 a free update that opens with a code-4 hold and a record
// W - 1 ahead (when update 1's code-1 hold is still held, the hold is
// refused, that call ends the update instead and the opening is lost); then
// free and fill take turns, r drawn from 0 to 3. The walk fails if the connection
// ends (channels->error) or the region is exhausted: no record of the walk
// may do either.
//
// The plant (ROCKETNET_ORDERING_WRAP_PLANT, the tla_trace_plant_ordering_wrap
// build): the reservation is v1.1.52's and earlier, which counted a duplicate as a fresh
// record, so an in-turn duplicate reserves its held run
// (MCRNOrderingWrapBrokenDupRun.cfg). PLANT-FIRED counts the duplicates
// deferred with a delivery free, which the clean code never does: update 2
// of every seed holds a run of r behind a table with exactly r deliveries
// free, and its tail sends duplicates at the read index.

#if defined( ROCKETNET_TLA_PLANT )
#include "../sdk/rocketnet_channel.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_message.h"
#include "tla_trace_record.h"

#define OW_WINDOW       64      // reliable_messages_in_flight: the code's smallest window
#define OW_DU           4       // unreliable_messages_per_update
#define OW_BEFORE_WRAP  48      // the read index starts this far short of 2^24
#define OW_HELD_USED    1       // rn_held_entry_t used: RN_HELD_COMPLETE (sdk/rocketnet_channel.cpp)
#define OW_FILL_TAIL    8       // a fill update's calls into the full table before it ends
#define OW_BYTES        8       // a single record's bytes
#define OW_TRIES        4       // a free update's draws for a call the code's state allows

struct ow_walk_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    uint32_t next_number;           // the sender's next number
    // the one block collecting at the read index
    bool block;
    uint32_t block_first;
    uint32_t block_count;
    bool block_got[3];
    // the update: a fill update aims for r free descriptors, then its tail
    bool fill;
    int fill_free;
    int fill_tail;
    int updates;                    // the updates ended
    int opening;                    // the free update's scripted calls left: a code-4 hold, a record at the edge
};

static uint32_t ow_u24( uint64_t value )
{
    return (uint32_t) ( value & ROCKETNET_MESSAGE_U24_MASK );
}

static rn_channel_receive_t * ow_rx( ow_walk_t * walk )
{
    return &walk->region->channel_receive[0];
}

// the wire orderings of the complete held entries on channel 0, ascending
static std::vector<uint32_t> ow_held( const ow_walk_t * walk )
{
    std::vector<uint32_t> held;
    const rn_region_t * region = walk->region;
    for ( uint32_t i = 0; i < region->held_entries; i++ )
    {
        const rn_held_entry_t * entry = &region->held_index[i];
        if ( entry->used == OW_HELD_USED && ( entry->key >> 32 ) == 0 )
        {
            held.push_back( ow_u24( entry->key ) );
        }
    }
    std::sort( held.begin(), held.end() );
    return held;
}

static bool ow_is_held( const ow_walk_t * walk, uint32_t ordering )
{
    const std::vector<uint32_t> held = ow_held( walk );
    return std::find( held.begin(), held.end(), ow_u24( ordering ) ) != held.end();
}

static tla_record_t ow_state( ow_walk_t * walk )
{
    const rn_channel_receive_t * rx = ow_rx( walk );
    const rn_channels_t * channels = &walk->ordered.channels;

    std::string held = "{";
    const std::vector<uint32_t> orderings = ow_held( walk );
    for ( size_t i = 0; i < orderings.size(); i++ )
    {
        held += ( i > 0 ? ", " : "" ) + tla_int( orderings[i] );
    }
    held += "}";

    const char * hold = "\"none\"";
    if ( rx->held_sequenced != RN_DESCRIPTOR_NONE )
    {
        hold = ( rx->held_sequenced & RN_SEQUENCED_RELIABLE_HOLD ) != 0 ? "\"r\"" : "\"u\"";
    }

    tla_record_t state;
    state.add( "read", tla_int( rx->ordered_read ) );
    state.add( "held", held );
    state.add( "deliveries", tla_int( channels->deliveries ) );
    state.add( "exhausted", tla_bool( rn_region_exhausted( walk->region ) ) );
    state.add( "uDelivered", tla_int( channels->unreliable_delivered ) );
    state.add( "seqHold", hold );
    return state;
}

// a number the window can take now into a free descriptor
static bool ow_number_free( ow_walk_t * walk, uint32_t number )
{
    const uint64_t R = walk->ordered.receive.R;
    if ( number < R || number - R >= walk->region->window )
    {
        return false;
    }
    return walk->region->receive_descriptors[number & ( walk->region->window - 1 )].live == 0;
}

static uint32_t ow_live( ow_walk_t * walk )
{
    uint32_t live = 0;
    for ( uint32_t i = 0; i < walk->region->window; i++ )
    {
        live += walk->region->receive_descriptors[i].live != 0 ? 1 : 0;
    }
    return live;
}

static bool ow_table_full( ow_walk_t * walk )
{
    return walk->ordered.channels.deliveries >= walk->ordered.channels.delivery_capacity;
}

static uint32_t ow_du( ow_walk_t * walk )
{
    return walk->region->delivery_entries - walk->region->window;
}

// one payload of one record through rn_ordered_receive
static int ow_receive( ow_walk_t * walk, const rocketnet_message_record_t * record, bool * fault )
{
    alignas( 16 ) static uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rocketnet_message_writer_t writer;
    rocketnet_message_writer_begin( &writer, payload, (int) sizeof( payload ) );
    if ( rocketnet_message_writer_add( &writer, record ) != 1 )
    {
        fprintf( stderr, "tla_trace: ordering-wrap: the writer refused a code-%d record\n", record->code );
        *fault = true;
        return 0;
    }
    const int bytes = rocketnet_message_writer_finish( &writer );
    return rn_ordered_receive( &walk->ordered, payload, bytes );
}

static rocketnet_message_record_t ow_record( int code, uint32_t number, uint32_t ordering )
{
    static const uint8_t data[OW_BYTES] = { 0x52, 0x4E, 0x4F, 0x57, 0x01, 0x02, 0x03, 0x04 };
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = code;
    record.data = data;
    record.bytes = OW_BYTES;
    if ( code != ROCKETNET_MESSAGE_CODE_UNRELIABLE )
    {
        record.ordering = ow_u24( ordering );
    }
    if ( code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED || code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED )
    {
        record.number = ow_u24( number );
        record.ring = ( number & ( OW_WINDOW - 1 ) ) * ( ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16 );
    }
    return record;
}

static rocketnet_message_record_t ow_fragment( uint32_t first, uint32_t count, uint32_t index, uint32_t ordering )
{
    static uint8_t data[ROCKETNET_MESSAGE_FRAGMENT_BYTES];
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.block = 1;
    record.number = ow_u24( first + index );
    record.ordering = ow_u24( ordering );
    record.fragment = index;
    record.fragments = count - 1;
    record.ring = ( ( first & ( OW_WINDOW - 1 ) ) + index ) * ( ROCKETNET_MESSAGE_FRAGMENT_BYTES / 16 );
    record.data = data;
    record.bytes = index + 1 == count ? 100 : (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES;
    return record;
}

// the complete held run after the read index
static uint32_t ow_run( ow_walk_t * walk )
{
    uint32_t run = 0;
    while ( run + 1 < walk->region->window && ow_is_held( walk, ow_rx( walk )->ordered_read + run + 1 ) )
    {
        run++;
    }
    return run;
}

// the calls a step can make
enum ow_kind_t
{
    OW_END,
    OW_FRESH,
    OW_FRAGMENT,
    OW_DUPLICATE,
    OW_UNRELIABLE,
    OW_HOLD,
};

struct ow_call_t
{
    ow_kind_t kind;
    uint32_t ordering;      // a fresh record's, a duplicate's or a sequenced record's
    int hold_code;          // a sequenced record's code: 1 or 4
};

// whether the walk may make the call in the code's state now: an honest
// record the model has an action for (the file's header)
static bool ow_allowed( ow_walk_t * walk, const ow_call_t & call )
{
    const rn_channel_receive_t * rx = ow_rx( walk );
    const uint32_t read = rx->ordered_read;
    const bool full = ow_table_full( walk );
    switch ( call.kind )
    {
        case OW_FRESH:
            // not a second message at an ordering, and a number the window takes, or a full table that defers it first
            return !( call.ordering == read && walk->block ) && !ow_is_held( walk, call.ordering ) &&
                   ( full || ow_number_free( walk, walk->next_number ) );
        case OW_FRAGMENT:
            // a block's next fragment, or a new block's numbers free, or a full table that defers its first fragment
            return walk->block || full ||
                   ( ow_number_free( walk, walk->next_number ) && ow_number_free( walk, walk->next_number + 1 ) &&
                     ow_number_free( walk, walk->next_number + 2 ) );
        case OW_DUPLICATE:
            return walk->ordered.receive.R > 0;
        case OW_UNRELIABLE:
            return walk->ordered.channels.unreliable_delivered < ow_du( walk ) && !full;
        case OW_HOLD:
            if ( rx->held_sequenced != RN_DESCRIPTOR_NONE )
            {
                return false;
            }
            return call.hold_code == ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED ? walk->region->sequenced_hold_free_count > 0
                                                                                 : !full && ow_number_free( walk, walk->next_number );
        case OW_END:
            break;
    }
    return true;
}

static int trace_ordering_wrap( uint64_t seed, int steps, const char * out )
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    enum rocketnet_status_t status = rocketnet_init( NULL, NULL, NULL );
    if ( status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "tla_trace: ordering-wrap: rocketnet_init: %s\n", rocketnet_status_name( status ) );
        return 1;
    }

    rocketnet_message_config_t config;
    rocketnet_message_default_config( &config );
    config.reliable_messages_in_flight = OW_WINDOW;
    config.unreliable_messages_per_update = OW_DU;
    config.receive_message_buffer_bytes = 262144;
    config.max_message_bytes = 4 * ROCKETNET_MESSAGE_FRAGMENT_BYTES;   // a block of up to four: the walk's are two or three
    config.send_message_buffer_bytes = 800000;

    ow_walk_t * walk = new ow_walk_t();
    memset( &walk->counters, 0, sizeof( walk->counters ) );
    walk->arena = rn_arena_create( NULL, &config, 1, &walk->counters );
    walk->region = walk->arena ? rn_arena_region( walk->arena, 0 ) : NULL;
    if ( !walk->region || !rn_ordered_init( &walk->ordered, walk->region, &config, NULL, 4, ROCKETNET_MESSAGE_SINGLE_BYTES ) ||
         walk->region->window != OW_WINDOW || ow_du( walk ) != OW_DU )
    {
        fprintf( stderr, "tla_trace: ordering-wrap: no region of window %d and Du %d (arena %d, region %d)\n", OW_WINDOW, OW_DU,
                 walk->arena ? 1 : 0, walk->region ? 1 : 0 );
        return 1;
    }
    rn_region_t * region = walk->region;
    rn_channels_t * channels = &walk->ordered.channels;
    const uint32_t W = region->window;
    const uint32_t wire = ROCKETNET_MESSAGE_U24_MASK + 1u;

    // the state 2^24 - OW_BEFORE_WRAP deliveries leave: not a step of this model (ReadInit)
    const uint32_t read_init = wire - OW_BEFORE_WRAP;
    ow_rx( walk )->ordered_read = read_init;
    walk->next_number = (uint32_t) walk->ordered.receive.R;

    // the first update fills the table whole, from no descriptor live
    tla_random_t random = { seed };
    walk->fill = true;
    walk->fill_free = 0;
    walk->fill_tail = OW_FILL_TAIL;
    std::vector<tla_record_t> records;
    tla_report_t report;
    bool fault = false;

    records.push_back( tla_step( "init", "init", "<<>>", ow_state( walk ) ) );

    for ( int step = 0; step < steps && !fault; step++ )
    {
        const char * op = "end_update";
        const char * act = "EndUpdate";
        std::string args = "<<>>";
        bool planted = false;

        rn_channel_receive_t * rx = ow_rx( walk );
        const uint32_t read = rx->ordered_read;
        const uint32_t live = ow_live( walk );
        const bool full = ow_table_full( walk );
        const bool u_room = channels->unreliable_delivered < ow_du( walk ) && !full;

        // the call, by the update's kind, the code's state and the seed
        ow_call_t call = { OW_END, read, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED };
        const uint32_t back = (uint32_t) random.below( 16 );
        if ( walk->fill )
        {
            // a fill update's phases: in turn while more than fill_free
            // descriptors are free, then the run read+1 .. read+fill_free, the
            // unreliable allowance, then its tail of calls into the table
            const int pick = random.below( 100 );
            const uint32_t ahead = 1 + (uint32_t) random.below( (int) W - 1 );
            const bool at_read = random.below( 2 ) == 0;
            const uint32_t spare = W - live;
            if ( walk->block )
            {
                call.kind = OW_FRAGMENT;
            }
            else if ( spare > (uint32_t) walk->fill_free && ow_number_free( walk, walk->next_number ) && !full )
            {
                call.kind = OW_FRESH;
            }
            else if ( spare > 0 && spare <= (uint32_t) walk->fill_free && ow_number_free( walk, walk->next_number ) && !full &&
                      ow_run( walk ) + 1 < W )
            {
                call.kind = OW_FRESH;
                call.ordering = read + ow_run( walk ) + 1;
            }
            else if ( u_room )
            {
                call.kind = OW_UNRELIABLE;
            }
            else if ( walk->fill_tail > 0 )
            {
                // the tail opens with a code-1 hold, one with the allowance
                // spent; update 2 takes none, so update 3's code-4 hold has
                // the channel's empty hold
                const bool holds = walk->updates != 1;
                const bool opens = walk->fill_tail == OW_FILL_TAIL;
                walk->fill_tail--;
                if ( holds && ( opens || pick < 15 ) )
                {
                    call.kind = OW_HOLD;
                    call.ordering = read + ahead;
                }
                else if ( full && pick < 35 )
                {
                    call.kind = OW_FRESH;       // deferred before the accept: its number is not taken
                    call.ordering = at_read ? read : read + ahead;
                }
                else if ( full && pick < 50 )
                {
                    call.kind = OW_FRAGMENT;    // a block's first fragment at the read index, deferred
                }
                else
                {
                    call.kind = OW_DUPLICATE;
                    call.ordering = pick < 80 ? read : read + ahead;
                }
            }
        }
        else if ( walk->opening > 0 )
        {
            // the seed's first free update opens with a code-4 hold, then a
            // fresh record at the honest edge, W - 1 ahead
            const uint32_t ahead = 1 + (uint32_t) random.below( (int) W - 1 );
            call.kind = walk->opening == 2 ? OW_HOLD : OW_FRESH;
            call.ordering = walk->opening == 2 ? read + ahead : read + W - 1;
            call.hold_code = ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
            walk->opening--;
        }
        else
        {
            // a free update draws up to OW_TRIES calls and makes the first
            // the code's state allows; a draw of OW_END ends the update
            for ( int t = 0; t < OW_TRIES && call.kind == OW_END; t++ )
            {
                const int pick = random.below( 100 );
                const uint32_t ahead = 1 + (uint32_t) random.below( (int) W - 1 );
                const bool at_edge = random.below( 5 ) == 0;
                const bool at_read = random.below( 2 ) == 0;
                ow_call_t want = { OW_END, read, ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED };
                if ( pick < 30 )
                {
                    want.kind = OW_FRESH;
                    want.ordering = at_read ? read : read + ( at_edge ? W - 1 : ahead );
                }
                else if ( pick < 42 )
                {
                    want.kind = OW_FRAGMENT;
                }
                else if ( pick < 54 )
                {
                    want.kind = OW_DUPLICATE;
                    want.ordering = at_read ? read : read + ahead;
                }
                else if ( pick < 64 )
                {
                    want.kind = OW_UNRELIABLE;
                }
                else if ( pick < 76 )
                {
                    want.kind = OW_HOLD;
                    want.ordering = read + ahead;
                    want.hold_code = at_read ? ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED : ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED;
                }
                else if ( pick < 96 )
                {
                    want.kind = OW_FRESH;
                }
                else
                {
                    break;
                }
                if ( ow_allowed( walk, want ) )
                {
                    call = want;
                }
            }
            // the update ends with its block complete where the table allows
            if ( call.kind == OW_END && walk->block && !full )
            {
                call.kind = OW_FRAGMENT;
            }
        }

        // a call the code's state does not allow ends the update instead
        if ( !ow_allowed( walk, call ) )
        {
            call.kind = OW_END;
        }
        const uint32_t ordering = call.ordering;
        const int hold_code = call.hold_code;

        const uint64_t deliveries_before = channels->deliveries;
        if ( call.kind == OW_FRESH )
        {
            const rocketnet_message_record_t record = ow_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, walk->next_number, ordering );
            const int result = ow_receive( walk, &record, &fault );
            const bool deferred = result == ROCKETNET_CHANNEL_RECEIVE_DEFERRED;
            if ( !deferred )
            {
                walk->next_number++;
            }
            op = "receive_ordered";
            act = "Receive";
            args = tla_sequence( { tla_int( ow_u24( ordering ) ), tla_bool( deferred ) } );
        }
        else if ( call.kind == OW_FRAGMENT )
        {
            const bool start = !walk->block;
            uint32_t first = walk->next_number;
            uint32_t count = 2 + (uint32_t) random.below( 2 );
            uint32_t index = 1;
            if ( !start )
            {
                first = walk->block_first;
                count = walk->block_count;
                std::vector<uint32_t> missing;
                for ( uint32_t i = 0; i < count; i++ )
                {
                    if ( !walk->block_got[i] )
                    {
                        missing.push_back( i );
                    }
                }
                index = missing[(size_t) random.below( (int) missing.size() )];
            }
            else
            {
                index = (uint32_t) random.below( (int) count );
            }
            bool completes = !start;
            for ( uint32_t i = 0; i < count && completes; i++ )
            {
                completes = i == index || walk->block_got[i];
            }
            const rocketnet_message_record_t record = ow_fragment( first, count, index, read );
            const int result = ow_receive( walk, &record, &fault );
            const bool deferred = result == ROCKETNET_CHANNEL_RECEIVE_DEFERRED;
            if ( !deferred )
            {
                if ( start )
                {
                    walk->block = true;
                    walk->block_first = first;
                    walk->block_count = count;
                    memset( walk->block_got, 0, sizeof( walk->block_got ) );
                    walk->next_number += count;
                }
                walk->block_got[index] = true;
                if ( completes )
                {
                    walk->block = false;
                }
            }
            if ( completes )
            {
                op = "receive_completing_fragment";
                act = "Receive";
                args = tla_sequence( { tla_int( ow_u24( read ) ), tla_bool( deferred ) } );
            }
            else
            {
                op = "receive_fragment";
                act = "ReceivePartial";
                args = tla_sequence( { tla_bool( deferred ) } );
            }
        }
        else if ( call.kind == OW_DUPLICATE )
        {
            // a number the window took already: behind R
            const uint64_t R = walk->ordered.receive.R;
            const uint32_t number = (uint32_t) ( R - 1 - std::min<uint64_t>( back, R - 1 ) );
            const rocketnet_message_record_t record = ow_record( ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, number, ordering );
            const int result = ow_receive( walk, &record, &fault );
            const bool deferred = result == ROCKETNET_CHANNEL_RECEIVE_DEFERRED;
            op = "receive_duplicate";
            act = "ReceiveDuplicate";
            args = tla_sequence( { tla_int( ow_u24( ordering ) ), tla_bool( deferred ) } );
            // the planted effect: a duplicate deferred with a delivery free
            planted = deferred && deliveries_before < channels->delivery_capacity;
        }
        else if ( call.kind == OW_UNRELIABLE )
        {
            const rocketnet_message_record_t record = ow_record( ROCKETNET_MESSAGE_CODE_UNRELIABLE, 0, 0 );
            ow_receive( walk, &record, &fault );
            op = "receive_unreliable";
            act = "ReceiveUnreliable";
        }
        else if ( call.kind == OW_HOLD )
        {
            const rocketnet_message_record_t record = ow_record( hold_code, walk->next_number, ordering );
            const int result = ow_receive( walk, &record, &fault );
            if ( hold_code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED && result != ROCKETNET_CHANNEL_RECEIVE_DEFERRED )
            {
                walk->next_number++;
            }
            op = "receive_sequenced";
            act = "HoldSequenced";
            args = tla_sequence( { hold_code == ROCKETNET_MESSAGE_CODE_RELIABLE_SEQUENCED ? "\"r\"" : "\"u\"" } );
        }
        else
        {
            rn_ordered_end_update( &walk->ordered );
            // the next update (the schedule in the file's header)
            walk->updates++;
            walk->fill = walk->updates == 1 || ( walk->updates > 2 && walk->updates % 2 == 1 );
            walk->fill_free = walk->updates == 1 ? 1 + random.below( 3 ) : random.below( 4 );
            walk->fill_tail = OW_FILL_TAIL;
            walk->opening = walk->updates == 2 ? 2 : 0;
        }

        if ( channels->error != ROCKETNET_DISCONNECT_REASON_NONE || rn_region_exhausted( region ) )
        {
            fprintf( stderr, "tla_trace: ordering-wrap: step %d (%s) ended the connection: error %d, exhausted %d\n", step, op,
                     channels->error, rn_region_exhausted( region ) ? 1 : 0 );
            fault = true;
        }
        if ( ow_rx( walk )->ordered_read >= ( 2147483647u / wire ) * wire )
        {
            fprintf( stderr, "tla_trace: ordering-wrap: the read index reached the trace's CounterMod\n" );
            fault = true;
        }

        report.act( act );
        records.push_back( tla_step( op, act, args, ow_state( walk ) ) );
        if ( planted )
        {
            report.planted( (int) records.size() );
        }
    }

    report.print();

    char constants[640];
    snprintf( constants, sizeof( constants ),
              "    W = %u\n"
              "    WireMod = %u\n"
              "    CounterMod = %u\n"
              "    Cap = %u\n"
              "    Du = %u\n"
              "    Masked = TRUE\n"
              "    ExactSeq = TRUE\n"
              "    DupOne = TRUE\n"
              "    PartialOne = TRUE\n"
              "    ReadInit = %u\n",
              W, wire, ( 2147483647u / wire ) * wire, channels->delivery_capacity, ow_du( walk ), read_init );
    const bool written = !fault && tla_write_trace( out, "MCTraceOrderingWrap", seed, "TraceRNOrderingWrap", "", constants, records );

    rn_arena_destroy( walk->arena );
    delete walk;
    rocketnet_term();

    return written ? 0 : 1;
}

TLA_TRACE_LAYER( "ordering-wrap", trace_ordering_wrap );
