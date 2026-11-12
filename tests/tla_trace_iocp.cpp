/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The IOCP layers of the TLA+ trace check (proofs/tla/README-TraceCheck.md).
//
// iocp-admit: the completion worker's admission of a received datagram to
// its receive bank (proofs/tla/RNIocpReceiveAdmit.tla), driven through
// rocketnet_iocp_receive_admit by its development seam. Each step either
// completes one datagram of a random kind (a player's, garbage that fails a
// stateless rung, or hostile: well formed with the server's filter value)
// and banks or counts it by the code's verdict, as
// rocketnet_iocp_bank_datagram does, or drains the bank as main does. The
// plant build (ROCKETNET_TLA_PLANT) includes its own copy of the admission's
// file compiled with ROCKETNET_IOCP_RECEIVE_ADMIT_PLANT (the capacity check
// first): garbage then takes a slot the spec says it is refused, and the
// trace fails TraceConsistent.
//
// iocp-lanes: the send lanes (proofs/tla/RNIocpSendLanes.tla) over the
// code's own free queues, destination hash and carve
// (rocketnet_server_iocp_test_lanes_*). Start takes an operation from the
// destination's lane, Post moves a lane's staged packets to in flight in
// staged order, Complete releases one in-flight operation to the lane its
// index names. Every lane's free queue is read back from the code after each
// step. The plant compiles the carve as index % lanes
// (ROCKETNET_IOCP_SEND_LANES_PLANT): operations land in the wrong lanes and
// the trace fails TraceConsistent.
//
// Both walks' code compiles in every development build, so they need no
// socket, no thread and no Windows.

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_receive_prefilter.h"
#include "../sdk/rocketnet_wire.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_server_iocp.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "tla_trace_record.h"

#if defined( ROCKETNET_TLA_PLANT )
#define ROCKETNET_IOCP_RECEIVE_ADMIT_PLANT
#define ROCKETNET_IOCP_SEND_LANES_PLANT
#include "../sdk/rocketnet_server_iocp_windows.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#if ROCKETNET_DEV

int rocketnet_server_iocp_test_receive_admit( const uint8_t * packet_filter, const uint8_t * packet_data, int packet_bytes,
                                              int num_packets, int capacity );
int rocketnet_server_iocp_test_lane_index( const rocketnet_address_t * to, int lanes );

#define IOCP_ADMIT_CAPACITY 4

enum
{
    IOCP_ADMIT_PLAYER = 0,
    IOCP_ADMIT_GARBAGE = 1,
    IOCP_ADMIT_HOSTILE = 2,
    IOCP_ADMIT_KINDS = 3
};

static const char * const iocp_admit_kind_names[IOCP_ADMIT_KINDS] = { "\"player\"", "\"garbage\"", "\"hostile\"" };

static const uint8_t iocp_admit_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x11, 0x22, 0x33, 0x44 };
static const uint8_t iocp_admit_other_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x11, 0x22, 0x33, 0x45 };

struct iocp_admit_walk_t
{
    std::vector<int> bank;
    int64_t arrived[IOCP_ADMIT_KINDS];
    int64_t refused[IOCP_ADMIT_KINDS];
    int64_t full[IOCP_ADMIT_KINDS];
    int64_t counted[IOCP_ADMIT_KINDS];
    int64_t delivered[IOCP_ADMIT_KINDS];
    bool evicted;
};

static std::string iocp_admit_by_kind( const int64_t * values )
{
    std::vector<std::string> keys;
    std::vector<std::string> texts;
    for ( int k = 0; k < IOCP_ADMIT_KINDS; ++k )
    {
        keys.push_back( iocp_admit_kind_names[k] );
        texts.push_back( tla_int( values[k] ) );
    }
    return tla_function( keys, texts );
}

static tla_record_t iocp_admit_record( const iocp_admit_walk_t & walk )
{
    std::string bank = "<<";
    for ( size_t i = 0; i < walk.bank.size(); ++i )
    {
        bank += ( i > 0 ? ", " : "" );
        bank += iocp_admit_kind_names[walk.bank[i]];
    }
    bank += ">>";

    tla_record_t record;
    record.add( "bank", bank );
    record.add( "arrived", iocp_admit_by_kind( walk.arrived ) );
    record.add( "refused", iocp_admit_by_kind( walk.refused ) );
    record.add( "full", iocp_admit_by_kind( walk.full ) );
    record.add( "counted", iocp_admit_by_kind( walk.counted ) );
    record.add( "delivered", iocp_admit_by_kind( walk.delivered ) );
    record.add( "evicted", tla_bool( walk.evicted ) );
    return record;
}

