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

#include <atomic>
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

// The reliable timeout ep_config gives: 0, the shipped default (no reliable
// timeout; probe pacing and the round-trip filter on);
// test_endpoint_cases_opted_in reruns the cases at 600000, the opted-in rule
// (every due number resent, as in v1.0).
static int g_ep_reliable_timeout_ms = 0;

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
    config->reliable_message_timeout_ms = g_ep_reliable_timeout_ms;
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
    {
        return false;
    }

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
    {
        return false;                                           // the window or the queue is full: try again later
    }
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
    {
        check( (int) delivery->channel == remote->channel_of[index] );   // codes 0 and 2 carry no channel on the wire (9.1)
    }
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
        {
            k++;
        }
    }

    rn_endpoint_tick( endpoint, now );

    // payloads the transport opened (RNEndpoint OnPayload), in arrival order

    for ( ;; )
    {
        int best = -1;
        for ( size_t k = 0; k < side->inbound.size(); k++ )
        {
            if ( side->inbound[k].arrive <= now && ( best < 0 || side->inbound[k].arrive < side->inbound[(size_t) best].arrive ) )
            {
                best = (int) k;
            }
        }
        if ( best < 0 )
        {
            break;
        }
        side->batch.push_back( side->inbound[(size_t) best] );
        side->inbound.erase( side->inbound.begin() + best );
        const ep_packet_t & packet = side->batch.back();

        bool seen = false;
        for ( size_t k = 0; k < side->opened.size(); k++ )
        {
            if ( side->opened[k] == packet.id )
            {
                seen = true;
            }
        }

        if ( seen )
        {
            stats->replay_dropped++;                            // the transport's replay window
            side->batch.pop_back();
            continue;
        }

        side->opened.push_back( packet.id );

        // P2: the transport acks what it opened, over a lossy path

        if ( ep_random( random ) % 100 < 25 )
        {
            stats->acks_lost++;
        }
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
    {
        if ( !ep_send_next( side, remote, random ) )
        {
            break;
        }
    }

    // RNEndpoint WritePacket, PacketFailed: the write pass into frames the
    // stand-in transport supplies, under ids it assigns

    for ( ;; )
    {
        const int request = rn_endpoint_next_packet_bytes( endpoint, now );
        if ( request == 0 )
        {
            break;
        }
        check( request > 0 && request <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

        ep_packet_t packet;
        packet.id = side->next_packet_id++;

        const int bytes = rn_endpoint_write_packet( endpoint, packet.data, (int) sizeof( packet.data ), packet.id, now );
        if ( bytes == 0 )
        {
            continue;
        }
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
    {
        return false;
    }
    for ( int k = 0; k < EP_MESSAGES; k++ )
    {
        if ( remote->kind_of[k] != EP_KIND_UNRELIABLE && side->delivered[k] != 1 )
        {
            return false;
        }
    }

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
        {
            break;
        }
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
        {
            check( side->delivered[k] == 1 );                   // ReliableExactlyOnce
        }
    }

    for ( int c = 0; c < EP_ORDERED_CHANNELS; c++ )
    {
        check( side->ordered_next[c] == side->ordered_order[c].size() );   // OrderedInOrder, all of them
    }
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
    // after the wait began; at the default (no reliable timeout) the wait never ends it
    if ( config.reliable_message_timeout_ms == 0 )
    {
        const double start = 1000.0 + steps * EP_STEP;
        rn_endpoint_reset( &b->endpoint, start );
        rn_endpoint_config_tick( &b->endpoint, 1, start );
        check( b->endpoint.config_wait_start == start );
        rn_endpoint_config_tick( &b->endpoint, 0, start + 600.0 );
        check( rn_endpoint_error( &b->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
        rn_endpoint_reset( &b->endpoint, start + 600.0 );
    }
    else
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
// library, lands here first and receives the next synthetic tick. Native
// shutdown helpers and transport threads may read it concurrently, so one
// atomic counter both counts reads and assigns distinct ticks.
// Elsewhere the intercept is absent, and the determinism check below stands
// alone.

#if defined( __linux__ ) && ROCKETNET_DEV
static std::atomic<uint64_t> ep_clock_reads( 0 );
extern "C" double __wrap_rocketnet_platform_time()
{
    const uint64_t tick = ep_clock_reads.fetch_add( 1, std::memory_order_relaxed ) + 1;
    return 1000.0 + (double) tick * ( 1.0 / 60.0 );
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
        const uint64_t before = ep_clock_reads.load( std::memory_order_relaxed );
        rocketnet_time();
        check( ep_clock_reads.load( std::memory_order_relaxed ) == before + 1 );
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
        const uint64_t clock_reads = ep_clock_reads.load( std::memory_order_relaxed );
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
        check( ep_clock_reads.load( std::memory_order_relaxed ) == clock_reads );                 // not one clock read from the endpoint
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
        {
            break;
        }
        check( request > 0 && request <= (int) sizeof( frame ) );

        const int written = rn_endpoint_write_packet( &endpoint, frame, (int) sizeof( frame ), packet_id, now );
        if ( written > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        {
            large_written++;
        }
        else if ( written > 0 )
        {
            bounded_packets_written++;
        }

        // an oversized code-1 send has been promoted and fragmented across
        // ordinary bounded reliable packets; uncapped budget still charges none

        if ( written > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        {
            rn_endpoint_packet_sent( &endpoint, packet_id, written + 64, now );
        }
        else if ( written > 0 )
        {
            rn_endpoint_packet_sent( &endpoint, packet_id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, written ), now );
        }
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
        {
            rn_endpoint_packet_sent( &endpoint, packet_id++, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), 1000.0 );
        }
    }

    check( endpoint.ordered.send.stalled );
    check( endpoint.ordered.send.next - endpoint.ordered.send.U == (uint64_t) config.reliable_messages_in_flight );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );

    // the next update, before any resend is due: nothing to write, no frame

    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.001 ) == 0 );

    // acknowledging the blocking numbers moves U' and clears the stall

    check( endpoint.ordered.send.stalled == 1 );
    for ( uint64_t id = 1; id < packet_id; id++ )
    {
        rn_endpoint_on_acked( &endpoint, id, 1000.002 );
    }
    check( endpoint.ordered.send.U_prime == (uint64_t) config.reliable_messages_in_flight );
    check( endpoint.ordered.send.stalled == 0 );

    rn_arena_destroy( arena );
}

