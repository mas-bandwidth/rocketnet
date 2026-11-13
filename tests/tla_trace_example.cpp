/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The example layer of the TLA+ trace check (proofs/tla/README-TraceCheck.md):
// the smallest whole layer, to copy. Its code is a bounded claim counter, the
// model RNTraceExample (proofs/tla/RNTraceExample.tla): a claim takes one of
// EXAMPLE_LIMIT places or is refused and counted, a drain frees them all.
// The walk drives the code one call per step and writes one record of every
// model variable after each; proofs/tla/TraceRNTraceExample.tla checks the
// trace.
//
// A real layer's code lives in sdk/. Its file here includes that .cpp under
// ROCKETNET_TLA_PLANT with the layer's plant define set, so tla_trace_plant
// carries the planted code in place of the library's:
//
//   #if defined( ROCKETNET_TLA_PLANT )
//   #define ROCKETNET_X_PLANT 1
//   #include "../sdk/rocketnet_x.cpp"
//   #endif // #if defined( ROCKETNET_TLA_PLANT )
//
// This example's code is in this file, so its plant is here too: under
// ROCKETNET_TLA_PLANT the claim's check is off by one and grants the place
// past the limit. The walk prints PLANT-FIRED n, the claims granted at the
// limit: tools/tla_trace_check.py requires n > 0 from the plant build and,
// for this layer, n = 0 from the clean one.

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

// RNTraceExample Claim and Refuse
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

static tla_record_t example_record( const example_counter_t & counter )
{
    tla_record_t record;
    record.add( "count", tla_int( counter.count ) );
    record.add( "refused", tla_int( counter.refused ) );
    return record;
}

static int trace_example( uint64_t seed, int steps, const char * out )
{
    example_counter_t counter = { 0, 0 };
    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    int fired = 0;

    records.push_back( example_record( counter ) );
    for ( int step = 0; step < steps; step++ )
    {
        if ( random.below( 4 ) == 0 )
        {
            example_drain( &counter );
        }
        else
        {
            const bool at_limit = counter.count >= EXAMPLE_LIMIT;
            if ( example_claim( &counter ) && at_limit )
            {
                fired++;
            }
        }
        records.push_back( example_record( counter ) );
    }

    printf( "PLANT-FIRED %d\n", fired );

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