// a datagram of the kind: the prefilter's header, then filler to its size
static int iocp_admit_datagram( tla_random_t & random, int kind, uint8_t * packet )
{
    memset( packet, 0xA5, ROCKETNET_RECEIVE_CEILING + 1 );
    uint8_t * p = packet;
    if ( kind == IOCP_ADMIT_GARBAGE )
    {
        // two stateless rungs: a wrong filter value, or too short to carry one
        if ( random.below( 2 ) == 0 )
        {
            rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_other_filter );
            return 1200;
        }
        rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_filter );
        return 3;
    }
    rocketnet_write_prefilter_header( &p, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_filter );
    return kind == IOCP_ADMIT_PLAYER ? 1200 : 600;
}

static int trace_iocp_admit( uint64_t seed, int steps, const char * out )
{
    iocp_admit_walk_t walk;
    memset( walk.arrived, 0, sizeof( walk.arrived ) );
    memset( walk.refused, 0, sizeof( walk.refused ) );
    memset( walk.full, 0, sizeof( walk.full ) );
    memset( walk.counted, 0, sizeof( walk.counted ) );
    memset( walk.delivered, 0, sizeof( walk.delivered ) );
    walk.evicted = false;

    tla_random_t random;
    random.state = seed;

    std::vector<uint8_t> packet( ROCKETNET_RECEIVE_CEILING + 1 );
    std::vector<tla_record_t> records;
    records.push_back( iocp_admit_record( walk ) );

    int arrivals = 0;
    int plant_fired = 0;
    for ( int step = 0; step < steps; ++step )
    {
        // Drain one step in five while the bank holds anything, so the
        // bank fills to capacity and refuses before main takes it
        if ( !walk.bank.empty() && random.below( 5 ) == 0 )
        {
            for ( int kind : walk.bank )
            {
                walk.delivered[kind]++;
            }
            walk.bank.clear();
            records.push_back( iocp_admit_record( walk ) );
            continue;
        }

        const int kind = random.below( IOCP_ADMIT_KINDS );
        const int bytes = iocp_admit_datagram( random, kind, packet.data() );
        const int drop =
            rocketnet_server_iocp_test_receive_admit( iocp_admit_filter, packet.data(), bytes, (int) walk.bank.size(), IOCP_ADMIT_CAPACITY );
        walk.arrived[kind]++;
        arrivals++;
        if ( drop == ROCKETNET_RECEIVE_ACCEPT )
        {
            walk.bank.push_back( kind );
            // garbage banked: only the plant's order lets it in
            plant_fired += kind == IOCP_ADMIT_GARBAGE ? 1 : 0;
        }
        else if ( drop == ROCKETNET_RECEIVE_DROP_BANK_FULL )
        {
            walk.full[kind]++;
            walk.counted[kind]++;
            for ( int held : walk.bank )
            {
                walk.evicted = walk.evicted || ( kind == IOCP_ADMIT_PLAYER && held == IOCP_ADMIT_GARBAGE );
            }
        }
        else
        {
            walk.refused[kind]++;
            walk.counted[kind]++;
        }
        records.push_back( iocp_admit_record( walk ) );
    }

    // the plant build must fire for its trace to be a witness; the clean
    // build never does
    printf( "PLANT-FIRED %d\n", plant_fired );

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    Capacity = %d\n    MaxArrivals = %d\n    FilterOn = TRUE\n    BankFirst = FALSE\n    UncountedFull = FALSE\n"
              "    CheckPastEnd = FALSE\n",
              IOCP_ADMIT_CAPACITY, arrivals + 1 );

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceIocpAdmitPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceIocpAdmitSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    return tla_write_trace( out, name, "TraceRNIocpReceiveAdmit", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "iocp-admit", trace_iocp_admit );

// ---------------------------------------------------------------------------

#define IOCP_LANES_LANES 3
#define IOCP_LANES_SIZE 4
#define IOCP_LANES_DESTS 6

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
};

static std::string iocp_lanes_sequence( const std::vector<std::string> & items )
{
    std::string text = "<<";
    for ( size_t i = 0; i < items.size(); ++i )
    {
        text += ( i > 0 ? ", " : "" ) + items[i];
    }
    return text + ">>";
}