// An ack that moves U' clears the stall, but by less room than the next
// message needs (a block of three numbers behind a full window with one
// number acknowledged), so the window still cannot number it. The endpoint
// asks for no frame: a remote endpoint that acknowledges one number per
// update must not make the server start and abort a frame every update. The
// stall is still marked and counted, as the walk's first write would.

static void test_endpoint_partial_ack_stall_requests_no_frame()
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

    // W singles of 1200 bytes, one number and one packet each, then a block
    // of three numbers

    const int W = config.reliable_messages_in_flight;
    for ( int i = 0; i <= W; i++ )
    {
        const int bytes = i < W ? 1200 : 3000;
        uint8_t * message = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE, bytes, endpoint_config.max_payload_bytes );
        check( message != NULL );
        if ( message )
        {
            memset( message, i, (size_t) bytes );
            check( rn_endpoint_finish( &endpoint, bytes, 0 ) != 0 );
        }
    }

    // updates at one time, before any resend is due, send the W singles and
    // stall on the block

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint64_t packet_id = 1;
    for ( int update = 0; update < W && !endpoint.ordered.send.stalled; update++ )
    {
        rn_endpoint_begin_update( &endpoint );
        rn_endpoint_begin_flush( &endpoint );
        while ( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 )
        {
            const int bytes = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), packet_id, 1000.0 );
            if ( bytes > 0 )
            {
                rn_endpoint_packet_sent( &endpoint, packet_id++, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), 1000.0 );
            }
        }
    }

    check( packet_id == (uint64_t) W + 1 );
    check( endpoint.ordered.send.stalled );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 1 );

    // one number acknowledged: the stall clears with one free number

    rn_endpoint_on_acked( &endpoint, 1, 1000.001 );
    check( endpoint.ordered.send.U_prime == 1 );
    check( endpoint.ordered.send.stalled == 0 );

    // the next update, before any resend is due: the block still does not
    // fit, so no frame is asked for, and the stall is marked again

    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.002 ) == 0 );
    check( endpoint.ordered.send.stalled == 1 );
    check( counters.values[ROCKETNET_COUNTER_MESSAGE_WINDOW_STALL] == 2 );

    rn_arena_destroy( arena );
}

// The same rule under probe pacing (standard/MESSAGES.md m9.3): with the
// reliable timeout off and nothing acknowledged for a second, only U is
// resent, once per resend delay. The endpoint asks for a frame exactly when
// U is due, and every frame it asks for is written: a connection held with
// several numbers on the wire (here small records and a block's chunks)
// costs the transport's per-update send capacity only for U's probe.

