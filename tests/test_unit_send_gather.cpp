/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The transport-neutral send gather (sdk/rocketnet_send_gather.h), at the
// cases proofs/tla/RNSendGatherLayout.tla's reversed witnesses break: a run
// never crosses destinations, a destination's packets keep their staged
// order, both caps cut runs, a smaller packet ends its run, a full table
// probes round its end. Every refusal is checked. Pure memory: no socket,
// no clock.

#include "test_unit_registry.h"

#include "../sdk/rocketnet.h"
#include "../sdk/rocketnet_address.h"
#include "../sdk/rocketnet_send_gather.h"

#include <string.h>
#include <vector>

#define check rocketnet_unit_check

struct gather_test_slot_t
{
    int packet_bytes;
    struct rocketnet_address_t to;
};

static struct rocketnet_address_t gather_test_address( int d )
{
    struct rocketnet_address_t a;
    memset( &a, 0, sizeof( a ) );
    a.type = ROCKETNET_ADDRESS_IPV4;
    a.data.ipv4[0] = 10;
    a.data.ipv4[2] = (uint8_t) ( d >> 8 );
    a.data.ipv4[3] = (uint8_t) d;
    a.port = 40000;
    return a;
}

struct gather_test_t
{
    std::vector<int> memory;
    struct rocketnet_send_gather_scratch_t scratch;
    std::vector<gather_test_slot_t> bank;
    std::vector<int> order;
    std::vector<struct rocketnet_send_gather_run_t> runs;
    int num_runs;

    explicit gather_test_t( int max_slots )
        : memory( rocketnet_send_gather_scratch_bytes( max_slots ) / sizeof( int ) ), bank(), order( (size_t) max_slots ),
          runs( (size_t) max_slots ), num_runs( 0 )
    {
        check( rocketnet_send_gather_scratch_init( &scratch, memory.data(), max_slots ) );
    }

    void stage( int d, int bytes )
    {
        gather_test_slot_t slot;
        slot.packet_bytes = bytes;
        slot.to = gather_test_address( d );
        bank.push_back( slot );
    }

    int gather( int max_segments, int max_bytes )
    {
        num_runs = rocketnet_send_gather( &bank[0].to, sizeof( gather_test_slot_t ), &bank[0].packet_bytes, sizeof( gather_test_slot_t ),
                                          (int) bank.size(), max_segments, max_bytes, &scratch, order.data(), runs.data() );
        return num_runs;
    }

    // run r's slots, in order
    std::vector<int> run( int r ) const
    {
        return std::vector<int>( order.begin() + runs[(size_t) r].first, order.begin() + runs[(size_t) r].first + runs[(size_t) r].count );
    }
};

static void test_send_gather_refuses_bad_arguments()
{
    check( rocketnet_send_gather_scratch_bytes( 0 ) == 0 );
    check( rocketnet_send_gather_scratch_bytes( -1 ) == 0 );

    struct rocketnet_send_gather_scratch_t scratch;
    int memory[64];
    check( !rocketnet_send_gather_scratch_init( &scratch, NULL, 4 ) );
    check( !rocketnet_send_gather_scratch_init( &scratch, memory, 0 ) );
    check( rocketnet_send_gather_scratch_init( &scratch, memory, 4 ) );
    check( scratch.buckets == 8 );
    check( rocketnet_send_gather_scratch_bytes( 4 ) == ( 8 + 2 * 4 ) * sizeof( int ) );

    gather_test_t t( 4 );
    for ( int i = 0; i < 5; i++ )
    {
        t.stage( 1, 100 );
    }
    t.order.resize( 5 );
    t.runs.resize( 5 );
    check( t.gather( 64, 65000 ) == -1 );    // more slots than the scratch holds
    t.bank.resize( 4 );
    check( t.gather( 0, 65000 ) == -1 );
    check( t.gather( 64, 0 ) == -1 );
    check( t.gather( 64, 65000 ) == 1 );

    // nothing staged: no runs
    check( rocketnet_send_gather( NULL, 0, NULL, 0, 0, 64, 65000, &t.scratch, NULL, NULL ) == 0 );
}

REGISTER_UNIT_TEST( test_send_gather_refuses_bad_arguments );

