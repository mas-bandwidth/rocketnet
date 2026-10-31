/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The endpoint as a pure machine (proofs/tla/RNEndpoint.tla):
// two endpoints back to back through a lossy, duplicating, reordering array
// that stands in for the transport, with time a loop variable from 1000.0.
// No socket, no clock, no transport.
//
// The array: each written packet is lost, sent once, or sent twice, and each
// copy arrives after its own delay, so packets overtake each other. A transport
// refusal is a packet written and never sent (rn_endpoint_packet_failed). The
// stand-in transport under each endpoint keeps the transport's two promises the
// endpoint relies on (standard/MESSAGES.md m1.4): a packet id opens once (the replay
// window drops the duplicate copy), and an opened packet's id is acked back
// to its sender, over the same kind of lossy path (P2; a lost ack is silence,
// P5).
//
// The properties checked are RNEndpoint's: ReliableExactlyOnce (every reliable
// message the other side sent, ordered or not, delivered exactly once),
// OrderedInOrder (each channel's ordered messages in send order) and
// UnreliableAtMostOnce.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <vector>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_endpoint.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_send_budget.h"

#define check rocketnet_unit_check

#define EP_MESSAGES             300         // each end sends this many
#define EP_ORDERED_CHANNELS     2           // channels 0 and 1 carry RELIABLE_ORDERED
#define EP_RELIABLE_CHANNEL     2           // RELIABLE
#define EP_UNRELIABLE_CHANNEL   3           // UNRELIABLE
#define EP_STEP                 0.01        // seconds per loop step
#define EP_MAX_STEPS            40000

enum
{
    EP_KIND_ORDERED = 0,
    EP_KIND_RELIABLE = 1,
    EP_KIND_UNRELIABLE = 2
};

struct ep_packet_t
{
    double arrive;
    uint64_t id;
    int bytes;
    uint8_t data[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
};

struct ep_ack_t
{
    double arrive;
    uint64_t id;
};

struct ep_side_t
{
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_endpoint_t endpoint;
    uint64_t next_packet_id;

    // the stand-in transport: packets and acks travelling to this side, the
    // ids already opened (the replay window), and this update's batch, which
    // deliveries point into until the next update
    std::vector<ep_packet_t> inbound;
    std::vector<ep_ack_t> acks;
    std::vector<uint64_t> opened;
    std::deque<ep_packet_t> batch;