static void test_endpoint_probe_hold_requests_frame_only_for_u()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.packets_per_update = 8;
    config.reliable_message_timeout_ms = 0;

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    const int sizes[] = { 10, 10, 3000, 10 };
    for ( int i = 0; i < 4; i++ )
    {
        uint8_t * message = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE, sizes[i], endpoint_config.max_payload_bytes );
        check( message != NULL );
        if ( message )
        {
            memset( message, i, (size_t) sizes[i] );
            check( rn_endpoint_finish( &endpoint, sizes[i], 0 ) != 0 );
        }
    }

    // 60 updates a second for 5 s, nothing ever acknowledged

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint64_t packet_id = 1;
    int held_updates = 0;
    int u_due_updates = 0;
    int aborted = 0;
    const rn_window_ack_t * ack = &endpoint.ordered.ack;
    const rn_region_t * region = endpoint.region;
    for ( int update = 0; update < 300; update++ )
    {
        const double now = 1000.0 + update / 60.0;
        rn_endpoint_begin_update( &endpoint );
        rn_endpoint_begin_flush( &endpoint );

        const bool held = rn_window_ack_probing( ack, now );
        const rn_send_number_t * u = &region->send_numbers[ack->send->U & ( region->window - 1 )];
        const bool u_due = now >= u->last_send + rn_window_ack_resend_delay( ack );

        int frames = 0;
        while ( rn_endpoint_next_packet_bytes( &endpoint, now ) > 0 )
        {
            frames++;
            const int bytes = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), packet_id, now );
            if ( bytes > 0 )
            {
                rn_endpoint_packet_sent( &endpoint, packet_id++, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), now );
            }
            else
            {
                aborted++;
            }
        }

        if ( held )
        {
            held_updates++;
            u_due_updates += u_due ? 1 : 0;
            check( frames == ( u_due ? 1 : 0 ) );
        }
    }

    check( endpoint.ordered.send.next - endpoint.ordered.send.U >= 4 );
    check( held_updates > 200 );
    check( u_due_updates > 0 && u_due_updates < held_updates / 2 );
    check( aborted == 0 );

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
    {
        rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, spent ), 1000.0 );
    }
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

// The unreliable queue names each entry by its descriptor and the ticket it
// was pushed with. Tickets wrap, so after the counter comes around a
// tombstone's ticket can be issued to its descriptor again: without the
// renumbering at the first push after a wrap the tombstone reads as live
// and the record is written twice.

static int ep_count_records( const uint8_t * payload, int bytes, const rocketnet_message_config_t * config )
{
    rocketnet_message_limits_t limits = {};
    limits.num_channels = config->num_channels;
    limits.enabled_codes = 0x1F;
    limits.reliable_messages_in_flight = config->reliable_messages_in_flight;
    limits.max_message_bytes = config->max_message_bytes;
    limits.receive_message_buffer_bytes = config->receive_message_buffer_bytes;

    rocketnet_message_reader_t reader;
    if ( rocketnet_message_reader_begin( &reader, payload, bytes, &limits ) != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        return -1;
    }

    int records = 0;
    rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        records++;
    }

    return records;
}

