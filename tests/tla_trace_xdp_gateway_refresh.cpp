/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace check of the XDP gateway refresh (proofs/tla/RNXdpGatewayRefresh.tla,
// proofs/tla/TraceRNXdpGatewayRefresh.tla): the walk drives the library's own
// rocketnet_xdp_gateway_refresh and rocketnet_xdp_reply_mac
// (sdk/rocketnet_xdp_frame.h) over a reply MAC table with one family's
// gateway, and is their environment too:
//
//   refresh         the receive call's refresh. Its resolve callback is the
//                   kernel's neighbour entry (the model's resolver), found
//                   and usable. The model's resolve always answers, so the
//                   walk fails a resolve (an error, no entry, or an unusable
//                   MAC) only when the entry holds the table's MAC already:
//                   the code must then keep the table, deadline moved, which
//                   is the model's Refresh with no change, and must not lose
//                   the gateway (the fills' lookups would find none). A
//                   failed resolve against a changed entry is outside the
//                   model and its unit tests' (tests/test_unit_xdp_gateway_refresh.cpp).
//                   One family only: the model has one MAC; the two-family
//                   slots are the unit tests'. A call that returns no change is one
//                   Refresh. A call that returns a change is three model
//                   steps: Refresh, with the state taken inside the resolve
//                   callback (the code has set its next deadline and asked
//                   the kernel), Resolve, the half-written table, which one
//                   memcpy never shows (a hidden record), and WriteLow, the
//                   state after the call.
//   copy_high/low   a fill stages a packet to an unlearned address: the real
//                   rocketnet_xdp_reply_mac gives the table's gateway MAC,
//                   and the walk copies it in two halves, ReadHigh and
//                   ReadLow, as the model splits a six-byte copy.
//   change_gateway  the router is replaced: the resolver's answer changes.
//   join            the fills are joined and the next update starts, the
//                   platform clock some whole seconds later.
//
// A MAC is two halves, the model's <<1, 1>> and <<2, 2>>: the walk's two
// gateway MACs differ in their first three bytes and in their last three,
// and each half maps to 1 or 2.
//
// The plant (ROCKETNET_XDP_GATEWAY_REFRESH_PLANT): a refresh at or past
// its deadline keeps the old deadline, so every later call resolves again.
// Its effect, reported at the Refresh record of such a call, is a deadline
// that did not move past the call's clock.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_XDP_GATEWAY_REFRESH_PLANT 1
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_xdp_frame.h"

#include "tla_trace_record.h"

#define GATEWAY_TRACE_PERIOD        5               // ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS, whole seconds
#define GATEWAY_TRACE_MAX_STEP      7               // the clock's step between updates: under, at and past the period
#define GATEWAY_TRACE_MAX_TIME      1000000         // the trace cfg's MaxTime: no walk reaches it
#define GATEWAY_TRACE_MAX_CHANGES   1000000         // the trace cfg's MaxChanges: no walk reaches it
#define GATEWAY_TRACE_ENTRIES       64              // the smallest table rocketnet_xdp_reply_macs_bind takes

static_assert( ROCKETNET_XDP_GATEWAY_REFRESH_SECONDS == (double) GATEWAY_TRACE_PERIOD, "the trace's Period is the refresh period, in whole seconds" );

// the two gateway MACs, the model's <<1, 1>> and <<2, 2>>
static const uint8_t gateway_trace_macs[2][6] = { { 0x02, 0x11, 0x11, 0x0a, 0x0a, 0x0a }, { 0x02, 0x22, 0x22, 0x0b, 0x0b, 0x0b } };

// the model's half (1 or 2) of three bytes; 0 for bytes neither MAC has
static int gateway_trace_half( const uint8_t * bytes, int offset )
{
    for ( int i = 0; i < 2; i++ )
    {
        if ( memcmp( bytes, gateway_trace_macs[i] + offset, 3 ) == 0 )
        {
            return i + 1;
        }
    }
    return 0;
}

struct gateway_trace_packet_t
{
    int high;
    int low;
    int time;
};

struct gateway_trace_walk_t
{
    struct rocketnet_xdp_reply_macs_t table;
    struct rocketnet_xdp_reply_mac_entry_t entries[GATEWAY_TRACE_ENTRIES];
    struct rocketnet_xdp_gateway_refresh_t refresh;

    // the environment: the kernel's neighbour entry, the clock, the fills
    int resolver;                   // 0 or 1: the MAC the kernel's neighbour entry holds
    int fail_resolve;               // 0: the resolve answers; 1, 2, 3: an error, no entry, an unusable MAC
    int now;
    int changed_at;
    int changes;
    const char * phase;
    const uint8_t * fill_mac;       // the MAC a fill is copying, NULL when no fill is mid-copy
    int read_high;                  // the half the fill has copied, 0 (Nil) for none
    std::vector<gateway_trace_packet_t> staged;

    // the state taken inside the resolve callback
    bool resolved;
    tla_record_t at_resolve;
};

static std::string gateway_trace_pair( int high, int low )
{
    std::vector<std::string> halves;
    halves.push_back( tla_int( high ) );
    halves.push_back( tla_int( low ) );
    return tla_sequence( halves );
}

static tla_record_t gateway_trace_state( const gateway_trace_walk_t & walk, const char * phase, int pending_low )
{
    tla_record_t state;
    state.add( "phase", tla_string( phase ) );
    state.add( "now", tla_int( walk.now ) );
    state.add( "scheduled", tla_bool( walk.refresh.scheduled ) );
    state.add( "nextTime", tla_int( (int64_t) walk.refresh.next_time ) );
    state.add( "table", gateway_trace_pair( gateway_trace_half( walk.table.gateway_mac, 0 ), gateway_trace_half( walk.table.gateway_mac + 3, 3 ) ) );
    state.add( "pendingLow", tla_int( pending_low ) );
    state.add( "resolver", gateway_trace_pair( walk.resolver + 1, walk.resolver + 1 ) );
    state.add( "changedAt", tla_int( walk.changed_at ) );
    state.add( "changes", tla_int( walk.changes ) );
    state.add( "readHigh", tla_int( walk.read_high ) );
    std::string staged = "{";
    for ( size_t i = 0; i < walk.staged.size(); i++ )
    {
        staged += ( i > 0 ? ", " : "" );
        staged += "[mac |-> " + gateway_trace_pair( walk.staged[i].high, walk.staged[i].low ) + ", time |-> " + tla_int( walk.staged[i].time ) + "]";
    }
    state.add( "staged", staged + "}" );
    return state;
}

// the kernel's neighbour entry: the walk's resolver, found and usable. The
// refresh has set its next deadline and asked: the model's Refresh to
// "resolve", whose state is taken here
static bool gateway_trace_resolve( void * context, int address_type, uint8_t * mac, bool * found )
{
    (void) address_type;
    gateway_trace_walk_t * walk = (gateway_trace_walk_t *) context;
    walk->resolved = true;
    walk->at_resolve = gateway_trace_state( *walk, "resolve", 0 );
    memcpy( mac, gateway_trace_macs[walk->resolver], 6 );
    *found = walk->fail_resolve != 2;
    if ( walk->fail_resolve == 3 )
    {
        mac[0] |= 1;
    }
    return walk->fail_resolve != 1;
}

static std::string gateway_trace_no_args()
{
    return tla_sequence( std::vector<std::string>() );
}

// a step inside one call the walk cannot observe: its state is <<>>, and the
// Trace module takes whatever the model's step gives (a deterministic step only)
static tla_record_t gateway_trace_hidden_step( const char * op, const char * act )
{
    tla_record_t step;
    step.add( "op", tla_string( op ) );
    step.add( "act", tla_string( act ) );
    step.add( "args", gateway_trace_no_args() );
    step.add( "state", tla_function( std::vector<std::string>(), std::vector<std::string>() ) );
    return step;
}

static int trace_xdp_gateway_refresh( uint64_t seed, int steps, const char * out )
{
    gateway_trace_walk_t walk;
    walk.staged.clear();
    walk.resolver = 0;
    walk.fail_resolve = 0;
    walk.now = 0;
    walk.changed_at = 0;
    walk.changes = 0;
    walk.phase = "receive";
    walk.fill_mac = NULL;
    walk.read_high = 0;
    walk.resolved = false;

    // create resolved the gateway: the table holds the resolver's MAC, and
    // the refresh has not yet scheduled
    if ( !rocketnet_xdp_reply_macs_bind( &walk.table, walk.entries, GATEWAY_TRACE_ENTRIES ) )
    {
        fprintf( stderr, "tla_trace: the reply MAC table did not bind\n" );
        return 1;
    }
    rocketnet_xdp_reply_macs_reset( &walk.table, gateway_trace_macs[0] );
    memset( &walk.refresh, 0, sizeof( walk.refresh ) );
    walk.refresh.enabled = true;
    walk.refresh.serves_both = false;
    walk.refresh.address_type = ROCKETNET_ADDRESS_IPV4;

    // an unlearned client: the fills' replies go to the gateway
    rocketnet_address_t client;
    memset( &client, 0, sizeof( client ) );
    client.type = ROCKETNET_ADDRESS_IPV4;
    client.data.ipv4[0] = 203;
    client.data.ipv4[1] = 0;
    client.data.ipv4[2] = 113;
    client.data.ipv4[3] = 7;
    client.port = 40000;

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    const size_t total = (size_t) steps + 1;

    records.push_back( tla_step( "init", "init", gateway_trace_no_args(), gateway_trace_state( walk, walk.phase, 0 ) ) );

    while ( records.size() < total )
    {
        // the router is replaced, in any phase
        if ( random.below( 8 ) == 0 )
        {
            walk.resolver = 1 - walk.resolver;
            walk.changed_at = walk.now;
            walk.changes++;
            report.act( "ChangeGateway" );
            records.push_back( tla_step( "change_gateway", "ChangeGateway", gateway_trace_no_args(), gateway_trace_state( walk, walk.phase, 0 ) ) );
            continue;
        }

        if ( strcmp( walk.phase, "receive" ) == 0 )
        {
            // the receive call's refresh
            const bool due = walk.refresh.scheduled && walk.now >= walk.refresh.next_time;
            struct rocketnet_xdp_gateway_change_t changes[2];
            walk.resolved = false;
            walk.fail_resolve = 0;
            if ( memcmp( walk.table.gateway_mac, gateway_trace_macs[walk.resolver], 6 ) == 0 && random.below( 2 ) == 0 )
            {
                walk.fail_resolve = 1 + random.below( 3 );
            }
            const int num_changes = rocketnet_xdp_gateway_refresh( &walk.refresh, &walk.table, (double) walk.now, gateway_trace_resolve, &walk, changes );
            const bool stale = due && walk.refresh.next_time <= (double) walk.now;
            walk.phase = "fill";
            if ( num_changes > 0 )
            {
                if ( !walk.resolved || walk.fail_resolve != 0 )
                {
                    fprintf( stderr, "tla_trace: a refresh reported a change without a resolve that answered\n" );
                    return 1;
                }
                report.act( "Refresh" );
                records.push_back( tla_step( "refresh", "Refresh", gateway_trace_no_args(), walk.at_resolve ) );
                if ( stale )
                {
                    report.planted( (int) records.size() );
                }
                if ( records.size() < total )
                {
                    report.act( "Resolve" );
                    records.push_back( gateway_trace_hidden_step( "refresh", "Resolve" ) );
                }
                if ( records.size() < total )
                {
                    report.act( "WriteLow" );
                    records.push_back( tla_step( "refresh", "WriteLow", gateway_trace_no_args(), gateway_trace_state( walk, walk.phase, 0 ) ) );
                }
            }
            else
            {
                report.act( "Refresh" );
                records.push_back( tla_step( "refresh", "Refresh", gateway_trace_no_args(), gateway_trace_state( walk, walk.phase, 0 ) ) );
                if ( stale )
                {
                    report.planted( (int) records.size() );
                }
            }
            continue;
        }

        // the fill phase: a fill copies the gateway MAC into a packet, half
        // by half, or the fills are joined and the next update starts
        if ( walk.read_high != 0 )
        {
            walk.staged.push_back( { walk.read_high, gateway_trace_half( walk.fill_mac + 3, 3 ), walk.now } );
            walk.read_high = 0;
            walk.fill_mac = NULL;
            report.act( "ReadLow" );
            records.push_back( tla_step( "copy_low", "ReadLow", gateway_trace_no_args(), gateway_trace_state( walk, walk.phase, 0 ) ) );
            continue;
        }

        if ( random.below( 3 ) != 0 )
        {
            uint64_t refused = 0;
            walk.fill_mac = rocketnet_xdp_reply_mac( &walk.table, &client, NULL, &refused );
            if ( !walk.fill_mac )
            {
                fprintf( stderr, "tla_trace: the reply to an unlearned client found no gateway MAC\n" );
                return 1;
            }
            walk.read_high = gateway_trace_half( walk.fill_mac, 0 );
            report.act( "ReadHigh" );
            records.push_back( tla_step( "copy_high", "ReadHigh", gateway_trace_no_args(), gateway_trace_state( walk, walk.phase, 0 ) ) );
            continue;
        }

        // the next update: the clock lands on the deadline about half the
        // time it can, else anywhere within the step
        int delta = 1 + random.below( GATEWAY_TRACE_MAX_STEP );
        const int to_deadline = (int) walk.refresh.next_time - walk.now;
        if ( to_deadline >= 1 && to_deadline <= GATEWAY_TRACE_MAX_STEP && random.below( 2 ) == 0 )
        {
            delta = to_deadline;
        }
        walk.now += delta;
        walk.phase = "receive";
        report.act( "Join" );
        records.push_back( tla_step( "join", "Join", gateway_trace_no_args(), gateway_trace_state( walk, walk.phase, 0 ) ) );
    }

    report.print();

    char constants[256];
    snprintf( constants, sizeof( constants ),
              "    Period = %d\n    MaxTime = %d\n    MaxStep = %d\n    MaxChanges = %d\n    RefreshInFill = FALSE\n    RefreshNever = FALSE\n",
              GATEWAY_TRACE_PERIOD, GATEWAY_TRACE_MAX_TIME, GATEWAY_TRACE_MAX_STEP, GATEWAY_TRACE_MAX_CHANGES );
    return tla_write_trace( out, "MCTraceXdpGatewayRefresh", seed, "TraceRNXdpGatewayRefresh", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "xdp-gateway-refresh", trace_xdp_gateway_refresh );