    // the application
    uint32_t next_index;                    // the next message index to send
    int kind_of[EP_MESSAGES];               // what each sent message was
    int channel_of[EP_MESSAGES];
    int sent[EP_MESSAGES];
    int delivered[EP_MESSAGES];             // of the other side's messages
    uint32_t ordered_next[EP_ORDERED_CHANNELS];  // per channel: how many ordered messages were delivered
    std::vector<uint32_t> ordered_order[EP_ORDERED_CHANNELS];   // the other side's ordered indices per channel, in send order
};

struct ep_stats_t
{
    int lost, duplicated, reordered, failed, replay_dropped, acks_lost;
    uint64_t trace;                         // a hash of everything the endpoints wrote and delivered
};

static uint64_t ep_random( uint64_t * state )
{
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state >> 33;
}

static void ep_hash( uint64_t * trace, const void * data, size_t bytes )
{
    const uint8_t * p = (const uint8_t *) data;
    for ( size_t i = 0; i < bytes; i++ )
    {
        *trace = ( *trace ^ p[i] ) * 1099511628211ULL;
    }
}

static void ep_config( rocketnet_message_config_t * config )
{
    rocketnet_message_default_config( config );
    config->reliable_messages_in_flight = 64;
    config->send_queue_messages = 128;
    config->packets_in_flight = 64;
    config->receive_message_buffer_bytes = 65536;
    config->max_message_bytes = 65536;
    config->send_message_buffer_bytes = 262144;
    config->packets_per_update = 4;
    config->message_resend_delay_min_ms = 20;
    config->message_resend_delay_initial_ms = 100;
    config->message_resend_delay_max_ms = 250;
    config->reliable_message_timeout_ms = 600000;
}

static void ep_side_create( ep_side_t * side, const rocketnet_message_config_t * config )
{
    memset( &side->counters, 0, sizeof( side->counters ) );
    side->arena = rn_arena_create( NULL, config, 1, &side->counters );
    check( side->arena != NULL );

    rn_endpoint_config_t endpoint_config;
    memset( &endpoint_config, 0, sizeof( endpoint_config ) );
    endpoint_config.config = config;
    endpoint_config.send_kbps = 100000;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    rn_endpoint_init( &side->endpoint, rn_arena_region( side->arena, 0 ), &endpoint_config );
    side->next_packet_id = 1;
    side->next_index = 0;
    memset( side->kind_of, 0, sizeof( side->kind_of ) );
    memset( side->channel_of, 0, sizeof( side->channel_of ) );
    memset( side->sent, 0, sizeof( side->sent ) );
    memset( side->delivered, 0, sizeof( side->delivered ) );
    memset( side->ordered_next, 0, sizeof( side->ordered_next ) );
}

static void ep_side_destroy( ep_side_t * side )
{
    rn_arena_destroy( side->arena );
}

static uint64_t ep_counter( const ep_side_t * side, int counter )
{
    return side->counters.values[counter];
}

// the application on `from` sends its next message, if the endpoint takes it

static bool ep_send_next( ep_side_t * from, ep_side_t * to, uint64_t * random )
{
    if ( from->next_index >= EP_MESSAGES )
        return false;

    const uint32_t index = from->next_index;
    const uint64_t pick = ep_random( random ) % 10;
    const int kind = pick < 5 ? EP_KIND_ORDERED : ( pick < 8 ? EP_KIND_RELIABLE : EP_KIND_UNRELIABLE );
    const int channel = kind == EP_KIND_ORDERED ? (int) ( ep_random( random ) % EP_ORDERED_CHANNELS ) : ( kind == EP_KIND_RELIABLE ? EP_RELIABLE_CHANNEL : EP_UNRELIABLE_CHANNEL );
    const int reliability = kind == EP_KIND_ORDERED ? ROCKETNET_RELIABILITY_RELIABLE_ORDERED : ( kind == EP_KIND_RELIABLE ? ROCKETNET_RELIABILITY_RELIABLE : ROCKETNET_RELIABILITY_UNRELIABLE );

    // the message: its index, its kind, then filler up to its size

    const int bytes = 8 + (int) ( ep_random( random ) % 200 );
    uint8_t data[256];
    memset( data, 0, sizeof( data ) );
    memcpy( data, &index, 4 );
    data[4] = (uint8_t) kind;
    for ( int i = 8; i < bytes; i++ )
    {
        data[i] = (uint8_t) ( index + (uint32_t) i );
    }

    const uint32_t serial = rn_endpoint_send( &from->endpoint, data, bytes, reliability, ROCKETNET_PRIORITY_MEDIUM, channel, 0 );
    if ( serial == 0 )
        return false;                                           // the window or the queue is full: try again later
    check( rn_endpoint_error( &from->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    from->kind_of[index] = kind;
    from->channel_of[index] = channel;
    from->sent[index] = 1;
    if ( kind == EP_KIND_ORDERED )
    {
        to->ordered_order[channel].push_back( index );
    }
    from->next_index++;

    return true;
}

static void ep_deliver( ep_side_t * side, ep_side_t * remote, const rn_delivery_t * delivery, ep_stats_t * stats )
{
    check( delivery->bytes >= 8 );

    uint32_t index;
    memcpy( &index, delivery->data, 4 );
    check( index < EP_MESSAGES );
    check( remote->sent[index] );                                 // only what the other side sent

    const int kind = delivery->data[4];
    check( kind == remote->kind_of[index] );
    if ( kind == EP_KIND_ORDERED )
        check( (int) delivery->channel == remote->channel_of[index] );   // codes 0 and 2 carry no channel on the wire (9.1)
    for ( int i = 8; i < delivery->bytes; i++ )
    {
        check( delivery->data[i] == (uint8_t) ( index + (uint32_t) i ) );
    }
    side->delivered[index]++;
    check( side->delivered[index] == 1 );                       // ReliableExactlyOnce's at most once, UnreliableAtMostOnce

    if ( kind == EP_KIND_ORDERED )
    {
        // OrderedInOrder: the next ordered message of its channel, in send order
        const int channel = delivery->channel;
        check( side->ordered_next[channel] < side->ordered_order[channel].size() );
        check( side->ordered_order[channel][side->ordered_next[channel]] == index );
        side->ordered_next[channel]++;
    }

    ep_hash( &stats->trace, &index, sizeof( index ) );
}

// one update of one side: the endpoint's events in, then its actions out

static void ep_update( ep_side_t * side, ep_side_t * remote, double now, uint64_t * random, ep_stats_t * stats )
{
    rn_endpoint_t * endpoint = &side->endpoint;

    rn_endpoint_begin_update( endpoint );
    side->batch.clear();

    // acks the transport brought back (RNEndpoint OnAcked)

    for ( size_t k = 0; k < side->acks.size(); )
    {
        if ( side->acks[k].arrive <= now )
        {
            rn_endpoint_on_acked( endpoint, side->acks[k].id, now );
            side->acks.erase( side->acks.begin() + (long) k );
        }
        else
            k++;
    }

    rn_endpoint_tick( endpoint, now );

    // payloads the transport opened (RNEndpoint OnPayload), in arrival order

    for ( ;; )
    {
        int best = -1;
        for ( size_t k = 0; k < side->inbound.size(); k++ )
            if ( side->inbound[k].arrive <= now && ( best < 0 || side->inbound[k].arrive < side->inbound[(size_t) best].arrive ) )
                best = (int) k;
        if ( best < 0 )
            break;
        side->batch.push_back( side->inbound[(size_t) best] );
        side->inbound.erase( side->inbound.begin() + best );
        const ep_packet_t & packet = side->batch.back();

        bool seen = false;
        for ( size_t k = 0; k < side->opened.size(); k++ )
            if ( side->opened[k] == packet.id )
                seen = true;

        if ( seen )
        {
            stats->replay_dropped++;                            // the transport's replay window
            side->batch.pop_back();
            continue;
        }

        side->opened.push_back( packet.id );

        // P2: the transport acks what it opened, over a lossy path

        if ( ep_random( random ) % 100 < 25 )
            stats->acks_lost++;
        else
        {
            ep_ack_t ack;
            ack.arrive = now + 0.01 + (double) ( ep_random( random ) % 40 ) * 0.001;
            ack.id = packet.id;
            remote->acks.push_back( ack );
        }

        rn_delivery_t out[256];
        int count = -1;
        rn_endpoint_on_payload( endpoint, packet.data, packet.bytes, out, 256, &count, now );
        check( count >= 0 && count < 256 );
        for ( int i = 0; i < count; i++ )
        {
            ep_deliver( side, remote, &out[i], stats );
        }
    }

    rn_endpoint_config_tick( endpoint, 1, now );                  // the stand-in transport is connected throughout
    check( rn_endpoint_error( endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // the application sends a few

    for ( int i = 0; i < 3; i++ )
        if ( !ep_send_next( side, remote, random ) )
            break;

    // RNEndpoint WritePacket, PacketFailed: the write pass into frames the
    // stand-in transport supplies, under ids it assigns

    for ( ;; )
    {
        const int request = rn_endpoint_next_packet_bytes( endpoint, now );
        if ( request == 0 )
            break;
        check( request > 0 && request <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

        ep_packet_t packet;
        packet.id = side->next_packet_id++;

        const int bytes = rn_endpoint_write_packet( endpoint, packet.data, (int) sizeof( packet.data ), packet.id, now );
        if ( bytes == 0 )
            continue;
        check( bytes > 0 && bytes <= request );
        packet.bytes = bytes;
        ep_hash( &stats->trace, packet.data, (size_t) bytes );

        const uint64_t fate = ep_random( random ) % 100;
        if ( fate < 4 )
        {
            stats->failed++;                                    // written, never sent: the id is burned
            rn_endpoint_packet_failed( endpoint, packet.id, now );
            continue;
        }

        rn_endpoint_packet_sent( endpoint, packet.id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), now );

        if ( fate < 24 )
        {
            stats->lost++;
            continue;
        }

        const int copies = fate < 34 ? 2 : 1;
        if ( copies == 2 )
        {
            stats->duplicated++;
        }

        for ( int c = 0; c < copies; c++ )
        {
            packet.arrive = now + 0.005 + (double) ( ep_random( random ) % 60 ) * 0.001;
            if ( !remote->inbound.empty() && packet.arrive < remote->inbound.back().arrive )
            {
                stats->reordered++;
            }
            remote->inbound.push_back( packet );
        }
    }
}

static bool ep_done( const ep_side_t * side, const ep_side_t * remote )
{
    // everything sent, every reliable message of the other side delivered, and
    // nothing in flight

    if ( side->next_index < EP_MESSAGES || remote->next_index < EP_MESSAGES )
        return false;
    for ( int k = 0; k < EP_MESSAGES; k++ )
        if ( remote->kind_of[k] != EP_KIND_UNRELIABLE && side->delivered[k] != 1 )
            return false;

    return side->endpoint.ordered.send.U == side->endpoint.ordered.send.next;
}

// Two endpoints back to back, time a loop variable from 1000.0. Returns the
// steps it took; the sides are left for the caller's checks.

static int ep_back_to_back( ep_side_t * a, ep_side_t * b, uint64_t seed, ep_stats_t * stats )
{
    memset( stats, 0, sizeof( *stats ) );
    stats->trace = 1469598103934665603ULL;

    uint64_t random = seed;

    rn_endpoint_reset( &a->endpoint, 1000.0 );
    rn_endpoint_reset( &b->endpoint, 1000.0 );
    rn_endpoint_announce_config( &a->endpoint, 1000.0 );        // RNEndpoint Reset, the CONFIG half
    rn_endpoint_announce_config( &b->endpoint, 1000.0 );

    int step = 0;
    for ( ; step < EP_MAX_STEPS; step++ )
    {
        const double now = 1000.0 + step * EP_STEP;
        ep_update( a, b, now, &random, stats );
        ep_update( b, a, now, &random, stats );
        if ( ep_done( a, b ) && ep_done( b, a ) )
            break;
    }

    return step;
}

static void ep_check_properties( const ep_side_t * side, const ep_side_t * remote )
{
    int unreliable_sent = 0, unreliable_delivered = 0;

    for ( int k = 0; k < EP_MESSAGES; k++ )
    {
        check( remote->sent[k] );
        if ( remote->kind_of[k] == EP_KIND_UNRELIABLE )
        {
            check( side->delivered[k] <= 1 );                   // UnreliableAtMostOnce
            unreliable_sent++;
            unreliable_delivered += side->delivered[k];
        }
        else
            check( side->delivered[k] == 1 );                   // ReliableExactlyOnce
    }

    for ( int c = 0; c < EP_ORDERED_CHANNELS; c++ )
        check( side->ordered_next[c] == side->ordered_order[c].size() );   // OrderedInOrder, all of them
    check( unreliable_sent > 0 );
    check( unreliable_delivered > 0 );
    check( unreliable_delivered < unreliable_sent );            // the array lost some
}

static void test_endpoint_back_to_back_over_lossy_array()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    check( rocketnet_message_validate_config( &config ) == 1 );

    ep_side_t * a = new ep_side_t();
    ep_side_t * b = new ep_side_t();
    ep_side_create( a, &config );
    ep_side_create( b, &config );

    ep_stats_t stats;
    const int steps = ep_back_to_back( a, b, 20261017, &stats );
    check( steps < EP_MAX_STEPS );                              // no hang: everything reliable arrived

    ep_check_properties( a, b );
    ep_check_properties( b, a );

    // the array did all it is meant to

    check( stats.lost > 0 );
    check( stats.duplicated > 0 );
    check( stats.replay_dropped > 0 );
    check( stats.reordered > 0 );
    check( stats.failed > 0 );
    check( stats.acks_lost > 0 );
    check( ep_counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );
    check( ep_counter( b, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );
    check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    check( rn_endpoint_error( &b->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );

    // CONFIG (10.3): each end announced its CONFIG, and took and matched
    // the other side's through the same lossy array

    check( a->endpoint.ordered.config_announced && b->endpoint.ordered.config_announced );
    check( a->endpoint.ordered.config_received && b->endpoint.ordered.config_received );

    // an end whose CONFIG from the other side never arrives is a RELIABLE_TIMEOUT reliable_timeout
    // after the wait began
    {
        const double start = 1000.0 + steps * EP_STEP;
        const double timeout = (double) config.reliable_message_timeout_ms / 1000.0;
        rn_endpoint_reset( &b->endpoint, start );
        rn_endpoint_config_tick( &b->endpoint, 0, start - 1.0 );   // the wait starts only once the transport is connected
        check( b->endpoint.config_wait_start < 0.0 );
        rn_endpoint_config_tick( &b->endpoint, 1, start );
        check( rn_endpoint_error( &b->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
        rn_endpoint_config_tick( &b->endpoint, 1, start + timeout * 0.5 );
        check( rn_endpoint_error( &b->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
        rn_endpoint_config_tick( &b->endpoint, 0, start + timeout );   // started, the wait times out whatever the transport says
        check( rn_endpoint_error( &b->endpoint ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
        rn_endpoint_reset( &b->endpoint, start + timeout );
        check( rn_endpoint_error( &b->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    // a reset starts a fresh connection on the same region

    rn_endpoint_reset( &a->endpoint, 1000.0 + steps * EP_STEP );
    check( a->endpoint.ordered.send.next == 0 );
    check( a->endpoint.unreliable_count == 0 );
    check( rn_endpoint_next_packet_bytes( &a->endpoint, 1000.0 + steps * EP_STEP ) == 0 );

    ep_side_destroy( a );
    ep_side_destroy( b );
    delete a;
    delete b;
}

// ---------------------------------------------------------------------------
// purity: no allocator call after init, and no clock
// ---------------------------------------------------------------------------

static uint64_t ep_mallocs = 0;
static uint64_t ep_frees = 0;

// The clock intercept. Where the build links the test with
// -Wl,--wrap=rocketnet_platform_time (CMakeLists.txt, GNU-style linkers),
// every call to the library's one clock, from any object file of the
// library, lands here first and is counted, then answered by the real clock.
// Elsewhere the intercept is absent, and the determinism check below stands
// alone.

#if defined( __linux__ ) && ROCKETNET_DEV
static uint64_t ep_clock_reads = 0;
static double ep_fake_clock = 1000.0;
extern "C" double __real_rocketnet_platform_time();
extern "C" double __wrap_rocketnet_platform_time()
{
    ep_clock_reads++;
    ep_fake_clock += ( 1.0 / 60.0 );
    return ep_fake_clock;
}
#endif // #if defined( __linux__ ) && ROCKETNET_DEV

static void * ep_malloc( void * context, size_t bytes )
{
    (void) context;
    ep_mallocs++;
    return malloc( bytes );
}

static void ep_free( void * context, void * p )
{
    (void) context;
    ep_frees++;
    free( p );
}

static void test_endpoint_never_allocates_or_reads_a_clock()
{
    rocketnet_term();
    check( rocketnet_init( NULL, ep_malloc, ep_free ) == ROCKETNET_STATUS_OK );

    rocketnet_message_config_t config;
    ep_config( &config );

#if defined( __linux__ ) && ROCKETNET_DEV
    // the intercept works: a read of the library's clock is counted
    {
        const uint64_t before = ep_clock_reads;
        rocketnet_time();
        check( ep_clock_reads == before + 1 );
    }
#endif // #if defined( __linux__ ) && ROCKETNET_DEV

    uint64_t traces[2];
    int steps[2];

    for ( int run = 0; run < 2; run++ )
    {
        ep_side_t * a = new ep_side_t();
        ep_side_t * b = new ep_side_t();
        ep_side_create( a, &config );
        ep_side_create( b, &config );
        check( ep_mallocs >= 2 );

        // after init: not one allocator call from reset, send, write, ack,
        // payload, tick, receipts, error or deadline

        const uint64_t mallocs = ep_mallocs;
        const uint64_t frees = ep_frees;
#if defined( __linux__ ) && ROCKETNET_DEV
        const uint64_t clock_reads = ep_clock_reads;
#endif // #if defined( __linux__ ) && ROCKETNET_DEV

        ep_stats_t stats;
        steps[run] = ep_back_to_back( a, b, 77, &stats );
        check( steps[run] < EP_MAX_STEPS );
        ep_check_properties( a, b );
        ep_check_properties( b, a );

        rocketnet_message_receipt_t receipts[16];
        rn_endpoint_take_receipts( &a->endpoint, receipts, 16 );

        check( ep_mallocs == mallocs );
        check( ep_frees == frees );
#if defined( __linux__ ) && ROCKETNET_DEV
        check( ep_clock_reads == clock_reads );                 // not one clock read from the endpoint
#endif // #if defined( __linux__ ) && ROCKETNET_DEV
        traces[run] = stats.trace;

        ep_side_destroy( a );
        ep_side_destroy( b );
        delete a;
        delete b;
    }

    // no clock: time is only the argument, so the same inputs give the same
    // packets, deliveries and deadlines, run after run, whatever the wall
    // clock reads between them

    check( steps[0] == steps[1] );
    check( traces[0] == traces[1] );

    rocketnet_term();
    check( rocketnet_init( NULL, NULL, NULL ) == ROCKETNET_STATUS_OK );
}

// R6 (standard/MESSAGES.md m9.4; proofs/tla/RNSendBudget.tla UncappedNeverDropsOrWaits):
// an uncapped endpoint's large sequenced state, larger than one packet, is
// written whole and never touches the bucket: no charge, no settle, no
// refusal, no drop. Simulated time from 1000.0; no transport but this loop.

static void test_endpoint_uncapped_large_sequenced_skips_the_bucket()
{
    rocketnet_message_config_t config;
    ep_config( &config );

    rocketnet_counters_t counters;
    memset( &counters, 0, sizeof( counters ) );

    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config;
    memset( &endpoint_config, 0, sizeof( endpoint_config ) );
    endpoint_config.config = &config;
    endpoint_config.send_kbps = -1;
    endpoint_config.max_payload_bytes = 4 * ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );

    const double now = 1000.0;
    rn_endpoint_reset( &endpoint, now );
    check( endpoint.send_budget_enabled == 0 );

    const int bytes = 3000;
    uint8_t * p = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, bytes, endpoint_config.max_payload_bytes );
    check( p != NULL );

    if ( p )
    {
        memset( p, 0x5a, (size_t) bytes );
        check( rn_endpoint_finish( &endpoint, bytes, 0 ) != 0 );
    }

    check( endpoint.unreliable_count == 0 );
    check( endpoint.ordered.channels.ready_head != RN_DESCRIPTOR_NONE );

    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_tick( &endpoint, now );
    rn_endpoint_config_tick( &endpoint, 1, now );

    static uint8_t frame[4 * ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint64_t packet_id = 1;
    int large_written = 0;
    int bounded_packets_written = 0;

    for ( int i = 0; i < 16; i++ )
    {
        const int request = rn_endpoint_next_packet_bytes( &endpoint, now );
        if ( request == 0 )
            break;
        check( request > 0 && request <= (int) sizeof( frame ) );

        const int written = rn_endpoint_write_packet( &endpoint, frame, (int) sizeof( frame ), packet_id, now );
        if ( written > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
            large_written++;
        else if ( written > 0 )
            bounded_packets_written++;

        // an oversized code-1 send has been promoted and fragmented across
        // ordinary bounded reliable packets; uncapped budget still charges none

        if ( written > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
            rn_endpoint_packet_sent( &endpoint, packet_id, written + 64, now );
        else if ( written > 0 )
            rn_endpoint_packet_sent( &endpoint, packet_id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, written ), now );
        packet_id++;
    }

    check( large_written == 0 );
    check( bounded_packets_written > 1 );                              // reliable fragmentation, not a wide unreliable record
    check( endpoint.unreliable_count == 0 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET] == 0 );
    check( endpoint.budget.refusals == 0 );                            // no settle against a bucket it does not hold
    check( endpoint.budget.bytes_spent == 0 );
    check( endpoint.budget.unreliable_dropped == 0 );

    rn_arena_destroy( arena );
}

// A large unreliable-sequenced record that the bucket cannot pay is dropped with
// no packet written. The drop is counted, but it is not a written packet, so it
// does not use up the per-update packet allowance.

static void test_endpoint_dropped_large_sequenced_is_not_a_written_packet()
{
    rocketnet_message_config_t config;
    ep_config( &config );

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.send_kbps = 64;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    // one 200 byte packet spends part of the bucket, enough that a whole-packet record no longer fits; time does not move, so
    // nothing refills it

    uint8_t * spend = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 200, endpoint_config.max_payload_bytes );
    check( spend != NULL );
    if ( spend )
    {
        memset( spend, 0x50, 200 );
        check( rn_endpoint_finish( &endpoint, 200, 0 ) != 0 );
    }

    rn_endpoint_begin_update( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
    const int spent = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
    check( spent > 0 );
    if ( spent > 0 )
    {
        rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, spent ), 1000.0 );
    }
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) == 0 );

    // the largest record that is not promoted: its RECORDS payload (the 10 header
    // bytes of a code 1 record and the form byte) fills one whole packet, more
    // than the bucket now holds. A small unreliable record follows it.

    const int large_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 11;
    uint8_t * large = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, large_bytes, endpoint_config.max_payload_bytes );
    check( large != NULL );
    if ( large )
    {
        memset( large, 0x5a, (size_t) large_bytes );
        check( rn_endpoint_finish( &endpoint, large_bytes, 0 ) != 0 );
    }

    uint8_t * small = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100, endpoint_config.max_payload_bytes );
    check( small != NULL );
    if ( small )
    {
        memset( small, 0x33, 100 );
        check( rn_endpoint_finish( &endpoint, 100, 0 ) != 0 );
    }

    check( endpoint.unreliable_count == 2 );

    rn_endpoint_begin_update( &endpoint );

    // the head is the large record: over budget, consumed with no packet; the
    // call goes on to the small record

    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET] == 1 );
    check( endpoint.unreliable_count == 1 );
    check( endpoint.packet_kind == RN_ENDPOINT_PACKET_UNRELIABLE );
    check( endpoint.written == 0 );

    const int written = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 2, 1000.0 );
    check( written > 0 );
    if ( written > 0 )
    {
        rn_endpoint_packet_sent( &endpoint, 2, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, written ), 1000.0 );
    }
    check( endpoint.written == 1 );
    check( endpoint.unreliable_count == 0 );

    rn_arena_destroy( arena );
}

// A transport can run out of frame capacity after one packet, without asking
// next_packet_bytes for the zero that normally closes the write pass. New
// updates still refill the bucket, offer reliable work first, and drop an
// unreliable record only when that update's remaining budget cannot pay it.

static void test_endpoint_capacity_cut_starts_new_write_pass()
{
    rocketnet_message_config_t config;
    ep_config( &config );

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.send_kbps = 64;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    for ( int i = 0; i < 2; i++ )
    {
        uint8_t * message = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 1000, endpoint_config.max_payload_bytes );
        check( message != NULL );

        if ( message )
        {
            memset( message, 0x50 + i, 1000 );
            check( rn_endpoint_finish( &endpoint, 1000, 0 ) != 0 );
        }
    }

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_begin_update( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );

    const int first = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
    check( first > 0 );
    if ( first > 0 )
    {
        rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, first ), 1000.0 );
    }
    check( endpoint.unreliable_count == 1 );
    check( endpoint.write_phase == RN_ENDPOINT_WRITE_UNRELIABLE );

    uint8_t * reliable = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE, 1000, endpoint_config.max_payload_bytes );
    check( reliable != NULL );

