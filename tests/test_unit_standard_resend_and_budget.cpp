/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The vector twins of RN-2 m9 (standard/MESSAGES.md, Acknowledgement mapping,
// resend and send budget). Each generator drives the implementation from the
// inputs the document states, starting the clock at 1000.0. No socket, no
// thread, no wall clock.

#include "test_unit_standard.h"

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_endpoint.h"
#include "rocketnet_message.h"
#include "rocketnet_send_budget.h"
#include "rocketnet_window_send.h"
#include "rocketnet_window_ack.h"
#include "rocketnet_channel.h"

#include <string.h>

#define check rocketnet_unit_check

// ---------------------------------------------------------------------------
// m9.resend-trace: the sent-packet map, the adaptive resend delay, and the
// reliable timeout over two paths, time an argument from 1000.0.
// ---------------------------------------------------------------------------

struct resend_trace_fixture_t
{
    rocketnet_message_config_t config;
    rn_arena_t * arena;
    rn_region_t * region;
    rn_ordered_t ordered;
    rn_window_send_t & send;
    rn_window_ack_t & ack;
    uint64_t next_packet_id;

    resend_trace_fixture_t() : send( ordered.send ), ack( ordered.ack ) {}
};

static void resend_trace_open( resend_trace_fixture_t * f )
{
    rocketnet_message_default_config( &f->config );
    check( rocketnet_message_validate_config( &f->config ) );

    f->arena = rn_arena_create( NULL, &f->config, 1, NULL );
    check( f->arena != NULL );

    f->region = rn_arena_region( f->arena, 0 );
    rn_region_carve( f->region );
    check( rn_ordered_init( &f->ordered, f->region, &f->config, NULL, ROCKETNET_PACKET_BUILDER_MAX_PACKETS, ROCKETNET_MESSAGE_SINGLE_BYTES ) );

    f->next_packet_id = 1;
}

static void resend_trace_close( resend_trace_fixture_t * f )
{
    rn_arena_destroy( f->arena );
    f->arena = NULL;
}

static uint64_t resend_trace_send( resend_trace_fixture_t * f, double now )
{
    const uint32_t d = rn_region_send_alloc( f->region, 16 );
    check( d != RN_DESCRIPTOR_NONE );

    uint64_t number = 0, ring_start = 0;
    check( rn_window_send_first_write( &f->send, ROCKETNET_MESSAGE_CODE_RELIABLE_ORDERED, 16, d, &number, &ring_start ) == RN_WINDOW_SEND_NUMBERED );

    const uint64_t packet_id = f->next_packet_id++;
    rn_window_ack_commit_packet( &f->ack, packet_id, &number, 1, now );
    return packet_id;
}

// One resend pass through the product walk (rn_ordered_write); the count is
// the numbers each written packet carries.
static uint32_t resend_trace_pass( resend_trace_fixture_t * f, double now )
{
    alignas( 16 ) static uint8_t frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    uint8_t * payloads[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];
    for ( int i = 0; i < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; i++ )
    {
        payloads[i] = frames[i];
    }

    const int written = rn_ordered_write( &f->ordered, now, payloads, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );
    uint32_t resent = 0;
    for ( int p = 0; p < written; p++ )
    {
        uint32_t count = 0;
        const uint64_t * numbers = rn_packet_builder_numbers( &f->ordered.builder, p, &count );
        check( numbers != NULL || count == 0 );

        resent += count;
        rn_ordered_commit( &f->ordered, p, f->next_packet_id++, now );
    }

    return resent;
}

static int standard_resend_and_budget_generate( const char * id, uint8_t * out, int capacity )
{
    if ( strcmp( id, "m9.resend-trace" ) == 0 )
    {
        int n = 0;
        const double t0 = 1000.0;

        if ( capacity < 16 )
            return -1;

        // short path: a 20 ms sample gives the adaptive resend delay of 150 ms
        {
            resend_trace_fixture_t f;
            resend_trace_open( &f );
            resend_trace_send( &f, t0 );
            const uint64_t second = resend_trace_send( &f, t0 );
            check( rn_window_ack_on_ack( &f.ack, second, t0 + 0.020 ) == 1 );

            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 0.040 );
            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 0.109 );
            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 0.111 );
            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 0.221 );
            out[n++] = (uint8_t) rn_window_ack_check_timeout( &f.ack, t0 + 9.999 );
            out[n++] = (uint8_t) rn_window_ack_check_timeout( &f.ack, t0 + 10.000 );
            out[n++] = 4;                                // the standard's code for RELIABLE_TIMED_OUT
            check( f.ack.error == ROCKETNET_DISCONNECT_REASON_RELIABLE_TIMED_OUT );

            resend_trace_close( &f );
        }

        // long path: a 400 ms sample is capped at the default 2 s adaptive resend delay
        {
            resend_trace_fixture_t f;
            resend_trace_open( &f );
            resend_trace_send( &f, t0 );
            const uint64_t second = resend_trace_send( &f, t0 );
            check( rn_window_ack_on_ack( &f.ack, second, t0 + 0.400 ) == 1 );

            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 0.400 );
            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 1.629 );
            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 1.631 );
            out[n++] = (uint8_t) resend_trace_pass( &f, t0 + 2.001 );
            resend_trace_close( &f );
        }

        return n;
    }

    if ( strcmp( id, "m9.budget-trace" ) == 0 )
    {
        int n = 0;

        if ( capacity < 32 )
            return -1;

        rocketnet_send_budget_t budget;
        check( rocketnet_send_budget_init( &budget, ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS, 1000.0 ) );

        rocketnet_send_budget_begin_update( &budget, 1000.0 );
        out[n++] = (uint8_t) rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1452 );
        for ( int i = 0; i < 7; i++ )
            out[n++] = (uint8_t) rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 1452 );
        out[n++] = (uint8_t) rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 1452 );

        // no time passes: the reliable item does not fit and waits, and the
        // unreliable item behind it is dropped rather than spending its bytes

        rocketnet_send_budget_begin_update( &budget, 1000.0 );
        out[n++] = (uint8_t) rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1452 );
        out[n++] = (uint8_t) rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_UNRELIABLE, 100 );

        // 100 ms of refill at the default rate

        rocketnet_send_budget_begin_update( &budget, 1000.1 );
        out[n++] = (uint8_t) rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1452 );

        // the setter: the old rate refills to now, the new rate holds after

        out[n++] = (uint8_t) rocketnet_send_budget_set_kbps( &budget, 2000, 1000.2 );

        // 100 ms at the new rate

        rocketnet_send_budget_begin_update( &budget, 1000.3 );
        out[n++] = (uint8_t) rocketnet_send_budget_admit( &budget, ROCKETNET_SEND_CLASS_NEW_RELIABLE, 1452 );

        // a budget of 0 detaches the bucket and nothing is charged; the large
        // state is promoted to RELIABLE_SEQUENCED (the promotion
        // rule), so its first packet is no larger than one maximum packet
        {
            rocketnet_message_config_t config;
            rocketnet_message_default_config( &config );
            check( rocketnet_message_validate_config( &config ) );

            rn_arena_t * arena = rn_arena_create( NULL, &config, 1, NULL );
            check( arena != NULL );

            rn_endpoint_config_t endpoint_config;
            memset( &endpoint_config, 0, sizeof( endpoint_config ) );
            endpoint_config.config = &config;
            endpoint_config.send_kbps = ROCKETNET_DEFAULT_CLIENT_TO_SERVER_KBPS;
            endpoint_config.max_payload_bytes = 65536;

            rn_endpoint_t endpoint;
            rn_endpoint_init( &endpoint, rn_arena_region( arena, 0 ), &endpoint_config );
            rn_endpoint_reset( &endpoint, 1000.0 );
            out[n++] = (uint8_t) rn_endpoint_set_send_kbps( &endpoint, 0, 1000.0 );
            check( endpoint.send_budget_enabled == 0 );

            const int bytes = 13000;
            uint8_t * p = rn_endpoint_start( &endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, bytes, endpoint_config.max_payload_bytes );
            check( p != NULL );
            if ( p )
                memset( p, 0x5a, (size_t) bytes );
            check( rn_endpoint_finish( &endpoint, bytes, 0 ) != 0 );

            rn_endpoint_begin_update( &endpoint );
            const int request = rn_endpoint_next_packet_bytes( &endpoint, 1000.0 );
            out[n++] = (uint8_t) ( request > 0 );

            static uint8_t frame[16384];
            const int written = request > 0 ? rn_endpoint_write_packet( &endpoint, frame, (int) sizeof( frame ), 1, 1000.0 ) : 0;
            if ( written > 0 )
                rn_endpoint_packet_sent( &endpoint, 1, written + 26, 1000.0 );
            out[n++] = (uint8_t) ( written > ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
            out[n++] = (uint8_t) ( endpoint.unreliable_count == 0 );
            out[n++] = (uint8_t) ( endpoint.budget.bytes_spent == 0 );
            out[n++] = (uint8_t) ( endpoint.budget.unreliable_dropped == 0 );
            rn_arena_destroy( arena );
        }

        return n;
    }

    return -1;
}

// Re-pin m9 because the trace now waits for the adaptive resend delay.
static const rn_standard_vector_t standard_resend_and_budget_table[] = {
    RN_STANDARD_VECTOR( "m9.resend-trace", "0000000100010400000001" ),
    RN_STANDARD_VECTOR( "m9.budget-trace", "0000000000000000020102000100010100010101" ),
};

static void test_standard_resend_and_budget_vectors()
{
    const int count = (int) ( sizeof( standard_resend_and_budget_table ) / sizeof( standard_resend_and_budget_table[0] ) );
    check( rn_standard_vectors_walk( standard_resend_and_budget_table, count, standard_resend_and_budget_generate ) == count );
}

REGISTER_UNIT_TEST( test_standard_resend_and_budget_vectors );
