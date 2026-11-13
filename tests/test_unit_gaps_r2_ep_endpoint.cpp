/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The endpoint (sdk/rocketnet_endpoint.cpp, proofs/tla/RNEndpoint.tla) at the
// edges the mutation campaigns found unchecked: a slot reset after a runtime
// cap, the serial wrap, the refusals of send, the in-place whole-frame finish
// (its refill, its writer refusal, its zero-charge drop, the ordering index it
// writes), the in-place header and the affordability test, the write pass's
// guards (a second plan or write while a packet is written, a plan that never
// got a frame), the budget's large sequenced drop, the write-first question,
// the runtime cap's refusals, the receive copy's capacity and the LOSS of a
// failed packet's receipts. Each case drives one endpoint (two for the receive
// copy) as a pure machine: no socket, no clock, no transport; time is an
// argument from 1000.0.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_allocator.h"
#include "../sdk/rocketnet_channel.h"
#include "../sdk/rocketnet_counters.h"
#include "../sdk/rocketnet_endpoint.h"
#include "../sdk/rocketnet_message.h"
#include "../sdk/rocketnet_send_budget.h"

// a failed expectation names the fault before the check prints its line; the
// condition is evaluated once
#define gx_expect( condition, fault )                                                                              \
do                                                                                                                 \
{                                                                                                                  \
    const bool gx_held = (condition);                                                                              \
    if ( !gx_held )                                                                                                \
    {                                                                                                              \
        printf( "fault: %s\n", fault );                                                                            \
        fflush( stdout );                                                                                          \
    }                                                                                                              \
    rocketnet_unit_check( gx_held );                                                                               \
} while(0)

static const double GX_NOW = 1000.0;

struct gx_t
{
    rocketnet_message_config_t config;
    rocketnet_counters_t counters;
    rn_arena_t * arena;
    rn_endpoint_t endpoint;
};

// one endpoint on its own arena, reset at GX_NOW; kbps 0 or lower is uncapped
static void gx_create( gx_t * x, int32_t kbps )
{
    rocketnet_message_default_config( &x->config );
    x->config.reliable_messages_in_flight = 64;
    x->config.send_queue_messages = 128;
    x->config.packets_in_flight = 64;
    x->config.receive_message_buffer_bytes = 65536;
    x->config.max_message_bytes = 65536;
    x->config.send_message_buffer_bytes = 262144;
    x->config.packets_per_update = 4;

    memset( &x->counters, 0, sizeof( x->counters ) );
    x->arena = rn_arena_create( NULL, &x->config, 1, &x->counters );
    rocketnet_unit_check( x->arena != NULL );

    rn_endpoint_config_t endpoint_config;
    memset( &endpoint_config, 0, sizeof( endpoint_config ) );
    endpoint_config.config = &x->config;
    endpoint_config.send_kbps = kbps;
    endpoint_config.max_payload_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES;
    rn_endpoint_init( &x->endpoint, rn_arena_region( x->arena, 0 ), &endpoint_config );
    rn_endpoint_reset( &x->endpoint, GX_NOW );
}

static void gx_destroy( gx_t * x )
{
    rn_arena_destroy( x->arena );
}

static uint64_t gx_counter( const gx_t * x, int counter )
{
    return x->counters.values[counter];
}

static uint32_t gx_send( gx_t * x, int reliability, int channel, int bytes, uint8_t fill )
{
    uint8_t data[2048];
    rocketnet_unit_check( bytes > 0 && bytes <= (int) sizeof( data ) );
    memset( data, fill, (size_t) bytes );
    return rn_endpoint_send( &x->endpoint, data, bytes, reliability, ROCKETNET_PRIORITY_MEDIUM, channel, 0 );
}

// the wire bytes the transport reports for a sealed payload with the smallest ack block
static int gx_wire( int payload_bytes )
{
    return ROCKETNET_PACKET_OVERHEAD_BYTES + ROCKETNET_ACK_BLOCK_MIN_BYTES + payload_bytes;
}

static void gx_limits( const gx_t * x, rocketnet_message_limits_t * limits )
{
    memset( limits, 0, sizeof( *limits ) );
    limits->num_channels = x->config.num_channels;
    limits->enabled_codes = 0x1F;
    limits->reliable_messages_in_flight = x->config.reliable_messages_in_flight;
    limits->max_message_bytes = x->config.max_message_bytes;
    limits->receive_message_buffer_bytes = x->config.receive_message_buffer_bytes;
}

