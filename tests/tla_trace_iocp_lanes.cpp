/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The IOCP send-lanes layer of the TLA+ trace check
// (proofs/tla/README-TraceCheck.md, proofs/tla/TraceRNIocpSendLanes.tla),
// layer iocp-lanes: the send lanes
// (proofs/tla/RNIocpSendLanes.tla) over the code's own free queues,
// destination hash and carve (rocketnet_server_iocp_test_lanes_*). Each step
// is one call:
// - acquire: start_packet's pop from the destination's lane. The lane the
//   hash returned and the operation the pop returned are the arguments of
//   Start, or StartRefused when the pop returns none.
// - post: a send thread posts one lane it owns, its bank whole, in staged
//   order (the walk's bank, as the worker's three banks seen as one
//   sequence). The thread and the lane it walks to are Post's arguments: the
//   lanes of thread t come from the code's own walk,
//   rocketnet_iocp_send_thread_lanes (lane l is thread l % threads's), and
//   the thread count from rocketnet_server_iocp_send_threads, the functions
//   the transport's send threads call at create.
// - release: a completion releases one in-flight operation to the lane its
//   index names (Complete).
// Every lane's free queue is read back from the code after each step; the
// code's hash of every destination is the cfg's DestLaneAt.
//
// The plants are two defects in the transport's file, each compiled alone
// into a plant binary of its own (the row's plants,
// tests/tla_trace_iocp_lanes.row), so each must be rejected at the first
// record it changes and neither can hide the other. The carve (ROCKETNET_IOCP_SEND_LANES_PLANT) is
// index % lanes: a release lands in another lane's queue (or finds it full
// and is refused). The thread walk (ROCKETNET_IOCP_SEND_THREAD_PLANT) takes
// each thread one lane past its own: a thread posts a lane another thread
// owns. The planted effect is shown at the release's record, or at the
// post's.
//
// The walk's code compiles in every development build, so it needs no
// socket, no thread and no Windows.

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_server_iocp.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

#if defined( ROCKETNET_TLA_PLANT )
#include "../sdk/rocketnet_server_iocp_windows.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#if ROCKETNET_DEV

int rocketnet_server_iocp_test_lane_index( const rocketnet_address_t * to, int lanes );
int rocketnet_server_iocp_test_send_thread_lanes( int thread, int threads, int lanes, int * owned );

#define IOCP_LANES_LANES 3
#define IOCP_LANES_SIZE 4
#define IOCP_LANES_DESTS 6
#define IOCP_LANES_OPS ( IOCP_LANES_LANES * IOCP_LANES_SIZE )

// the send threads the code runs for these lanes
#define IOCP_LANES_THREADS rocketnet_server_iocp_send_threads( IOCP_LANES_LANES )

// rocketnet_iocp_packet_ref: the low 16 bits of a packet handle name the operation
#define IOCP_LANES_INDEX_BITS 16

struct iocp_lanes_staged_t
{
    int op;
    int dest;
    int64_t seq;
};

struct iocp_lanes_walk_t
{
    rocketnet_server_iocp_test_lanes_t * lanes;
    rocketnet_address_t dest[IOCP_LANES_DESTS + 1];
    std::vector<iocp_lanes_staged_t> bank[IOCP_LANES_LANES];
    std::vector<int> inflight;
    int64_t started[IOCP_LANES_DESTS + 1];
    std::vector<int64_t> posted[IOCP_LANES_DESTS + 1];
    std::vector<int> posted_by[IOCP_LANES_LANES];
};

