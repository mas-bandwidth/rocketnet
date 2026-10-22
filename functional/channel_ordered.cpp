/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    Functional checks for reliable ordered delivery of chunked messages.
*/

#include "channel_harness.h"
#include "../tests/portable_crt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rocketnet_ordered_check_handler( const char * condition, const char * function, const char * file, int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
    fflush( stdout );
    exit( 1 );
}

#define rocketnet_ordered_check( condition )                                                   \
do                                                                                             \
{                                                                                              \
    if ( !(condition) )                                                                        \
    {                                                                                          \
        rocketnet_ordered_check_handler( #condition, (const char*) __FUNCTION__,               \
                                         (const char*) __FILE__, __LINE__ );                    \
    }                                                                                          \
} while( 0 )

static const int rocketnet_ordered_message_count = 64;
static const double rocketnet_ordered_step = 1.0 / 60.0;
static const double rocketnet_ordered_timeout = 180.0;
// Completion must fit within this multiple of the budget lower bound
// (payload bits over the sender's kbps). The multiple is measured, not
// derived: the lower bound counts only the send budget, and a completed run
// also pays the two-message pipeline, the 50 ms link, the resend of every lost
// chunk and head-of-line waits, so the multiple sits well above 1 even on a
// clean link. Over 10% loss and seeds 1-2000 of scripts/channel-seed-sweep
// (test_ordered_256k_chunked_stream_over_lossy_reordering_link, the larger factor)
// the completed runs have a median of 13.5 and a maximum of 17.67; the
// multiple is that maximum plus a margin of about 13%. The earlier 15.0 came
// from three runs on the pinned seed and 6% of seeds exceeded it. A run that
// does not complete inside rocketnet_ordered_timeout is "incomplete" (a
// reliable timeout), a different failure than this bound. Re-measure with
// scripts/channel-seed-sweep --seeds 1-2000 when the resend rule changes.
static const double rocketnet_ordered_multiple = 20.0;

static int rocketnet_ordered_size( int index )
{
    static const int sizes[8] = { 16, 1000, 65536, 128, 131072, 512, 262144, 64 };
    return sizes[index % 8];
}

static uint8_t rocketnet_ordered_byte( int message, int offset )
{
    uint32_t x = (uint32_t) message * 0x9E3779B9u + (uint32_t) offset * 0x85EBCA6Bu + 0xC2B2AE35u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    return (uint8_t) ( x >> 24 );
}

static void rocketnet_ordered_fill( uint8_t * data, int message, int bytes )
{
    for ( int i = 0; i < bytes; i++ )
        data[i] = rocketnet_ordered_byte( message, i );
}

static float rocketnet_ordered_loss_percent( void )
{
    char value[256];
    if ( rocketnet_test_getenv( "ROCKETNET_CHANNEL_LOSS_PERCENT", value, sizeof( value ) ) && value[0] )
        return (float) atof( value );
    return 10.0f;
}

// ROCKETNET_CHANNEL_METRICS set: print the measurement of a green run too
static bool rocketnet_ordered_metrics( void )
{
    char value[256];
    return rocketnet_test_getenv( "ROCKETNET_CHANNEL_METRICS", value, sizeof( value ) ) != 0;
}

struct rocketnet_ordered_measurement_t
{
    double completion_seconds;
    double lower_bound_seconds;
    double latency_seconds[64];
    uint64_t payload_bytes;
    uint64_t resends;
    int64_t wire_to_server;
    int64_t wire_to_client;
    int delivered;
};

static void rocketnet_ordered_print_measurement( const char * name,
                                                  const rocketnet_ordered_measurement_t * m,
                                                  int client_to_server,
                                                  const char * reason )
{
    printf( "%s: %s delivered=%d/%d completion_ms=%.1f resends=%llu wire_to_server=%lld wire_to_client=%lld payload_bytes=%llu budget_min_ms=%.1f factor=%.3f\n",
            name, reason, m->delivered, rocketnet_ordered_message_count,
            m->completion_seconds * 1000.0, (unsigned long long) m->resends,
            (long long) m->wire_to_server, (long long) m->wire_to_client,
            (unsigned long long) m->payload_bytes, m->lower_bound_seconds * 1000.0,
            m->lower_bound_seconds > 0.0 ? m->completion_seconds / m->lower_bound_seconds : 0.0 );

    if ( rocketnet_ordered_metrics() )
    {
        int bins[8] = { 0 };
        const double limits[7] = { 0.050, 0.100, 0.250, 0.500, 1.0, 2.0, 5.0 };
        for ( int i = 0; i < m->delivered; i++ )
        {
            int bin = 0;
            while ( bin < 7 && m->latency_seconds[i] >= limits[bin] )
                bin++;
            bins[bin]++;
        }
        printf( "latency_histogram_ms<50:%d,50-100:%d,100-250:%d,250-500:%d,500-1000:%d,1000-2000:%d,2000-5000:%d,>=5000:%d direction=%s\n",
                bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6], bins[7],
                client_to_server ? "client_to_server" : "server_to_client" );
    }
}

