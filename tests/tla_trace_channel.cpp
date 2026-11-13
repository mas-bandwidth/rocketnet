/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the message layer's channels end to end
// (proofs/tla/README-TraceCheck.md, proofs/tla/RNChannel.tla,
// proofs/tla/TraceRNChannel.tla).
//
// The code under test is two endpoints (sdk/rocketnet_endpoint.cpp) back to
// back: A sends, B receives. Under them run the ordered send and its counters
// (rn_channels_send_ordered), late numbering in the write pass
// (rn_ordered_write, rn_window_send_first_write and _block), the packet
// builder and the sent map (rn_window_ack_commit_packet), the unreliable
// path and the send budget, B's window, dispatch, block collection and
// ordered delivery (rn_ordered_receive, rn_channels_deliver_ordered), the
// update's end (rn_endpoint_begin_update) and A's ack translation
// (rn_endpoint_on_acked_ids). The walk is the application on both ends and
// the transport between them: the wire in each direction, its loss,
// duplication and reorder, the replay window and the packet acks.
//
// The instance is RNChannel's: four reliable ordered messages, 0, 1, 2 and 3,
// sent in that order; 0, 2 and 3 on code channel 0 (model channel 1), 1 on
// code channel 1 (model channel 2). Message 2 is a block of two fragments
// (ROCKETNET_MESSAGE_FRAGMENT_BYTES + CH_TAIL bytes), the rest are small; one
// unreliable message of CH_UNRELIABLE_BYTES rides on code channel
// CH_UNRELIABLE_CHANNEL. The model's reliable number n is the code's number
// base + n, base the first number after A's CONFIG. Each message carries a
// token at the start of each fragment, 0xA0 + 4 * message + index, which the
// walk reads back from B's ring and from its deliveries, so a chunk placed at
// the wrong index or a delivery of the wrong message is in the state.
//
// The ops and their records:
//
//   probe_write    A's channel_send[0].sequenced_write set to 1, once, first:
//                  GhostSequencedWrite
//   probe_read     B's channel_receive[0].sequenced_watermark set to 1, once,
//                  second: GhostSequencedRead
//   send_ordered   rn_endpoint_send of the next message: Enqueue
//   write_pass     time moves on, then A's write pass (begin_update,
//                  begin_flush, next_packet_bytes, write_packet, packet_sent),
//                  one packet at a time: NumberRecord for each message the
//                  window numbered (A's send.next), then WritePacket(s, pid,
//                  unrel) for the packet written, s read from A's sent map
//                  entry for it and unrel from its kind, or
//                  DropUnreliableForBudget when the pass dropped the
//                  unreliable message (ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET)
//   open_payload   the head of the forward wire handed to B: OpenPacket(p),
//                  then DeliverMessage(m) for each ordered delivery
//                  rn_endpoint_on_payload returned, in its order (a copy
//                  whose id the replay window has seen is not handed to the
//                  code, and is the OpenPacket alone)
//   fix_batch      B's update stops receiving: FixBatch
//   send_ack       the ids B opened since the last ack go back on the
//                  reverse wire: SendAck
//   end_update     rn_endpoint_begin_update on B: EndUpdate
//   acked_ids      the head of the reverse wire, as A's packet ids, into
//                  rn_endpoint_on_acked_ids: ReceiveAck(q)
//   fwd_drop, fwd_duplicate, fwd_reorder, rev_drop, rev_duplicate,
//   rev_reorder    the wires: FwdEnvironment, RevEnvironment
//
// Every act is the code's result: which messages were numbered (send.next),
// whether a packet was written and of which kind, the drop counter, and the
// deliveries on_payload returned. The op names the call.
//
// Every state field after a call is read from the code, or is the walk's
// own part of the system (the application's sends and what it took, the
// wires, the replay window, the acks owed, B's update phase):
//
//   from A         queued (the sends accepted), admitted (send.next), acked
//                  (send.U and the per-number acked flag), written (the
//                  per-number last send), smap (the sent map's unacked
//                  entries and their references), ordered_write and
//                  sequenced_write (channel_send), uDropped (the counter)
//   from B         received (rn_window_receive_duplicate), live (the receive
//                  descriptors), chunks (the token of each live number, in
//                  the receive ring or the delivery it is in), ordered_read
//                  and sequenced_watermark (channel_receive), rxQueue, batch
//                  and deliveredCount (the deliveries returned this update),
//                  uCount (the unreliable deliveries), resetOK (both
//                  counters zero after each ordered send and delivery)
//   the walk's     sentTrace, trace, phase, pending, seen, uSent, probeWrite,
//                  probeRead, fwdWire, fwdSent, fwdPulse, revWire, revSent,
//                  revPulse; taken 0, notReceived 0 and misused FALSE (the
//                  application takes every delivery, AppMayLeave FALSE)
//   fixed          waitedForRoom and roomReturned FALSE (W = 64 always has
//                  room for the block), refused FALSE (the walk never sends
//                  into a full queue), forgotten {}, reserved 0,
//                  duplicateReady {}, whole and placed TRUE, collision FALSE:
//                  the values of the model's healthy instance, which its
//                  actions keep
//
// A call that is several model steps writes several records: a write pass
// that numbered a message and wrote a packet, a payload with deliveries. The
// last record of a call is read from the code; the ones before it are that
// state with the later steps' fields taken back: admitted before the packet
// for NumberRecord, and for OpenPacket and each DeliverMessage the read
// indices, rxQueue, batch, deliveredCount and resetOK as the deliveries so far leave
// them (a channel's read index after a delivery that is not its last of the
// call is the delivered ordering + 1, the code's rule; its watermark is the
// code's, after the call).
//
// Packet ids are renamed as the model does (RNChannel README, "Packet
// identities"): a written packet takes the least model id not in the sent
// map, on either wire or owed an ack, and the walk keeps which code id each
// model id stands for. MaxPid bounds the ids; the walk writes no packet when
// none is free.
//
// The walk is honest where the model is: it sends message 3 only while one
// of numbers 0 and 1 at most is unacked, so a packet never carries three
// small records (the model's Loads); the forward wire holds at most
// CH_CAPACITY copies and the reverse CH_ACK_CAPACITY; drops are bounded so
// the ids do not run out. Time is an argument: each write pass adds one of a
// few steps to now. The send budget is CH_KBPS. On an even seed the
// unreliable message is sent right after the first chunk is written, with no
// time passed, so the budget drops it; on an odd seed it is sent once every
// reliable number is acked, after the budget has refilled, so it is written
// and opened. On a seed 3 mod 4 the forward wire drops each copy of the
// block's first chunk (within CH_FWD_DROPS) until B has received the second,
// so the second chunk is placed first. When every message is delivered and acked and the wires are
// empty, the walk ends B's updates until the trace has its steps.
//
// The plants, ROCKETNET_CHANNEL_SEQUENCED_WRITE_PLANT and
// ROCKETNET_CHANNEL_SEQUENCED_READ_PLANT (one plant binary each): the ordered
// send, or the ordered delivery, leaves the channel's sequenced counter as it
// was (MCRNChannelBrokenNoSequencedReset.cfg, its two halves). PLANT-FIRED
// counts the ordered sends and deliveries on code channel 0 that leave its
// counter at the probe's 1; the first is message 0's send, or its delivery.