// interleaved staging to three destinations: each destination's packets
// are one run, in staged order, destinations in order of their first slot;
// an aborted slot is in no run (witnesses MatchAny and AppendHead)

static void test_send_gather_groups_in_staged_order()
{
    gather_test_t t( 9 );
    const int d[] = { 2, 1, 3, 2, 1, 3, 2, 1, 3 };
    for ( int i = 0; i < 9; i++ )
    {
        t.stage( d[i], i == 4 ? 0 : 1200 );
    }

    check( t.gather( 64, 65000 ) == 3 );
    const std::vector<int> r0 = t.run( 0 );
    const std::vector<int> r1 = t.run( 1 );
    const std::vector<int> r2 = t.run( 2 );
    check( r0.size() == 3 && r0[0] == 0 && r0[1] == 3 && r0[2] == 6 );
    check( r1.size() == 2 && r1[0] == 1 && r1[1] == 7 );
    check( r2.size() == 3 && r2[0] == 2 && r2[1] == 5 && r2[2] == 8 );
    for ( int r = 0; r < 3; r++ )
    {
        check( t.runs[(size_t) r].segment_bytes == 1200 && t.runs[(size_t) r].last_bytes == 1200 );
    }
}

REGISTER_UNIT_TEST( test_send_gather_groups_in_staged_order );

// the segment limit and the byte cap each cut a run, and a packet over the
// byte cap is a run of its own (witness NoByteCap)

static void test_send_gather_caps_cut_runs()
{
    gather_test_t t( 8 );
    for ( int i = 0; i < 7; i++ )
    {
        t.stage( 1, 1000 );
    }
    check( t.gather( 3, 65000 ) == 3 );
    check( t.runs[0].count == 3 && t.runs[1].count == 3 && t.runs[2].count == 1 );

    check( t.gather( 64, 2500 ) == 4 );
    check( t.runs[0].count == 2 && t.runs[1].count == 2 && t.runs[2].count == 2 && t.runs[3].count == 1 );

    gather_test_t big( 4 );
    big.stage( 1, 500 );
    big.stage( 1, 3000 );
    big.stage( 1, 500 );
    check( big.gather( 64, 2000 ) == 3 );
    check( big.runs[1].count == 1 && big.runs[1].segment_bytes == 3000 );
}

REGISTER_UNIT_TEST( test_send_gather_caps_cut_runs );

// a smaller packet is the last of its run; a larger one starts a new run
// (witness ShortOpen)

static void test_send_gather_smaller_packet_ends_its_run()
{
    gather_test_t t( 6 );
    t.stage( 1, 1200 );
    t.stage( 1, 900 );
    t.stage( 1, 900 );
    t.stage( 1, 1200 );
    t.stage( 1, 1300 );
    // 1200 900 | 900 | 1200 | 1300: the first 900 ends its run, so the
    // second starts one, and each larger packet starts its own
    check( t.gather( 64, 65000 ) == 4 );
    check( t.runs[0].first == 0 && t.runs[0].count == 2 && t.runs[0].segment_bytes == 1200 && t.runs[0].last_bytes == 900 );
    check( t.runs[1].first == 2 && t.runs[1].count == 1 && t.runs[1].segment_bytes == 900 );
    check( t.runs[2].first == 3 && t.runs[2].count == 1 && t.runs[2].segment_bytes == 1200 );
    check( t.runs[3].first == 4 && t.runs[3].count == 1 && t.runs[3].segment_bytes == 1300 );
}

REGISTER_UNIT_TEST( test_send_gather_smaller_packet_ends_its_run );

// a full table: as many destinations as slots, so every probe chain is
// long and wraps the table's end (witness NoProbeMod)

static void test_send_gather_full_table_wraps()
{
    const int n = 256;
    gather_test_t t( n );
    for ( int i = 0; i < n; i++ )
    {
        t.stage( i, 1000 );
    }
    check( t.gather( 64, 65000 ) == n );
    for ( int r = 0; r < n; r++ )
    {
        check( t.runs[(size_t) r].count == 1 && t.order[(size_t) r] == r );
    }
}

REGISTER_UNIT_TEST( test_send_gather_full_table_wraps );
