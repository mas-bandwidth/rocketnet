/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The seeded resend delay (standard/MESSAGES.md m9.2; proofs/tla/RNResendTiming.tla
// Seed and SeededGapBounded): until a connection's message layer has an RTT
// sample of its own, the first update the transport reports a measured round
// trip for the connection takes it as the first sample, so the resend delay
// is 2 * rtt + 4 * rtt + 30 ms from then on rather than
// message_resend_delay_initial_ms. A transport with no measurement keeps the
// initial delay, and the layer's own first sample is never replaced.
//
// Each case drives a message object over an in-memory link whose rtt entry
// the case sets. Time is an argument; nothing here reads a real clock, opens
// a socket or sleeps.

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test_unit_registry.h"
#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_message_object.h"
#include "../sdk/rocketnet_send_budget.h"
#include "../sdk/rocketnet_window_ack.h"

#define check rocketnet_unit_check

#if ROCKETNET_DEV

struct seed_transport_t
{
    alignas( 16 ) uint8_t frame[16 + ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint64_t next_packet;
    uint64_t last_packet;       // the id of the last packet finished
    uint64_t ack_packet;        // acked at the next update, then cleared
    uint64_t ack_batch;
    int sends;                  // packets finished: every one carries the CONFIG record
    double first_resend;        // the time of the second send, 0 before it
    double now;
    double rtt;                 // the transport's reported round trip, seconds; 0 is none
};

static void seed_link_update( void * context, double )
{
    seed_transport_t * t = (seed_transport_t *) context;
    t->ack_batch = t->ack_packet;
    t->ack_packet = 0;
}

static int seed_link_payloads( void *, const int ** slots, uint8_t * const ** data, const int ** bytes )
{
    *slots = NULL;
    *data = NULL;
    *bytes = NULL;
    return 0;
}

static int seed_link_acked( void * context, const int ** slots, const uint64_t ** packets )
{
    seed_transport_t * t = (seed_transport_t *) context;
    *slots = NULL;
    *packets = &t->ack_batch;
    return t->ack_batch ? 1 : 0;
}

static uint8_t * seed_link_start( void * context, int, int bytes, rocketnet_packet_context_t * packet )
{
    seed_transport_t * t = (seed_transport_t *) context;
    if ( bytes > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES )
        return NULL;
    memset( packet, 0, sizeof( *packet ) );
    packet->frame = t->frame;
    packet->payload_index = 16;
    packet->payload_capacity = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    packet->packet_id = ++t->next_packet;
    return t->frame;
}

static int seed_link_finish( void * context, rocketnet_packet_context_t * packet, int bytes )
{
    seed_transport_t * t = (seed_transport_t *) context;
    t->sends++;
    if ( t->sends == 2 )
        t->first_resend = t->now;
    t->last_packet = packet->packet_id;
    return rocketnet_send_budget_wire_bytes( ROCKETNET_ACK_BLOCK_MIN_BYTES, bytes );
}

static void seed_link_abort( void *, rocketnet_packet_context_t * ) {}
static void seed_link_send( void * ) {}
static uint64_t seed_link_connection( void *, int ) { return 1; }
static void seed_link_disconnect( void *, int ) {}
static int seed_link_state( void * ) { return ROCKETNET_CLIENT_TRANSPORT_STATE_CONNECTED; }

static double seed_link_rtt( void * context, int )
{
    return ( (seed_transport_t *) context )->rtt;
}

static rn_message_link_t seed_link( seed_transport_t * t, bool measures )
{
    rn_message_link_t link;
    memset( &link, 0, sizeof( link ) );
    link.transport = t;
    link.update = seed_link_update;
    link.payloads = seed_link_payloads;
    link.acked = seed_link_acked;
    link.start_packet = seed_link_start;
    link.finish_packet = seed_link_finish;
    link.abort_packet = seed_link_abort;
    link.send_packets = seed_link_send;
    link.connection = seed_link_connection;
    link.disconnect = seed_link_disconnect;
    link.state = seed_link_state;
    link.rtt = measures ? seed_link_rtt : NULL;
    return link;
}

static const double seed_start = 1000.0;
static const double seed_tick = 1.0 / 60.0;
static const double seed_rtt = 0.050;                               // the transport's measured round trip
static const double seed_reported_at = 0.200;                       // when the transport first reports it
static const double seed_delay = 2.0 * 0.050 + 4.0 * 0.050 + 0.030; // the seeded resend delay, 330 ms

struct seed_run_t
{
    seed_transport_t transport;
    rn_message_link_t link;
    rn_message_object_t * object;
    rn_window_ack_t * ack;
    double time;
};

// One object, server or client, over the in-memory link, with the
// connection's CONFIG sent once at seed_start and never acked.
static void seed_open( seed_run_t * run, bool is_server, bool measures )
{
    memset( &run->transport, 0, sizeof( run->transport ) );
    run->link = seed_link( &run->transport, measures );
    run->object = rn_message_object_create( NULL, &run->link, is_server ? 1 : 0, 1, NULL, NULL, 0,
                                            is_server ? ROCKETNET_DEFAULT_SERVER_TO_CLIENT_KBPS : ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS,
                                            seed_start );
    check( run->object != NULL );
    run->ack = &run->object->connections[0].ordered.ack;
    run->time = seed_start;
}

static void seed_step( seed_run_t * run )
{
    run->transport.now = run->time;
    check( rn_message_object_update( run->object, run->time ) == ROCKETNET_UPDATE_OK );
    rn_message_object_send_packets( run->object );
    run->time += seed_tick;
}

// The transport reports its round trip from seed_reported_at on; returns the
// number of CONFIG sends up to `until` seconds after the start.
static int seed_run_until( seed_run_t * run, double until )
{
    while ( run->time < seed_start + until )
    {
        if ( run->time >= seed_start + seed_reported_at )
            run->transport.rtt = seed_rtt;
        seed_step( run );
    }
    return run->transport.sends;
}

static void seed_close( seed_run_t * run )
{
    rn_message_object_destroy( run->object );
    run->object = NULL;
}

// test_resend_seeded_from_transport_rtt: the lock-in. A connection whose
// transport has measured its round trip resends its unacked CONFIG at the
// delay derived from that measurement, 330 ms for a 50 ms round trip, not at
// message_resend_delay_initial_ms (2 s). Both ends: the server's connection,
// bound by Observe, and the client's, bound at create.
static void test_resend_seeded_from_transport_rtt()
{
    for ( int is_server = 0; is_server <= 1; is_server++ )
    {
        seed_run_t run;
        seed_open( &run, is_server != 0, true );

        // before the transport has a measurement the initial delay stands

        check( seed_run_until( &run, seed_reported_at - seed_tick ) == 1 );
        check( run.ack->has_rtt == 0 );
        check( fabs( run.ack->resend_delay - 2.0 ) < 1e-12 );

        // the first update it reports one seeds the estimate

        seed_run_until( &run, seed_reported_at + 2.0 * seed_tick );
        check( run.ack->has_rtt == 1 );
        check( fabs( run.ack->srtt - seed_rtt ) < 1e-12 );
        check( fabs( run.ack->rttvar - seed_rtt ) < 1e-12 );
        check( fabs( run.ack->resend_delay - seed_delay ) < 1e-12 );

        // the CONFIG first resends at the first update past 330 ms, and every
        // 330 ms after it: five more sends before 2 s, where the unseeded
        // delay sends none

        const int sends = seed_run_until( &run, 1.9 );
        check( run.transport.first_resend >= seed_start + seed_delay );
        check( run.transport.first_resend < seed_start + seed_delay + seed_tick );
        check( sends >= 6 );

        seed_close( &run );
    }
}

REGISTER_UNIT_TEST( test_resend_seeded_from_transport_rtt );

// test_resend_unseeded_without_transport_rtt: a link that measures nothing
// (rtt NULL) and a transport that has no measurement yet (rtt 0) keep the
// configured initial delay: one CONFIG send before 2 s, the resend at 2 s.
static void test_resend_unseeded_without_transport_rtt()
{
    for ( int measures = 0; measures <= 1; measures++ )
    {
        seed_run_t run;
        seed_open( &run, true, measures != 0 );

        while ( run.time < seed_start + 1.99 )
            seed_step( &run );                                      // the transport reports 0 throughout
        check( run.transport.sends == 1 );
        check( run.ack->has_rtt == 0 );
        check( fabs( run.ack->resend_delay - 2.0 ) < 1e-12 );

        while ( run.time < seed_start + 2.0 + 2.0 * seed_tick )
            seed_step( &run );
        check( run.transport.sends == 2 );
        check( run.transport.first_resend >= seed_start + 2.0 );

        seed_close( &run );
    }
}

REGISTER_UNIT_TEST( test_resend_unseeded_without_transport_rtt );

// test_resend_seed_never_replaces_own_sample: once the layer's own ack has
// given a sample, a later transport report does not touch the estimate.
// The CONFIG is acked 100 ms after its send (srtt = rttvar = 100 ms, resend
// delay 630 ms); the transport then reports 50 ms and the estimate stays.
static void test_resend_seed_never_replaces_own_sample()
{
    seed_run_t run;
    seed_open( &run, true, true );

    seed_step( &run );
    check( run.transport.sends == 1 );
    const uint64_t config_packet = run.transport.last_packet;

    while ( run.time < seed_start + 0.1 )
        seed_step( &run );
    run.transport.ack_packet = config_packet;
    const double acked_at = run.time;
    seed_step( &run );
    check( run.ack->has_rtt == 1 );

    const double own = acked_at - seed_start;
    check( fabs( run.ack->srtt - own ) < 1e-12 );
    check( fabs( run.ack->resend_delay - ( 6.0 * own + 0.030 ) ) < 1e-12 );

    seed_run_until( &run, 1.0 );
    check( fabs( run.ack->srtt - own ) < 1e-12 );
    check( fabs( run.ack->rttvar - own ) < 1e-12 );
    check( fabs( run.ack->resend_delay - ( 6.0 * own + 0.030 ) ) < 1e-12 );

    seed_close( &run );
}

REGISTER_UNIT_TEST( test_resend_seed_never_replaces_own_sample );

// test_resend_seed_clamped_and_positive_only: the seed goes through the
// sample rule's clamp, and a report that is not positive seeds nothing.
static void test_resend_seed_clamped_and_positive_only()
{
    seed_run_t run;
    seed_open( &run, true, true );
    rn_window_ack_t * ack = run.ack;

    check( rn_window_ack_seed_rtt( ack, 0.0 ) == 0 );
    check( rn_window_ack_seed_rtt( ack, -0.010 ) == 0 );
    check( ack->has_rtt == 0 && fabs( ack->resend_delay - 2.0 ) < 1e-12 );

    // a 1 s round trip: 6 s before the clamp, the configured 2 s maximum after it

    check( rn_window_ack_seed_rtt( ack, 1.0 ) == 1 );
    check( ack->has_rtt == 1 && fabs( ack->resend_delay - 2.0 ) < 1e-12 );
    check( fabs( ack->srtt - 1.0 ) < 1e-12 );

    // only the first seed is taken

    check( rn_window_ack_seed_rtt( ack, 0.050 ) == 0 );
    check( fabs( ack->srtt - 1.0 ) < 1e-12 );

    seed_close( &run );
}

REGISTER_UNIT_TEST( test_resend_seed_clamped_and_positive_only );

#endif // #if ROCKETNET_DEV
