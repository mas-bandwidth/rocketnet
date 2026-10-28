/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The kernel prefilter counters (sdk/rocketnet_wire.h
// ROCKETNET_SERVER_XDP_COUNTER_*) surface through the transport counter
// surface via rocketnet_counters_apply_prefilter. Every case runs in memory:
// no transport, no transport, no kernel.

#include "test_unit_registry.h"

#include "rocketnet_internal.h"
#include "rocketnet_counters.h"
#include "rocketnet_wire.h"

#define ROCKETNET_COUNTERS_CHECK rocketnet_unit_check
#include "counters_expect.h"

#include <stdint.h>
#include <string.h>

#define check rocketnet_unit_check

// the transport counter of each kernel prefilter counter, in wire order

static const int prefilter_kernel_counters[] = {
    ROCKETNET_COUNTER_XDP_KERNEL_REDIRECTED,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_SMALL,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_TOO_LARGE,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_FILTER,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_INVALID_TYPE,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_WRONG_EPOCH,
    ROCKETNET_COUNTER_XDP_KERNEL_PASSED_IPV6_EXTENSION,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_REPLAYED,
    ROCKETNET_COUNTER_XDP_KERNEL_PASSED_FRAGMENT,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_FIRST_FRAGMENT,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_NO_SOCKET,
    ROCKETNET_COUNTER_XDP_KERNEL_DROPPED_IP_OPTIONS_OR_EXTENSION,
};
static_assert( sizeof( prefilter_kernel_counters ) / sizeof( prefilter_kernel_counters[0] ) == ROCKETNET_SERVER_XDP_NUM_COUNTERS,
               "every kernel prefilter counter needs a test mapping" );

static void test_prefilter_counters_map_each_kernel_counter()
{
    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
    {
        struct rocketnet_counters_t counters;
        memset( &counters, 0, sizeof( counters ) );
        counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] = 7;

        uint64_t prefilter[ROCKETNET_SERVER_XDP_NUM_COUNTERS] = { 0 };
        prefilter[i] = 1000 + (uint64_t) i;

        rocketnet_counters_apply_prefilter( &counters, prefilter );

        check( counters.values[prefilter_kernel_counters[i]] == 1000 + (uint64_t) i );
        for ( int j = 0; j < ROCKETNET_SERVER_XDP_NUM_COUNTERS; j++ )
            if ( j != i )
                check( counters.values[prefilter_kernel_counters[j]] == 0 );
        check( counters.values[ROCKETNET_COUNTER_PACKETS_RECEIVED] == 7 );
    }
}

REGISTER_UNIT_TEST( test_prefilter_counters_map_each_kernel_counter );

static void test_prefilter_counters_are_assigned_not_added()
{
    struct rocketnet_counters_t counters;
    uint64_t seeded[ROCKETNET_NUM_COUNTERS];
    for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
    {
        seeded[i] = UINT64_C( 0xA000000000000000 ) + (uint64_t) i;
        counters.values[i] = seeded[i];
    }

    const uint64_t first[ROCKETNET_SERVER_XDP_NUM_COUNTERS] = {
        101,
        203,
        307,
        409,
        503,
        607,
        709,
        811,
        907,
        1009,
        1103,
        1201,
    };
    rocketnet_counters_apply_prefilter( &counters, first );

    for ( int counter = 0; counter < ROCKETNET_NUM_COUNTERS; counter++ )
    {
        int mapped = -1;
        for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
            if ( prefilter_kernel_counters[i] == counter )
                mapped = i;
        check( counters.values[counter] == ( mapped < 0 ? seeded[counter] : first[mapped] ) );
    }

    const uint64_t second[ROCKETNET_SERVER_XDP_NUM_COUNTERS] = {
        1301,
        1409,
        1511,
        1601,
        1709,
        1801,
        1901,
        2003,
        2111,
        2203,
        2309,
        2411,
    };
    rocketnet_counters_apply_prefilter( &counters, second );

    for ( int counter = 0; counter < ROCKETNET_NUM_COUNTERS; counter++ )
    {
        int mapped = -1;
        for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
            if ( prefilter_kernel_counters[i] == counter )
                mapped = i;
        check( counters.values[counter] == ( mapped < 0 ? seeded[counter] : second[mapped] ) );
    }
}

REGISTER_UNIT_TEST( test_prefilter_counters_are_assigned_not_added );