static tla_record_t iocp_lanes_record( const iocp_lanes_walk_t & walk )
{
    std::vector<std::string> lane_keys;
    std::vector<std::string> free_values;
    std::vector<std::string> bank_values;
    for ( int l = 0; l < IOCP_LANES_LANES; ++l )
    {
        lane_keys.push_back( tla_int( l ) );
        int indexes[IOCP_LANES_OPS];
        const int count = rocketnet_server_iocp_test_lanes_free( walk.lanes, l, indexes, IOCP_LANES_OPS );
        std::vector<std::string> free_items;
        for ( int k = 0; k < count; ++k )
        {
            free_items.push_back( tla_int( indexes[k] ) );
        }
        free_values.push_back( tla_sequence( free_items ) );
        std::vector<std::string> bank_items;
        for ( const iocp_lanes_staged_t & staged : walk.bank[l] )
        {
            std::vector<std::string> entry;
            entry.push_back( tla_int( staged.op ) );
            entry.push_back( tla_int( staged.dest ) );
            entry.push_back( tla_int( staged.seq ) );
            bank_items.push_back( tla_sequence( entry ) );
        }
        bank_values.push_back( tla_sequence( bank_items ) );
    }

    std::vector<std::string> posted_by_values;
    for ( int l = 0; l < IOCP_LANES_LANES; ++l )
    {
        std::string threads = "{";
        for ( size_t i = 0; i < walk.posted_by[l].size(); ++i )
        {
            threads += ( i > 0 ? ", " : "" ) + tla_int( walk.posted_by[l][i] );
        }
        posted_by_values.push_back( threads + "}" );
    }

    std::string inflight = "{";
    for ( size_t i = 0; i < walk.inflight.size(); ++i )
    {
        inflight += ( i > 0 ? ", " : "" ) + tla_int( walk.inflight[i] );
    }
    inflight += "}";

    std::vector<std::string> dest_keys;
    std::vector<std::string> started_values;
    std::vector<std::string> posted_values;
    for ( int d = 1; d <= IOCP_LANES_DESTS; ++d )
    {
        dest_keys.push_back( tla_int( d ) );
        started_values.push_back( tla_int( walk.started[d] ) );
        std::vector<std::string> posted_items;
        for ( int64_t seq : walk.posted[d] )
        {
            posted_items.push_back( tla_int( seq ) );
        }
        posted_values.push_back( tla_sequence( posted_items ) );
    }

    tla_record_t record;
    record.add( "free", tla_function( lane_keys, free_values ) );
    record.add( "bank", tla_function( lane_keys, bank_values ) );
    record.add( "inflight", inflight );
    record.add( "started", tla_function( dest_keys, started_values ) );
    record.add( "posted", tla_function( dest_keys, posted_values ) );
    record.add( "postedBy", tla_function( lane_keys, posted_by_values ) );
    return record;
}

static std::string iocp_lanes_args( int a )
{
    return tla_sequence( std::vector<std::string>( 1, tla_int( a ) ) );
}