    if ( reliable )
    {
        memset( reliable, 0x7a, 1000 );
        check( rn_endpoint_finish( &endpoint, 1000, 0 ) != 0 );
    }

    rn_endpoint_begin_update( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.25 ) > 0 );
    check( endpoint.packet_kind == RN_ENDPOINT_PACKET_ORDERED );
    check( endpoint.budget.last_time == 1000.25 );

    const int second = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 2, 1000.25 );
    check( second > 0 );
    if ( second > 0 )
    {
        rn_endpoint_packet_sent( &endpoint, 2, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, second ), 1000.25 );
    }
    check( endpoint.written == 1 );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.25 ) > 0 );
    check( rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 3, 1000.25 ) == 0 );
    check( endpoint.unreliable_count == 0 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET] == 1 );

    rn_arena_destroy( arena );
}

// A reliable block keeps its unsent chunks across the same capacity cut.

static void test_endpoint_reliable_continues_after_capacity_cut()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.packets_per_update = 1;

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.send_kbps = 64;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    uint8_t * message = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE, 3000, endpoint_config.max_payload_bytes );
    check( message != NULL );

    if ( message )
    {
        memset( message, 0x5a, 3000 );
        check( rn_endpoint_finish( &endpoint, 3000, 0 ) != 0 );
    }

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_begin_update( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );

    const int first = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
    check( first > 0 );
    if ( first > 0 )
    {
        rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, first ), 1000.0 );
    }
    check( endpoint.write_phase == RN_ENDPOINT_WRITE_RELIABLE );

    rn_endpoint_begin_update( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.25 ) > 0 );
    check( endpoint.packet_kind == RN_ENDPOINT_PACKET_ORDERED );

    const int second = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 2, 1000.25 );
    check( second > 0 );
    if ( second > 0 )
    {
        rn_endpoint_packet_sent( &endpoint, 2, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, second ), 1000.25 );
    }
    check( endpoint.budget.last_time == 1000.25 );
    check( endpoint.written == 1 );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.25 ) == 0 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET] == 0 );
    rn_arena_destroy( arena );
}

