/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The IOCP admission layer of the TLA+ trace check
// (proofs/tla/README-TraceCheck.md), layer iocp-admit: the completion
// worker's admission of a received datagram to its receive bank
// (proofs/tla/RNIocpReceiveAdmit.tla), driven through
// rocketnet_iocp_receive_admit by its development seam. Each step either
// completes one datagram of a random kind (a player's, garbage that fails a
// stateless rung, or hostile: well formed with the server's filter value)
// and banks or counts it by the code's verdict, as
// rocketnet_iocp_bank_datagram does, or drains the bank as main does. The
// record's act is the verdict the call returned (Refuse for a prefilter
// reason, Bank for ACCEPT, Full for BANK_FULL), its argument the kind.
//
// The bank, the tallies and the eviction flag are the walk's, as the
// bank_datagram path keeps them from the verdict: the code under test is the
// verdict, and the model checks it against the bank's fill and the kind.
//
// The plant (ROCKETNET_IOCP_RECEIVE_ADMIT_PLANT) is the capacity check
// first, the order the blast row measured: garbage then meets the capacity
// check, and is banked or refused BANK_FULL where the clean code refuses it
// by its prefilter reason. That verdict on garbage is the planted effect.
//
// The walk's code compiles in every development build, so it needs no
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
#include "../sdk/rocketnet_server_iocp_windows.cpp"
#endif // #if defined( ROCKETNET_TLA_PLANT )

#if ROCKETNET_DEV

int rocketnet_server_iocp_test_receive_admit( const uint8_t * packet_filter, const uint8_t * packet_data, int packet_bytes,
                                              int num_packets, int capacity );

#define IOCP_ADMIT_CAPACITY 4

enum
{
    IOCP_ADMIT_PLAYER = 0,
    IOCP_ADMIT_GARBAGE = 1,
    IOCP_ADMIT_HOSTILE = 2,
    IOCP_ADMIT_KINDS = 3
};

static const char * const iocp_admit_kind_names[IOCP_ADMIT_KINDS] = { "player", "garbage", "hostile" };

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
        keys.push_back( tla_string( iocp_admit_kind_names[k] ) );
        texts.push_back( tla_int( values[k] ) );
    }
    return tla_function( keys, texts );
}

static tla_record_t iocp_admit_record( const iocp_admit_walk_t & walk )
{
    std::vector<std::string> bank;
    for ( size_t i = 0; i < walk.bank.size(); ++i )
    {
        bank.push_back( tla_string( iocp_admit_kind_names[walk.bank[i]] ) );
    }

    tla_record_t record;
    record.add( "bank", tla_sequence( bank ) );
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
    tla_report_t report;
    records.push_back( tla_step( "init", "init", "<<>>", iocp_admit_record( walk ) ) );

    int arrivals = 0;
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
            report.act( "Drain" );
            records.push_back( tla_step( "drain", "Drain", "<<>>", iocp_admit_record( walk ) ) );
            continue;
        }

        const int kind = random.below( IOCP_ADMIT_KINDS );
        const int bytes = iocp_admit_datagram( random, kind, packet.data() );
        const int drop =
            rocketnet_server_iocp_test_receive_admit( iocp_admit_filter, packet.data(), bytes, (int) walk.bank.size(), IOCP_ADMIT_CAPACITY );
        walk.arrived[kind]++;
        arrivals++;
        const char * act = NULL;
        if ( drop == ROCKETNET_RECEIVE_ACCEPT )
        {
            act = "Bank";
            walk.bank.push_back( kind );
        }
        else if ( drop == ROCKETNET_RECEIVE_DROP_BANK_FULL )
        {
            act = "Full";
            walk.full[kind]++;
            walk.counted[kind]++;
            for ( int held : walk.bank )
            {
                walk.evicted = walk.evicted || ( kind == IOCP_ADMIT_PLAYER && held == IOCP_ADMIT_GARBAGE );
            }
        }
        else
        {
            act = "Refuse";
            walk.refused[kind]++;
            walk.counted[kind]++;
        }
        report.act( act );
        records.push_back( tla_step( "arrive", act, tla_sequence( std::vector<std::string>( 1, tla_string( iocp_admit_kind_names[kind] ) ) ),
                                     iocp_admit_record( walk ) ) );
        // the planted effect, where it shows: garbage met the capacity
        // check (banked, or refused BANK_FULL), which the clean code's
        // prefilter never lets it reach while the filter is published
        if ( kind == IOCP_ADMIT_GARBAGE && ( drop == ROCKETNET_RECEIVE_ACCEPT || drop == ROCKETNET_RECEIVE_DROP_BANK_FULL ) )
        {
            report.planted( (int) records.size() );
        }
    }

    report.print();

    char constants[512];
    snprintf( constants, sizeof( constants ),
              "    Capacity = %d\n    MaxArrivals = %d\n    FilterOn = TRUE\n    BankFirst = FALSE\n    UncountedFull = FALSE\n"
              "    CheckPastEnd = FALSE\n",
              IOCP_ADMIT_CAPACITY, arrivals + 1 );

    return tla_write_trace( out, "MCTraceIocpAdmit", seed, "TraceRNIocpReceiveAdmit", "", constants, records ) ? 0 : 1;
}

TLA_TRACE_LAYER( "iocp-admit", trace_iocp_admit );

#endif // #if ROCKETNET_DEV