static int trace_iocp_lanes( uint64_t seed, int steps, const char * out )
{
    iocp_lanes_walk_t walk;
    walk.lanes = rocketnet_server_iocp_test_lanes_create( IOCP_LANES_LANES, IOCP_LANES_SIZE );
    if ( !walk.lanes )
    {
        fprintf( stderr, "tla_trace: the lanes could not be seeded\n" );
        return 1;
    }
    memset( walk.started, 0, sizeof( walk.started ) );

    // six destinations; the code's hash puts them in lanes, written into the
    // cfg as DestLaneAt
    for ( int d = 1; d <= IOCP_LANES_DESTS; ++d )
    {
        memset( &walk.dest[d], 0, sizeof( walk.dest[d] ) );
        walk.dest[d].type = ROCKETNET_ADDRESS_IPV4;
        walk.dest[d].data.ipv4[0] = 172;
        walk.dest[d].data.ipv4[1] = 31;
        walk.dest[d].data.ipv4[2] = 91;
        walk.dest[d].data.ipv4[3] = (uint8_t) ( 160 + d );
        walk.dest[d].port = (uint16_t) ( 40000 + d );
    }

    tla_random_t random;
    random.state = seed;

    std::vector<tla_record_t> records;
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", iocp_lanes_record( walk ) ) );

    int64_t starts = 0;

    // Start: the pop's result names the action; an empty lane refuses and
    // nothing changes
    auto acquire = [&]( int d )
    {
        int lane = 0;
        const int op = rocketnet_server_iocp_test_lanes_acquire( walk.lanes, &walk.dest[d], &lane );
        std::vector<std::string> args;
        args.push_back( tla_int( d ) );
        args.push_back( tla_int( lane ) );
        const char * act = "StartRefused";
        if ( op >= 0 )
        {
            act = "Start";
            args.push_back( tla_int( op ) );
            walk.bank[lane].push_back( { op, d, walk.started[d] } );
            walk.started[d]++;
            starts++;
        }
        report.act( act );
        records.push_back( tla_step( "acquire", act, tla_sequence( args ), iocp_lanes_record( walk ) ) );
    };

    // Post: send thread t posts lane l, the bank whole in staged order (the
    // caller has checked the bank is not empty: an empty bank posts nothing,
    // and the walk makes no call)
    auto post = [&]( int t, int l )
    {
        for ( const iocp_lanes_staged_t & staged : walk.bank[l] )
        {
            walk.inflight.push_back( staged.op );
            walk.posted[staged.dest].push_back( staged.seq );
        }
        walk.bank[l].clear();
        bool known = false;
        for ( int seen : walk.posted_by[l] )
        {
            known = known || seen == t;
        }
        if ( !known )
        {
            walk.posted_by[l].push_back( t );
        }
        report.act( "Post" );
        records.push_back( tla_step( "post", "Post", tla_sequence( { tla_int( t ), tla_int( l ) } ), iocp_lanes_record( walk ) ) );
        // the planted effect, where it shows: a thread posted a lane another
        // thread owns
        if ( rocketnet_server_iocp_send_thread_of_lane( l, IOCP_LANES_THREADS ) != t )
        {
            report.planted( (int) records.size() );
        }
    };

    // The prologue, the same in the clean and the plant build: a packet to
    // each destination twice, then every send thread posts each lane it owns
    // by the code's walk. The first post names the walk's thread and lane,
    // so a walk that hands a thread another thread's lane first differs from
    // the clean trace at a record the walk reports as planted, never earlier.
    for ( int round = 0; round < 2; ++round )
    {
        for ( int d = 1; d <= IOCP_LANES_DESTS; ++d )
        {
            acquire( d );
        }
    }
    for ( int t = 0; t < IOCP_LANES_THREADS; ++t )
    {
        int owned[ROCKETNET_IOCP_MAX_LANES];
        const int num_owned = rocketnet_server_iocp_test_send_thread_lanes( t, IOCP_LANES_THREADS, IOCP_LANES_LANES, owned );
        for ( int k = 0; k < num_owned; ++k )
        {
            if ( !walk.bank[owned[k]].empty() )
            {
                post( t, owned[k] );
            }
        }
    }
    if ( walk.posted_by[0].empty() && walk.posted_by[1].empty() && walk.posted_by[2].empty() )
    {
        fprintf( stderr, "tla_trace: the prologue posted no lane\n" );
        return 1;
    }

    while ( (int) records.size() < steps + 1 )
    {
        const int action = random.below( 3 );
        if ( action == 0 )
        {
            acquire( 1 + random.below( IOCP_LANES_DESTS ) );
        }
        else if ( action == 1 )
        {
            // Post: one send thread, one lane it owns by the code's walk; an
            // empty bank posts nothing, and the walk makes no call
            const int t = random.below( IOCP_LANES_THREADS );
            int owned[ROCKETNET_IOCP_MAX_LANES];
            const int num_owned = rocketnet_server_iocp_test_send_thread_lanes( t, IOCP_LANES_THREADS, IOCP_LANES_LANES, owned );
            if ( num_owned == 0 )
            {
                continue;
            }
            const int l = owned[random.below( num_owned )];
            if ( walk.bank[l].empty() )
            {
                continue;
            }
            post( t, l );
        }
        else
        {
            // Complete: any in-flight operation, released to its lane
            if ( walk.inflight.empty() )
            {
                continue;
            }
            const size_t k = (size_t) random.below( (int) walk.inflight.size() );
            const int op = walk.inflight[k];
            walk.inflight.erase( walk.inflight.begin() + (long) k );
            // false only when the lane the release names is full, which only
            // a release to the wrong lane finds: the operation is then in no
            // free queue and not in flight, which no model step makes
            const bool released = rocketnet_server_iocp_test_lanes_release( walk.lanes, op );
            report.act( "Complete" );
            records.push_back( tla_step( "release", "Complete", iocp_lanes_args( op ), iocp_lanes_record( walk ) ) );
            // the planted effect, where it shows: the operation is not at the
            // tail of its own carve's free queue
            int indexes[IOCP_LANES_OPS];
            const int count = rocketnet_server_iocp_test_lanes_free( walk.lanes, op / IOCP_LANES_SIZE, indexes, IOCP_LANES_OPS );
            if ( !released || count == 0 || indexes[count - 1] != op )
            {
                report.planted( (int) records.size() );
            }
        }
    }

    rocketnet_server_iocp_test_lanes_destroy( walk.lanes );

    report.print();

    // the code's hash of each destination, as the model's DestLaneAt: d * Lanes + l
    std::string dest_lane_at = "{";
    for ( int d = 1; d <= IOCP_LANES_DESTS; ++d )
    {
        const int lane = rocketnet_server_iocp_test_lane_index( &walk.dest[d], IOCP_LANES_LANES );
        dest_lane_at += ( d > 1 ? ", " : "" ) + tla_int( (int64_t) d * IOCP_LANES_LANES + lane );
    }
    dest_lane_at += "}";

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    Lanes = %d\n    Size = %d\n    Dests = {1, 2, 3, 4, 5, 6}\n    DestLaneAt = %s\n    IndexBits = %d\n"
              "    MaxStarts = %d\n    AnyLane = FALSE\n    ReleaseMod = FALSE\n    Threads = %d\n    OwnerDefect = 0\n",
              IOCP_LANES_LANES, IOCP_LANES_SIZE, dest_lane_at.c_str(), IOCP_LANES_INDEX_BITS, (int) starts + 1, IOCP_LANES_THREADS );

    return tla_write_trace( out, "MCTraceIocpLanes", seed, "TraceRNIocpSendLanes", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "iocp-lanes", trace_iocp_lanes );

#endif // #if ROCKETNET_DEV