// TLA RNChannel.Enqueue: each accepted send advances one channel ordering index.
static uint32_t rocketnet_ordered_send( rocketnet_channel_harness_t * h,
                                        int client_to_server,
                                        const uint8_t * data,
                                        int bytes )
{
    if ( client_to_server )
        return rocketnet_channel_harness_client_send( h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, data, bytes );
    return rocketnet_channel_harness_server_send( h, 0, ROCKETNET_RELIABILITY_RELIABLE_ORDERED, data, bytes );
}

// A maximum-size send needs the full receive/send span ring; wait for earlier
// chunked sends to be transport-ACKed before requesting that single span.
static int rocketnet_ordered_span_ring_empty( const rocketnet_channel_harness_t * h, int client_to_server )
{
    const rn_message_object_t * sender = client_to_server ? &h->message_client->object : &h->message_server->object;
    const int connection = client_to_server ? 0 : h->client_slot;
    return sender->connections[connection].region->span_first == RN_DESCRIPTOR_NONE;
}

// TLA RNChannel.DeliverMessage: a complete message is visible only at its channel's next index.
static int rocketnet_ordered_collect( rocketnet_channel_harness_t * h,
                                      int client_to_server,
                                      rocketnet_channel_delivery_t * delivery )
{
    if ( client_to_server )
        return rocketnet_channel_harness_server_collect( h, 0, delivery );
    return rocketnet_channel_harness_client_collect( h, 0, delivery );
}

