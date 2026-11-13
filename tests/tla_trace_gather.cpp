/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace walk of the send gather (X2, sdk/rocketnet_send_gather.cpp),
// checked by proofs/tla/TraceRNSendGatherCall.tla (proofs/tla/README-TraceCheck.md)
// against proofs/tla/RNSendGatherCall.tla, the gather as its caller sees it:
// MCRNSendGatherLayout.cfg checks that module's results are the layout
// model's. The walk stages a bank of GATHER_SLOTS slots one slot per step,
// each to one of three destinations at one of three sizes (0 is an aborted
// slot), then hands the bank to the code; one record per step (tla_step):
// a stage is Stage(s, d, b), a call is Gather(o, r) with the order the code
// wrote and the runs it returned, numbered from 1 as the model numbers them.
//
// The plant binary's code is the gather with ROCKETNET_SEND_GATHER_PLANT
// (the placement last to first). The walk reports (tla_report_t) the
// planted effect where it shows: an order that puts a destination's slot
// before an earlier-staged slot of the same destination, which the clean
// code never writes.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_SEND_GATHER_PLANT 1
#include "../sdk/rocketnet_send_gather.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_send_gather.h"

#include <stdlib.h>
#include <string.h>

#include "tla_trace_record.h"

#define GATHER_SLOTS 6
#define GATHER_DESTS 3
#define GATHER_MAX_SEGMENTS 3
#define GATHER_MAX_BYTES 3000

static const int gather_sizes[] = { 0, 600, 1200 };

struct gather_slot_t
{
    struct rocketnet_address_t to;
    int packet_bytes;
};

// the bank as the model's variables: dest 0 is a slot not yet staged
static tla_record_t gather_state( const int * dest_of, const gather_slot_t * bank )
{
    std::vector<std::string> slot_keys;
    std::vector<std::string> dest_values;
    std::vector<std::string> size_values;
    for ( int s = 0; s < GATHER_SLOTS; s++ )
    {
        slot_keys.push_back( tla_int( s ) );
        dest_values.push_back( tla_int( dest_of[s] ) );
        size_values.push_back( tla_int( bank[s].packet_bytes ) );
    }
    tla_record_t state;
    state.add( "dest", tla_function( slot_keys, dest_values ) );
    state.add( "size", tla_function( slot_keys, size_values ) );
    return state;
}

static int trace_gather( uint64_t seed, int steps, const char * out )
{
    struct rocketnet_address_t dests[GATHER_DESTS + 1];
    memset( dests, 0, sizeof( dests ) );
    for ( int d = 1; d <= GATHER_DESTS; d++ )
    {
        dests[d].type = ROCKETNET_ADDRESS_IPV4;
        dests[d].data.ipv4[0] = 10;
        dests[d].data.ipv4[3] = (uint8_t) d;
        dests[d].port = (uint16_t) ( 40000 + d );
    }

    struct rocketnet_send_gather_scratch_t scratch;
    std::vector<int> memory( rocketnet_send_gather_scratch_bytes( GATHER_SLOTS ) / sizeof( int ) + 1 );
    if ( !rocketnet_send_gather_scratch_init( &scratch, memory.data(), GATHER_SLOTS ) )
    {
        return 1;
    }

    gather_slot_t bank[GATHER_SLOTS];
    int dest_of[GATHER_SLOTS];
    memset( bank, 0, sizeof( bank ) );
    for ( int s = 0; s < GATHER_SLOTS; s++ )
    {
        dest_of[s] = 0;
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", gather_state( dest_of, bank ) ) );

    int next_slot = 0;
    for ( int step = 0; step < steps; step++ )
    {
        if ( next_slot < GATHER_SLOTS )
        {
            // the caller writes the next slot of the bank
            const int s = next_slot++;
            dest_of[s] = 1 + random.below( GATHER_DESTS );
            bank[s].to = dests[dest_of[s]];
            bank[s].packet_bytes = gather_sizes[random.below( (int) ( sizeof( gather_sizes ) / sizeof( gather_sizes[0] ) ) )];
            report.act( "Stage" );
            records.push_back( tla_step( "stage", "Stage", tla_sequence( { tla_int( s ), tla_int( dest_of[s] ), tla_int( bank[s].packet_bytes ) } ),
                                         gather_state( dest_of, bank ) ) );
            continue;
        }

        // the call, on the whole bank
        next_slot = 0;
        int order[GATHER_SLOTS];
        struct rocketnet_send_gather_run_t runs[GATHER_SLOTS];
        const int num_runs = rocketnet_send_gather( &bank[0].to, sizeof( gather_slot_t ), &bank[0].packet_bytes, sizeof( gather_slot_t ), GATHER_SLOTS,
                                                    GATHER_MAX_SEGMENTS, GATHER_MAX_BYTES, &scratch, order, runs );
        if ( num_runs < 0 || num_runs > GATHER_SLOTS )
        {
            fprintf( stderr, "tla_trace gather: the call returned %d runs\n", num_runs );
            return 1;
        }

        int num_order = 0;
        for ( int r = 0; r < num_runs; r++ )
        {
            num_order += runs[r].count;
        }
        if ( num_order > GATHER_SLOTS )
        {
            fprintf( stderr, "tla_trace gather: the runs hold %d packets of %d slots\n", num_order, GATHER_SLOTS );
            return 1;
        }

        std::vector<std::string> order_values;
        bool reordered = false;
        for ( int j = 0; j < num_order; j++ )
        {
            order_values.push_back( tla_int( order[j] ) );
            // the planted effect: a slot placed after a later-staged slot of its destination
            for ( int i = 0; i < j; i++ )
            {
                if ( dest_of[order[i]] == dest_of[order[j]] && order[i] > order[j] )
                {
                    reordered = true;
                }
            }
        }

        std::vector<std::string> run_values;
        for ( int r = 0; r < num_runs; r++ )
        {
            run_values.push_back( "[first |-> " + tla_int( runs[r].first + 1 ) + ", count |-> " + tla_int( runs[r].count ) + ", seg |-> " +
                                  tla_int( runs[r].segment_bytes ) + ", last |-> " + tla_int( runs[r].last_bytes ) + "]" );
        }

        report.act( "Gather" );
        records.push_back( tla_step( "gather", "Gather", tla_sequence( { tla_sequence( order_values ), tla_sequence( run_values ) } ),
                                     gather_state( dest_of, bank ) ) );
        if ( reordered )
        {
            report.planted( (int) records.size() );
        }
    }

    report.print();

    std::string dest_set;
    for ( int d = 1; d <= GATHER_DESTS; d++ )
    {
        dest_set += ( d > 1 ? ", " : "" ) + tla_int( d );
    }
    std::string size_set;
    for ( size_t i = 0; i < sizeof( gather_sizes ) / sizeof( gather_sizes[0] ); i++ )
    {
        size_set += ( i > 0 ? ", " : "" ) + tla_int( gather_sizes[i] );
    }

    char constants[512];
    snprintf( constants, sizeof( constants ), "    S = %d\n    Dests = {%s}\n    Sizes = {%s}\n    MaxSeg = %d\n    MaxBytes = %d\n", GATHER_SLOTS,
              dest_set.c_str(), size_set.c_str(), GATHER_MAX_SEGMENTS, GATHER_MAX_BYTES );

    return tla_write_trace( out, "MCTraceGather", seed, "TraceRNSendGatherCall", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "gather", trace_gather );