static uint32_t ep_send_unreliable( rn_endpoint_t * endpoint, int channel, int bytes, uint8_t fill )
{
    uint8_t * message = rn_endpoint_start( endpoint, channel, ROCKETNET_RELIABILITY_UNRELIABLE, bytes, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    check( message != NULL );
    if ( !message )
    {
        return RN_DESCRIPTOR_NONE;
    }

    memset( message, fill, (size_t) bytes );
    check( rn_endpoint_finish( endpoint, bytes, 0 ) != 0 );

    rn_region_t * region = endpoint->region;
    const uint32_t tail = ( endpoint->unreliable_ring_head + endpoint->unreliable_ring_used - 1 ) % region->unreliable_ring_entries;
    return region->unreliable_ring[tail].descriptor;
}

static void test_endpoint_unreliable_tombstone_never_revives_after_ticket_wrap()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.packets_per_update = 1;

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_endpoint_init( &endpoint, region, &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    // three records just short of the wrap; channels take turns in a packet,
    // so the first packet carries the first record (channel 0) and the third
    // (channel 1), and the second stays queued at the head: the third's entry
    // is a tombstone behind a live head

    region->schedule_ticket_next = UINT32_MAX - 2;

    ep_send_unreliable( &endpoint, 0, 800, 0x11 );
    const uint32_t b = ep_send_unreliable( &endpoint, 0, 800, 0x22 );
    const uint32_t c = ep_send_unreliable( &endpoint, 1, 300, 0x33 );

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
    const int first = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
    check( first > 0 );
    check( ep_count_records( frame, first, &config ) == 2 );
    rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, first ), 1000.0 );
    check( endpoint.unreliable_count == 1 );
    check( endpoint.unreliable_head == b );

    uint32_t tombstone_ticket = 0;
    for ( uint32_t i = 0; i < endpoint.unreliable_ring_used; i++ )
    {
        const rn_unreliable_entry_t * entry = &region->unreliable_ring[( endpoint.unreliable_ring_head + i ) % region->unreliable_ring_entries];
        if ( entry->descriptor == c )
        {
            tombstone_ticket = entry->ticket;
        }
    }
    check( tombstone_ticket != 0 );

    // reliable sends draw tickets until the counter comes around to the
    // tombstone's ticket: forward to the end of the lap, then one by one

    if ( region->schedule_ticket_next < UINT32_MAX - 1 )
    {
        region->schedule_ticket_next = UINT32_MAX - 1;
    }
    for ( int i = 0; i < 16 && region->schedule_ticket_next != tombstone_ticket; i++ )
    {
        rn_region_schedule_ticket( region );
    }
    check( region->schedule_ticket_next == tombstone_ticket );

    // the next send takes the freed descriptor back: it is the tombstone's

    const uint32_t d = ep_send_unreliable( &endpoint, 1, 100, 0x44 );
    check( d == c );
    check( endpoint.unreliable_count == 2 );

    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.1 ) > 0 );
    const int second = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 2, 1000.1 );
    check( second > 0 );
    check( ep_count_records( frame, second, &config ) == 2 );

    rn_arena_destroy( arena );
}

// The unreliable records of one written packet, by their first byte (the
// fill each case writes), in wire order; -1 for an unparsable payload.
static int ep_record_fills( const uint8_t * payload, int bytes, const rocketnet_message_config_t * config, uint8_t * fills, int capacity )
{
    rocketnet_message_limits_t limits = {};
    limits.num_channels = config->num_channels;
    limits.enabled_codes = 0x1F;
    limits.reliable_messages_in_flight = config->reliable_messages_in_flight;
    limits.max_message_bytes = config->max_message_bytes;
    limits.receive_message_buffer_bytes = config->receive_message_buffer_bytes;

    rocketnet_message_reader_t reader;
    if ( rocketnet_message_reader_begin( &reader, payload, bytes, &limits ) != ROCKETNET_MESSAGE_VIOLATION_NONE )
    {
        return -1;
    }

    int count = 0;
    rocketnet_message_record_t record;
    while ( rocketnet_message_reader_next( &reader, &record ) )
    {
        if ( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE && record.bytes > 0 && count < capacity )
        {
            fills[count++] = record.data[0];
        }
    }

    return count;
}

// The ring after the renumber at the first push past a ticket wrap
// (rn_unreliable_renumber; proofs/tla/RNUnreliableRingLayout.tla Push): the
// tombstones are gone, the live entries are kept in queue order from the
// head, each with a new ticket of the new lap that its descriptor carries,
// and the pushed descriptor follows with the next ticket. Two shapes: a
// tombstone whose descriptor was reused by a live message before the wrap
// (stale, not naming the pushed descriptor), and the tombstone of the
// descriptor the push takes (naming it).

static void ep_ring_expect( const rn_endpoint_t * endpoint, const uint32_t * descriptors, uint32_t count )
{
    const rn_region_t * region = endpoint->region;
    check( endpoint->unreliable_ring_used == count );
    check( region->schedule_ticket_wrapped == 0 );
    uint32_t previous = 0;
    for ( uint32_t i = 0; i < count && i < endpoint->unreliable_ring_used; i++ )
    {
        const rn_unreliable_entry_t * entry = &region->unreliable_ring[( endpoint->unreliable_ring_head + i ) % region->unreliable_ring_entries];
        check( entry->descriptor == descriptors[i] );
        check( entry->ticket == region->send_descriptors[entry->descriptor].enqueue_ticket );
        check( entry->ticket >= 1 && entry->ticket < 16 );                     // the new lap
        check( i == 0 || entry->ticket == previous + 1 );                       // queue order, one ticket each
        previous = entry->ticket;
    }
    check( endpoint->unreliable_head == descriptors[0] );
}