static int rocketnet_ordered_run( const char * name, int client_to_server )
{
    rocketnet_channel_harness_t h;
    if ( !rocketnet_channel_harness_create( &h ) )
    {
        rocketnet_ordered_check( 0 );
        return 0;
    }

    rocketnet_ordered_check( rocketnet_channel_harness_set_budgets( &h, 1000, 10000 ) );
    if ( !rocketnet_channel_harness_connect( &h, 60 * 10 ) )
    {
        rocketnet_ordered_check( 0 );
        rocketnet_channel_harness_destroy( &h );
        return 0;
    }

    // A 50 ms base delay plus [0, 50 ms] jitter permits at most 50 ms skew.
    rocketnet_channel_harness_set_link( &h, 0.050, 0.050, rocketnet_ordered_loss_percent(), 5.0f );

    uint8_t payload[262144];
    double send_time[64] = { 0 };
    rocketnet_ordered_measurement_t measurement;
    memset( &measurement, 0, sizeof(measurement) );
    for ( int i = 0; i < rocketnet_ordered_message_count; i++ )
        measurement.payload_bytes += (uint64_t) rocketnet_ordered_size( i );

    const uint32_t send_kbps = client_to_server ? 1000 : 10000;
    measurement.lower_bound_seconds = (double) measurement.payload_bytes * 8.0 / ( (double) send_kbps * 1000.0 );
    const double start_time = h.now;
    int next_to_send = 0;
    int updates = 0;

    while ( measurement.delivered < rocketnet_ordered_message_count &&
            h.now - start_time < rocketnet_ordered_timeout && updates < 60 * 190 )
    {
        // Keep a small fixed pipeline so the test measures the transport's
        // budget and head-of-line progress without exhausting its send arena.
        while ( next_to_send < rocketnet_ordered_message_count &&
                next_to_send < measurement.delivered + 2 )
        {
            const int bytes = rocketnet_ordered_size( next_to_send );
            if ( bytes >= RN_SPAN_MIN_BYTES && !rocketnet_ordered_span_ring_empty( &h, client_to_server ) )
                break;
            rocketnet_ordered_fill( payload, next_to_send, bytes );
            const uint32_t serial = rocketnet_ordered_send( &h, client_to_server, payload, bytes );
            if ( serial == 0 )
            {
                break;
            }
            send_time[next_to_send] = h.now;
            next_to_send++;
        }

        rocketnet_channel_harness_pump( &h, rocketnet_ordered_step );
        updates++;

        rocketnet_channel_delivery_t delivery;
        while ( rocketnet_ordered_collect( &h, client_to_server, &delivery ) )
        {
            const int expected_index = measurement.delivered;
            if ( expected_index >= rocketnet_ordered_message_count )
                break;
            const int expected_bytes = rocketnet_ordered_size( expected_index );
            rocketnet_ordered_fill( payload, expected_index, expected_bytes );
            const uint64_t expected_hash = rocketnet_channel_hash( payload, expected_bytes );
            const int exact = delivery.channel == 0 && delivery.bytes == expected_bytes &&
                              delivery.hash == expected_hash &&
                              memcmp( delivery.data, payload, (size_t) expected_bytes ) == 0;
            if ( !exact )
            {
                measurement.completion_seconds = h.now - start_time;
                measurement.resends = h.message_client->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT] +
                                      h.message_server->object.counters.values[ROCKETNET_COUNTER_MESSAGE_RESENT];
                measurement.wire_to_server = h.wire_bytes_to_server;
                measurement.wire_to_client = h.wire_bytes_to_client;
                rocketnet_ordered_print_measurement( name, &measurement, client_to_server, "byte_or_order_mismatch" );
                rocketnet_ordered_check( exact );
            }
            measurement.latency_seconds[expected_index] = h.now - send_time[expected_index];
            measurement.delivered++;
        }

        if ( next_to_send == rocketnet_ordered_message_count && measurement.delivered == rocketnet_ordered_message_count )
            break;
    }

    measurement.completion_seconds = h.now - start_time;
    rocketnet_channel_harness_read_counters( &h );
    measurement.resends = h.resends;
    measurement.wire_to_server = h.wire_bytes_to_server;
    measurement.wire_to_client = h.wire_bytes_to_client;
    const int complete = next_to_send == rocketnet_ordered_message_count &&
                         measurement.delivered == rocketnet_ordered_message_count;
    const int within_budget = measurement.completion_seconds <= rocketnet_ordered_multiple * measurement.lower_bound_seconds;
    const int no_overflow = rocketnet_channel_harness_overflows( &h ) == 0;
    const int no_allocations = rocketnet_channel_allocator_calls == h.allocations_after_create;

    if ( !complete || !within_budget || !no_overflow || !no_allocations )
    {
        printf( "seed=0x%016llx\n", (unsigned long long) h.seed );
        const char * reason = !complete ? "incomplete" : !within_budget ? "budget_multiple" :
                             !no_overflow ? "harness_overflow" : "allocation_after_create";
        const rn_message_object_t * sender = client_to_server ? &h.message_client->object : &h.message_server->object;
        const int connection = client_to_server ? 0 : h.client_slot;
        printf( "ordered_state next_to_send=%d sender_state=%d sender_disconnect_reason=%d memory_exhausted=%llu send_store_full=%llu\n",
                next_to_send, sender->hot_connections[connection].state, sender->connections[connection].disconnect_reason,
                (unsigned long long) sender->counters.values[ROCKETNET_COUNTER_MESSAGE_MEMORY_EXHAUSTED],
                (unsigned long long) sender->counters.values[ROCKETNET_COUNTER_MESSAGE_SEND_STORE_FULL] );
        rocketnet_ordered_print_measurement( name, &measurement, client_to_server, reason );
    }
    else if ( rocketnet_ordered_metrics() )
    {
        rocketnet_ordered_print_measurement( name, &measurement, client_to_server, "green_measurement" );
    }

    rocketnet_ordered_check( complete );
    rocketnet_ordered_check( within_budget );
    rocketnet_ordered_check( no_overflow );
    rocketnet_ordered_check( no_allocations );
    rocketnet_channel_harness_destroy( &h );
    return 1;
}

void ordered_256k_chunked_stream_over_lossy_reordering_link()
{
    rocketnet_ordered_run( "test_ordered_256k_chunked_stream_over_lossy_reordering_link", 0 );
}

void ordered_small_and_large_interleaved()
{
    rocketnet_ordered_run( "test_ordered_small_and_large_interleaved", 1 );
}
