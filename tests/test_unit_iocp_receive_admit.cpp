/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The IOCP completion worker's admission of one received datagram to its
// receive bank (proofs/tla/RNIocpReceiveAdmit.tla): the worker prefilter runs
// before the capacity check, so a datagram main's stateless rungs refuse
// never takes a slot and a full bank refuses only datagrams the prefilter
// accepted. Pure: no socket, no thread, no time. The admission compiles in
// every development build, so these run on Linux and macOS too.

#include "test_unit_registry.h"

#include "../sdk/rocketnet_internal.h"
#include "../sdk/rocketnet_protocol.h"
#include "../sdk/rocketnet_receive_prefilter.h"
#include "../sdk/rocketnet_wire.h"

#include <string.h>

#if ROCKETNET_DEV

int rocketnet_server_iocp_test_receive_admit( const uint8_t * packet_filter, const uint8_t * packet_data, int packet_bytes,
                                              int num_packets, int capacity );

static const uint8_t iocp_admit_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x11, 0x22, 0x33, 0x44 };
static const uint8_t iocp_admit_other_filter[ROCKETNET_FILTER_VALUE_BYTES] = { 0x11, 0x22, 0x33, 0x45 };

static int iocp_admit_packet( uint8_t * packet, uint8_t type, const uint8_t * filter, int bytes )
{
    memset( packet, 0xA5, ROCKETNET_RECEIVE_CEILING + 1 );
    uint8_t * p = packet;
    rocketnet_write_prefilter_header( &p, type, filter );
    return bytes;
}

// The prefilter's reason wins over a full bank: garbage at a full bank is
// named by what is wrong with it, never counted as a bank-full drop.

static void test_iocp_receive_admit_filters_before_capacity()
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING + 1];
    const int capacity = 4;

    const int garbage = iocp_admit_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_other_filter, 1200 );
    for ( int num_packets = 0; num_packets <= capacity; ++num_packets )
    {
        rocketnet_unit_check( rocketnet_server_iocp_test_receive_admit( iocp_admit_filter, packet, garbage, num_packets, capacity ) ==
                              ROCKETNET_RECEIVE_DROP_WRONG_VALUE );
    }

    const int payload = iocp_admit_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_filter, 1200 );
    for ( int num_packets = 0; num_packets < capacity; ++num_packets )
    {
        rocketnet_unit_check( rocketnet_server_iocp_test_receive_admit( iocp_admit_filter, packet, payload, num_packets, capacity ) ==
                              ROCKETNET_RECEIVE_ACCEPT );
    }
    rocketnet_unit_check( rocketnet_server_iocp_test_receive_admit( iocp_admit_filter, packet, payload, capacity, capacity ) ==
                          ROCKETNET_RECEIVE_DROP_BANK_FULL );
}

REGISTER_UNIT_TEST( test_iocp_receive_admit_filters_before_capacity );

// Before the filter is published every datagram is offered to the bank, and
// only the capacity refuses: the protocol filters on main, as before.

static void test_iocp_receive_admit_without_filter_banks_everything()
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING + 1];
    const int garbage = iocp_admit_packet( packet, ROCKETNET_CONNECTION_CHALLENGE_PACKET, iocp_admit_other_filter, 3 );
    rocketnet_unit_check( rocketnet_server_iocp_test_receive_admit( NULL, packet, garbage, 0, 2 ) == ROCKETNET_RECEIVE_ACCEPT );
    rocketnet_unit_check( rocketnet_server_iocp_test_receive_admit( NULL, packet, garbage, 1, 2 ) == ROCKETNET_RECEIVE_ACCEPT );
    rocketnet_unit_check( rocketnet_server_iocp_test_receive_admit( NULL, packet, garbage, 2, 2 ) == ROCKETNET_RECEIVE_DROP_BANK_FULL );
}

REGISTER_UNIT_TEST( test_iocp_receive_admit_without_filter_banks_everything );

// With room, the admission is exactly the shared prefilter: every reason
// main's stateless rungs name, and acceptance, for each type and a spread of
// sizes, so the worker refuses nothing main would accept and names each
// refusal as main would.