#if defined( ROCKETNET_TLA_PLANT )
#include "../sdk/rocketnet_channel.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_endpoint.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_send_budget.h"
#include "tla_trace_record.h"

#define CH_MESSAGES             4       // RNChannel's Messages, 0..3
#define CH_NUMS                 5       // its Nums, 0..4
#define CH_WINDOW               64      // reliable_messages_in_flight
#define CH_PACKETS              64      // packets_in_flight: the sent map
#define CH_MAX_PID              63      // the model's MaxPid
#define CH_CAPACITY             6       // copies on the forward wire
#define CH_ACK_CAPACITY         3       // acks on the reverse wire
#define CH_FWD_DROPS            5       // forward copies dropped at most
#define CH_REV_DROPS            3       // acks dropped at most
#define CH_MAX_WRITES           40      // packets A writes at most
#define CH_SMALL_BYTES          24      // a small message
#define CH_TAIL                 400     // the block's second fragment: F + CH_TAIL is past the single-record limit (rn_channels_send_ordered), so two chunks
#define CH_UNRELIABLE_BYTES     1000    // the unreliable message: more than the bucket holds after a chunk
#define CH_UNRELIABLE_CHANNEL   3
#define CH_KBPS                 200     // the send budget: 2500 bytes of depth, one admitted packet (1452) after a chunk only once 10 ms have refilled it
#define CH_TOKEN                0xA0    // a fragment's token: CH_TOKEN + 4 * message + index
#define CH_UNRELIABLE_TOKEN     0xEE

// RNChannel's instance
static int ch_channel( int m )
{
    return m == 1 ? 2 : 1;
}

static int ch_order( int m )
{
    return m <= 1 ? 0 : ( m == 2 ? 1 : 2 );
}

static int ch_first( int m )
{
    return m <= 2 ? m : 4;
}

static int ch_parts( int m )
{
    return m == 2 ? 2 : 1;
}

struct ch_packet_t
{
    int pid;
    std::vector<int> nums;
    bool unrel;
    uint64_t id;
    std::vector<uint8_t> bytes;
};

struct ch_side_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_endpoint_t endpoint;
};

struct ch_walk_t
{
    rocketnet_message_config_t config;
    ch_side_t a, b;
    double now;
    uint64_t base;                          // the code's number of the model's number 0
    uint64_t over_budget_base;
    uint64_t next_id;                       // A's next transport packet id
    uint64_t pid_code[CH_MAX_PID + 1];      // the code id each model id stands for, 0 for none

    // the application
    int queued;
    std::vector<int> sent_trace[3];         // by model channel 1, 2
    std::vector<int> trace[3];
    std::vector<int> rx_queue;              // this update's ordered deliveries
    std::vector<int> batch;                 // their numbers
    const uint8_t * delivered_data[CH_MESSAGES];
    int delivered_count;
    int u_count;
    bool u_sent;
    bool u_queued;
    bool reset_ok;
    bool probe_write;
    bool probe_read;

    // the transport
    std::vector<ch_packet_t> fwd;
    std::vector<std::vector<int>> rev;
    int fwd_sent, rev_sent, fwd_pulse, rev_pulse;
    std::vector<uint64_t> opened;           // the replay window: code ids B opened
    std::vector<int> pending;               // model ids owed an ack
    std::vector<int> seen;                  // model ids opened since their write
    std::deque<std::vector<uint8_t>> update_payloads;   // B's payloads, alive until its next update
    bool receiving;                         // B's phase: "receive", else "process"
    int fwd_drops, rev_drops, writes;
    bool chunk_written;
};

static std::string ch_set( std::vector<int> values )
{
    std::sort( values.begin(), values.end() );
    values.erase( std::unique( values.begin(), values.end() ), values.end() );
    std::string text = "{";
    for ( size_t i = 0; i < values.size(); i++ )
    {
        text += ( i > 0 ? ", " : "" ) + tla_int( values[i] );
    }
    return text + "}";
}

static std::string ch_ints( const std::vector<int> & values )
{
    std::vector<std::string> items;
    for ( size_t i = 0; i < values.size(); i++ )
    {
        items.push_back( tla_int( values[i] ) );
    }
    return tla_sequence( items );
}

static std::string ch_by_channel( const std::string & one, const std::string & two )
{
    return tla_function( { "1", "2" }, { one, two } );
}

static bool ch_has( const std::vector<int> & values, int value )
{
    return std::find( values.begin(), values.end(), value ) != values.end();
}

static std::string ch_packet_value( const ch_packet_t & packet )
{
    return "[id |-> " + tla_int( packet.pid ) + ", nums |-> " + ch_set( packet.nums ) + ", unrel |-> " + tla_bool( packet.unrel ) + "]";
}

static rn_region_t * ch_region( ch_side_t * side )
{
    return side->endpoint.region;
}

static uint64_t ch_counter( const ch_side_t * side, int counter )
{
    return side->counters.values[counter];
}

// the model number of a code number, or -1 outside the instance
static int ch_model_number( const ch_walk_t * walk, uint64_t number )
{
    return number >= walk->base && number < walk->base + CH_NUMS ? (int) ( number - walk->base ) : -1;
}

static int ch_pid_of( const ch_walk_t * walk, uint64_t id )
{
    for ( int p = 0; p <= CH_MAX_PID; p++ )
    {
        if ( walk->pid_code[p] == id )
        {
            return p;
        }
    }
    return -1;
}

// A's sent map: the entries not yet acked, oldest first, as model ids and numbers
struct ch_entry_t
{
    int pid;
    std::vector<int> nums;
};

static std::vector<ch_entry_t> ch_smap( ch_walk_t * walk, bool * fault )
{
    std::vector<ch_entry_t> entries;
    const rn_region_t * region = ch_region( &walk->a );
    const rn_window_ack_t * ack = &walk->a.endpoint.ordered.ack;
    for ( uint64_t i = ack->pkt_head; i < ack->pkt_tail; i++ )
    {
        const rn_sent_packet_t * sent = &region->sent_packets[i % region->packets];
        if ( sent->packet_id == 0 || sent->ref_count == 0 )
        {
            continue;
        }
        ch_entry_t entry;
        entry.pid = ch_pid_of( walk, sent->packet_id );
        for ( uint32_t k = 0; k < sent->ref_count; k++ )
        {
            const uint32_t ref = region->sent_refs[( (uint64_t) sent->ref_first + k ) % region->sent_ref_entries];
            const int n = ch_model_number( walk, ref );
            if ( n < 0 )
            {
                fprintf( stderr, "tla_trace: channel: sent map entry %" PRIu64 " names number %u, outside the instance\n", sent->packet_id, ref );
                *fault = true;
            }
            entry.nums.push_back( n );
        }
        if ( entry.pid < 0 )
        {
            fprintf( stderr, "tla_trace: channel: sent map entry %" PRIu64 " has no model id\n", sent->packet_id );
            *fault = true;
        }
        entries.push_back( entry );
    }
    return entries;
}

