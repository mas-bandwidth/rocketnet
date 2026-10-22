/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST command line (docs/DST.md section 2): one seed drives one
    single-process run of N clients and a server over the in-memory network
    simulator, the message layer on every channel type. A passing range prints
    one PASS line and exits 0; a failing seed prints its FAIL line and the
    command that replays it, and the run ends with exit 1; a usage error exits
    2. No wall clock, no socket, no thread.
*/

#include "rocketnet.h"
#include "dst_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct rocketnet_dst_substream_t dst_keys_stream;

static const char * dst_keys_name( void )
{
    return "dst-keys";
}

static uint32_t dst_keys_random( void )
{
    return (uint32_t) rocketnet_dst_substream_draw( &dst_keys_stream );
}

static void dst_keys_buf( void * const buffer, const size_t size )
{
    uint8_t * out = (uint8_t*) buffer;
    for ( size_t i = 0; i < size; i++ )
        out[i] = (uint8_t) rocketnet_dst_substream_draw( &dst_keys_stream );
}

static struct randombytes_implementation dst_keys_implementation =
{
    dst_keys_name,
    dst_keys_random,
    NULL,
    NULL,
    dst_keys_buf,
    NULL,
};

static int dst_fault_name( const char * name, size_t bytes, uint32_t * mask )
{
    struct
    {
        const char * name;
        uint32_t bit;
    } kinds[] =
    {
        { "loss", ROCKETNET_DST_FAULT_LOSS },
        { "dup", ROCKETNET_DST_FAULT_DUP },
        { "jitter", ROCKETNET_DST_FAULT_JITTER },
        { "reorder", ROCKETNET_DST_FAULT_REORDER },
        { "delay", ROCKETNET_DST_FAULT_DELAY },
        { "skew", ROCKETNET_DST_FAULT_SKEW },
        { "crash-client", ROCKETNET_DST_FAULT_CRASH_CLIENT },
        { "crash-server", ROCKETNET_DST_FAULT_CRASH_SERVER },
    };
    for ( size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++ )
    {
        if ( strlen( kinds[i].name ) == bytes && memcmp( kinds[i].name, name, bytes ) == 0 )
        {
            *mask |= kinds[i].bit;
            return 1;
        }
    }
    return 0;
}

static int dst_parse_faults( const char * list, uint32_t * mask )
{
    *mask = 0;
    if ( strcmp( list, "none" ) == 0 )
        return 1;
    if ( strcmp( list, "all" ) == 0 )
    {
        *mask = ROCKETNET_DST_FAULT_ALL;
        return 1;
    }
    const char * p = list;
    while ( *p )
    {
        const char * comma = strchr( p, ',' );
        const size_t bytes = comma ? (size_t) ( comma - p ) : strlen( p );
        if ( bytes == 0 || !dst_fault_name( p, bytes, mask ) )
            return 0;
        p += bytes;
        if ( *p == ',' )
            p++;
    }
    return 1;
}