static int gx_count_records( const gx_t * x, const uint8_t * payload, int bytes )
{
    rocketnet_message_limits_t limits;
    gx_limits( x, &limits );

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

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2158
// mutation gap: hunt 1/2192
//
// A connection uncapped by default but capped at runtime
// (rn_endpoint_set_send_kbps) is uncapped again when its slot is reset, and
// the reset ring holds nothing of the previous connection. A reset that kept
// send_budget_enabled would read a zeroed bucket (no in-place packet
// affordable); a reset ring that counted one stale entry could walk the
// previous connection's message.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_reset_after_runtime_cap_is_uncapped_with_an_empty_ring()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    rocketnet_unit_check( endpoint->send_budget_enabled == 0 );
    rocketnet_unit_check( rn_endpoint_set_send_kbps( endpoint, 100, GX_NOW ) );
    rocketnet_unit_check( endpoint->send_budget_enabled == 1 );

    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 100, (uint8_t) ( 0x10 + i ) ) != 0 );
    }
    rocketnet_unit_check( endpoint->unreliable_ring_used == 3 );

    rn_endpoint_reset( endpoint, GX_NOW + 1.0 );

    gx_expect( endpoint->send_budget_enabled == 0,
               "a connection capped at runtime keeps send_budget_enabled across a slot reset, over a zeroed bucket" );
    gx_expect( rn_endpoint_in_place_affordable( endpoint, 100, GX_NOW + 1.0 ),
               "the reset (uncapped) connection cannot pay an in-place packet: it reads the zeroed bucket of the old cap" );
    gx_expect( endpoint->unreliable_ring_used == 0, "the reset unreliable ring counts a stale entry of the previous connection" );

    rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 100, 0x77 ) != 0 );
    gx_expect( endpoint->unreliable_ring_used == 1, "one send after the reset left more than one ring entry" );

    uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_begin_update( endpoint );
    rn_endpoint_begin_flush( endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW + 1.0 ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    gx_expect( !rn_endpoint_write_may_be_refused( endpoint ), "the reset (uncapped) connection says its budget may refuse a packet" );
    const int written = rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 1, GX_NOW + 1.0 );
    rocketnet_unit_check( written > 0 );
    gx_expect( gx_count_records( &x, frame, written ) == 1, "the first packet after the reset carries a message of the previous connection" );
    rn_endpoint_packet_sent( endpoint, 1, gx_wire( written ), GX_NOW + 1.0 );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2262
//
// A second start while one is pending is refused, counted and logged with
// its own reason: the caller learns it must finish or abort the first.
// ---------------------------------------------------------------------------

static char gx_log[4096];
static size_t gx_log_bytes = 0;

static void gx_log_capture( void * context, rocketnet_log_level_t level, const char * message, size_t length )
{
    (void) context;
    (void) level;
    if ( gx_log_bytes + length + 1 < sizeof( gx_log ) )
    {
        memcpy( gx_log + gx_log_bytes, message, length );
        gx_log_bytes += length;
        gx_log[gx_log_bytes++] = '\n';
        gx_log[gx_log_bytes] = '\0';
    }
}