static bool ch_acked( ch_walk_t * walk, int n )
{
    const rn_window_send_t * send = &walk->a.endpoint.ordered.send;
    const uint64_t number = walk->base + (uint64_t) n;
    if ( number < send->U )
    {
        return true;
    }
    const rn_send_number_t * slot = &ch_region( &walk->a )->send_numbers[number % CH_WINDOW];
    return number < send->next && slot->number == number && slot->acked != 0;
}

static bool ch_written( ch_walk_t * walk, int n )
{
    const rn_window_send_t * send = &walk->a.endpoint.ordered.send;
    const uint64_t number = walk->base + (uint64_t) n;
    if ( ch_acked( walk, n ) )
    {
        return true;
    }
    const rn_send_number_t * slot = &ch_region( &walk->a )->send_numbers[number % CH_WINDOW];
    return number < send->next && slot->number == number && slot->last_send >= 0.0;
}

static int ch_admitted( ch_walk_t * walk )
{
    int admitted = 0;
    for ( int m = 0; m < CH_MESSAGES; m++ )
    {
        if ( walk->base + (uint64_t) ch_first( m ) < walk->a.endpoint.ordered.send.next )
        {
            admitted = m + 1;
        }
    }
    return admitted;
}

static bool ch_received( ch_walk_t * walk, int n )
{
    rocketnet_message_record_t record;
    memset( &record, 0, sizeof( record ) );
    record.code = ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED;
    record.number = (uint32_t) ( ( walk->base + (uint64_t) n ) & ROCKETNET_MESSAGE_U24_MASK );
    return rn_window_receive_duplicate( &walk->b.endpoint.ordered.receive, &record );
}

static const rn_receive_descriptor_t * ch_live( ch_walk_t * walk, int n )
{
    const uint64_t number = walk->base + (uint64_t) n;
    const rn_receive_descriptor_t * descriptor = &ch_region( &walk->b )->receive_descriptors[number % CH_WINDOW];
    return descriptor->live && descriptor->number == number ? descriptor : NULL;
}

static std::string ch_token( const uint8_t * data )
{
    const int b = data[0];
    if ( b < CH_TOKEN || b >= CH_TOKEN + 4 * CH_MESSAGES )
    {
        return "<<-2, -2>>";
    }
    return tla_sequence( { tla_int( ( b - CH_TOKEN ) / 4 ), tla_int( ( b - CH_TOKEN ) % 4 ) } );
}

// chunks: each live number's token, from the delivery it is in this update, else from B's ring
static std::string ch_chunks( ch_walk_t * walk )
{
    std::vector<std::string> keys, values;
    for ( int m = 0; m < CH_MESSAGES; m++ )
    {
        std::vector<std::string> index_keys, tokens;
        for ( int i = 0; i < ch_parts( m ); i++ )
        {
            const int n = ch_first( m ) + i;
            const rn_receive_descriptor_t * descriptor = ch_live( walk, n );
            std::string token = "<<-1, -1>>";
            if ( descriptor && walk->delivered_data[m] )
            {
                token = ch_token( walk->delivered_data[m] + (size_t) i * ROCKETNET_MESSAGE_FRAGMENT_BYTES );
            }
            else if ( descriptor )
            {
                token = ch_token( ch_region( &walk->b )->receive_ring + (uint64_t) descriptor->ring * 16 );
            }
            index_keys.push_back( tla_int( i ) );
            tokens.push_back( token );
        }
        keys.push_back( tla_int( m ) );
        values.push_back( tla_function( index_keys, tokens ) );
    }
    return tla_function( keys, values );
}

static std::string ch_fwd_wire( const ch_walk_t * walk )
{
    std::vector<std::string> copies;
    for ( size_t i = 0; i < walk->fwd.size(); i++ )
    {
        copies.push_back( "[pkt |-> " + ch_packet_value( walk->fwd[i] ) + ", due |-> 0]" );
    }
    return tla_sequence( copies );
}

static std::string ch_rev_wire( const ch_walk_t * walk )
{
    std::vector<std::string> copies;
    for ( size_t i = 0; i < walk->rev.size(); i++ )
    {
        copies.push_back( "[pkt |-> " + ch_set( walk->rev[i] ) + ", due |-> 0]" );
    }
    return tla_sequence( copies );
}

static std::string ch_counters( const rn_region_t * region, bool send, bool sequenced )
{
    std::string values[2];
    for ( int c = 0; c < 2; c++ )
    {
        const uint32_t value = send ? ( sequenced ? region->channel_send[c].sequenced_write : region->channel_send[c].ordered_write )
                                    : ( sequenced ? region->channel_receive[c].sequenced_watermark : region->channel_receive[c].ordered_read );
        values[c] = tla_int( value );
    }
    return ch_by_channel( values[0], values[1] );
}

static void ch_set_field( tla_record_t * record, const char * field, const std::string & value )
{
    for ( size_t i = 0; i < record->fields.size(); i++ )
    {
        if ( record->fields[i] == field )
        {
            record->values[i] = value;
            return;
        }
    }
    fprintf( stderr, "tla_trace: channel: no state field %s\n", field );
    tla_value_errors()++;
}