static void test_endpoint_unreliable_renumber_at_ticket_wrap_keeps_queue_order()
{
    for ( int shape = 0; shape < 2; shape++ )
    {
        rocketnet_message_config_t config;
        ep_config( &config );
        config.packets_per_update = 1;

        rocketnet_counters_t counters = {};
        rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
        check( arena != NULL );

        rn_endpoint_config_t endpoint_config = {};
        endpoint_config.config = &config;
        endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

        rn_endpoint_t endpoint;
        rn_region_t * region = rn_arena_region( arena, 0 );
        rn_endpoint_init( &endpoint, region, &endpoint_config );
        rn_endpoint_reset( &endpoint, 1000.0 );

        // a packet carries a (channel 0) and c (channel 1); b stays queued at
        // the head, c's entry is a tombstone behind it

        region->schedule_ticket_next = UINT32_MAX - 8;
        ep_send_unreliable( &endpoint, 0, 800, 0x11 );
        const uint32_t b = ep_send_unreliable( &endpoint, 0, 800, 0x22 );
        const uint32_t c = ep_send_unreliable( &endpoint, 1, 300, 0x33 );

        uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        rn_endpoint_begin_update( &endpoint );
        rn_endpoint_begin_flush( &endpoint );
        check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
        const int first = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
        check( first > 0 );
        rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, first ), 1000.0 );
        check( endpoint.unreliable_count == 1 && endpoint.unreliable_head == b );
        check( endpoint.unreliable_ring_used == 2 );

        uint32_t expect[4];
        uint32_t d = RN_DESCRIPTOR_NONE;
        if ( shape == 0 )
        {
            // f takes c's descriptor before the wrap: c's entry is now stale
            // for a live descriptor; the wrap; then d takes the other free one

            const uint32_t f = ep_send_unreliable( &endpoint, 1, 100, 0x66 );
            check( f == c );
            check( endpoint.unreliable_ring_used == 3 );
            while ( !region->schedule_ticket_wrapped )
            {
                rn_region_schedule_ticket( region );
            }
            d = ep_send_unreliable( &endpoint, 0, 150, 0x77 );
            check( d != c && d != b );
            expect[0] = b;
            expect[1] = f;
            expect[2] = d;
            ep_ring_expect( &endpoint, expect, 3 );
        }
        else
        {
            // the wrap; then d takes c's descriptor back: c's tombstone names it

            while ( !region->schedule_ticket_wrapped )
            {
                rn_region_schedule_ticket( region );
            }
            d = ep_send_unreliable( &endpoint, 1, 150, 0x77 );
            check( d == c );
            expect[0] = b;
            expect[1] = d;
            ep_ring_expect( &endpoint, expect, 2 );
        }

        // the queued messages go out once each

        int seen[256] = {};
        for ( int p = 0; p < 4 && endpoint.unreliable_count > 0; p++ )
        {
            rn_endpoint_begin_update( &endpoint );
            rn_endpoint_begin_flush( &endpoint );
            const double now = 1000.1 + 0.1 * p;
            check( rn_endpoint_next_packet_bytes( &endpoint, now ) > 0 );
            const int bytes = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), (uint64_t) ( 2 + p ), now );
            check( bytes > 0 );
            uint8_t fills[8];
            const int count = ep_record_fills( frame, bytes, &config, fills, 8 );
            check( count >= 0 );
            for ( int k = 0; k < count; k++ )
            {
                seen[fills[k]]++;
            }
            rn_endpoint_packet_sent( &endpoint, (uint64_t) ( 2 + p ), rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), now );
        }
        check( endpoint.unreliable_count == 0 );
        check( seen[0x22] == 1 && seen[0x77] == 1 && seen[0x66] == ( shape == 0 ? 1 : 0 ) );
        check( seen[0x11] == 0 && seen[0x33] == 0 );

        rn_arena_destroy( arena );
    }
}