static void test_gaps_r2_ep_second_start_logs_the_pending_start()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    rocketnet_unit_check( rn_endpoint_start( endpoint, 0, ROCKETNET_RELIABILITY_RELIABLE, 100, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) != NULL );

    gx_log_bytes = 0;
    gx_log[0] = '\0';
    const rocketnet_log_level_t saved = rocketnet_log_level_current();
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_INFO );
    rocketnet_quiet( false );
    rocketnet_log_function( gx_log_capture, NULL );

    const uint64_t refused = gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    uint8_t * second = rn_endpoint_start( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

    rocketnet_log_function( NULL, NULL );
    rocketnet_log_level( saved );

    rocketnet_unit_check( second == NULL );
    rocketnet_unit_check( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused + 1 );
    gx_expect( strstr( gx_log, "a start is already pending" ) != NULL,
               "a start refused because one is pending logs nothing that says so" );

    rn_endpoint_abort( endpoint );
    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2296
// mutation gap: hunt 1/2449
//
// A region the receive side exhausted: an unreliable start sets the send
// error the owner kicks the slot on at once (rocketnet_message_server.cpp
// reads connection->send_error after a refused start), and the in-place header
// sends the message to the region path, which refuses it. A type the channel
// set does not enable is likewise never written in place.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_in_place_header_and_start_on_a_disabled_type_or_exhausted_region()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    rocketnet_unit_check( rn_endpoint_in_place_header( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, GX_NOW ) == 0 );

    const uint32_t enabled = endpoint->ordered.channels.enabled_types;
    endpoint->ordered.channels.enabled_types = enabled & ~( 1u << ROCKETNET_RELIABILITY_UNRELIABLE );
    gx_expect( rn_endpoint_in_place_header( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, GX_NOW ) == RN_ENDPOINT_IN_PLACE_NO,
               "a type the connection does not enable is offered the in-place path" );
    endpoint->ordered.channels.enabled_types = enabled;

    rn_region_exhaust( endpoint->region );
    gx_expect( rn_endpoint_in_place_header( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, GX_NOW ) == RN_ENDPOINT_IN_PLACE_NO,
               "an exhausted region is offered the in-place path" );

    gx_expect( rn_endpoint_start( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ) == NULL,
               "an unreliable start on an exhausted region was given room" );
    gx_expect( endpoint->send_error == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED,
               "an unreliable start on an exhausted region does not set the send error the owner ends the connection on" );
    rocketnet_unit_check( rn_endpoint_error( endpoint ) == ROCKETNET_DISCONNECT_REASON_MESSAGE_MEMORY_EXHAUSTED );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2343
// mutation gap: hunt 1/2344
// mutation gap: hunt 1/2345
// mutation gap: hunt 1/2615
// mutation gap: hunt 1/2616
// mutation gap: hunt 1/2618
// mutation gap: hunt 1/2619
//
// The queued send's serial steps to 1 past 0xFFFFFFFF, never 0 (a 0 serial
// is a refusal; proofs/tla/RNSentRingLayout.tla IssueSerial). Refused sends
// (no data, a priority other than MEDIUM, a channel outside the set) return
// 0, and the first two count MESSAGE_SEND_REFUSED.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_send_serial_wraps_to_one_and_refusals_return_zero()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    endpoint->serial = 0xFFFFFFFFu;
    const uint32_t serial = gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 50, 0x21 );
    gx_expect( serial == 1, "the serial after 0xFFFFFFFF is not 1 (0 reads as a refusal, 2 skips one)" );
    gx_expect( endpoint->serial == 1, "the endpoint's last serial after the wrap is not 1" );

    uint8_t data[64];
    memset( data, 0x22, sizeof( data ) );

    uint64_t refused = gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    gx_expect( rn_endpoint_send( endpoint, NULL, 10, ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_PRIORITY_MEDIUM, 0, 0 ) == 0,
               "a send with no data returned a serial" );
    gx_expect( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused + 1, "a send with no data is not counted MESSAGE_SEND_REFUSED" );

    refused = gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED );
    gx_expect( rn_endpoint_send( endpoint, data, 10, ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_PRIORITY_MEDIUM + 1, 0, 0 ) == 0,
               "a send at a priority other than MEDIUM returned a serial" );
    gx_expect( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_SEND_REFUSED ) == refused + 1,
               "a send at a priority other than MEDIUM is not counted MESSAGE_SEND_REFUSED" );

    rocketnet_quiet( true );
    const uint32_t outside = rn_endpoint_send( endpoint, data, 10, ROCKETNET_RELIABILITY_UNRELIABLE, ROCKETNET_PRIORITY_MEDIUM, -1, 0 );
    rocketnet_quiet( false );
    gx_expect( outside == 0, "a send whose start was refused (channel -1) returned a serial" );
    gx_expect( endpoint->pending_type == -1, "a refused send left a start pending" );
    gx_expect( endpoint->unreliable_count == 1, "a refused send queued a message" );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2486
