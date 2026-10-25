/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    CBMC harness for rocketnet_ack_apply_run: neither skip nor double-count
    an acked range. The run is not proved. The body is the static leaf in
    sdk/rocketnet_ack.cpp.
    The endpoint re-declares the fields that leaf reads and writes.

    The run is symbolic in its start and its length. The length is at most
    ACK_RUN_BOUND and at most the retained window (4096), and the whole run
    sits inside that window. Each sequence in the run has a matching sent
    record and carries no packet id and no aggregate, so the batch never
    defers one. Under those conditions the returned count equals the number
    of records that were unacked, each of those records ends acked, and the
    marking walk visits each sequence once when any of them was unacked.

    The harness initializes exactly the sent records in the symbolic run.
    The leaf reads no other record under the run assumptions; the negative
    RTT sample and absent packet ids/aggregates bypass the other arrays.

    Flags: --bounds-check --pointer-check --signed-overflow-check
           --unsigned-overflow-check --div-by-zero-check --unwinding-assertions
    Unwinding: ACK_RUN_BOUND. Bound 8 and the complete window of 4096 are
    both unproved. A bound covers only runs of that length.
*/

#include <stdint.h>

#ifndef ACK_RUN_BOUND
#define ACK_RUN_BOUND 4096
#endif

#ifndef __CPROVER__
#define __CPROVER_assert( c, m ) ( (void) ( c ), (void) ( m ) )
#define __CPROVER_assume( c ) ( (void) ( c ) )
static uint64_t nondet_uint64( void ) { return 0; }
static unsigned char nondet_uchar( void ) { return 0; }
#else
uint64_t nondet_uint64( void );
unsigned char nondet_uchar( void );
#endif

#define ROCKETNET_SENT_PACKET_BUFFER_SIZE      4096
#define ROCKETNET_RTT_HISTORY_SIZE             2048
#define ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE ( ROCKETNET_SENT_PACKET_BUFFER_SIZE / 2 )

struct rocketnet_sent_packet_record_t
{
    uint64_t sequence;
    uint64_t packet_id;
    uint64_t aggregate_id;
    double send_time;
    uint32_t wire_bytes;
    uint8_t acked;
    uint8_t has_packet_id;
    uint8_t was_keep_alive;
    uint8_t pad;
};

struct rocketnet_packet_aggregate_t
{
    uint64_t id;
    uint64_t packet_id;
    uint32_t remaining;
    uint32_t pad;
};