// The ring filled to its last entry (rn_unreliable_push's full-ring pass):
// the ring holds two entries per send descriptor, so it fills only with
// tombstones. Live messages queued behind a live head, the rest of the ring
// stale entries (descriptors reused since, as a sent message leaves them),
// then one push more: the pass closes the live entries up from the head in
// order before the push, and every live message is written once.
static void test_endpoint_unreliable_ring_full_push_keeps_every_live_message()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.packets_per_update = 1;

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_endpoint_init( &endpoint, region, &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    const uint32_t entries = region->unreliable_ring_entries;
    check( entries >= 8 );

    // four live messages, then stale entries naming their descriptors with
    // an old ticket fill the ring to its last entry

    uint32_t live[5];
    for ( int i = 0; i < 4; i++ )
    {
        live[i] = ep_send_unreliable( &endpoint, 0, 100, (uint8_t) ( 0xA0 + i ) );
    }
    check( endpoint.unreliable_ring_used == 4 );
    const uint32_t head = endpoint.unreliable_ring_head;
    for ( uint32_t i = 4; i < entries; i++ )
    {
        rn_unreliable_entry_t * entry = &region->unreliable_ring[( head + i ) % entries];
        entry->descriptor = live[i % 4];
        entry->ticket = region->send_descriptors[live[i % 4]].enqueue_ticket - 1;     // an older lap's: stale
    }
    endpoint.unreliable_ring_used = entries;

    // the push at a full ring

    live[4] = ep_send_unreliable( &endpoint, 0, 100, 0xA4 );
    check( endpoint.unreliable_ring_used == 5 );
    check( endpoint.unreliable_ring_head == head );
    for ( uint32_t i = 0; i < 5; i++ )
    {
        const rn_unreliable_entry_t * entry = &region->unreliable_ring[( head + i ) % entries];
        check( entry->descriptor == live[i] );
        check( entry->ticket == region->send_descriptors[live[i]].enqueue_ticket );
    }

    // all five go out once each, in order

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
    const int bytes = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
    check( bytes > 0 );
    uint8_t fills[8];
    check( ep_record_fills( frame, bytes, &config, fills, 8 ) == 5 );
    for ( int i = 0; i < 5; i++ )
    {
        check( fills[i] == 0xA0 + i );
    }
    rn_endpoint_packet_sent( &endpoint, 1, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), 1000.0 );
    check( endpoint.unreliable_count == 0 );

    rn_arena_destroy( arena );
}

// The renumber at a ticket wrap over a full ring: it reads exactly the used
// entries, so the live head is renumbered once, not read again past the end.
static void test_endpoint_unreliable_renumber_at_ticket_wrap_over_a_full_ring()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.packets_per_update = 1;

    rocketnet_counters_t counters = {};
    rn_arena_t * arena = rn_arena_create( NULL, &config, 1, &counters );
    check( arena != NULL );

    rn_endpoint_config_t endpoint_config = {};
    endpoint_config.config = &config;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;

    rn_endpoint_t endpoint;
    rn_region_t * region = rn_arena_region( arena, 0 );
    rn_endpoint_init( &endpoint, region, &endpoint_config );
    rn_endpoint_reset( &endpoint, 1000.0 );

    const uint32_t entries = region->unreliable_ring_entries;
    check( entries >= 8 );

    region->schedule_ticket_next = UINT32_MAX - 8;
    uint32_t live[5];
    for ( int i = 0; i < 4; i++ )
    {
        live[i] = ep_send_unreliable( &endpoint, 0, 100, (uint8_t) ( 0xA0 + i ) );
    }
    check( endpoint.unreliable_ring_used == 4 );
    const uint32_t head = endpoint.unreliable_ring_head;
    for ( uint32_t i = 4; i < entries; i++ )
    {
        rn_unreliable_entry_t * entry = &region->unreliable_ring[( head + i ) % entries];
        entry->descriptor = live[i % 4];
        entry->ticket = region->send_descriptors[live[i % 4]].enqueue_ticket - 1;     // an older lap's: stale
    }
    endpoint.unreliable_ring_used = entries;

    while ( !region->schedule_ticket_wrapped )
    {
        rn_region_schedule_ticket( region );
    }

    live[4] = ep_send_unreliable( &endpoint, 0, 100, 0xA4 );
    ep_ring_expect( &endpoint, live, 5 );

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_begin_update( &endpoint );
    rn_endpoint_begin_flush( &endpoint );
    check( rn_endpoint_next_packet_bytes( &endpoint, 1000.0 ) > 0 );
    const int bytes = rn_endpoint_write_packet( &endpoint, frame, sizeof( frame ), 1, 1000.0 );
    check( bytes > 0 );
    uint8_t fills[8];
    check( ep_record_fills( frame, bytes, &config, fills, 8 ) == 5 );
    for ( int i = 0; i < 5; i++ )
    {
        check( fills[i] == 0xA0 + i );
    }

    rn_arena_destroy( arena );
}

