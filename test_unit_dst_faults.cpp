/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "test_unit_registry.h"
#include "dst_harness.h"

#include <math.h>
#include <string.h>

#define check rocketnet_unit_check

static uint64_t dst_fault_client_seed()
{
    for ( uint64_t seed = 1; seed < 10000; seed++ )
    {
        struct rocketnet_dst_fault_state_t state;
        rocketnet_dst_faults_init( &state, seed, ROCKETNET_DST_FAULT_CRASH_CLIENT, 500, 2 );
        for ( int i = 0; i < ROCKETNET_DST_CLIENT_CRASHES; i++ )
            if ( state.client[i].enabled && state.client[i].peer <= 2 )
                return seed;
    }
    return 0;
}

static uint64_t dst_fault_server_seed()
{
    for ( uint64_t seed = 1; seed < 10000; seed++ )
    {
        struct rocketnet_dst_fault_state_t state;
        rocketnet_dst_faults_init( &state, seed, ROCKETNET_DST_FAULT_CRASH_SERVER, 500, 2 );
        if ( state.server.enabled )
            return seed;
    }
    return 0;
}

static struct rocketnet_dst_options_t dst_fault_options( uint32_t faults )
{
    struct rocketnet_dst_options_t options;
    memset( &options, 0, sizeof(options) );
    options.steps = 2000;
    options.fault_steps = 500;
    options.shape = ROCKETNET_DST_SHAPE_PUSH;
    options.clients = 2;
    options.faults = faults;
    options.force_fail_step = -1;
    return options;
}

static void dst_fault_crash_restart_client()
{
    const uint64_t seed = dst_fault_client_seed();
    check( seed != 0 );
    struct rocketnet_dst_options_t options = dst_fault_options( ROCKETNET_DST_FAULT_CRASH_CLIENT );
    struct rocketnet_dst_result_t result;
    check( rocketnet_dst_run_seed_with_options( seed, &options, &result ) );
    check( result.passed );
    check( result.client_restarts > 0 );
    check( result.restarted_clients_connected > 0 );
}
REGISTER_UNIT_TEST( dst_fault_crash_restart_client );

static void dst_fault_server_restart()
{
    const uint64_t seed = dst_fault_server_seed();
    check( seed != 0 );
    struct rocketnet_dst_options_t options = dst_fault_options( ROCKETNET_DST_FAULT_CRASH_SERVER );
    struct rocketnet_dst_result_t result;
    check( rocketnet_dst_run_seed_with_options( seed, &options, &result ) );
    check( result.passed );
    check( result.server_restarts == 1 );
    check( result.connected_clients == options.clients );
}
REGISTER_UNIT_TEST( dst_fault_server_restart );

static void dst_fault_skew_bounded()
{
    struct rocketnet_dst_fault_state_t state;
    rocketnet_dst_faults_init( &state, 0x51EED, ROCKETNET_DST_FAULT_SKEW, 2000, 8 );
    int skewed_peers = 0;
    int observed_skew = 0;
    for ( int peer = 0; peer <= 8; peer++ )
    {
        if ( state.clock_offset[peer] != 0.0 || state.clock_rate[peer] != 1.0 )
            skewed_peers++;
        double previous = rocketnet_dst_faults_clock( &state, peer, 1000.0 );
        for ( int step = 1; step <= 4000; step++ )
        {
            const double truth = 1000.0 + step * 0.01;
            const double clock = rocketnet_dst_faults_clock( &state, peer, truth );
            const double bound = 0.25 + 0.005 * ( truth - 1000.0 );
            check( clock >= previous );
            check( fabs( clock - truth ) <= bound + 1e-12 );
            if ( clock != truth )
                observed_skew = 1;
            previous = clock;
        }
    }
    check( skewed_peers > 0 );
    check( observed_skew );
}
REGISTER_UNIT_TEST( dst_fault_skew_bounded );

static void dst_fault_reorder_delivers()
{
    struct rocketnet_dst_fault_state_t state;
    rocketnet_dst_faults_init( &state, 7, ROCKETNET_DST_FAULT_REORDER, 500, 1 );
    state.reorder_percent = 100;

    struct rocketnet_network_simulator_t * sim = rocketnet_network_simulator_create( NULL, 8, 1 );
    check( sim != NULL );
    rocketnet_network_simulator_config( sim, 0.005, 0.0, 0.0f, 0.0f );
    struct rocketnet_address_t from, to, received_from;
    check( rocketnet_address_parse( &from, "198.51.100.1:40000" ) );
    check( rocketnet_address_parse( &to, "198.51.100.2:40001" ) );

    const uint8_t held[] = { 1 };
    const uint8_t overtakes[] = { 2 };
    int reordered = 0;
    const double held_time = rocketnet_dst_faults_send_time( &state, 0, 1000.0, &reordered );
    check( reordered );
    rocketnet_network_simulator_send( sim, &from, &to, held, 1, held_time );
    state.reorder_percent = 0;
    const double next_time = rocketnet_dst_faults_send_time( &state, 0, 1000.0, &reordered );
    rocketnet_network_simulator_send( sim, &from, &to, overtakes, 1, next_time );

    uint8_t packet[ROCKETNET_RECEIVE_CEILING];
    int seen = 0;
    int bytes;
    while ( ( bytes = rocketnet_network_simulator_receive( sim, &to, 1002.0, &received_from, packet ) ) > 0 )
    {
        check( bytes == 1 );
        check( packet[0] == 1 || packet[0] == 2 );
        seen |= 1 << packet[0];
    }
    check( seen == ( ( 1 << 1 ) | ( 1 << 2 ) ) );
    rocketnet_network_simulator_destroy( sim );
}
REGISTER_UNIT_TEST( dst_fault_reorder_delivers );

// Filled only from an unchanged current-base Linux run of this exact seed and
// option tuple. Zero deliberately keeps the regression red until that frozen
// compatibility oracle has been measured and reviewed.
static const uint64_t ROCKETNET_DST_CORE_TRACE_D57 = 0;

static void dst_fault_off_same_trace()
{
    const struct rocketnet_dst_options_t options = dst_fault_options( 0 );
    struct rocketnet_dst_result_t result;
    check( ROCKETNET_DST_CORE_TRACE_D57 != 0 );
    check( rocketnet_dst_run_seed_with_options( 0xD57, &options, &result ) );
    check( result.passed );
    check( result.trace_hash == ROCKETNET_DST_CORE_TRACE_D57 );
}
REGISTER_UNIT_TEST( dst_fault_off_same_trace );