static tla_record_t ch_state( ch_walk_t * walk, bool * fault )
{
    std::vector<int> written, acked, received, live;
    for ( int n = 0; n < CH_NUMS; n++ )
    {
        if ( ch_written( walk, n ) )
        {
            written.push_back( n );
        }
        if ( ch_acked( walk, n ) )
        {
            acked.push_back( n );
        }
        if ( ch_received( walk, n ) )
        {
            received.push_back( n );
        }
        if ( ch_live( walk, n ) )
        {
            live.push_back( n );
        }
    }
    std::vector<int> admitted;
    for ( int m = 0; m < ch_admitted( walk ); m++ )
    {
        admitted.push_back( m );
    }

    const std::vector<ch_entry_t> entries = ch_smap( walk, fault );
    std::vector<std::string> smap;
    for ( size_t i = 0; i < entries.size(); i++ )
    {
        smap.push_back( "[id |-> " + tla_int( entries[i].pid ) + ", nums |-> " + ch_set( entries[i].nums ) + "]" );
    }

    const rn_region_t * a = ch_region( &walk->a );
    const rn_region_t * b = ch_region( &walk->b );

    tla_record_t state;
    state.add( "written", ch_set( written ) );
    state.add( "waitedForRoom", "FALSE" );
    state.add( "roomReturned", "FALSE" );
    state.add( "refused", "FALSE" );
    state.add( "uDropped", tla_int( (int64_t) ( ch_counter( &walk->a, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) - walk->over_budget_base ) ) );
    state.add( "queued", tla_int( walk->queued ) );
    state.add( "admitted", ch_set( admitted ) );
    state.add( "acked", ch_set( acked ) );
    state.add( "smap", tla_sequence( smap ) );
    state.add( "forgotten", "{}" );
    state.add( "reserved", "0" );
    state.add( "ordered_write", ch_counters( a, true, false ) );
    state.add( "sequenced_write", ch_counters( a, true, true ) );
    state.add( "ordered_read", ch_counters( b, false, false ) );
    state.add( "sequenced_watermark", ch_counters( b, false, true ) );
    state.add( "sentTrace", ch_by_channel( ch_ints( walk->sent_trace[1] ), ch_ints( walk->sent_trace[2] ) ) );
    state.add( "trace", ch_by_channel( ch_ints( walk->trace[1] ), ch_ints( walk->trace[2] ) ) );
    state.add( "rxQueue", ch_ints( walk->rx_queue ) );
    state.add( "chunks", ch_chunks( walk ) );
    state.add( "received", ch_set( received ) );
    state.add( "live", ch_set( live ) );
    state.add( "batch", ch_set( walk->batch ) );
    state.add( "phase", walk->receiving ? "\"receive\"" : "\"process\"" );
    state.add( "pending", ch_set( walk->pending ) );
    state.add( "duplicateReady", "{}" );
    state.add( "whole", "TRUE" );
    state.add( "placed", "TRUE" );
    state.add( "collision", "FALSE" );
    state.add( "uSent", tla_bool( walk->u_sent ) );
    state.add( "uCount", tla_int( walk->u_count ) );
    state.add( "seen", ch_set( walk->seen ) );
    state.add( "probeWrite", tla_bool( walk->probe_write ) );
    state.add( "probeRead", tla_bool( walk->probe_read ) );
    state.add( "resetOK", tla_bool( walk->reset_ok ) );
    state.add( "taken", "0" );
    state.add( "notReceived", "0" );
    state.add( "misused", "FALSE" );
    state.add( "deliveredCount", tla_int( walk->delivered_count ) );
    state.add( "fwdWire", ch_fwd_wire( walk ) );
    state.add( "fwdSent", tla_int( walk->fwd_sent ) );
    state.add( "fwdPulse", tla_int( walk->fwd_pulse ) );
    state.add( "revWire", ch_rev_wire( walk ) );
    state.add( "revSent", tla_int( walk->rev_sent ) );
    state.add( "revPulse", tla_int( walk->rev_pulse ) );
    return state;
}

// Link's send counter under "arbitrary" with BurstStart = BurstLen = 1: Horizon is 4
static int ch_next_count( int sent )
{
    return sent < 4 ? sent + 1 : 3;
}

static std::vector<int> ch_free_pids( const ch_walk_t * walk, const std::vector<ch_entry_t> & entries )
{
    std::vector<int> used = walk->pending;
    for ( size_t i = 0; i < entries.size(); i++ )
    {
        used.push_back( entries[i].pid );
    }
    for ( size_t i = 0; i < walk->fwd.size(); i++ )
    {
        used.push_back( walk->fwd[i].pid );
    }
    for ( size_t i = 0; i < walk->rev.size(); i++ )
    {
        used.insert( used.end(), walk->rev[i].begin(), walk->rev[i].end() );
    }
    std::vector<int> unused;
    for ( int p = 0; p <= CH_MAX_PID; p++ )
    {
        if ( !ch_has( used, p ) )
        {
            unused.push_back( p );
        }
    }
    return unused;
}

static int ch_unacked( ch_walk_t * walk )
{
    int unacked = 0;
    const int admitted = ch_admitted( walk );
    for ( int m = 0; m < admitted; m++ )
    {
        for ( int i = 0; i < ch_parts( m ); i++ )
        {
            unacked += ch_acked( walk, ch_first( m ) + i ) ? 0 : 1;
        }
    }
    return unacked;
}

static void ch_side_create( ch_side_t * side, const rocketnet_message_config_t * config, bool * fault )
{
    memset( &side->counters, 0, sizeof( side->counters ) );
    side->arena = rn_arena_create( NULL, config, 1, &side->counters );
    if ( !side->arena )
    {
        fprintf( stderr, "tla_trace: channel: no arena\n" );
        *fault = true;
        return;
    }
    rn_endpoint_config_t endpoint_config;
    memset( &endpoint_config, 0, sizeof( endpoint_config ) );
    endpoint_config.config = config;
    endpoint_config.send_kbps = CH_KBPS;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    rn_endpoint_init( &side->endpoint, rn_arena_region( side->arena, 0 ), &endpoint_config );
}

// the CONFIG exchange before record 1: each end's CONFIG written, opened by
// the other and acked, until both ends took the other's and every number is acked
static bool ch_handshake( ch_walk_t * walk )
{
    ch_side_t * sides[2] = { &walk->a, &walk->b };
    uint64_t ids[2] = { 1, 1 };
    for ( int round = 0; round < 8; round++ )
    {
        for ( int s = 0; s < 2; s++ )
        {
            rn_endpoint_t * from = &sides[s]->endpoint;
            rn_endpoint_t * to = &sides[1 - s]->endpoint;
            rn_endpoint_begin_update( from );
            rn_endpoint_begin_flush( from );
            while ( rn_endpoint_next_packet_bytes( from, walk->now ) > 0 )
            {
                static uint8_t payload[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
                const uint64_t id = ids[s]++;
                const int bytes = rn_endpoint_write_packet( from, payload, (int) sizeof( payload ), id, walk->now );
                if ( bytes <= 0 )
                {
                    break;
                }
                rn_endpoint_packet_sent( from, id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), walk->now );
                int count = 0;
                rn_endpoint_on_payload( to, payload, bytes, NULL, 0, &count, walk->now );
                rn_endpoint_on_acked( from, id, walk->now );
            }
        }
        walk->now += 0.01;
        const bool done = walk->a.endpoint.ordered.config_received && walk->b.endpoint.ordered.config_received &&
                          walk->a.endpoint.ordered.send.U == walk->a.endpoint.ordered.send.next &&
                          walk->b.endpoint.ordered.send.U == walk->b.endpoint.ordered.send.next;
        if ( done )
        {
            rn_endpoint_begin_update( &walk->a.endpoint );
            rn_endpoint_begin_update( &walk->b.endpoint );
            walk->next_id = ids[0];
            return true;
        }
    }
    return false;
}