// ---------------------------------------------------------------------------
// the reliable timeout off by default (standard/MESSAGES.md m9.3, 10.3): a
// heard connection whose reliable stream never progresses never ends, and
// the wait for the remote CONFIG has no clock. Opted in, both end with
// RELIABLE_TIMED_OUT at the configured value, as in v1.0.
// ---------------------------------------------------------------------------

// One update of a lone endpoint whose every packet is lost. The transport
// hears the remote end: it acks keep_alive_id, a packet that carried no
// reliable number (the sent map never held it). Returns the packets written.
static int ep_lone_update( ep_side_t * side, double now, int transport_connected, uint64_t keep_alive_id )
{
    rn_endpoint_t * endpoint = &side->endpoint;

    rn_endpoint_begin_update( endpoint );
    rn_endpoint_on_acked( endpoint, keep_alive_id, now );
    rn_endpoint_tick( endpoint, now );
    rn_endpoint_config_tick( endpoint, transport_connected, now );

    int written = 0;
    for ( ;; )
    {
        const int request = rn_endpoint_next_packet_bytes( endpoint, now );
        if ( request == 0 )
        {
            break;
        }
        check( request > 0 && request <= ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

        uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        const uint64_t id = side->next_packet_id++;
        const int bytes = rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), id, now );
        if ( bytes == 0 )
        {
            continue;
        }
        check( bytes > 0 && bytes <= request );
        rn_endpoint_packet_sent( endpoint, id, rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes ), now );
        written++;
    }

    return written;
}

#define EP_KEEP_ALIVE_IDS ( (uint64_t) 1 << 40 )    // far above any id the endpoint writes