// A window stalled on unacked numbers writes nothing new until an ack moves
// it, so the endpoint asks for no frame: a server that started one per
// stalled connection and aborted it would spend the transport's per-update
// send capacity on packets that never exist.

static void test_endpoint_stalled_window_requests_no_frame()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.packets_per_update = 8;

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    for ( int i = 0; i <= config.reliable_messages_in_flight; i++ )
    {
        uint8_t * message = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE, 10, endpoint_config.max_payload_bytes );
        check( message != NULL );
        if ( message )
        {
            memset( message, i, 10 );
            check( rn_endpoint_finish( &endpoint, 10, 0 ) != 0 );
        }
    }

    // the first update numbers W messages and stalls on the next

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint64_t packet_id = 1;
    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    while ( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 )
    {
        const int bytes = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), packet_id, 1000.0 );
        if ( bytes > 0 )
            rn_endpoint_packet_sent( &endpoint, packet_id++, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), 1000.0 );
    }

    check( endpoint.ordered.send.stalled );
    check( endpoint.ordered.send.next - endpoint.ordered.send.U == (uint64_t) config.reliable_messages_in_flight );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );

    // the next update, before any resend is due: nothing to write, no frame

    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.001 ) == 0 );

    rn_arena_destroy( arena );
}

// A pass that ended stays ended for the rest of the flush: a reliable packet
// the budget cannot pay is not planned again, frame after frame, while the
// send loop serves other connections. The next flush plans it again.

