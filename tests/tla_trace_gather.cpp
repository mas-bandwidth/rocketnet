/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace walk of the send gather (X2, sdk/rocketnet_send_gather.cpp),
// checked by proofs/tla/TraceRNSendGatherLayout.tla: seeded random banks of
// GATHER_SLOTS slots, each to one of three destinations at one of three
// sizes (0 is an aborted slot), handed to the code; each case records the
// bank and what the code returned. The model's table is the code's: the
// same bucket count and each destination's home bucket from
// rocketnet_send_gather_home.
// The walk prints PLANT-FIRED n, the cases with three or more packets to one
// destination: a property of the input, which only those cases can show
// against the plant (ROCKETNET_SEND_GATHER_PLANT, a chain appended after its
// head), so the clean walk prints it too.

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
#define GATHER_CASES 150
#define GATHER_MAX_SEGMENTS 3
#define GATHER_MAX_BYTES 3000

static const int gather_sizes[] = { 0, 600, 1200 };

struct gather_slot_t
{
    struct rocketnet_address_t to;
    int packet_bytes;
};

static int trace_gather( uint64_t seed, int steps, const char * out )
{
    (void) steps;

    struct rocketnet_address_t dests[GATHER_DESTS];
    for ( int d = 0; d < GATHER_DESTS; d++ )
    {
        memset( &dests[d], 0, sizeof( dests[d] ) );
        dests[d].type = ROCKETNET_ADDRESS_IPV4;
        dests[d].data.ipv4[0] = 10;
        dests[d].data.ipv4[3] = (uint8_t) ( 1 + d );
        dests[d].port = (uint16_t) ( 40000 + d );
    }

    struct rocketnet_send_gather_scratch_t scratch;
    std::vector<int> memory( rocketnet_send_gather_scratch_bytes( GATHER_SLOTS ) / sizeof( int ) + 1 );
    if ( !rocketnet_send_gather_scratch_init( &scratch, memory.data(), GATHER_SLOTS ) )
    {
        return 1;
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    int reorderable = 0;

    for ( int n = 0; n < GATHER_CASES; n++ )
    {
        gather_slot_t bank[GATHER_SLOTS];
        int dest_of[GATHER_SLOTS];
        int staged_to[GATHER_DESTS] = { 0, 0, 0 };
        for ( int s = 0; s < GATHER_SLOTS; s++ )
        {
            dest_of[s] = random.below( GATHER_DESTS );
            bank[s].to = dests[dest_of[s]];
            bank[s].packet_bytes = gather_sizes[random.below( (int) ( sizeof( gather_sizes ) / sizeof( gather_sizes[0] ) ) )];
            if ( bank[s].packet_bytes > 0 )
            {
                staged_to[dest_of[s]]++;
            }
        }
        for ( int d = 0; d < GATHER_DESTS; d++ )
        {
            if ( staged_to[d] >= 3 )
            {
                reorderable++;
                break;
            }
        }

        int order[GATHER_SLOTS];
        struct rocketnet_send_gather_run_t runs[GATHER_SLOTS];
        const int num_runs = rocketnet_send_gather( &bank[0].to, sizeof( gather_slot_t ), &bank[0].packet_bytes, sizeof( gather_slot_t ), GATHER_SLOTS,
                                                    GATHER_MAX_SEGMENTS, GATHER_MAX_BYTES, &scratch, order, runs );
        if ( num_runs < 0 )
        {
            return 1;
        }

        int num_order = 0;
        for ( int r = 0; r < num_runs; r++ )
        {
            num_order += runs[r].count;
        }

        std::vector<std::string> slot_keys;
        std::vector<std::string> dest_values;
        std::vector<std::string> size_values;
        for ( int s = 0; s < GATHER_SLOTS; s++ )
        {
            slot_keys.push_back( tla_int( s ) );
            dest_values.push_back( tla_int( 1 + dest_of[s] ) );
            size_values.push_back( tla_int( bank[s].packet_bytes ) );
        }

        std::string order_text = "<<";
        for ( int j = 0; j < num_order; j++ )
        {
            order_text += ( j > 0 ? ", " : "" ) + tla_int( order[j] );
        }
        order_text += ">>";

        std::string runs_text = "<<";
        for ( int r = 0; r < num_runs; r++ )
        {
            runs_text += ( r > 0 ? ", " : "" );
            runs_text += "[first |-> " + tla_int( runs[r].first + 1 ) + ", count |-> " + tla_int( runs[r].count ) + ", seg |-> " +
                         tla_int( runs[r].segment_bytes ) + ", last |-> " + tla_int( runs[r].last_bytes ) + "]";
        }
        runs_text += ">>";

        tla_record_t record;
        record.add( "dest", tla_function( slot_keys, dest_values ) );
        record.add( "size", tla_function( slot_keys, size_values ) );
        record.add( "order", order_text );
        record.add( "runs", runs_text );
        records.push_back( record );
    }

    // the plant reverses a destination's packets after its second: only a
    // case with three or more to one destination can show it
    printf( "PLANT-FIRED %d\n", reorderable );

    std::string hash = "MCHash(d) == CASE ";
    for ( int d = 0; d < GATHER_DESTS; d++ )
    {
        hash += ( d > 0 ? " [] " : "" );
        hash += "d = " + tla_int( 1 + d ) + " -> " + tla_int( rocketnet_send_gather_home( &dests[d], scratch.buckets ) );
    }
    const std::string definitions = hash + "\n";

    char constants[1024];
    snprintf( constants, sizeof( constants ),
              "    S = %d\n    Buckets = %d\n    Dests = {1, 2, 3}\n    Sizes = {0, 600, 1200}\n    Hash <- MCHash\n"
              "    MaxSeg = %d\n    MaxBytes = %d\n    ProbeRule = \"mod\"\n    MatchRule = \"dest\"\n    PlaceRule = \"forward\"\n"
              "    CapRule = \"bytes\"\n    ShortRule = \"close\"\n",
              GATHER_SLOTS, scratch.buckets, GATHER_MAX_SEGMENTS, GATHER_MAX_BYTES );

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceGatherPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceGatherSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    return tla_write_trace( out, name, "TraceRNSendGatherLayout", definitions, constants, records, "TraceEnds" ) ? 0 : 1;
}

TLA_TRACE_LAYER( "gather", trace_gather );