// mutation gap: hunt 1/2570
// mutation gap: hunt 1/2575
//
// The in-place packet's budget. A payload the seal path refuses (no wire
// charge, rocketnet_send_budget_max_wire_bytes 0) is never affordable and is
// dropped at finish without offering the bucket a 0-byte admit (which counts
// a bad-argument refusal). The finish refills the bucket to its own time
// before it admits: a start whose bucket was empty is paid at finish by the
// time that passed.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_in_place_budget_at_finish()
{
    // affordability: no wire charge is never affordable

    {
        gx_t x;
        gx_create( &x, 100000 );
        rn_endpoint_t * endpoint = &x.endpoint;
        rocketnet_unit_check( rocketnet_send_budget_max_wire_bytes( 2000 ) == 0 );
        rocketnet_unit_check( rn_endpoint_in_place_affordable( endpoint, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, GX_NOW ) );
        gx_expect( !rn_endpoint_in_place_affordable( endpoint, 2000, GX_NOW ),
                   "a payload with no wire charge (beyond the seal path) is called affordable" );
        gx_destroy( &x );
    }

    // the finish refills: an empty bucket at the start, one second later at finish

    {
        gx_t x;
        gx_create( &x, 64 );
        rn_endpoint_t * endpoint = &x.endpoint;
        endpoint->budget.tokens = 0.0;

        static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
        rn_endpoint_in_place_begin( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, 100, 0 );
        memset( frame, 0x31, 100 );
        int payload_bytes = -1;
        const uint64_t over = gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        const uint32_t serial = rn_endpoint_in_place_finish( endpoint, frame, 100, GX_NOW + 1.0, &payload_bytes );
        rocketnet_unit_check( serial != 0 );
        gx_expect( payload_bytes == 101, "the in-place finish dropped a packet the bucket refilled to its time could pay (no refill at finish)" );
        gx_expect( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == over,
                   "the in-place finish counted an over-budget drop the refilled bucket could pay" );
        gx_destroy( &x );
    }

    // a finished payload the seal path refuses: dropped, counted, no 0-byte admit

    {
        gx_t x;
        gx_create( &x, 64 );
        rn_endpoint_t * endpoint = &x.endpoint;

        static uint8_t frame[2048];
        const int bytes = 1500;
        rn_endpoint_in_place_begin( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE, bytes, 0 );
        memset( frame, 0x32, (size_t) bytes );
        int payload_bytes = -1;
        const uint64_t over = gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET );
        const uint64_t refusals = endpoint->budget.refusals;
        const uint64_t spent = endpoint->budget.bytes_spent;
        const uint32_t serial = rn_endpoint_in_place_finish( endpoint, frame, bytes, GX_NOW, &payload_bytes );
        rocketnet_unit_check( serial != 0 );
        gx_expect( payload_bytes == 0, "a finished payload with no wire charge was handed to the transport" );
        gx_expect( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == over + 1,
                   "a finished payload with no wire charge is not counted MESSAGE_UNRELIABLE_OVER_BUDGET" );
        gx_expect( endpoint->budget.refusals == refusals, "the finish offered the bucket a 0-byte admit (a bad-argument refusal counted)" );
        gx_expect( endpoint->budget.bytes_spent == spent, "a dropped in-place packet was charged" );
        gx_destroy( &x );
    }
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2565
// mutation gap: hunt 1/2568
//
// A whole-frame record the writer refuses (here a begin whose header does not
// match the record's: a programmer error, asserted in a debug build) finishes
// with no payload, and the message is counted lost,
// MESSAGE_UNRELIABLE_SEND_FAILED, in every build.
// ---------------------------------------------------------------------------

static int gx_asserts = 0;

static void gx_assert_hook( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    gx_asserts++;
}

static void test_gaps_r2_ep_in_place_writer_refusal_is_a_failed_send()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    memset( frame, 0x41, sizeof( frame ) );

    // a sequenced record's header is 10 bytes; a begin that says 0 puts the
    // data where the writer's header goes, and the writer refuses it

    rn_endpoint_in_place_begin( endpoint, 0, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 100, 0 );

    const uint64_t failed = gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED );
    gx_asserts = 0;
    rocketnet_assert_function( gx_assert_hook );
    int payload_bytes = -1;
    const uint32_t serial = rn_endpoint_in_place_finish( endpoint, frame, 100, GX_NOW, &payload_bytes );
    rocketnet_assert_function( NULL );

    rocketnet_unit_check( serial != 0 );
    rocketnet_unit_check( gx_asserts <= 1 );
    gx_expect( payload_bytes == 0, "a record the writer refused was handed to the transport" );
    gx_expect( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_SEND_FAILED ) == failed + 1,
               "a whole-frame record the writer refused (length 0) is not counted MESSAGE_UNRELIABLE_SEND_FAILED" );
    gx_expect( endpoint->pending_type == -1 && endpoint->pending_in_place == 0, "a refused whole-frame finish left its start pending" );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2541
//
// The whole-frame sequenced record carries its channel's ordering index (the
// channel's ordered_write when it is finished), as the queued path does.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_in_place_sequenced_record_carries_its_ordering()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    const int channel = 1;
    const int bytes = 100;
    endpoint->region->channel_send[channel].ordered_write = 7;   // seven ordered messages before this one
    endpoint->region->channel_send[channel].sequenced_write = 3;

    const int header = rn_endpoint_in_place_header( endpoint, channel, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, bytes, ROCKETNET_MAX_PACKET_PAYLOAD_BYTES, GX_NOW );
    rocketnet_unit_check( header == 10 );

    static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_in_place_begin( endpoint, channel, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, bytes, header );
    memset( frame + header, 0x5c, (size_t) bytes );
    int payload_bytes = -1;
    rocketnet_unit_check( rn_endpoint_in_place_finish( endpoint, frame, bytes, GX_NOW, &payload_bytes ) != 0 );
    rocketnet_unit_check( payload_bytes == header + bytes + 1 );

    rocketnet_message_limits_t limits;
    gx_limits( &x, &limits );
    rocketnet_message_reader_t reader;
    rocketnet_unit_check( rocketnet_message_reader_begin( &reader, frame, payload_bytes, &limits ) == ROCKETNET_MESSAGE_VIOLATION_NONE );
    rocketnet_message_record_t record;
    rocketnet_unit_check( rocketnet_message_reader_next( &reader, &record ) );
    rocketnet_unit_check( record.code == ROCKETNET_MESSAGE_CODE_UNRELIABLE_SEQUENCED );
    rocketnet_unit_check( record.channel == (uint32_t) channel );
    rocketnet_unit_check( record.bytes == bytes );
    gx_expect( record.sequencing == 3, "the whole-frame sequenced record carries the wrong sequencing index" );
    gx_expect( record.ordering == 7, "the whole-frame sequenced record does not carry its channel's ordering index" );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2757
