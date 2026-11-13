/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The example layer of the TLA+ trace check (proofs/tla/README-TraceCheck.md):
// the smallest whole layer, to copy. Its code is a bounded claim counter, the
// model RNTraceExample (proofs/tla/RNTraceExample.tla): a claim takes one of
// EXAMPLE_LIMIT places or is refused and counted, a drain frees them all.
//
// The walk calls the code one operation per step and writes one record per
// step: the operation it called ("claim", "drain"), the model action the
// code's own result says it took ("Claim" or "Refuse" for a claim, "Drain"),
// and every model variable after it (tla_step). TraceRNTraceExample.tla
// allows only that action, and only when the operation can be it, so a claim
// whose code silently did nothing is rejected even though a Drain from the
// same state could look alike.
//
// A real layer's code lives in sdk/. Its file here includes that .cpp under
// ROCKETNET_TLA_PLANT with the layer's plant define set, so the layer's own
// plant binary, tla_trace_plant_<layer>, carries the planted code in place
// of the library's:
//
//   #if defined( ROCKETNET_TLA_PLANT )
//   #define ROCKETNET_X_PLANT 1
//   #include "../sdk/rocketnet_x.cpp"
//   #endif // #if defined( ROCKETNET_TLA_PLANT )
//
// This example's code is in this file, so its plant is here too: under
// ROCKETNET_TLA_PLANT the claim's check is off by one and grants the place
// past the limit. The walk reports (tla_report_t) the planted EFFECT, a claim
// that leaves more than EXAMPLE_LIMIT places taken, at the record where it
// happened: the clean code's count must be 0, the plant's must not, and TLC
// must reject the planted trace at one of those records.

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

#define EXAMPLE_LIMIT 3

struct example_counter_t
{
    int count;
    int refused;
};

// RNTraceExample Claim, or Refuse when it returns false
static bool example_claim( example_counter_t * counter )
{
#if defined( ROCKETNET_TLA_PLANT )
    if ( counter->count > EXAMPLE_LIMIT )
#else  // #if defined( ROCKETNET_TLA_PLANT )
    if ( counter->count >= EXAMPLE_LIMIT )
#endif // #if defined( ROCKETNET_TLA_PLANT )
    {
        counter->refused++;
        return false;
    }
    counter->count++;
    return true;
}

// RNTraceExample Drain
static void example_drain( example_counter_t * counter )
{
    counter->count = 0;
}

static tla_record_t example_state( const example_counter_t & counter )
{
    tla_record_t state;
    state.add( "count", tla_int( counter.count ) );
    state.add( "refused", tla_int( counter.refused ) );
    return state;
}

static int trace_example( uint64_t seed, int steps, const char * out )
{
    example_counter_t counter = { 0, 0 };
    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", example_state( counter ) ) );
    for ( int step = 0; step < steps; step++ )
    {
        if ( random.below( 4 ) == 0 )
        {
            example_drain( &counter );
            report.act( "Drain" );
            records.push_back( tla_step( "drain", "Drain", example_state( counter ) ) );
        }
        else
        {
            const char * act = example_claim( &counter ) ? "Claim" : "Refuse";
            report.act( act );
            records.push_back( tla_step( "claim", act, example_state( counter ) ) );
            if ( counter.count > EXAMPLE_LIMIT )
            {
                report.planted( (int) records.size() );
            }
        }
    }

    report.print();

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceExamplePlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceExampleSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    char constants[64];
    snprintf( constants, sizeof( constants ), "    Limit = %d\n", EXAMPLE_LIMIT );
    return tla_write_trace( out, name, "TraceRNTraceExample", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "example", trace_example );
