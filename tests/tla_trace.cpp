/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The trace harness of the TLA+ trace check (proofs/tla/README-TraceCheck.md).
// A seeded random walk drives the code of one layer through the actions its
// model names, and after every step writes one record of the model's
// variables, read from the code's own state and outputs. The records go out
// as a TLA+ module that defines the trace, with a cfg that hands it to the
// layer's Trace<Spec>.tla; TLC then checks the trace is a behaviour of the
// spec. No socket, no clock, no thread: the walk interleaves any threads'
// calls itself, one call per step.
//
//   tla_trace --layer NAME --seed N --out DIR [--steps K]
//   tla_trace --list
//
// Each layer is one file, tests/tla_trace_<layer>.cpp, that registers its
// walk with TLA_TRACE_LAYER (tests/tla_trace_record.h); this file only
// dispatches. Each layer has a plant build per plant its row names,
// tla_trace_plant_<layer> (or --<k> for the k-th of several): this file and
// that layer alone, compiled with ROCKETNET_TLA_PLANT and that plant's
// define, so the layer compiles its own code with that one plant; the walk
// is the same, and each plant's trace must fail TraceConsistent.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tla_trace_record.h"

// the library configuration this translation unit was compiled with, as
// the preprocessor sees it: tools/tla_trace_check.py compares the line with
// the library's own definitions (tla_trace_config.txt, written by CMake from
// rocketnet_dev), and the clean binary's line with the plant binary's, so a
// planted copy of library code is never compiled with a configuration the
// library does not have (code compiled out, other sizes).
#define TLA_TRACE_TEXT( x ) #x
#define TLA_TRACE_VALUE( x ) TLA_TRACE_TEXT( x )

static void print_config()
{
    printf( "CONFIG" );
#if defined( ROCKETNET_DEV )
    printf( " ROCKETNET_DEV=%s", TLA_TRACE_VALUE( ROCKETNET_DEV ) );
#else  // #if defined( ROCKETNET_DEV )
    printf( " ROCKETNET_DEV=undef" );
#endif // #if defined( ROCKETNET_DEV )
#if defined( ROCKETNET_MAX_CLIENTS )
    printf( " ROCKETNET_MAX_CLIENTS=%s", TLA_TRACE_VALUE( ROCKETNET_MAX_CLIENTS ) );
#else  // #if defined( ROCKETNET_MAX_CLIENTS )
    printf( " ROCKETNET_MAX_CLIENTS=undef" );
#endif // #if defined( ROCKETNET_MAX_CLIENTS )
#if defined( ROCKETNET_HISTORY_PACKETS )
    printf( " ROCKETNET_HISTORY_PACKETS=%s", TLA_TRACE_VALUE( ROCKETNET_HISTORY_PACKETS ) );
#else  // #if defined( ROCKETNET_HISTORY_PACKETS )
    printf( " ROCKETNET_HISTORY_PACKETS=undef" );
#endif // #if defined( ROCKETNET_HISTORY_PACKETS )
#if defined( ROCKETNET_XDP )
    printf( " ROCKETNET_XDP=%s", TLA_TRACE_VALUE( ROCKETNET_XDP ) );
#else  // #if defined( ROCKETNET_XDP )
    printf( " ROCKETNET_XDP=undef" );
#endif // #if defined( ROCKETNET_XDP )
#if defined( ROCKETNET_IO_URING )
    printf( " ROCKETNET_IO_URING=%s", TLA_TRACE_VALUE( ROCKETNET_IO_URING ) );
#else  // #if defined( ROCKETNET_IO_URING )
    printf( " ROCKETNET_IO_URING=undef" );
#endif // #if defined( ROCKETNET_IO_URING )
#if defined( ROCKETNET_XDP_MEMORY_MIB )
    printf( " ROCKETNET_XDP_MEMORY_MIB=%s", TLA_TRACE_VALUE( ROCKETNET_XDP_MEMORY_MIB ) );
#else  // #if defined( ROCKETNET_XDP_MEMORY_MIB )
    printf( " ROCKETNET_XDP_MEMORY_MIB=undef" );
#endif // #if defined( ROCKETNET_XDP_MEMORY_MIB )
    printf( "\n" );
}

static void usage()
{
    fprintf( stderr, "usage: tla_trace --layer NAME --seed N --out DIR [--steps K] | tla_trace --list\n" );
}

int main( int argc, char ** argv )
{
    const tla_trace_table_t & table = tla_trace_layers();
    if ( table.refused > 0 )
    {
        bool full = false;
        for ( int i = 0; i < table.refused && i < TLA_TRACE_MAX_LAYERS; i++ )
        {
            printf( "REFUSED %s %s\n", table.refusal[i].name, table.refusal[i].reason );
            full = full || strcmp( table.refusal[i].reason, "table-full" ) == 0;
        }
        fprintf( stderr, "tla_trace: %d layer registrations refused\n", table.refused );
        if ( full )
        {
            fprintf( stderr, "tla_trace: more layers than TLA_TRACE_MAX_LAYERS (%d): raise it in tests/tla_trace_record.h\n",
                     TLA_TRACE_MAX_LAYERS );
        }
        return 3;
    }

    const char * layer = NULL;
    const char * out = NULL;
    uint64_t seed = 0;
    bool have_seed = false;
    int steps = 400;

    for ( int i = 1; i < argc; i++ )
    {
        if ( strcmp( argv[i], "--list" ) == 0 && argc == 2 )
        {
            for ( int j = 0; j < table.count; j++ )
            {
                printf( "%s\n", table.layer[j].name );
            }
            return 0;
        }
        else if ( strcmp( argv[i], "--layer" ) == 0 && i + 1 < argc )
        {
            layer = argv[++i];
        }
        else if ( strcmp( argv[i], "--out" ) == 0 && i + 1 < argc )
        {
            out = argv[++i];
        }
        else if ( strcmp( argv[i], "--seed" ) == 0 && i + 1 < argc )
        {
            seed = strtoull( argv[++i], NULL, 10 );
            have_seed = true;
        }
        else if ( strcmp( argv[i], "--steps" ) == 0 && i + 1 < argc )
        {
            steps = atoi( argv[++i] );
        }
        else
        {
            usage();
            return 2;
        }
    }

    if ( !layer || !out || !have_seed || steps < 1 )
    {
        usage();
        return 2;
    }

    for ( int j = 0; j < table.count; j++ )
    {
        if ( strcmp( layer, table.layer[j].name ) == 0 )
        {
            print_config();
            return table.layer[j].walk( seed, steps, out ) == 0 ? 0 : 1;
        }
    }

    fprintf( stderr, "tla_trace: no layer %s\n", layer );
    return 2;
}