// mutation gap: hunt 1/2758
// mutation gap: hunt 1/2832
// mutation gap: hunt 1/2833
// mutation gap: hunt 1/2920
// mutation gap: hunt 1/2948
//
// The write pass's guards: no write without a plan, no packet_sent without a
// written packet, no NULL frame, and while a packet is written (until it is
// sent or failed) no second plan and no second write over it.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_write_pass_guards()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    rn_endpoint_packet_sent( endpoint, 5, 100, GX_NOW );
    gx_expect( endpoint->written == 0 && endpoint->packet_written == 0,
               "packet_sent with no written packet counted a packet against the update" );

    gx_expect( rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 1, GX_NOW ) == 0,
               "write_packet with no plan reported bytes written" );

    rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_RELIABLE, 0, 300, 0x6b ) != 0 );

    rn_endpoint_begin_update( endpoint );
    rn_endpoint_begin_flush( endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

    gx_expect( rn_endpoint_write_packet( endpoint, NULL, (int) sizeof( frame ), 1, GX_NOW ) == 0,
               "write_packet into no frame reported bytes written" );

    const int written = rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 1, GX_NOW );
    rocketnet_unit_check( written > 300 );
    rocketnet_unit_check( endpoint->packet_written == 1 );

    gx_expect( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == 0, "a second plan was made while the written packet is not yet sent or failed" );

    static uint8_t second[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    memset( second, 0xEE, sizeof( second ) );
    gx_expect( rn_endpoint_write_packet( endpoint, second, (int) sizeof( second ), 2, GX_NOW ) == 0,
               "a second write_packet while a packet is written reported bytes written" );
    gx_expect( second[0] == 0xEE, "a second write_packet while a packet is written wrote into its frame" );

    rn_endpoint_packet_sent( endpoint, 1, gx_wire( written ), GX_NOW );
    rocketnet_unit_check( endpoint->written == 1 );
    rocketnet_unit_check( endpoint->packet_written == 0 );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2971
// mutation gap: hunt 1/2972
// mutation gap: hunt 1/2973
//
// An unreliable plan that never got a frame (packet_failed with nothing
// written) ends the pass: the records stay queued, and the pass plans nothing
// more until the next flush, so a refused frame is not asked for again and
// again within one flush.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_unreliable_plan_without_a_frame_ends_the_pass()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 100, 0x12 ) != 0 );

    rn_endpoint_begin_update( endpoint );
    rn_endpoint_begin_flush( endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    rocketnet_unit_check( endpoint->packet_kind == RN_ENDPOINT_PACKET_UNRELIABLE );

    rn_endpoint_packet_failed( endpoint, 0, GX_NOW );

    gx_expect( endpoint->write_phase == RN_ENDPOINT_WRITE_DONE, "an unreliable plan with no frame did not end the write pass" );
    gx_expect( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == 0, "the flush plans the unreliable packet again after its frame was refused" );
    gx_expect( endpoint->unreliable_count == 1, "the refused plan's record left the queue" );

    rn_endpoint_begin_flush( endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2983
// mutation gap: hunt 1/2985
//
// A written unreliable packet the transport refuses: each receipt serial it
// carries takes exactly one LOSS, and no serial of an earlier packet (still
// in receipt_serials past this packet's count) takes one.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_failed_unreliable_packet_loses_exactly_its_receipts()
{
    gx_t x;
    gx_create( &x, 0 );
    rn_endpoint_t * endpoint = &x.endpoint;

    static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    rn_endpoint_begin_update( endpoint );

    // packet 1: two receipts, sent and acked

    const uint32_t s1 = gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 0, 40, 0x01 );
    const uint32_t s2 = gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 0, 40, 0x02 );
    rocketnet_unit_check( s1 != 0 && s2 != 0 );

    rn_endpoint_begin_flush( endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    const int first = rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 1, GX_NOW );
    rocketnet_unit_check( first > 0 );
    rocketnet_unit_check( endpoint->receipt_count == 2 );
    rn_endpoint_packet_sent( endpoint, 1, gx_wire( first ), GX_NOW );
    rn_endpoint_on_acked( endpoint, 1, GX_NOW );

    // packet 2: one receipt, written and refused by the transport

    const uint32_t s3 = gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE_WITH_ACK_RECEIPT, 0, 40, 0x03 );
    rocketnet_unit_check( s3 != 0 );
    rn_endpoint_begin_flush( endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    const int second = rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 2, GX_NOW );
    rocketnet_unit_check( second > 0 );
    rocketnet_unit_check( endpoint->receipt_count == 1 );
    rn_endpoint_packet_failed( endpoint, 2, GX_NOW );

    rn_endpoint_tick( endpoint, GX_NOW );

    rocketnet_message_receipt_t receipts[16];
    const int count = rn_endpoint_take_receipts( endpoint, receipts, 16 );
    int acked = 0;
    int lost = 0;
    int lost_s3 = 0;
    for ( int i = 0; i < count; i++ )
    {
        if ( receipts[i].status == ROCKETNET_RECEIPT_STATUS_ACKED )
        {
            acked++;
        }
        else if ( receipts[i].status == ROCKETNET_RECEIPT_STATUS_LOSS )
        {
            lost++;
            lost_s3 += receipts[i].serial == s3 ? 1 : 0;
        }
    }

    rocketnet_unit_check( acked == 2 );
    gx_expect( lost_s3 == 1, "the receipt of a written unreliable packet the transport refused took no LOSS" );
    gx_expect( lost == 1, "a refused unreliable packet emitted LOSS for a serial it does not carry (an earlier packet's)" );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2861
//
// The budget drops an unreliable packet's records at write: the write pass is
// over for this flush, and a message queued after the drop waits for the next
// flush rather than being planned against the bucket the drop found empty
// (proofs/tla/RNChannel.tla DropUnreliableForBudget: nothing further this pass).
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_unreliable_budget_drop_ends_the_pass()
{
    gx_t x;
    gx_create( &x, 64 );
    rn_endpoint_t * endpoint = &x.endpoint;

    static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 100, 0x51 ) != 0 );
    rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 100, 0x52 ) != 0 );

    endpoint->budget.tokens = 10.0;                                     // time does not move, so nothing refills it

    rn_endpoint_begin_update( endpoint );
    rn_endpoint_begin_flush( endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    rocketnet_unit_check( endpoint->packet_kind == RN_ENDPOINT_PACKET_UNRELIABLE );
    rocketnet_unit_check( rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 1, GX_NOW ) == 0 );
    rocketnet_unit_check( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == 2 );
    rocketnet_unit_check( endpoint->unreliable_count == 0 );

    gx_expect( endpoint->write_phase == RN_ENDPOINT_WRITE_DONE, "the write pass is not over after the budget dropped its unreliable packet" );

    rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 100, 0x53 ) != 0 );
    gx_expect( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == 0,
               "a message queued after a budget drop was planned in the same pass, against the bucket the drop found empty" );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2743