static void test_iocp_receive_admit_matches_prefilter_with_room()
{
    uint8_t packet[ROCKETNET_RECEIVE_CEILING + 1];
    const int sizes[] = { 1, 5, 6, 33, 34, 35, 100, 709, 710, 711, 1200, ROCKETNET_RECEIVE_CEILING, ROCKETNET_RECEIVE_CEILING + 1 };
    int reasons_seen[ROCKETNET_RECEIVE_NUM_DROPS] = {};

    for ( int type = 0; type < 12; ++type )
    {
        for ( int filter = 0; filter < 2; ++filter )
        {
            for ( size_t s = 0; s < sizeof( sizes ) / sizeof( sizes[0] ); ++s )
            {
                const int bytes = iocp_admit_packet( packet, (uint8_t) type, filter ? iocp_admit_filter : iocp_admit_other_filter, sizes[s] );
                const int expected = (int) rocketnet_receive_prefilter( packet, bytes, iocp_admit_filter );
                const int admitted = rocketnet_server_iocp_test_receive_admit( iocp_admit_filter, packet, bytes, 0, 1 );
                rocketnet_unit_check( admitted == expected );
                rocketnet_unit_check( admitted != ROCKETNET_RECEIVE_DROP_BANK_FULL );
                reasons_seen[admitted]++;
            }
        }
    }

    // a wrong epoch, with the filter right

    iocp_admit_packet( packet, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_filter, 100 );
    packet[1] = ROCKETNET_FILTER_EPOCH_UNKEYED + 1;
    const int epoch = rocketnet_server_iocp_test_receive_admit( iocp_admit_filter, packet, 100, 0, 1 );
    rocketnet_unit_check( epoch == ROCKETNET_RECEIVE_DROP_WRONG_EPOCH );
    reasons_seen[epoch]++;

    // every prefilter outcome is reached by the walk above

    for ( int drop = ROCKETNET_RECEIVE_ACCEPT; drop < ROCKETNET_RECEIVE_DROP_BANK_FULL; ++drop )
    {
        rocketnet_unit_check( reasons_seen[drop] > 0 );
    }
}

REGISTER_UNIT_TEST( test_iocp_receive_admit_matches_prefilter_with_room );

// The scenario the blast row measured: a bank of 4 meets a flood of garbage
// and then the players' datagrams. With the filter on, every player datagram
// is banked; with it off (the old order), the garbage fills the bank first
// and the players are dropped as bank-full.

static void test_iocp_receive_admit_garbage_never_evicts_players()
{
    uint8_t garbage[ROCKETNET_RECEIVE_CEILING + 1];
    uint8_t player[ROCKETNET_RECEIVE_CEILING + 1];
    const int garbage_bytes = iocp_admit_packet( garbage, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_other_filter, 1200 );
    const int player_bytes = iocp_admit_packet( player, ROCKETNET_CONNECTION_PAYLOAD_PACKET, iocp_admit_filter, 1200 );
    const int capacity = 4;

    for ( int filtered = 0; filtered < 2; ++filtered )
    {
        const uint8_t * filter = filtered ? iocp_admit_filter : NULL;
        int num_packets = 0;
        int players_banked = 0;
        int players_full = 0;
        int garbage_refused = 0;
        for ( int i = 0; i < 16 + capacity; ++i )
        {
            const bool is_player = i >= 16;
            const int drop = rocketnet_server_iocp_test_receive_admit( filter, is_player ? player : garbage,
                                                                       is_player ? player_bytes : garbage_bytes, num_packets, capacity );
            if ( drop == ROCKETNET_RECEIVE_ACCEPT )
            {
                num_packets++;
                players_banked += is_player ? 1 : 0;
            }
            else if ( drop == ROCKETNET_RECEIVE_DROP_BANK_FULL )
            {
                players_full += is_player ? 1 : 0;
            }
            else
            {
                rocketnet_unit_check( !is_player && drop == ROCKETNET_RECEIVE_DROP_WRONG_VALUE );
                garbage_refused++;
            }
        }
        if ( filtered )
        {
            rocketnet_unit_check( players_banked == capacity && players_full == 0 && garbage_refused == 16 );
        }
        else
        {
            rocketnet_unit_check( players_banked == 0 && players_full == capacity && garbage_refused == 0 );
        }
    }
}

REGISTER_UNIT_TEST( test_iocp_receive_admit_garbage_never_evicts_players );

#endif // #if ROCKETNET_DEV