int main( int argc, char ** argv )
{
    struct rocketnet_dst_options_t options;
    memset( &options, 0, sizeof(options) );
    options.shape = ROCKETNET_DST_SHAPE_FULL;
    options.faults = ROCKETNET_DST_FAULT_ALL;
    options.force_fail_step = -1;

    uint64_t seeds_a = 0;
    uint64_t seeds_b = 0;
    int have_seeds = 0;
    int max_fails = 20;
    int verbose = 0;
    int have_steps = 0;

    for ( int i = 1; i < argc; i++ )
    {
        const char * arg = argv[i];
        if ( strcmp( arg, "--seeds" ) == 0 && i + 1 < argc )
        {
            const char * range = argv[++i];
            // a-b: an unsigned number, a '-', an unsigned number
            char * a_end = NULL;
            char * b_end = NULL;
            const unsigned long long a = strtoull( range, &a_end, 10 );
            const bool have_a = a_end != range && *a_end == '-';
            const unsigned long long b = have_a ? strtoull( a_end + 1, &b_end, 10 ) : 0;
            const bool have_b = have_a && b_end != a_end + 1;
            if ( !have_b || a > b )
            {
                fprintf( stderr, "dst: --seeds needs a-b with a <= b\n" );
                return 2;
            }
            seeds_a = a;
            seeds_b = b;
            have_seeds = 1;
        }
        else if ( strcmp( arg, "--shape" ) == 0 && i + 1 < argc )
        {
            const char * shape = argv[++i];
            if ( strcmp( shape, "push" ) == 0 )
                options.shape = ROCKETNET_DST_SHAPE_PUSH;
            else if ( strcmp( shape, "full" ) == 0 )
                options.shape = ROCKETNET_DST_SHAPE_FULL;
            else
            {
                fprintf( stderr, "dst: --shape is push or full\n" );
                return 2;
            }
        }
        else if ( strcmp( arg, "--clients" ) == 0 && i + 1 < argc )
        {
            options.clients = atoi( argv[++i] );
            if ( options.clients < 1 || options.clients > ROCKETNET_DST_MAX_CLIENTS )
            {
                fprintf( stderr, "dst: --clients must be 1..16\n" );
                return 2;
            }
        }
        else if ( strcmp( arg, "--steps" ) == 0 && i + 1 < argc )
        {
            const unsigned long long steps = strtoull( argv[++i], NULL, 10 );
            if ( steps == 0 )
            {
                fprintf( stderr, "dst: --steps must be a positive integer\n" );
                return 2;
            }
            options.fault_steps = steps;
            have_steps = 1;
        }
        else if ( strcmp( arg, "--faults" ) == 0 && i + 1 < argc )
        {
            if ( !dst_parse_faults( argv[++i], &options.faults ) )
            {
                fprintf( stderr, "dst: --faults names a kind that does not exist\n" );
                return 2;
            }
        }
        else if ( strcmp( arg, "--max-fails" ) == 0 && i + 1 < argc )
        {
            max_fails = atoi( argv[++i] );
            if ( max_fails < 1 )
            {
                fprintf( stderr, "dst: --max-fails must be positive\n" );
                return 2;
            }
        }
        else if ( strcmp( arg, "--verbose" ) == 0 )
            verbose = 1;
        else
        {
            fprintf( stderr, "dst: unknown or malformed argument: %s\n", arg );
            return 2;
        }
    }

    if ( !have_seeds )
    {
        fprintf( stderr, "dst: --seeds a-b is required\n" );
        return 2;
    }

    if ( !have_steps )
        options.fault_steps = options.shape == ROCKETNET_DST_SHAPE_PUSH ? 500 : 2000;
    {
        const uint64_t quiet = options.shape == ROCKETNET_DST_SHAPE_PUSH ? 1500 : 2000;
        options.steps = options.fault_steps + quiet;
    }

    rocketnet_dst_substream_init( &dst_keys_stream, seeds_a, "keys" );
    randombytes_set_implementation( &dst_keys_implementation );
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
        return 1;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    uint64_t runs = 0;
    uint64_t failed = 0;
    struct rocketnet_dst_trace_t range_trace;
    rocketnet_dst_trace_init( &range_trace );

    for ( uint64_t seed = seeds_a; ; )
    {
        rocketnet_dst_substream_init( &dst_keys_stream, seed, "keys" );

        struct rocketnet_dst_result_t result;
        if ( !rocketnet_dst_run_seed_with_options( seed, &options, &result ) )
        {
            fprintf( stderr, "dst: seed %llu could not build its world\n", (unsigned long long) seed );
            rocketnet_term();
            return 1;
        }
        runs++;
        rocketnet_dst_trace_u64( &range_trace, result.trace_hash );

        if ( verbose )
            printf( "seed=%llu trace=%016llx\n",
                    (unsigned long long) seed, (unsigned long long) result.trace_hash );

        if ( !result.passed )
        {
            char line[768];
            rocketnet_dst_format_failure( &result, &options, line, sizeof(line) );
            printf( "%s\n", line );
            failed++;
            if ( (int) failed >= max_fails )
                break;
        }

        uint64_t next_seed;
        if ( !rocketnet_dst_seed_range_next( seed, seeds_b, &next_seed ) )
            break;
        seed = next_seed;
    }

    if ( failed )
    {
        printf( "FAIL seeds=%llu-%llu runs=%llu failed=%llu\n",
                (unsigned long long) seeds_a, (unsigned long long) seeds_b,
                (unsigned long long) runs, (unsigned long long) failed );
        rocketnet_term();
        return 1;
    }

    printf( "PASS seeds=%llu-%llu runs=%llu trace=%016llx\n",
            (unsigned long long) seeds_a, (unsigned long long) seeds_b,
            (unsigned long long) runs, (unsigned long long) range_trace.hash );

    rocketnet_term();
    return 0;
}