static void test_endpoint_default_never_times_out_a_heard_stream()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.reliable_message_timeout_ms = 0;                     // the default: off
    check( rocketnet_message_validate_config( &config ) == 1 );

    ep_side_t * a = new ep_side_t();
    ep_side_create( a, &config );

    // the endpoint's CONFIG is its one reliable number, and nothing ever
    // acknowledges it; the transport is connected and hears the other end
    // every update. 2800 updates of 0.25 s run 700 s, past 10 s and 600 s.

    const double t0 = 1000.0;
    rn_endpoint_reset( &a->endpoint, t0 );
    rn_endpoint_announce_config( &a->endpoint, t0 );

    int written = 0;
    for ( int i = 0; i <= 2800; i++ )
    {
        const double now = t0 + i * 0.25;
        written += ep_lone_update( a, now, 1, EP_KEEP_ALIVE_IDS + (uint64_t) i );
        check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    check( a->endpoint.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( a->endpoint.config_error == ROCKETNET_DISCONNECT_REASON_NONE );
    check( a->endpoint.config_wait_start == t0 );               // the CONFIG wait began and has no clock
    check( !a->endpoint.ordered.config_received );
    check( a->endpoint.ordered.send.U < a->endpoint.ordered.send.next );    // the CONFIG still unacknowledged
    check( a->endpoint.ordered.ack.oldest_first_send == t0 );
    check( written >= 2800 );                                   // the oldest number kept going out (the probe)
    check( ep_counter( a, ROCKETNET_COUNTER_MESSAGE_RESENT ) > 0 );

    // the CONFIG wait alone, from a fresh start: no clock ends it

    const double start = t0 + 1000.0;
    rn_endpoint_reset( &a->endpoint, start );
    rn_endpoint_config_tick( &a->endpoint, 1, start );
    check( a->endpoint.config_wait_start == start );
    const double waits[3] = { 10.0, 600.0, 1000000.0 };
    for ( int i = 0; i < 3; i++ )
    {
        rn_endpoint_config_tick( &a->endpoint, 1, start + waits[i] );
        check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    ep_side_destroy( a );
    delete a;
}

static void test_endpoint_opted_in_timeout_ends_the_stall_and_the_config_wait()
{
    rocketnet_message_config_t config;
    ep_config( &config );
    config.reliable_message_timeout_ms = 10000;
    check( rocketnet_message_validate_config( &config ) == 1 );

    ep_side_t * a = new ep_side_t();
    ep_side_create( a, &config );

    // the stall: the CONFIG is first sent at t0 and never acknowledged while
    // the transport hears the other end; the transport is not yet connected,
    // so the CONFIG wait has not begun and only the stall can end it

    const double t0 = 1000.0;
    rn_endpoint_reset( &a->endpoint, t0 );
    rn_endpoint_announce_config( &a->endpoint, t0 );

    check( ep_lone_update( a, t0, 0, EP_KEEP_ALIVE_IDS ) >= 1 );
    check( a->endpoint.ordered.ack.has_oldest && a->endpoint.ordered.ack.oldest_first_send == t0 );

    for ( int i = 1; i < 40; i++ )
    {
        const double now = t0 + i * 0.25;
        ep_lone_update( a, now, 0, EP_KEEP_ALIVE_IDS + (uint64_t) i );
        check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    }

    ep_lone_update( a, t0 + 10.0, 0, EP_KEEP_ALIVE_IDS + 40 );     // 10 s from the first send
    check( a->endpoint.ordered.ack.error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( a->endpoint.config_wait_start < 0.0 );
    check( a->endpoint.config_error == ROCKETNET_DISCONNECT_REASON_NONE );

    // the CONFIG wait: RELIABLE_TIMED_OUT 10 s after it began

    const double start = t0 + 100.0;
    rn_endpoint_reset( &a->endpoint, start );
    check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    rn_endpoint_config_tick( &a->endpoint, 1, start );
    rn_endpoint_config_tick( &a->endpoint, 1, start + 9.999 );
    check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_NONE );
    rn_endpoint_config_tick( &a->endpoint, 1, start + 10.0 );
    check( a->endpoint.config_error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );
    check( rn_endpoint_error( &a->endpoint ) == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );

    ep_side_destroy( a );
    delete a;
}

REGISTER_UNIT_TEST( test_endpoint_default_never_times_out_a_heard_stream );
REGISTER_UNIT_TEST( test_endpoint_opted_in_timeout_ends_the_stall_and_the_config_wait );
REGISTER_UNIT_TEST( test_endpoint_back_to_back_over_lossy_array );
REGISTER_UNIT_TEST( test_endpoint_uncapped_large_sequenced_skips_the_bucket );
REGISTER_UNIT_TEST( test_endpoint_never_allocates_or_reads_a_clock );
REGISTER_UNIT_TEST( test_endpoint_capacity_cut_starts_new_write_pass );
REGISTER_UNIT_TEST( test_endpoint_dropped_large_sequenced_is_not_a_written_packet );
REGISTER_UNIT_TEST( test_endpoint_reliable_continues_after_capacity_cut );
REGISTER_UNIT_TEST( test_endpoint_stalled_window_requests_no_frame );
REGISTER_UNIT_TEST( test_endpoint_partial_ack_stall_requests_no_frame );
REGISTER_UNIT_TEST( test_endpoint_probe_hold_requests_frame_only_for_u );
REGISTER_UNIT_TEST( test_endpoint_ended_pass_stays_ended_until_the_next_flush );
REGISTER_UNIT_TEST( test_endpoint_unreliable_tombstone_never_revives_after_ticket_wrap );
REGISTER_UNIT_TEST( test_endpoint_unreliable_renumber_at_ticket_wrap_keeps_queue_order );
REGISTER_UNIT_TEST( test_endpoint_unreliable_ring_full_push_keeps_every_live_message );
REGISTER_UNIT_TEST( test_endpoint_unreliable_renumber_at_ticket_wrap_over_a_full_ring );

// The cases above that take ep_config's timeout, rerun opted in at 600000
// (no probe pacing, no round-trip filter: the v1.0 resend rule), in one
// process. The registered cases run them at the default; the three that set
// their own timeout (test_endpoint_default_never_times_out_a_heard_stream,
// test_endpoint_opted_in_timeout_ends_the_stall_and_the_config_wait,
// test_endpoint_probe_hold_requests_frame_only_for_u) run the same either way.
static void test_endpoint_cases_opted_in()
{
    static void ( *const cases[] )() = {
        test_endpoint_back_to_back_over_lossy_array,
        test_endpoint_uncapped_large_sequenced_skips_the_bucket,
        test_endpoint_never_allocates_or_reads_a_clock,
        test_endpoint_capacity_cut_starts_new_write_pass,
        test_endpoint_dropped_large_sequenced_is_not_a_written_packet,
        test_endpoint_reliable_continues_after_capacity_cut,
        test_endpoint_stalled_window_requests_no_frame,
        test_endpoint_ended_pass_stays_ended_until_the_next_flush,
        test_endpoint_unreliable_tombstone_never_revives_after_ticket_wrap,
    };
    g_ep_reliable_timeout_ms = 600000;
    for ( size_t i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ )
    {
        cases[i]();
    }
    g_ep_reliable_timeout_ms = 0;
}

REGISTER_UNIT_TEST( test_endpoint_cases_opted_in );