// the calls a step can make
enum ch_kind_t
{
    CH_NONE,
    CH_SEND,
    CH_WRITE,
    CH_OPEN,
    CH_FIX,
    CH_ACK,
    CH_END,
    CH_ACKED,
    CH_FWD_DROP,
    CH_FWD_DUPLICATE,
    CH_FWD_REORDER,
    CH_REV_DROP,
    CH_REV_DUPLICATE,
    CH_REV_REORDER,
    CH_UNRELIABLE,
};

static bool ch_same_packet( const ch_packet_t & x, const ch_packet_t & y )
{
    return x.pid == y.pid && x.nums == y.nums && x.unrel == y.unrel;
}

static int trace_channel( uint64_t seed, int steps, const char * out )
{
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    enum rocketnet_status_t status = rocketnet_init( NULL, NULL, NULL );
    if ( status != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "tla_trace: channel: rocketnet_init: %s\n", rocketnet_status_name( status ) );
        return 1;
    }

    ch_walk_t * walk = new ch_walk_t();
    rocketnet_message_default_config( &walk->config );
    walk->config.reliable_messages_in_flight = CH_WINDOW;
    walk->config.send_queue_messages = 128;
    walk->config.packets_in_flight = CH_PACKETS;
    walk->config.receive_message_buffer_bytes = 65536;
    walk->config.max_message_bytes = 65536;
    walk->config.send_message_buffer_bytes = 262144;
    walk->config.packets_per_update = 4;
    walk->config.message_resend_delay_min_ms = 20;
    walk->config.message_resend_delay_initial_ms = 100;
    walk->config.message_resend_delay_max_ms = 250;

    bool fault = false;
    ch_side_create( &walk->a, &walk->config, &fault );
    ch_side_create( &walk->b, &walk->config, &fault );
    if ( fault )
    {
        return 1;
    }

    walk->now = 1000.0;
    rn_endpoint_reset( &walk->a.endpoint, walk->now );
    rn_endpoint_reset( &walk->b.endpoint, walk->now );
    rn_endpoint_announce_config( &walk->a.endpoint, walk->now );
    rn_endpoint_announce_config( &walk->b.endpoint, walk->now );
    if ( !ch_handshake( walk ) || ch_region( &walk->a )->window != CH_WINDOW || ch_region( &walk->a )->packets != CH_PACKETS )
    {
        fprintf( stderr, "tla_trace: channel: the CONFIG exchange did not finish, or the region is not W %d, P %d\n", CH_WINDOW, CH_PACKETS );
        return 1;
    }
    walk->base = walk->a.endpoint.ordered.send.next;
    if ( walk->b.endpoint.ordered.receive.R != walk->base )
    {
        fprintf( stderr, "tla_trace: channel: B's window starts at %" PRIu64 ", A numbers from %" PRIu64 "\n", walk->b.endpoint.ordered.receive.R, walk->base );
        return 1;
    }
    walk->over_budget_base = ch_counter( &walk->a, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
    walk->reset_ok = true;
    walk->receiving = true;

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", "<<>>", ch_state( walk, &fault ) ) );

    const bool drop_unreliable = seed % 2 == 0;
    // seed 3 mod 4: the forward wire loses every copy of the block's first
    // chunk until B has its second, so the chunks arrive out of order
    const bool lose_first_chunk = seed % 4 == 3;
    // 0.01 refills 250 bytes, enough for a second packet after a chunk at CH_KBPS,
    // before the chunk's resend is due (20 ms at least): the next chunk goes ahead of it
    static const double dts[5] = { 0.0, 0.01, 0.03, 0.08, 0.15 };

    while ( (int) records.size() < steps + 1 && !fault )
    {
        // the probes, first
        if ( !walk->probe_write || !walk->probe_read )
        {
            const bool write = !walk->probe_write;
            if ( write )
            {
                ch_region( &walk->a )->channel_send[0].sequenced_write = 1;
                walk->probe_write = true;
            }
            else
            {
                ch_region( &walk->b )->channel_receive[0].sequenced_watermark = 1;
                walk->probe_read = true;
            }
            report.act( write ? "GhostSequencedWrite" : "GhostSequencedRead" );
            records.push_back( tla_step( write ? "probe_write" : "probe_read", write ? "GhostSequencedWrite" : "GhostSequencedRead", "<<>>",
                                         ch_state( walk, &fault ) ) );
            continue;
        }

        const std::vector<ch_entry_t> entries = ch_smap( walk, &fault );
        const bool pid_free = !ch_free_pids( walk, entries ).empty();
        const int unacked = ch_unacked( walk );
        bool small_unacked = false;
        if ( !ch_acked( walk, 0 ) && !ch_acked( walk, 1 ) )
        {
            small_unacked = true;
        }

        // the calls the state allows, each with its weight
        struct option_t
        {
            ch_kind_t kind;
            int weight;
        };
        std::vector<option_t> options;
        const bool can_write = walk->receiving && walk->fwd.size() < CH_CAPACITY && pid_free && walk->writes < CH_MAX_WRITES;
        if ( walk->queued < CH_MESSAGES && !( walk->queued == 3 && small_unacked ) )
        {
            options.push_back( { CH_SEND, 20 } );
        }
        if ( can_write )
        {
            options.push_back( { CH_WRITE, 30 } );
        }
        if ( walk->receiving && !walk->fwd.empty() )
        {
            options.push_back( { CH_OPEN, 30 } );
        }
        if ( walk->receiving )
        {
            options.push_back( { CH_FIX, 8 } );
        }
        else
        {
            if ( !walk->pending.empty() && walk->rev.size() < CH_ACK_CAPACITY )
            {
                options.push_back( { CH_ACK, 30 } );
            }
            options.push_back( { CH_END, 15 } );
        }
        if ( !walk->rev.empty() )
        {
            options.push_back( { CH_ACKED, 20 } );
        }
        if ( !walk->fwd.empty() && walk->fwd_drops < CH_FWD_DROPS )
        {
            options.push_back( { CH_FWD_DROP, 4 } );
        }
        if ( !walk->fwd.empty() && walk->fwd.size() < CH_CAPACITY )
        {
            options.push_back( { CH_FWD_DUPLICATE, 3 } );
        }
        if ( walk->fwd.size() >= 2 )
        {
            options.push_back( { CH_FWD_REORDER, 4 } );
        }
        if ( !walk->rev.empty() && walk->rev_drops < CH_REV_DROPS )
        {
            options.push_back( { CH_REV_DROP, 2 } );
        }
        if ( !walk->rev.empty() && walk->rev.size() < CH_ACK_CAPACITY )
        {
            options.push_back( { CH_REV_DUPLICATE, 1 } );
        }
        if ( walk->rev.size() >= 2 )
        {
            options.push_back( { CH_REV_REORDER, 2 } );
        }

        // the unreliable message: on an even seed right after the first
        // chunk is written, on an odd one once every number is acked
        ch_kind_t kind = CH_NONE;
        size_t lose = walk->fwd.size();
        if ( lose_first_chunk && walk->fwd_drops < CH_FWD_DROPS && !ch_received( walk, 3 ) )
        {
            for ( size_t i = 0; i < walk->fwd.size() && lose == walk->fwd.size(); i++ )
            {
                if ( std::find( walk->fwd[i].nums.begin(), walk->fwd[i].nums.end(), 2 ) != walk->fwd[i].nums.end() )
                {
                    lose = i;
                }
            }
        }
        if ( lose < walk->fwd.size() )
        {
            kind = CH_FWD_DROP;
        }
        else if ( !walk->u_queued && can_write &&
             ( drop_unreliable ? walk->chunk_written : ( walk->queued == CH_MESSAGES && unacked == 0 && ch_admitted( walk ) == CH_MESSAGES ) ) )
        {
            kind = CH_UNRELIABLE;
        }
        else
        {
            int total = 0;
            for ( size_t i = 0; i < options.size(); i++ )
            {
                total += options[i].weight;
            }
            int pick = random.below( total );
            for ( size_t i = 0; i < options.size() && kind == CH_NONE; i++ )
            {
                if ( pick < options[i].weight )
                {
                    kind = options[i].kind;
                }
                pick -= options[i].weight;
            }
        }

        if ( kind == CH_SEND )
        {
            const int m = walk->queued;
            const int channel = ch_channel( m ) - 1;
            const int bytes = m == 2 ? (int) ROCKETNET_MESSAGE_FRAGMENT_BYTES + CH_TAIL : CH_SMALL_BYTES;
            std::vector<uint8_t> data( (size_t) bytes, (uint8_t) ( 0x30 + m ) );
            for ( int i = 0; i < ch_parts( m ); i++ )
            {
                data[(size_t) i * ROCKETNET_MESSAGE_FRAGMENT_BYTES] = (uint8_t) ( CH_TOKEN + 4 * m + i );
            }
            const uint32_t serial = rn_endpoint_send( &walk->a.endpoint, data.data(), bytes, ROCKETNET_RELIABILITY_RELIABLE_ORDERED,
                                                      ROCKETNET_PRIORITY_MEDIUM, channel, 0 );
            if ( serial == 0 )
            {
                fprintf( stderr, "tla_trace: channel: the send of message %d was refused\n", m );
                fault = true;
                break;
            }
            walk->queued++;
            walk->sent_trace[ch_channel( m )].push_back( m );
            const uint32_t sequenced = ch_region( &walk->a )->channel_send[channel].sequenced_write;
            walk->reset_ok = walk->reset_ok && sequenced == 0;
            report.act( "Enqueue" );
            records.push_back( tla_step( "send_ordered", "Enqueue", "<<>>", ch_state( walk, &fault ) ) );
            if ( channel == 0 && sequenced != 0 )
            {
                report.planted( (int) records.size() );
            }
        }
        else if ( kind == CH_WRITE || kind == CH_UNRELIABLE )
        {
            if ( kind == CH_UNRELIABLE )
            {
                std::vector<uint8_t> data( CH_UNRELIABLE_BYTES, 0x55 );
                data[0] = CH_UNRELIABLE_TOKEN;
                if ( rn_endpoint_send( &walk->a.endpoint, data.data(), CH_UNRELIABLE_BYTES, ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_PRIORITY_MEDIUM,
                                       CH_UNRELIABLE_CHANNEL, 0 ) == 0 )
                {
                    fprintf( stderr, "tla_trace: channel: the unreliable send was refused\n" );
                    fault = true;
                    break;
                }
                walk->u_queued = true;
                walk->now += drop_unreliable ? 0.0 : 0.25;
            }
            else
            {
                walk->now += dts[random.below( 5 )];
            }

            rn_endpoint_t * a = &walk->a.endpoint;
            rn_endpoint_begin_update( a );
            rn_endpoint_tick( a, walk->now );
            rn_endpoint_begin_flush( a );
            for ( int packet = 0; packet < 4 && !fault; packet++ )
            {
                const std::vector<ch_entry_t> now_entries = ch_smap( walk, &fault );
                const std::vector<int> free_pids = ch_free_pids( walk, now_entries );
                if ( free_pids.empty() || walk->fwd.size() >= CH_CAPACITY || walk->writes >= CH_MAX_WRITES )
                {
                    break;
                }
                if ( rn_endpoint_next_packet_bytes( a, walk->now ) == 0 )
                {
                    break;
                }
                const int packet_kind = a->packet_kind;
                const int admitted_before = ch_admitted( walk );
                const uint64_t dropped_before = ch_counter( &walk->a, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
                const tla_record_t before = ch_state( walk, &fault );

                ch_packet_t written;
                written.pid = free_pids[0];
                written.id = walk->next_id++;
                written.unrel = packet_kind == RN_ENDPOINT_PACKET_UNRELIABLE;
                written.bytes.assign( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, 0 );
                const int bytes = rn_endpoint_write_packet( a, written.bytes.data(), (int) written.bytes.size(), written.id, walk->now );

                // the messages the window numbered: one NumberRecord each, before the packet
                const int admitted_after = ch_admitted( walk );
                for ( int m = admitted_before; m < admitted_after; m++ )
                {
                    tla_record_t step = before;
                    std::vector<int> admitted;
                    for ( int k = 0; k <= m; k++ )
                    {
                        admitted.push_back( k );
                    }
                    ch_set_field( &step, "admitted", ch_set( admitted ) );
                    report.act( "NumberRecord" );
                    records.push_back( tla_step( "write_pass", "NumberRecord", "<<>>", step ) );
                }

                if ( bytes > 0 )
                {
                    rn_endpoint_packet_sent( a, written.id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), walk->now );
                    written.bytes.resize( (size_t) bytes );
                    walk->pid_code[written.pid] = written.id;
                    if ( !written.unrel )
                    {
                        const std::vector<ch_entry_t> after = ch_smap( walk, &fault );
                        if ( after.empty() || after.back().pid != written.pid )
                        {
                            fprintf( stderr, "tla_trace: channel: packet %" PRIu64 " written with no sent map entry\n", written.id );
                            fault = true;
                            break;
                        }
                        written.nums = after.back().nums;
                    }
                    else
                    {
                        walk->u_sent = true;
                    }
                    walk->seen.erase( std::remove( walk->seen.begin(), walk->seen.end(), written.pid ), walk->seen.end() );
                    walk->fwd_sent = ch_next_count( walk->fwd_sent );
                    walk->fwd.push_back( written );
                    walk->writes++;
                    for ( size_t i = 0; i < written.nums.size(); i++ )
                    {
                        walk->chunk_written = walk->chunk_written || written.nums[i] == 2 || written.nums[i] == 3;
                    }
                    report.act( "WritePacket" );
                    records.push_back( tla_step( "write_pass", "WritePacket",
                                                 tla_sequence( { ch_set( written.nums ), tla_int( written.pid ), tla_bool( written.unrel ) } ),
                                                 ch_state( walk, &fault ) ) );
                }
                else if ( ch_counter( &walk->a, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) > dropped_before )
                {
                    walk->u_sent = true;
                    report.act( "DropUnreliableForBudget" );
                    records.push_back( tla_step( "write_pass", "DropUnreliableForBudget", "<<>>", ch_state( walk, &fault ) ) );
                }
                else if ( admitted_after > admitted_before )
                {
                    // numbered, and the budget held the packet: the last NumberRecord is the code's state
                    records.back() = tla_step( "write_pass", "NumberRecord", "<<>>", ch_state( walk, &fault ) );
                }
            }
            // a write pass that numbered or wrote nothing is no model step
        }
        else if ( kind == CH_OPEN )
        {
            const ch_packet_t packet = walk->fwd.front();
            walk->fwd.erase( walk->fwd.begin() );
            walk->fwd_pulse = 1 - walk->fwd_pulse;
            const bool replay = std::find( walk->opened.begin(), walk->opened.end(), packet.id ) != walk->opened.end();
            const bool was_seen = ch_has( walk->seen, packet.pid );
            if ( !ch_has( walk->pending, packet.pid ) )
            {
                walk->pending.push_back( packet.pid );
            }
            if ( !was_seen )
            {
                walk->seen.push_back( packet.pid );
            }

            // the code's call: what it delivered, in its order
            const rn_region_t * b = ch_region( &walk->b );
            int pre_read[3] = { 0, 0, 0 }, pre_mark[3] = { 0, 0, 0 };
            for ( int c = 1; c <= 2; c++ )
            {
                pre_read[c] = (int) b->channel_receive[c - 1].ordered_read;
                pre_mark[c] = (int) b->channel_receive[c - 1].sequenced_watermark;
            }
            const std::vector<int> rx_before = walk->rx_queue;
            const std::vector<int> batch_before = walk->batch;
            const int count_before = walk->delivered_count;

            std::vector<int> delivered;
            if ( !replay )
            {
                walk->opened.push_back( packet.id );
                walk->update_payloads.push_back( packet.bytes );
                const std::vector<uint8_t> & payload = walk->update_payloads.back();
                rn_delivery_t deliveries[16];
                int count = 0;
                if ( !rn_endpoint_on_payload( &walk->b.endpoint, payload.data(), (int) payload.size(), deliveries, 16, &count, walk->now ) )
                {
                    fprintf( stderr, "tla_trace: channel: B's payload ended the connection (error %d)\n", rn_endpoint_error( &walk->b.endpoint ) );
                    fault = true;
                }
                for ( int k = 0; k < count; k++ )
                {
                    const int first = deliveries[k].bytes > 0 ? deliveries[k].data[0] : 0;
                    if ( deliveries[k].code == ROCKETNET_MESSAGE_CODE_UNRELIABLE && first == CH_UNRELIABLE_TOKEN )
                    {
                        walk->u_count++;
                        continue;
                    }
                    // a delivery that is not one of the instance's messages on its channel is message 9, which the model never delivers
                    int m = 9;
                    if ( deliveries[k].code == ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED && first >= CH_TOKEN && first < CH_TOKEN + 4 * CH_MESSAGES &&
                         ( first - CH_TOKEN ) % 4 == 0 && (int) deliveries[k].channel == ch_channel( ( first - CH_TOKEN ) / 4 ) - 1 )
                    {
                        m = ( first - CH_TOKEN ) / 4;
                        walk->delivered_data[m] = deliveries[k].data;
                        for ( int i = 0; i < ch_parts( m ); i++ )
                        {
                            walk->batch.push_back( ch_first( m ) + i );
                        }
                    }
                    delivered.push_back( m );
                    walk->rx_queue.push_back( m );
                    walk->delivered_count++;
                }
            }

            int final_read[3] = { 0, 0, 0 }, final_mark[3] = { 0, 0, 0 };
            for ( int c = 1; c <= 2; c++ )
            {
                final_read[c] = (int) b->channel_receive[c - 1].ordered_read;
                final_mark[c] = (int) b->channel_receive[c - 1].sequenced_watermark;
            }
            const bool reset_before = walk->reset_ok;
            for ( size_t k = 0; k < delivered.size(); k++ )
            {
                if ( delivered[k] < CH_MESSAGES )
                {
                    walk->reset_ok = walk->reset_ok && final_mark[ch_channel( delivered[k] )] == 0;
                }
            }
            const tla_record_t last = ch_state( walk, &fault );

            // one record for the open and one per delivery; each but the last
            // is the code's state after the call with the later deliveries
            // taken back (the file's header)
            bool fired = false;
            for ( size_t s = 0; s <= delivered.size(); s++ )
            {
                tla_record_t step = last;
                if ( s < delivered.size() )
                {
                    std::vector<int> rx = rx_before;
                    std::vector<int> batch = batch_before;
                    int read_index[3] = { 0, pre_read[1], pre_read[2] };
                    int watermark[3] = { 0, pre_mark[1], pre_mark[2] };
                    bool reset = reset_before;
                    for ( size_t k = 0; k < s; k++ )
                    {
                        const int m = delivered[k];
                        rx.push_back( m );
                        if ( m >= CH_MESSAGES )
                        {
                            continue;
                        }
                        reset = reset && final_mark[ch_channel( m )] == 0;
                        for ( int i = 0; i < ch_parts( m ); i++ )
                        {
                            batch.push_back( ch_first( m ) + i );
                        }
                        const int c = ch_channel( m );
                        bool later = false;
                        for ( size_t j = k + 1; j < delivered.size(); j++ )
                        {
                            later = later || ( delivered[j] < CH_MESSAGES && ch_channel( delivered[j] ) == c );
                        }
                        read_index[c] = later ? ch_order( m ) + 1 : final_read[c];
                        watermark[c] = final_mark[c];
                    }
                    ch_set_field( &step, "ordered_read", ch_by_channel( tla_int( read_index[1] ), tla_int( read_index[2] ) ) );
                    ch_set_field( &step, "sequenced_watermark", ch_by_channel( tla_int( watermark[1] ), tla_int( watermark[2] ) ) );
                    ch_set_field( &step, "rxQueue", ch_ints( rx ) );
                    ch_set_field( &step, "batch", ch_set( batch ) );
                    ch_set_field( &step, "deliveredCount", tla_int( count_before + (int) s ) );
                    ch_set_field( &step, "resetOK", tla_bool( reset ) );
                }
                const char * act = s == 0 ? "OpenPacket" : "DeliverMessage";
                report.act( act );
                records.push_back( tla_step( "open_payload", act,
                                             s == 0 ? tla_sequence( { ch_packet_value( packet ) } ) : tla_sequence( { tla_int( delivered[s - 1] ) } ), step ) );
                // the planted effect: an ordered delivery on code channel 0 that leaves its watermark at the probe's 1
                if ( s > 0 && !fired && delivered[s - 1] < CH_MESSAGES && ch_channel( delivered[s - 1] ) == 1 && final_mark[1] != 0 )
                {
                    report.planted( (int) records.size() );
                    fired = true;
                }
            }
        }
        else if ( kind == CH_FIX )
        {
            walk->receiving = false;
            report.act( "FixBatch" );
            records.push_back( tla_step( "fix_batch", "FixBatch", "<<>>", ch_state( walk, &fault ) ) );
        }
        else if ( kind == CH_ACK )
        {
            std::vector<int> ack = walk->pending;
            std::sort( ack.begin(), ack.end() );
            walk->rev.push_back( ack );
            walk->rev_sent = ch_next_count( walk->rev_sent );
            walk->pending.clear();
            report.act( "SendAck" );
            records.push_back( tla_step( "send_ack", "SendAck", "<<>>", ch_state( walk, &fault ) ) );
        }
        else if ( kind == CH_END )
        {
            rn_endpoint_begin_update( &walk->b.endpoint );
            for ( size_t k = 0; k < walk->rx_queue.size(); k++ )
            {
                const int m = walk->rx_queue[k];
                if ( m < CH_MESSAGES )
                {
                    walk->trace[ch_channel( m )].push_back( m );
                }
            }
            walk->rx_queue.clear();
            walk->batch.clear();
            memset( walk->delivered_data, 0, sizeof( walk->delivered_data ) );
            walk->update_payloads.clear();
            walk->receiving = true;
            report.act( "EndUpdate" );
            records.push_back( tla_step( "end_update", "EndUpdate", "<<>>", ch_state( walk, &fault ) ) );
        }
        else if ( kind == CH_ACKED )
        {
            const std::vector<int> q = walk->rev.front();
            walk->rev.erase( walk->rev.begin() );
            walk->rev_pulse = 1 - walk->rev_pulse;
            std::vector<uint64_t> ids;
            for ( size_t i = 0; i < q.size(); i++ )
            {
                ids.push_back( walk->pid_code[q[i]] );
            }
            rn_endpoint_on_acked_ids( &walk->a.endpoint, ids.data(), (uint32_t) ids.size(), walk->now );
            report.act( "ReceiveAck" );
            records.push_back( tla_step( "acked_ids", "ReceiveAck", tla_sequence( { ch_set( q ) } ), ch_state( walk, &fault ) ) );
        }
        else if ( kind == CH_FWD_DROP || kind == CH_FWD_DUPLICATE || kind == CH_FWD_REORDER )
        {
            const size_t i = lose < walk->fwd.size() ? lose : (size_t) random.below( (int) walk->fwd.size() );
            const char * op = "fwd_drop";
            if ( kind == CH_FWD_DROP )
            {
                walk->fwd.erase( walk->fwd.begin() + (long) i );
                walk->fwd_drops++;
            }
            else if ( kind == CH_FWD_DUPLICATE )
            {
                walk->fwd.push_back( walk->fwd[i] );
                op = "fwd_duplicate";
            }
            else
            {
                const size_t j = i + 1 < walk->fwd.size() ? i : i - 1;
                if ( ch_same_packet( walk->fwd[j], walk->fwd[j + 1] ) )
                {
                    continue;
                }
                std::swap( walk->fwd[j], walk->fwd[j + 1] );
                op = "fwd_reorder";
            }
            report.act( "FwdEnvironment" );
            records.push_back( tla_step( op, "FwdEnvironment", "<<>>", ch_state( walk, &fault ) ) );
        }
        else if ( kind == CH_REV_DROP || kind == CH_REV_DUPLICATE || kind == CH_REV_REORDER )
        {
            const size_t i = (size_t) random.below( (int) walk->rev.size() );
            const char * op = "rev_drop";
            if ( kind == CH_REV_DROP )
            {
                walk->rev.erase( walk->rev.begin() + (long) i );
                walk->rev_drops++;
            }
            else if ( kind == CH_REV_DUPLICATE )
            {
                walk->rev.push_back( walk->rev[i] );
                op = "rev_duplicate";
            }
            else
            {
                const size_t j = i + 1 < walk->rev.size() ? i : i - 1;
                if ( walk->rev[j] == walk->rev[j + 1] )
                {
                    continue;
                }
                std::swap( walk->rev[j], walk->rev[j + 1] );
                op = "rev_reorder";
            }
            report.act( "RevEnvironment" );
            records.push_back( tla_step( op, "RevEnvironment", "<<>>", ch_state( walk, &fault ) ) );
        }

        if ( rn_endpoint_error( &walk->a.endpoint ) != ROCKETNET_DISCONNECT_REASON_NONE ||
             rn_endpoint_error( &walk->b.endpoint ) != ROCKETNET_DISCONNECT_REASON_NONE )
        {
            fprintf( stderr, "tla_trace: channel: a connection ended: A %d, B %d\n", rn_endpoint_error( &walk->a.endpoint ),
                     rn_endpoint_error( &walk->b.endpoint ) );
            fault = true;
        }
    }

    // a call of several steps can pass the last record: the trace is steps + 1 records
    if ( records.size() > (size_t) steps + 1 )
    {
        records.resize( (size_t) steps + 1 );
    }

    report.print();

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    W = %d\n"
              "    SendCapacity = %d\n"
              "    P = %d\n"
              "    MaxPid = %d\n"
              "    Capacity = %d\n"
              "    MaxDelay = 0\n"
              "    Duplication = TRUE\n"
              "    AckCapacity = %d\n"
              "    Reordering = TRUE\n"
              "    FwdPattern = \"arbitrary\"\n"
              "    RevPattern = \"arbitrary\"\n"
              "    BurstStart = 1\n"
              "    BurstLen = 1\n"
              "    BrokenOutOfOrder = FALSE\n"
              "    BrokenTwice = FALSE\n"
              "    BrokenDropOnRetransmit = FALSE\n"
              "    BrokenPartialChunk = FALSE\n"
              "    BrokenChunkBlocksSmall = FALSE\n"
              "    BrokenNoSequencedReset = FALSE\n"
              "    ProbeSequenced = TRUE\n"
              "    AppMayLeave = FALSE\n"
              "    BrokenSilentDrop = FALSE\n",
              CH_WINDOW, CH_MESSAGES, CH_PACKETS, CH_MAX_PID, CH_CAPACITY, CH_ACK_CAPACITY );
    const bool written = !fault && tla_write_trace( out, "MCTraceChannel", seed, "TraceRNChannel", "", constants, records );

    rn_arena_destroy( walk->a.arena );
    rn_arena_destroy( walk->b.arena );
    delete walk;
    rocketnet_term();

    return written ? 0 : 1;
}

TLA_TRACE_LAYER( "channel", trace_channel );
