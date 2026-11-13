/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The example layer of the TLA+ trace check (proofs/tla/README-TraceCheck.md):
// the smallest whole layer, to copy. Its code is a bounded claim counter, the
// model RNTraceExample (proofs/tla/RNTraceExample.tla): a claim of k places
// takes them when k of EXAMPLE_LIMIT are free or is refused and counted, a
// drain frees them all.
//
// The walk calls the code one operation per step and writes one record per
// step: the operation it called ("claim", "drain"), its arguments and what it
// returned (k for a claim, the places a drain freed), the model action the code's own return value says it took ("Claim" or
// "Refuse" for a claim, "Drain"), and every model variable after it
// (tla_step). proofs/tla/TraceRNTraceExample.tla
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
// ROCKETNET_TLA_PLANT the claim's check is off by one and grants a claim
// one place past the limit. The walk reports (tla_report_t) the planted EFFECT, a claim
// that leaves more than EXAMPLE_LIMIT places taken, at the record where it
// happened: the clean code's count must be 0, the plant's must not, and TLC
// must reject the planted trace at one of those records.

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

#define EXAMPLE_LIMIT 3
#define EXAMPLE_MAX_CLAIM 2

struct example_counter_t
{
    int count;
    int refused;
};

// RNTraceExample Claim(k), or Refuse(k) when it returns false
static bool example_claim( example_counter_t * counter, int k )
{
#if defined( ROCKETNET_TLA_PLANT )
    if ( counter->count + k > EXAMPLE_LIMIT + 1 )
#else  // #if defined( ROCKETNET_TLA_PLANT )
    if ( counter->count + k > EXAMPLE_LIMIT )
#endif // #if defined( ROCKETNET_TLA_PLANT )
    {
        counter->refused++;
        return false;
    }
    counter->count += k;
    return true;
}

// RNTraceExample Drain(n): returns n, the places it freed
static int example_drain( example_counter_t * counter )
{
    const int freed = counter->count;
    counter->count = 0;
    return freed;
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

    records.push_back( tla_step( "init", "init", "<<>>", example_state( counter ) ) );
    for ( int step = 0; step < steps; step++ )
    {
        if ( random.below( 4 ) == 0 )
        {
            // what the call returns is an argument of its action, so TLC checks it too
            const int freed = example_drain( &counter );
            report.act( "Drain" );
            records.push_back( tla_step( "drain", "Drain", tla_sequence( std::vector<std::string>( 1, tla_int( freed ) ) ), example_state( counter ) ) );
        }
        else
        {
            const int k = 1 + random.below( EXAMPLE_MAX_CLAIM );
            // the action is the call's return value, never the state's change
            const char * act = example_claim( &counter, k ) ? "Claim" : "Refuse";
            report.act( act );
            records.push_back( tla_step( "claim", act, tla_sequence( std::vector<std::string>( 1, tla_int( k ) ) ), example_state( counter ) ) );
            // the planted effect, where it shows: more places taken than exist
            if ( counter.count > EXAMPLE_LIMIT )
            {
                report.planted( (int) records.size() );
            }
        }
    }

    report.print();

    char constants[96];
    snprintf( constants, sizeof( constants ), "    Limit = %d\n    MaxClaim = %d\n", EXAMPLE_LIMIT, EXAMPLE_MAX_CLAIM );
    return tla_write_trace( out, "MCTraceExample", seed, "TraceRNTraceExample", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "example", trace_example );
