/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0 or later, or the Más Bandwidth Source License 1.0 (MBSL-1.0). See LICENSE and MBSL.md.
*/

// The trace check's walk over the path probe's re-try schedule
// (proofs/tla/RNPathProbeRetryLayout.tla, checked by
// proofs/tla/TraceRNPathProbeRetryLayout.tla). The schedule's arithmetic is
// sdk/rocketnet_link_mtu.h: rocketnet_link_mtu_retry_next, which the client
// calls at a size's rule-out and at the end of each re-try ruled out again
// (rocketnet_client_probe_wait), and rocketnet_link_mtu_retry_gap_seconds,
// which it calls. The walk makes exactly those calls, on the client's own
// two fields (probe_retries and probe_retry_time), and after every call
// writes one record of the model's variables. No socket, no clock, no
// connection.
//
// What this binds, and what it does not: the times and the count the code
// computes, at the code's constants (10 s, three flat gaps, 300 s, eight),
// for calls at the rule-out and at any time up to MaxRun after the slot
// before. When a call is made, and that a re-try is made at all, is the
// walk's here and RNPathProbe.tla's (ClientRuleOut, ClientRetry), traced by
// the path-probe layer. Times are whole seconds from the rule-out, which the
// code's doubles hold exactly.
//
//   tla_trace --layer path-probe-retry --seed N --out DIR [--steps K]
//
// writes DIR/MCTracePathProbeRetrySeed<seed>.tla and .cfg, one record a
// call: the operation (schedule), its argument (how long after the last
// slot the call is made, 0 at the rule-out), the model action (Schedule) and
// the model's variables after it. The plant build (ROCKETNET_TLA_PLANT sets
// ROCKETNET_LINK_MTU_RETRY_PLANT, in sdk/rocketnet_link_mtu.h) doubles the
// gap one re-try early; its trace must fail TraceConsistent. The walk
// reports (tla_report_t) the planted effect, a next re-try that is not the
// design's first time after the call, at the record where it happened,
// computed from the design's own sum of gaps: none in the clean build.

// the plant build: sdk/rocketnet_link_mtu.h doubles the gap one re-try early
#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_LINK_MTU_RETRY_PLANT 1
#endif // #if defined( ROCKETNET_TLA_PLANT )
#include "../sdk/rocketnet_link_mtu.h"

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the longest a call comes after the slot before it: a re-try ruled out
// again takes about half a second, and a client that ran no update for a
// while (or a re-try whose pairs met loss) may come much later, past
// several slots at the cap
#define RETRY_WALK_MAX_RUN 1000

// the exploration bound the cfg names: past every slot a walk reaches
#define RETRY_WALK_HORIZON 1000000

struct retry_walk_t
{
    int retry;
    double retry_time;
    int64_t now;
    bool started;
};

static tla_record_t retry_walk_record( const retry_walk_t & w )
{
    tla_record_t state;
    state.add( "retry", tla_int( w.retry ) );
    state.add( "slot", tla_int( (int64_t) w.retry_time ) );
    state.add( "now", tla_int( w.now ) );
    state.add( "started", tla_bool( w.started ) );
    return state;
}

// the design's first re-try after now, from the rule-out at 0: 10, 20, 30,
// then gaps of 20, 40, 80, 160, then 300 for ever (RNPathProbeRetryLayout
// DesignOffset), summed here without the code's loop or its jump
static int64_t retry_walk_design_next( int64_t now )
{
    int64_t time = 0;
    int64_t gap = (int64_t) ROCKETNET_LINK_MTU_RETRY_FIRST_SECONDS;
    for ( int j = 1;; j++ )
    {
        if ( j > ROCKETNET_LINK_MTU_RETRY_FLAT_COUNT )
        {
            gap = gap * 2 < (int64_t) ROCKETNET_LINK_MTU_RETRY_MAX_SECONDS ? gap * 2 : (int64_t) ROCKETNET_LINK_MTU_RETRY_MAX_SECONDS;
        }
        time += gap;
        if ( gap == (int64_t) ROCKETNET_LINK_MTU_RETRY_MAX_SECONDS && time <= now )
        {
            time += ( ( now - time ) / gap + 1 ) * gap;
            return time;
        }
        if ( time > now )
        {
            return time;
        }
    }
}

static int retry_walk( uint64_t seed, int steps, const char * out )
{
    retry_walk_t w = { 0, 0.0, 0, false };
    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;

    records.push_back( tla_step( "init", "init", "<<>>", retry_walk_record( w ) ) );
    for ( int step = 0; step < steps; step++ )
    {
        // the rule-out's own call is at its time; each later call ends a
        // re-try ruled out again: mostly within a second or two of its slot,
        // sometimes much later
        int d = 0;
        if ( w.started )
        {
            const int kind = random.below( 8 );
            d = kind < 2 ? 0 : kind < 6 ? 1 + random.below( 2 )
                                        : random.below( RETRY_WALK_MAX_RUN + 1 );
        }
        const int64_t now = (int64_t) w.retry_time + d;
        rocketnet_link_mtu_retry_next( &w.retry, &w.retry_time, (double) now );
        w.now = now;
        w.started = true;
        report.act( "Schedule" );
        records.push_back( tla_step( "schedule", "Schedule", tla_sequence( std::vector<std::string>( 1, tla_int( d ) ) ), retry_walk_record( w ) ) );
        // the planted effect, where it shows: a next re-try off the design's
        if ( (int64_t) w.retry_time != retry_walk_design_next( now ) )
        {
            report.planted( (int) records.size() );
        }
    }

    report.print();

    char constants[256];
    snprintf( constants, sizeof( constants ),
              "    First = %d\n    Flat = %d\n    Cap = %d\n    Saturate = %d\n    CounterMod = %d\n    MaxRun = %d\n    Horizon = %d\n    Broken = \"none\"\n",
              (int) ROCKETNET_LINK_MTU_RETRY_FIRST_SECONDS, ROCKETNET_LINK_MTU_RETRY_FLAT_COUNT, (int) ROCKETNET_LINK_MTU_RETRY_MAX_SECONDS,
              ROCKETNET_LINK_MTU_RETRY_SATURATE, ROCKETNET_LINK_MTU_RETRY_SATURATE + 1, RETRY_WALK_MAX_RUN, RETRY_WALK_HORIZON );
    return tla_write_trace( out, "MCTracePathProbeRetry", seed, "TraceRNPathProbeRetryLayout", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "path-probe-retry", retry_walk );
