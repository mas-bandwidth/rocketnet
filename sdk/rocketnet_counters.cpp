/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#include "rocketnet_counters.h"
#include "rocketnet_internal.h"
#include "rocketnet_wire.h"

#include <stdio.h>

int rocketnet_num_counters() { return ROCKETNET_NUM_COUNTERS; }

uint64_t rocketnet_counter_value( const rocketnet_counters_t * counters, int counter )
{
    if ( !rocketnet_api_null_guard( counters ) )
        return 0;

    return counter >= 0 && counter < ROCKETNET_NUM_COUNTERS ? counters->values[counter] : 0;
}

// The name, class and family tables are generated from the counter list in
// rocketnet_counters.h, one row each, so an index, its name, its class and its
// family cannot drift apart.

static const char * const rocketnet_counter_names[ROCKETNET_NUM_COUNTERS] = {
#define ROCKETNET_COUNTER_NAME_ROW( name, klass, family ) #name,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_NAME_ROW )
#undef ROCKETNET_COUNTER_NAME_ROW
};

static const uint8_t rocketnet_counter_classes[ROCKETNET_NUM_COUNTERS] = {
#define ROCKETNET_COUNTER_CLASS_ROW( name, klass, family ) ROCKETNET_COUNTER_CLASS_##klass,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_CLASS_ROW )
#undef ROCKETNET_COUNTER_CLASS_ROW
};

static const int16_t rocketnet_counter_families[ROCKETNET_NUM_COUNTERS] = {
#define ROCKETNET_COUNTER_FAMILY_ROW( name, klass, family ) ROCKETNET_COUNTER_##family,
    ROCKETNET_COUNTER_ROWS( ROCKETNET_COUNTER_FAMILY_ROW )
#undef ROCKETNET_COUNTER_FAMILY_ROW
};

const char * rocketnet_counter_name( int counter )
{
    if ( counter < 0 || counter >= ROCKETNET_NUM_COUNTERS )
    {
        rocketnet_assert( 0 );
        return "???";
    }

    return rocketnet_counter_names[counter];
}

int rocketnet_counter_class( int counter )
{
    if ( counter < 0 || counter >= ROCKETNET_NUM_COUNTERS )
        return -1;

    return rocketnet_counter_classes[counter];
}

#if ROCKETNET_DEV
const char * rocketnet_counter_class_name( int counter_class )
{
    switch ( counter_class )
    {
        case ROCKETNET_COUNTER_CLASS_REFUSAL:   return "refusal";
        case ROCKETNET_COUNTER_CLASS_EXPECTED:  return "expected";
        case ROCKETNET_COUNTER_CLASS_PATH_LOSS: return "path_loss";
        case ROCKETNET_COUNTER_CLASS_ARRIVAL:   return "arrival";
        default:                                return NULL;
    }
}
#endif // #if ROCKETNET_DEV

// RECEIVED == the sum of its family's outcomes, for every family, checked in
// index order of the RECEIVED counter so the first unbalanced family is the
// same one every time. A counter is a RECEIVED counter when some counter's
// family names it; the sum of a family is every counter whose family it is.

void rocketnet_counters_apply_prefilter( struct rocketnet_counters_t * counters, const uint64_t * prefilter )
{
    // one transport counter per kernel prefilter counter, in
    // ROCKETNET_SERVER_XDP_COUNTER_* index order

    static const int rocketnet_prefilter_table[ROCKETNET_SERVER_XDP_NUM_COUNTERS] = {
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
    static_assert( sizeof( rocketnet_prefilter_table ) / sizeof( rocketnet_prefilter_table[0] ) == ROCKETNET_SERVER_XDP_NUM_COUNTERS,
                   "a new kernel prefilter counter needs its transport row" );

    if ( !rocketnet_api_null_guard( counters ) )
        return;
    if ( !rocketnet_api_null_guard( prefilter ) )
        return;

    for ( int i = 0; i < ROCKETNET_SERVER_XDP_NUM_COUNTERS; i++ )
        counters->values[rocketnet_prefilter_table[i]] = prefilter[i];
}

int rocketnet_counters_conserved( const struct rocketnet_counters_t * counters, char * report, int report_bytes )
{
    if ( report && report_bytes > 0 )
        report[0] = '\0';

    if ( !rocketnet_api_null_guard( counters ) )
    {
        if ( report && report_bytes > 0 )
            snprintf( report, (size_t) report_bytes, "counters is NULL" );
        return 0;
    }

    for ( int received = 0; received < ROCKETNET_NUM_COUNTERS; received++ )
    {
        uint64_t outcomes = 0;
        int is_received_counter = 0;
        for ( int i = 0; i < ROCKETNET_NUM_COUNTERS; i++ )
        {
            if ( rocketnet_counter_families[i] == received )
            {
                outcomes += counters->values[i];
                is_received_counter = 1;
            }
        }

        if ( is_received_counter && counters->values[received] != outcomes )
        {
            if ( report && report_bytes > 0 )
                snprintf( report, (size_t) report_bytes, "%s received=%llu outcomes=%llu",
                          rocketnet_counter_names[received],
                          (unsigned long long) counters->values[received],
                          (unsigned long long) outcomes );
            return 0;
        }
    }

    return 1;
}
