/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The DST harness core (docs/DST.md sections 1, 2, 6): one master seed
    split into named substreams, a trace hash, and the PASS/FAIL output and
    replay lines of a seeded single-process run. This header carries the
    deterministic primitives the core is built on: FNV-1a 64, splitmix64's
    finaliser, and a per-substream split state.
*/

#ifndef ROCKETNET_DST_HARNESS_H
#define ROCKETNET_DST_HARNESS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rocketnet_network_simulator.h"
#include "rocketnet_address.h"

// FNV-1a 64 over a byte range.
static inline uint64_t rocketnet_dst_fnv1a64( const uint8_t * data, size_t bytes )
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for ( size_t i = 0; i < bytes; i++ )
    {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// splitmix64's finaliser.
static inline uint64_t rocketnet_dst_splitmix64_finalise( uint64_t z )
{
    z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
    z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebULL;
    return z ^ ( z >> 31 );
}

// A substream: its own 64-bit state, drawn from the master seed and the
// substream's name. A draw advances the state and returns the mixed value.
struct rocketnet_dst_substream_t
{
    uint64_t state;
};

static inline void rocketnet_dst_substream_init( struct rocketnet_dst_substream_t * stream,
                                                 uint64_t master_seed, const char * name )
{
    size_t name_len = strlen( name );
    uint64_t name_hash = rocketnet_dst_fnv1a64( (const uint8_t*) name, name_len );
    stream->state = master_seed ^ name_hash;
}

static inline uint64_t rocketnet_dst_substream_draw( struct rocketnet_dst_substream_t * stream )
{
    stream->state += 0x9E3779B97F4A7C15ULL;
    return rocketnet_dst_splitmix64_finalise( stream->state );
}

static inline uint64_t rocketnet_dst_substream_uniform( struct rocketnet_dst_substream_t * stream,
                                                        uint64_t lo, uint64_t hi )
{
    uint64_t draw = rocketnet_dst_substream_draw( stream );
    return lo + draw % ( hi - lo + 1 );
}

static inline double rocketnet_dst_substream_real( struct rocketnet_dst_substream_t * stream )
{
    uint64_t draw = rocketnet_dst_substream_draw( stream );
    return (double) ( draw >> 11 ) * 0x1p-53;
}

// The trace accumulator: FNV-1a 64 over fixed-width records appended in step
// order. Every append folds the record's bytes into the running hash.
struct rocketnet_dst_trace_t
{
    uint64_t hash;
};

static inline void rocketnet_dst_trace_init( struct rocketnet_dst_trace_t * trace )
{
    trace->hash = 0xcbf29ce484222325ULL;
}

static inline void rocketnet_dst_trace_bytes( struct rocketnet_dst_trace_t * trace,
                                              const uint8_t * data, size_t bytes )
{
    for ( size_t i = 0; i < bytes; i++ )
    {
        trace->hash ^= data[i];
        trace->hash *= 0x100000001b3ULL;
    }
}

static inline void rocketnet_dst_trace_u64( struct rocketnet_dst_trace_t * trace, uint64_t v )
{
    uint8_t b[8];
    for ( int i = 0; i < 8; i++ )
        b[i] = (uint8_t) ( v >> ( 8 * i ) );
    rocketnet_dst_trace_bytes( trace, b, 8 );
}

// The per-seed run result: whether the seed passed, the trace hash, and on a
// failure the seed, step, check name and detail.
struct rocketnet_dst_result_t
{
    int passed;
    uint64_t trace_hash;
    uint64_t seed;
    uint64_t step;
    char check[64];
    char detail[256];
};

// The seeded single-process run of N clients and a server through the network
// simulator. This is the layer-1 core: the run loop, the seed split, the
// trace hash, the output lines and the replay. It folds every wire send and
// every delivery into the trace, and every random choice is drawn from the
// substream the design names. No wall clock, no socket, no thread.
static inline int rocketnet_dst_run_seed( uint64_t seed, uint64_t steps, int pinned_clients,
                                          struct rocketnet_dst_result_t * result )
{
    memset( result, 0, sizeof( *result ) );
    result->seed = seed;

    struct rocketnet_dst_substream_t wire, link, loss, jitter, dup, schedule;
    rocketnet_dst_substream_init( &wire, seed, "wire" );
    rocketnet_dst_substream_init( &link, seed, "link" );
    rocketnet_dst_substream_init( &loss, seed, "fault.loss" );
    rocketnet_dst_substream_init( &jitter, seed, "fault.jitter" );
    rocketnet_dst_substream_init( &dup, seed, "fault.dup" );
    rocketnet_dst_substream_init( &schedule, seed, "schedule" );

    uint64_t simulator_seed = rocketnet_dst_substream_draw( &wire );
    double base_latency = 0.001 + rocketnet_dst_substream_real( &link ) * 0.049;
    double base_jitter = rocketnet_dst_substream_real( &jitter ) * 0.02;
    float loss_percent = (float) ( rocketnet_dst_substream_real( &loss ) * 3.0 );
    float dup_percent = (float) ( rocketnet_dst_substream_real( &dup ) * 2.0 );

    int num_clients = pinned_clients;
    if ( num_clients <= 0 )
        num_clients = (int) rocketnet_dst_substream_uniform( &schedule, 2, 8 );

    struct rocketnet_network_simulator_t * simulator = rocketnet_network_simulator_create( NULL, 4096, simulator_seed );
    rocketnet_network_simulator_config( simulator, base_latency, base_jitter, loss_percent, dup_percent );

    struct rocketnet_address_t server_address;
    rocketnet_address_parse( &server_address, "203.0.113.10:40000" );

    struct rocketnet_dst_trace_t trace;
    rocketnet_dst_trace_init( &trace );

    uint8_t buffer[2048];
    struct rocketnet_dst_substream_t * traffic = (struct rocketnet_dst_substream_t*)
        malloc( sizeof( struct rocketnet_dst_substream_t ) * num_clients );
    if ( !traffic )
    {
        rocketnet_network_simulator_destroy( simulator );
        return 0;
    }
    for ( int i = 0; i < num_clients; i++ )
    {
        char name[32];
        snprintf( name, sizeof(name), "traffic.c%d", i );
        rocketnet_dst_substream_init( &traffic[i], seed, name );
    }

    for ( uint64_t k = 0; k < steps; k++ )
    {
        double time = 1000.0 + (double) k * 0.01;
        for ( int i = 0; i < num_clients; i++ )
        {
            if ( rocketnet_dst_substream_real( &traffic[i] ) < 0.5 )
            {
                char address_string[64];
                snprintf( address_string, sizeof(address_string), "198.51.100.%d:56789", 10 + i );
                struct rocketnet_address_t client_address;
                rocketnet_address_parse( &client_address, address_string );
                int size = (int) rocketnet_dst_substream_uniform( &traffic[i], 14, 256 );
                for ( int b = 0; b < size; b++ )
                    buffer[b] = (uint8_t) ( ( b + k ) % 256 );
                rocketnet_network_simulator_send( simulator, &client_address, &server_address, buffer, size, time );
                rocketnet_dst_trace_u64( &trace, k );
                rocketnet_dst_trace_u64( &trace, (uint64_t) ( i + 1 ) );
                rocketnet_dst_trace_u64( &trace, 0 );
                rocketnet_dst_trace_u64( &trace, (uint64_t) size );
                rocketnet_dst_trace_u64( &trace, buffer[0] );
            }
        }
        struct rocketnet_address_t from;
        int got;
        while ( ( got = rocketnet_network_simulator_receive( simulator, &server_address, time, &from, buffer ) ) > 0 )
        {
            rocketnet_dst_trace_u64( &trace, k );
            rocketnet_dst_trace_u64( &trace, 0 );
            rocketnet_dst_trace_u64( &trace, (uint64_t) got );
            rocketnet_dst_trace_u64( &trace, buffer[0] );
        }
    }

    free( traffic );
    rocketnet_network_simulator_destroy( simulator );

    result->trace_hash = trace.hash;
    result->passed = 1;
    return 1;
}

// The live command a failing seed names in its replay line, with every
// non-default flag of this run. The run is exactly `seed` alone.
static inline void rocketnet_dst_replay_command( uint64_t seed, uint64_t steps, int pinned_clients,
                                                 char * out, size_t out_bytes )
{
    if ( pinned_clients > 0 )
        snprintf( out, out_bytes, "./bin/dst --seeds %llu-%llu --steps %llu --clients %d",
                  (unsigned long long) seed, (unsigned long long) seed,
                  (unsigned long long) steps, pinned_clients );
    else
        snprintf( out, out_bytes, "./bin/dst --seeds %llu-%llu --steps %llu",
                  (unsigned long long) seed, (unsigned long long) seed,
                  (unsigned long long) steps );
}

#endif // #ifndef ROCKETNET_DST_HARNESS_H