static void test_endpoint_ended_pass_stays_ended_until_the_next_flush()
{
    rocketnet_message_config_t config;
    ep_config( &config );

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.send_kbps = 64;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    // one 200 byte packet spends enough of the bucket that a 1000 byte
    // reliable record no longer fits; time does not move

    uint8_t * spend = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 200, endpoint_config.max_payload_bytes );
    check( spend != NULL );
    if ( spend )
    {
        memset( spend, 0x50, 200 );
        check( rn_endpoint_finish( &endpoint, 200, 0 ) != 0 );
    }

    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
    const int spent = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
    check( spent > 0 );
    if ( spent > 0 )
        rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, spent ), 1000.0 );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) == 0 );

    uint8_t * reliable = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE, 1000, endpoint_config.max_payload_bytes );
    check( reliable != NULL );
    if ( reliable )
    {
        memset( reliable, 0x7a, 1000 );
        check( rn_endpoint_finish( &endpoint, 1000, 0 ) != 0 );
    }

    // a second flush in the same update plans the reliable packet once; the
    // budget refuses it and the pass ends

    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
    check( endpoint.packet_kind == RN_ENDPOINT_PACKET_ORDERED );
    check( rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 2, 1000.0 ) == 0 );
    check( endpoint.budget.reliable_waiting );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) == 0 );

    // asked again in the same flush, the ended pass plans nothing

    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) == 0 );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) == 0 );

    // the next flush, after the bucket refills, plans and writes it

    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1001.0 ) > 0 );
    check( endpoint.packet_kind == RN_ENDPOINT_PACKET_ORDERED );
    check( rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 3, 1001.0 ) > 0 );

    rn_arena_destroy( arena );
}

REGISTER_UNIT_TEST( test_endpoint_back_to_back_over_lossy_array );
REGISTER_UNIT_TEST( test_endpoint_uncapped_large_sequenced_skips_the_bucket );
REGISTER_UNIT_TEST( test_endpoint_never_allocates_or_reads_a_clock );
REGISTER_UNIT_TEST( test_endpoint_capacity_cut_starts_new_write_pass );
REGISTER_UNIT_TEST( test_endpoint_dropped_large_sequenced_is_not_a_written_packet );
REGISTER_UNIT_TEST( test_endpoint_reliable_continues_after_capacity_cut );
REGISTER_UNIT_TEST( test_endpoint_stalled_window_requests_no_frame );
REGISTER_UNIT_TEST( test_endpoint_ended_pass_stays_ended_until_the_next_flush );