static tla_record_t iocp_lanes_record( const iocp_lanes_walk_t & walk )
{
    std::vector<std::string> lane_keys;
    std::vector<std::string> free_values;
    std::vector<std::string> bank_values;
    for ( int l = 0; l < IOCP_LANES_LANES; ++l )
    {
        lane_keys.push_back( tla_int( l ) );
        int indexes[IOCP_LANES_LANES * IOCP_LANES_SIZE];
        const int count = rocketnet_server_iocp_test_lanes_free( walk.lanes, l, indexes, IOCP_LANES_LANES * IOCP_LANES_SIZE );
        std::vector<std::string> free_items;
        for ( int k = 0; k < count; ++k )
        {
            free_items.push_back( tla_int( indexes[k] ) );
        }
        free_values.push_back( iocp_lanes_sequence( free_items ) );
        std::vector<std::string> bank_items;
        for ( const iocp_lanes_staged_t & staged : walk.bank[l] )
        {
            bank_items.push_back( "<<" + tla_int( staged.op ) + ", " + tla_int( staged.dest ) + ", " + tla_int( staged.seq ) + ">>" );
        }
        bank_values.push_back( iocp_lanes_sequence( bank_items ) );
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
        posted_values.push_back( iocp_lanes_sequence( posted_items ) );
    }

    tla_record_t record;
    record.add( "free", tla_function( lane_keys, free_values ) );
    record.add( "bank", tla_function( lane_keys, bank_values ) );
    record.add( "inflight", inflight );
    record.add( "started", tla_function( dest_keys, started_values ) );
    record.add( "posted", tla_function( dest_keys, posted_values ) );
    return record;
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
    // trace as DestLane
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
    records.push_back( iocp_lanes_record( walk ) );

    int64_t starts = 0;
    int plant_fired = 0;
    bool refused = false;
    for ( int step = 0; step < steps && !refused; ++step )
    {
        const int action = random.below( 3 );
        if ( action == 0 )
        {
            // Start: an empty lane refuses and nothing changes, as the
            // model's guard says
            const int d = 1 + random.below( IOCP_LANES_DESTS );
            int lane = 0;
            const int op = rocketnet_server_iocp_test_lanes_acquire( walk.lanes, &walk.dest[d], &lane );
            if ( op < 0 )
            {
                continue;
            }
            walk.bank[lane].push_back( { op, d, walk.started[d] } );
            walk.started[d]++;
            starts++;
        }
        else if ( action == 1 )
        {
            // Post: one lane's bank, whole, in staged order
            const int l = random.below( IOCP_LANES_LANES );
            if ( walk.bank[l].empty() )
            {
                continue;
            }
            for ( const iocp_lanes_staged_t & staged : walk.bank[l] )
            {
                walk.inflight.push_back( staged.op );
                walk.posted[staged.dest].push_back( staged.seq );
            }
            walk.bank[l].clear();
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
            if ( !rocketnet_server_iocp_test_lanes_release( walk.lanes, op ) )
            {
                // only a release to the wrong lane finds it full; the walk
                // writes the state the refusal left (the operation in no
                // free queue and not in flight, which no model step makes)
                // and stops
                plant_fired++;
                refused = true;
                records.push_back( iocp_lanes_record( walk ) );
                continue;
            }
            // the release landed in the lane of its carve, or the plant moved it
            const int own_lane = op / IOCP_LANES_SIZE;
            int indexes[IOCP_LANES_LANES * IOCP_LANES_SIZE];
            const int count = rocketnet_server_iocp_test_lanes_free( walk.lanes, own_lane, indexes, IOCP_LANES_LANES * IOCP_LANES_SIZE );
            plant_fired += count > 0 && indexes[count - 1] == op ? 0 : 1;
        }
        records.push_back( iocp_lanes_record( walk ) );
    }

    // the plant build must fire for its trace to be a witness; the clean
    // build never does
    printf( "PLANT-FIRED %d\n", plant_fired );

    std::string dest_lane = "MCDestLane == (";
    for ( int d = 1; d <= IOCP_LANES_DESTS; ++d )
    {
        const int lane = rocketnet_server_iocp_test_lane_index( &walk.dest[d], IOCP_LANES_LANES );
        dest_lane += ( d > 1 ? " @@ " : "" ) + tla_int( d ) + " :> " + tla_int( lane );
    }
    dest_lane += ")\n";
    rocketnet_server_iocp_test_lanes_destroy( walk.lanes );

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    Lanes = %d\n    Size = %d\n    Dests = {1, 2, 3, 4, 5, 6}\n    DestLane <- MCDestLane\n    IndexBits = 16\n"
              "    MaxStarts = %d\n    AnyLane = FALSE\n    ReleaseMod = FALSE\n",
              IOCP_LANES_LANES, IOCP_LANES_SIZE, (int) starts + 1 );

#if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceIocpLanesPlantSeed" + tla_int( (int64_t) seed );
#else  // #if defined( ROCKETNET_TLA_PLANT )
    const std::string name = "MCTraceIocpLanesSeed" + tla_int( (int64_t) seed );
#endif // #if defined( ROCKETNET_TLA_PLANT )

    return tla_write_trace( out, name, "TraceRNIocpSendLanes", dest_lane, constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "iocp-lanes", trace_iocp_lanes );

#endif // #if ROCKETNET_DEV