struct rocketnet_ack_endpoint_t
{
    uint64_t highest_sequence;
    uint8_t keep_alive_acked;
    int has_rtt_sample;
    float rtt_history[ROCKETNET_RTT_HISTORY_SIZE];
    struct rocketnet_sent_packet_record_t sent[ROCKETNET_SENT_PACKET_BUFFER_SIZE];
    struct rocketnet_packet_aggregate_t aggregates[ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
};

unsigned long long g_ack_visit_count = 0;

static int rocketnet_ack_apply_run( struct rocketnet_ack_endpoint_t * endpoint,
                                    uint64_t low, uint64_t high, double time,
                                    uint64_t * acked_packet_ids, int acked_capacity, int * acked_count,
                                    uint64_t * unknown_sequence_counter,
                                    uint64_t * batch_full_counter )
{
    if ( high > endpoint->highest_sequence )
    {
        uint64_t above_low = low > endpoint->highest_sequence + 1 ? low : endpoint->highest_sequence + 1;
        *unknown_sequence_counter += high - above_low + 1;
        if ( low > endpoint->highest_sequence )
            return 0;
        high = endpoint->highest_sequence;
    }

    uint64_t window_low = 1;
    if ( endpoint->highest_sequence >= ROCKETNET_SENT_PACKET_BUFFER_SIZE )
        window_low = endpoint->highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;

    if ( high < window_low )
        return 0;
    if ( low < window_low )
        low = window_low;

    int all_acked = 1;
    for ( uint64_t seq = high; seq >= low; seq-- )
    {
        struct rocketnet_sent_packet_record_t * rec = &endpoint->sent[seq % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
        if ( rec->sequence == seq && !rec->acked )
        {
            all_acked = 0;
            break;
        }
    }
    if ( all_acked )
        return 0;

    int newly_acked = 0;

    for ( uint64_t sequence = high;; sequence-- )
    {
        g_ack_visit_count++;
        struct rocketnet_sent_packet_record_t * record = &endpoint->sent[sequence % ROCKETNET_SENT_PACKET_BUFFER_SIZE];

        if ( record->sequence != sequence )
        {
            ( *unknown_sequence_counter )++;
        }
        else if ( !record->acked )
        {
            if ( ( record->has_packet_id || record->aggregate_id != 0 ) && *acked_count >= acked_capacity )
            {
                ( *batch_full_counter )++;
            }
            else
            {
                record->acked = 1;
                newly_acked++;

                if ( record->was_keep_alive )
                    endpoint->keep_alive_acked = 1;

                double sample = time - record->send_time;
                if ( sample >= 0.0 )
                {
                    endpoint->rtt_history[sequence % ROCKETNET_RTT_HISTORY_SIZE] = (float) ( sample * 1000.0 );
                    endpoint->has_rtt_sample = 1;
                }

                if ( record->has_packet_id )
                {
                    acked_packet_ids[( *acked_count )++] = record->packet_id;
                }
                else if ( record->aggregate_id != 0 )
                {
                    struct rocketnet_packet_aggregate_t * aggregate = &endpoint->aggregates[record->aggregate_id % ROCKETNET_PACKET_AGGREGATE_BUFFER_SIZE];
                    if ( aggregate->id == record->aggregate_id && aggregate->remaining > 0 )
                    {
                        aggregate->remaining--;
                        if ( aggregate->remaining == 0 )
                            acked_packet_ids[( *acked_count )++] = aggregate->packet_id;
                    }
                }
            }
        }

        if ( sequence == low )
            break;
    }

    return newly_acked;
}

int main( void )
{
    struct rocketnet_ack_endpoint_t endpoint;
    uint64_t low;
    uint64_t high;
    uint64_t len;
    uint64_t window_low;
    uint64_t i;
    uint64_t n_unacked;
    uint64_t acked_packet_ids[1];
    int acked_count;
    uint64_t unknown_sequence_counter;
    uint64_t batch_full_counter;
    int newly_acked;

    endpoint.highest_sequence = nondet_uint64();
    low = nondet_uint64();
    high = nondet_uint64();

    __CPROVER_assume( ACK_RUN_BOUND >= 1 );
    __CPROVER_assume( ACK_RUN_BOUND <= ROCKETNET_SENT_PACKET_BUFFER_SIZE );
    __CPROVER_assume( low >= 1 );
    __CPROVER_assume( low <= high );
    __CPROVER_assume( high - low <= (uint64_t) ACK_RUN_BOUND - 1 );
    __CPROVER_assume( high <= endpoint.highest_sequence );

    window_low = 1;
    if ( endpoint.highest_sequence >= ROCKETNET_SENT_PACKET_BUFFER_SIZE )
        window_low = endpoint.highest_sequence - ROCKETNET_SENT_PACKET_BUFFER_SIZE + 1;
    __CPROVER_assume( low >= window_low );

    len = high - low + 1;

    for ( i = 0; i < len; i++ )
    {
        uint64_t seq = low + i;
        struct rocketnet_sent_packet_record_t * rec =
            &endpoint.sent[seq % ROCKETNET_SENT_PACKET_BUFFER_SIZE];
        rec->sequence = seq;
        rec->acked = nondet_uchar() & 1;
        rec->has_packet_id = 0;
        rec->aggregate_id = 0;
        rec->was_keep_alive = 0;
        rec->send_time = 0.0;
    }

    n_unacked = 0;
    for ( i = 0; i < len; i++ )
    {
        uint64_t seq = low + i;
        if ( endpoint.sent[seq % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked == 0 )
            n_unacked++;
    }

    acked_count = 0;
    unknown_sequence_counter = 0;
    batch_full_counter = 0;
    /* A negative sample skips the rtt store. The count does not depend on it. */
    newly_acked = rocketnet_ack_apply_run( &endpoint, low, high, -1.0,
                                           acked_packet_ids, 1, &acked_count,
                                           &unknown_sequence_counter,
                                           &batch_full_counter );

    __CPROVER_assert( (uint64_t) newly_acked == n_unacked,
                      "the run neither skips nor double-counts" );
    __CPROVER_assert( unknown_sequence_counter == 0,
                      "a sent in-window sequence is not skipped as unknown" );
    __CPROVER_assert( batch_full_counter == 0,
                      "the run does not defer a record" );

    for ( i = 0; i < len; i++ )
    {
        uint64_t seq = low + i;
        __CPROVER_assert( endpoint.sent[seq % ROCKETNET_SENT_PACKET_BUFFER_SIZE].acked == 1,
                          "every sequence in the run ends acked" );
    }

    if ( n_unacked == 0 )
        __CPROVER_assert( g_ack_visit_count == 0,
                          "an all-acked run does not enter the marking walk" );
    else
        __CPROVER_assert( g_ack_visit_count == len,
                          "the marking walk visits each sequence once" );

    return 0;
}
