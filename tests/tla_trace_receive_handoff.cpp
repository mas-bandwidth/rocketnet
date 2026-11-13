/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The receive handoff of the gso server (sdk/rocketnet_receive_handoff.h in
// sdk/rocketnet_server_gso_linux.cpp), checked by
// proofs/tla/TraceRNReceiveHandoff.tla (README-TraceCheck.md) against
// proofs/tla/RNReceiveHandoff.tla.
//
//   tla_trace --layer receive-handoff --seed N --out DIR [--steps K]
//
// writes DIR/MCTraceReceiveHandoffSeed<seed>.tla and .cfg
// (MCTraceReceiveHandoffPlantSeed<seed> from tla_trace_plant_receive_handoff).
// The walk drives queue 0 of the rocketnet_server_gso_test_open fixture: no
// socket, no thread, no clock. A worker pass is the worker's staging of n
// datagrams (rocketnet_server_gso_test_receive_bank) and its end
// (rocketnet_server_gso_test_receive_end_pass, rocketnet_server_gso_end_pass);
// a call is one rocketnet_server_gso_receive_packets with its per-update
// budget cut to the call's, and with the receive mutex held against it on a
// busy call, so main's try-acquire fails. Cap is the code's
// ROCKETNET_GSO_RECV_QUEUE_SIZE: in the walk's last quarter a pass now and
// then stages all the room the worker's buffer has left, so the worker fills
// it (and the calls after it drain the full buffer to the walk's end).
//
// One record per step (tla_step):
//   bank      a pass staging n datagrams              Bank(n)
//   end_pass  the pass's end, when its buffer holds results   Commit
//   call      a receive call                          Call(busy, b) with what
//             it returned, its first datagram and how many (CallGave)
// Every model variable is read from the queue as the library holds it
// (rocketnet_server_gso_test_receive_state), but arrived, delivered,
// committedTop: arrived and delivered are the walk's counts of what it staged
// and what the calls returned, and committedTop the newest datagram a commit
// made visible. late, the history of DeliveredByNextCall, is a ghost: the
// model computes it, and the walk leaves it out.
//
// The plant (ROCKETNET_RECEIVE_HANDOFF_PLANT, in
// rocketnet_server_gso_receive_from_queue) reads only a buffer the worker has
// left, the old ready-slot rule (RNReceiveHandoff's ReadOnlyLeft). Its effect shows at the first call that took the
// mutex and left main sharing the worker's buffer with a limit below that
// buffer's committed count, which the clean code never does: nothing commits
// during the walk's call. The walk reports that record.

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_RECEIVE_HANDOFF_PLANT 1
#include "../sdk/rocketnet_server_gso_linux.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#include "../sdk/rocketnet_internal.h"

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_DEV

#include "../sdk/rocketnet_constants.h"
#include "../sdk/rocketnet_server_gso.h"

#include <stdio.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

// the budgets a call may have: one datagram, a few, more than a pass
// stages, and the code's whole per-update budget, so calls are cut and calls
// drain
static const int receive_handoff_budgets[] = { 1, 3, 64, ROCKETNET_SERVER_MAX_RECEIVE_PACKETS };

// the passes' sizes, but for the pass that fills the buffer
#define RECEIVE_HANDOFF_MAX_PASS 8

// arrived may not pass it (the model's MaxRx): far past any walk
#define RECEIVE_HANDOFF_MAX_RX 100000000

struct receive_handoff_walk_t
{
    uint32_t arrived;
    uint32_t delivered;
    uint32_t committed_top;
};

static std::string receive_handoff_buffers( const uint32_t * values )
{
    std::vector<std::string> keys;
    std::vector<std::string> out;
    for ( int b = 0; b < 3; b++ )
    {
        keys.push_back( tla_int( b ) );
        out.push_back( tla_int( values[b] ) );
    }
    return tla_function( keys, out );
}

// the model's NONE for a free buffer of -1
static int receive_handoff_role( int index )
{
    return index < 0 ? 3 : index;
}

static tla_record_t receive_handoff_record( const struct rocketnet_server_gso_test_receive_state_t & state, const receive_handoff_walk_t & walk )
{
    tla_record_t record;
    record.add( "worker", tla_int( receive_handoff_role( state.worker ) ) );
    record.add( "main", tla_int( receive_handoff_role( state.main ) ) );
    record.add( "tail", tla_int( receive_handoff_role( state.tail ) ) );
    record.add( "free", tla_int( receive_handoff_role( state.free ) ) );
    record.add( "first", receive_handoff_buffers( state.first ) );
    record.add( "count", receive_handoff_buffers( state.count ) );
    record.add( "committed", receive_handoff_buffers( state.committed ) );
    record.add( "mainCursor", tla_int( state.main_cursor ) );
    record.add( "mainLimit", tla_int( state.main_limit ) );
    record.add( "tailCursor", tla_int( state.tail_cursor ) );
    record.add( "tailEnd", tla_int( state.tail_end ) );
    record.add( "arrived", tla_int( walk.arrived ) );
    record.add( "delivered", tla_int( walk.delivered ) );
    record.add( "committedTop", tla_int( walk.committed_top ) );
    return record;
}

static int trace_receive_handoff( uint64_t seed, int steps, const char * out )
{
    struct rocketnet_server_gso_t * gso = rocketnet_server_gso_test_open( 1 );
    if ( !gso )
    {
        fprintf( stderr, "tla_trace: receive-handoff: the fixture could not be opened\n" );
        return 1;
    }

    tla_random_t random = { seed };
    std::vector<tla_record_t> records;
    tla_report_t report;
    receive_handoff_walk_t walk = { 0, 0, 0 };
    struct rocketnet_server_gso_test_receive_state_t state;

    rocketnet_server_gso_test_receive_state( gso, &state );
    records.push_back( tla_step( "init", "init", "<<>>", receive_handoff_record( state, walk ) ) );

    int failed = 0;
    for ( int step = 0; step < steps && !failed; step++ )
    {
        rocketnet_server_gso_test_receive_state( gso, &state );

        // one record a step: a worker pass's staging, its end, or a call
        const uint32_t room = (uint32_t) ROCKETNET_GSO_RECV_QUEUE_SIZE - state.count[state.worker];
        int kind = random.below( 10 );
        if ( kind < 3 && room == 0 )
        {
            kind = 3;
        }
        if ( kind >= 3 && kind < 5 && state.count[state.worker] == 0 )
        {
            kind = 5;
        }

        if ( kind < 3 )
        {
            // a worker pass stages n datagrams
            uint32_t n = 1 + (uint32_t) random.below( RECEIVE_HANDOFF_MAX_PASS );
            if ( step >= steps - steps / 4 && random.below( 8 ) == 0 )
            {
                n = room;
            }
            n = n < room ? n : room;
            if ( !rocketnet_server_gso_test_receive_bank( gso, walk.arrived + 1, (int) n ) )
            {
                fprintf( stderr, "tla_trace: receive-handoff: the worker's buffer refused %u datagrams with %u of room\n", n, room );
                failed = 1;
                break;
            }
            walk.arrived += n;
            rocketnet_server_gso_test_receive_state( gso, &state );
            report.act( "Bank" );
            records.push_back( tla_step( "bank", "Bank", tla_sequence( { tla_int( n ) } ), receive_handoff_record( state, walk ) ) );
            continue;
        }

        if ( kind < 5 )
        {
            // the pass ends: its buffer holds results, so it commits
            const uint32_t top = state.first[state.worker] + state.count[state.worker] - 1;
            rocketnet_server_gso_test_receive_end_pass( gso );
            walk.committed_top = top;
            rocketnet_server_gso_test_receive_state( gso, &state );
            report.act( "Commit" );
            records.push_back( tla_step( "end_pass", "Commit", "<<>>", receive_handoff_record( state, walk ) ) );
            continue;
        }

        // a receive call
        const bool busy = random.below( 6 ) == 0;
        const int budget = receive_handoff_budgets[random.below( (int) ( sizeof( receive_handoff_budgets ) / sizeof( receive_handoff_budgets[0] ) ) )];
        const uint64_t busy_before = state.lock_busy;

        if ( budget > state.capacity )
        {
            fprintf( stderr, "tla_trace: receive-handoff: a budget of %d is past the fixture's %d\n", budget, state.capacity );
            failed = 1;
            break;
        }

        uint32_t first = 0;
        const int n = rocketnet_server_gso_test_receive_call( gso, budget, busy, &first );
        if ( n < 0 )
        {
            fprintf( stderr, "tla_trace: receive-handoff: a call returned datagrams out of order\n" );
            failed = 1;
            break;
        }

        rocketnet_server_gso_test_receive_state( gso, &state );
        if ( ( state.lock_busy - busy_before ) != ( busy ? 1u : 0u ) )
        {
            fprintf( stderr, "tla_trace: receive-handoff: the try-acquire did not fail exactly on the busy call\n" );
            failed = 1;
            break;
        }

        walk.delivered += (uint32_t) n;

        report.act( "Call" );
        records.push_back( tla_step( "call", "Call", tla_sequence( { tla_bool( busy ), tla_int( budget ), tla_int( first ), tla_int( n ) } ),
                                     receive_handoff_record( state, walk ) ) );

        // the planted effect: main shares the worker's buffer and sees less
        // than its committed count, right after taking the mutex
        if ( !busy && state.main == state.worker && state.main_limit < state.committed[state.main] )
        {
            report.planted( (int) records.size() );
        }
    }

    rocketnet_server_gso_test_close( gso );

    if ( failed )
    {
        return 1;
    }

    report.print();

    std::string budgets = "{";
    for ( size_t i = 0; i < sizeof( receive_handoff_budgets ) / sizeof( receive_handoff_budgets[0] ); i++ )
    {
        budgets += ( i > 0 ? ", " : "" ) + tla_int( receive_handoff_budgets[i] );
    }
    budgets += "}";

    const std::string constants = "    Cap = " + tla_int( ROCKETNET_GSO_RECV_QUEUE_SIZE ) + "\n" + "    MaxRx = " + tla_int( RECEIVE_HANDOFF_MAX_RX ) + "\n" +
                                  "    Budgets = " + budgets + "\n" + "    ReadOnlyLeft = FALSE\n";

    return tla_write_trace( out, "MCTraceReceiveHandoff", seed, "TraceRNReceiveHandoff", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "receive-handoff", trace_receive_handoff );

#endif // #if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_LINUX && ROCKETNET_DEV