// mutation gap: hunt 1/2748
//
// A large sequenced state the bucket cannot pay is dropped at its plan: the
// bucket records the drop (unreliable_dropped and its bytes, as
// rocketnet_send_budget_admit's drop does) and the class order reaches the
// unreliable rank (RNSendBudget rank_reached).
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_large_sequenced_budget_drop_is_recorded_in_the_bucket()
{
    gx_t x;
    gx_create( &x, 64 );
    rn_endpoint_t * endpoint = &x.endpoint;

    const int large_bytes = ROCKETNET_MAX_PACKET_PAYLOAD_BYTES - 11;   // a code 1 record whose RECORDS payload is one whole packet
    rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_UNRELIABLE_SEQUENCED, 0, large_bytes, 0x5a ) != 0 );
    rocketnet_unit_check( endpoint->unreliable_count == 1 );

    endpoint->budget.tokens = 100.0;

    rn_endpoint_begin_update( endpoint );
    rn_endpoint_begin_flush( endpoint );
    const uint64_t dropped = endpoint->budget.unreliable_dropped;
    const uint64_t dropped_bytes = endpoint->budget.unreliable_dropped_bytes;
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == 0 );
    rocketnet_unit_check( gx_counter( &x, ROCKETNET_COUNTER_MESSAGE_UNRELIABLE_OVER_BUDGET ) == 1 );
    rocketnet_unit_check( endpoint->unreliable_count == 0 );

    gx_expect( endpoint->budget.unreliable_dropped == dropped + 1, "the bucket does not record the large sequenced drop (unreliable_dropped)" );
    gx_expect( endpoint->budget.unreliable_dropped_bytes == dropped_bytes + (uint64_t) rocketnet_send_budget_max_wire_bytes( ROCKETNET_MAX_PACKET_PAYLOAD_BYTES ),
               "the bucket does not record the large sequenced drop's bytes" );
    gx_expect( endpoint->budget.rank_reached == rocketnet_send_budget_class_rank( ROCKETNET_SEND_CLASS_UNRELIABLE ),
               "the large sequenced plan did not move the class order to the unreliable rank" );

    gx_destroy( &x );
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2798
// mutation gap: hunt 1/2803
//
// rn_endpoint_write_may_be_refused: an uncapped connection never says its
// budget may refuse (it holds no bucket), and a capped one holding exactly
// one maximum packet's charge admits any one packet, so it says no.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_write_may_be_refused_edges()
{
    static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];

    {
        gx_t x;
        gx_create( &x, 0 );
        rn_endpoint_t * endpoint = &x.endpoint;
        rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_RELIABLE, 0, 300, 0x61 ) != 0 );
        rn_endpoint_begin_update( endpoint );
        rn_endpoint_begin_flush( endpoint );
        rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        gx_expect( !rn_endpoint_write_may_be_refused( endpoint ), "an uncapped connection says its (absent) budget may refuse the planned packet" );
        const int written = rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 1, GX_NOW );
        rocketnet_unit_check( written > 0 );
        rn_endpoint_packet_sent( endpoint, 1, gx_wire( written ), GX_NOW );
        gx_destroy( &x );
    }

    {
        gx_t x;
        gx_create( &x, 100000 );
        rn_endpoint_t * endpoint = &x.endpoint;
        rocketnet_unit_check( gx_send( &x, ROCKETNET_RELIABILITY_RELIABLE, 0, 300, 0x62 ) != 0 );
        rn_endpoint_begin_update( endpoint );
        rn_endpoint_begin_flush( endpoint );
        rocketnet_unit_check( rn_endpoint_next_packet_bytes( endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
        endpoint->budget.tokens = (double) ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES;
        rocketnet_unit_check( !endpoint->budget.reliable_waiting );
        gx_expect( !rn_endpoint_write_may_be_refused( endpoint ),
                   "a bucket holding exactly one maximum packet's charge says it may refuse a packet" );
        endpoint->budget.tokens = (double) ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES - 1.0;
        rocketnet_unit_check( rn_endpoint_write_may_be_refused( endpoint ) );
        endpoint->budget.tokens = (double) ROCKETNET_SEND_BUDGET_MAX_PACKET_WIRE_BYTES;
        const int written = rn_endpoint_write_packet( endpoint, frame, (int) sizeof( frame ), 1, GX_NOW );
        rocketnet_unit_check( written > 0 );
        rn_endpoint_packet_sent( endpoint, 1, gx_wire( written ), GX_NOW );
        gx_destroy( &x );
    }
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2922
// mutation gap: hunt 1/2925
// mutation gap: hunt 1/2940
//
// The runtime cap's refusals change nothing: no endpoint, a value above the
// maximum (not offered to the bucket, which would count a bad-argument
// refusal), and a cap whose bucket cannot start at an invalid time (the
// connection stays uncapped).
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_set_send_kbps_refusals_change_nothing()
{
    gx_expect( !rn_endpoint_set_send_kbps( NULL, 100, GX_NOW ), "set_send_kbps with no endpoint did not refuse" );

    {
        gx_t x;
        gx_create( &x, 64 );
        rn_endpoint_t * endpoint = &x.endpoint;
        const uint64_t refusals = endpoint->budget.refusals;
        gx_expect( !rn_endpoint_set_send_kbps( endpoint, 0x7fffffff, GX_NOW ), "a cap above ROCKETNET_SEND_BUDGET_MAX_KBPS was taken" );
        gx_expect( endpoint->budget.refusals == refusals, "a cap above the maximum reached the bucket and counted a refusal there" );
        gx_expect( endpoint->budget.kbps == 64 && endpoint->send_budget_enabled == 1, "a cap above the maximum changed the connection's cap" );
        gx_destroy( &x );
    }

    {
        gx_t x;
        gx_create( &x, 0 );
        rn_endpoint_t * endpoint = &x.endpoint;
        gx_expect( !rn_endpoint_set_send_kbps( endpoint, 100, 2.0e300 ), "a cap whose bucket could not start (invalid time) was reported taken" );
        gx_expect( endpoint->send_budget_enabled == 0, "a cap whose bucket could not start capped the connection over a zeroed bucket" );
        gx_expect( rn_endpoint_in_place_affordable( endpoint, 100, GX_NOW ), "the still-uncapped connection cannot pay an in-place packet" );
        gx_destroy( &x );
    }
}

// ---------------------------------------------------------------------------
// mutation gap: hunt 1/2633
//
// rn_endpoint_on_payload copies at most out_capacity deliveries into out: a
// payload delivering three messages into an array of two leaves the element
// after it untouched.
// ---------------------------------------------------------------------------

static void test_gaps_r2_ep_on_payload_copies_at_most_out_capacity()
{
    gx_t sender;
    gx_create( &sender, 0 );
    gx_t receiver;
    gx_create( &receiver, 0 );

    for ( int i = 0; i < 3; i++ )
    {
        rocketnet_unit_check( gx_send( &sender, ROCKETNET_RELIABILITY_UNRELIABLE, 0, 50, (uint8_t) ( 0x70 + i ) ) != 0 );
    }

    static uint8_t frame[ROCKETNET_MAX_PACKET_PAYLOAD_BYTES];
    rn_endpoint_begin_update( &sender.endpoint );
    rn_endpoint_begin_flush( &sender.endpoint );
    rocketnet_unit_check( rn_endpoint_next_packet_bytes( &sender.endpoint, GX_NOW ) == ROCKETNET_MAX_PACKET_PAYLOAD_BYTES );
    const int written = rn_endpoint_write_packet( &sender.endpoint, frame, (int) sizeof( frame ), 1, GX_NOW );
    rocketnet_unit_check( written > 0 );
    rocketnet_unit_check( gx_count_records( &sender, frame, written ) == 3 );
    rn_endpoint_packet_sent( &sender.endpoint, 1, gx_wire( written ), GX_NOW );

    rn_delivery_t out[3];
    memset( out, 0xA5, sizeof( out ) );
    rn_delivery_t guard;
    memset( &guard, 0xA5, sizeof( guard ) );

    rn_endpoint_begin_update( &receiver.endpoint );
    int count = -1;
    rocketnet_unit_check( rn_endpoint_on_payload( &receiver.endpoint, frame, written, out, 2, &count, GX_NOW ) );
    gx_expect( memcmp( &out[2], &guard, sizeof( guard ) ) == 0, "on_payload copied a delivery past out_capacity" );
    gx_expect( count == 2, "on_payload reported more copies than out_capacity" );
    rocketnet_unit_check( out[0].bytes == 50 && out[1].bytes == 50 );

    gx_destroy( &receiver );
    gx_destroy( &sender );
}

REGISTER_UNIT_TEST( test_gaps_r2_ep_reset_after_runtime_cap_is_uncapped_with_an_empty_ring );
REGISTER_UNIT_TEST( test_gaps_r2_ep_second_start_logs_the_pending_start );
REGISTER_UNIT_TEST( test_gaps_r2_ep_in_place_header_and_start_on_a_disabled_type_or_exhausted_region );
REGISTER_UNIT_TEST( test_gaps_r2_ep_send_serial_wraps_to_one_and_refusals_return_zero );
REGISTER_UNIT_TEST( test_gaps_r2_ep_in_place_budget_at_finish );
REGISTER_UNIT_TEST( test_gaps_r2_ep_in_place_writer_refusal_is_a_failed_send );
REGISTER_UNIT_TEST( test_gaps_r2_ep_in_place_sequenced_record_carries_its_ordering );
REGISTER_UNIT_TEST( test_gaps_r2_ep_write_pass_guards );
REGISTER_UNIT_TEST( test_gaps_r2_ep_unreliable_plan_without_a_frame_ends_the_pass );
REGISTER_UNIT_TEST( test_gaps_r2_ep_failed_unreliable_packet_loses_exactly_its_receipts );
REGISTER_UNIT_TEST( test_gaps_r2_ep_unreliable_budget_drop_ends_the_pass );
REGISTER_UNIT_TEST( test_gaps_r2_ep_large_sequenced_budget_drop_is_recorded_in_the_bucket );
REGISTER_UNIT_TEST( test_gaps_r2_ep_write_may_be_refused_edges );
REGISTER_UNIT_TEST( test_gaps_r2_ep_set_send_kbps_refusals_change_nothing );
REGISTER_UNIT_TEST( test_gaps_r2_ep_on_payload_copies_at_most_out_capacity );
