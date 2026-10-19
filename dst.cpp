/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST command line (docs/DST.md section 2): one seed drives one
    single-process run of N clients and a server through the in-memory
    network simulator, with every random choice drawn from the substream the
    design names. A passing range prints one PASS line and exits 0; a failing
    seed prints a FAIL line and its replay command and the range ends with
    exit 1; a usage error exits 2. No wall clock, no socket, no thread.
*/

#include "rocketnet.h"
#include "dst_harness.h"
#include "../sodium/sodium.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct rocketnet_dst_substream_t keys_stream;

static const char * dst_keys_implementation_name( void )
{
    return "dst-keys";
}

static uint32_t dst_keys_random( void )
{
    return (uint32_t) rocketnet_dst_substream_draw( &keys_stream );
}

static void dst_keys_buf( void * const buf, const size_t size )
{
    uint8_t * out = (uint8_t*) buf;
    for ( size_t i = 0; i < size; i++ )
        out[i] = (uint8_t) rocketnet_dst_substream_draw( &keys_stream );
}

static const struct randombytes_implementation dst_keys_implementation = {
    dst_keys_implementation_name,
    dst_keys_random,
    NULL,
    NULL,
    dst_keys_buf,
    NULL,
};

int main( int argc, char ** argv )
{
    int pinned_clients = 0;
    uint64_t steps = 2000;
    uint64_t seeds_a = 0, seeds_b = 0;
    int have_seeds = 0;
    int max_fails = 20;
    int verbose = 0;

    for ( int i = 1; i < argc; i++ )
    {
        const char * arg = argv[i];
        if ( strncmp( arg, "--seeds", 7 ) == 0 && i + 1 < argc )
        {
            const char * range = argv[++i];
            unsigned long long a, b;
            if ( sscanf( range, "%llu-%llu", &a, &b ) != 2 || a > b )
            {
                fprintf( stderr, "dst: --seeds needs a-b with a <= b\n" );
                return 2;
            }
            seeds_a = a;
            seeds_b = b;
            have_seeds = 1;
        }
        else if ( strncmp( arg, "--clients", 9 ) == 0 && i + 1 < argc )
        {
            pinned_clients = atoi( argv[++i] );
            if ( pinned_clients < 1 || pinned_clients > 16 )
            {
                fprintf( stderr, "dst: --clients must be 1..16\n" );
                return 2;
            }
        }
        else if ( strncmp( arg, "--steps", 7 ) == 0 && i + 1 < argc )
        {
            steps = strtoull( argv[++i], NULL, 10 );
            if ( !steps )
            {
                fprintf( stderr, "dst: --steps must be a positive integer\n" );
                return 2;
            }
        }
        else if ( strncmp( arg, "--max-fails", 11 ) == 0 && i + 1 < argc )
        {
            max_fails = atoi( argv[++i] );
        }
        else if ( strcmp( arg, "--verbose" ) == 0 )
        {
            verbose = 1;
        }
        else if ( strncmp( arg, "--shape", 7 ) == 0 && i + 1 < argc )
        {
            i++;
        }
        else if ( strncmp( arg, "--faults", 8 ) == 0 && i + 1 < argc )
        {
            i++;
        }
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

    if ( !rocketnet_init( NULL ) )
        return 1;
    rocketnet_log_level( ROCKETNET_LOG_LEVEL_NONE );

    uint64_t runs = 0;
    uint64_t failed = 0;
    uint64_t trace = 0xcbf29ce484222325ULL;

    for ( uint64_t seed = seeds_a; seed <= seeds_b; seed++ )
    {
        rocketnet_dst_substream_init( &keys_stream, seed, "keys" );
        randombytes_set_implementation( (randombytes_implementation*) &dst_keys_implementation );

        struct rocketnet_dst_result_t result;
        rocketnet_dst_run_seed( seed, steps, pinned_clients, &result );
        runs++;

        for ( int i = 0; i < 8; i++ )
        {
            trace ^= (uint8_t) ( result.trace_hash >> ( 8 * i ) );
            trace *= 0x100000001b3ULL;
        }

        if ( verbose )
            printf( "seed=%llu trace=%016llx\n",
                    (unsigned long long) seed,
                    (unsigned long long) result.trace_hash );

        if ( !result.passed )
        {
            char replay[512];
            rocketnet_dst_replay_command( seed, steps, pinned_clients, replay, sizeof( replay ) );
            printf( "FAIL seed=%llu step=%llu check=%s what=%s\n",
                    (unsigned long long) result.seed, (unsigned long long) result.step,
                    result.check, result.detail );
            printf( "replay: %s\n", replay );
            failed++;
            if ( (int) failed >= max_fails )
                break;
        }
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
            (unsigned long long) runs, (unsigned long long) trace );

    rocketnet_term();
    return 0;
}
