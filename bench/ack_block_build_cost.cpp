/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The ack block builder's cost per call (bench/ack_block_build_cost.md):
// rocketnet_ack_block_build over fixed-seed replay windows of seven shapes --
// sparse loss, dense loss, alternating bits, a full window, an empty window
// (only largest received), a window wrapped across the 4096-bit boundary and
// a largest below 4096 -- at the full block capacity, the size every sealed
// session packet reserves. The builder allocates nothing; every window and
// output buffer is static, so no allocation happens inside the timed loop.
// Prints the median and the best nanoseconds per build over the batches.
//
// Built by hand against a Release build (no CMake target). On Linux, after
// make:
//
//   c++ -O2 -std=c++11 -Isdk bench/ack_block_build_cost.cpp build/librocketnet_dev.a -luring -lxdp -lbpf -lpthread -o bin/ack_block_build_cost
//   ./bin/ack_block_build_cost
//
// On macOS drop the Linux libraries:
//
//   c++ -O2 -std=c++11 -Isdk bench/ack_block_build_cost.cpp build/librocketnet_dev.a -o bin/ack_block_build_cost

#include <algorithm>
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rocketnet_ack.h"
#include "rocketnet_encryption_map.h"
#include "rocketnet_replay.h"

#define BENCH_BATCHES           201
#define BENCH_BUILDS_PER_BATCH  2000

static uint64_t bench_random_state = 0x2545F4914F6CDD1DULL;

static uint64_t bench_random()
{
    bench_random_state ^= bench_random_state << 13;
    bench_random_state ^= bench_random_state >> 7;
    bench_random_state ^= bench_random_state << 17;
    return bench_random_state;
}

static void bench_set( struct rocketnet_replay_protection_t * replay, uint64_t sequence, int value )
{
    const uint64_t bit = sequence % ROCKETNET_REPLAY_WINDOW_BITS;
    const uint64_t mask = 1ULL << ( bit % 64 );
    if ( value )
        replay->window[bit / 64] |= mask;
    else
        replay->window[bit / 64] &= ~mask;
}

enum bench_shape_t
{
    SHAPE_SPARSE_LOSS,
    SHAPE_DENSE_LOSS,
    SHAPE_ALTERNATING,
    SHAPE_FULL,
    SHAPE_EMPTY,
    SHAPE_WRAPPED,
    SHAPE_BELOW_4096,
    SHAPE_COUNT
};

static const char * bench_shape_name[SHAPE_COUNT] =
{
    "sparse loss (1/64)",
    "dense loss (63/64)",
    "alternating bits",
    "full window",
    "empty window",
    "wrapped at 4096",
    "largest below 4096",
};

static void bench_window( struct rocketnet_replay_protection_t * replay, int shape )
{
    const int words = ROCKETNET_REPLAY_WINDOW_BITS / 64;
    uint64_t largest = 1000000;
    for ( int w = 0; w < words; w++ )
    {
        uint64_t word = 0;
        switch ( shape )
        {
            case SHAPE_SPARSE_LOSS:
                word = ~( bench_random() & bench_random() & bench_random() & bench_random() & bench_random() & bench_random() );
                break;
            case SHAPE_DENSE_LOSS:
                word = bench_random() & bench_random() & bench_random() & bench_random() & bench_random() & bench_random();
                break;
            case SHAPE_ALTERNATING:
                word = 0x5555555555555555ULL;
                break;
            case SHAPE_FULL:
                word = ~0ULL;
                break;
            case SHAPE_EMPTY:
                word = 0;
                break;
            case SHAPE_WRAPPED:
                word = ~( bench_random() & bench_random() & bench_random() );
                break;
            case SHAPE_BELOW_4096:
                word = ~( bench_random() & bench_random() & bench_random() );
                break;
            default:
                break;
        }
        replay->window[w] = word;
    }
    if ( shape == SHAPE_WRAPPED )
        largest = 244 * (uint64_t) ROCKETNET_REPLAY_WINDOW_BITS + 1500;
    if ( shape == SHAPE_BELOW_4096 )
        largest = 3000;
    replay->highest_opened = largest;
    bench_set( replay, largest, 1 );
}

static struct rocketnet_replay_protection_t bench_replay;
static uint8_t bench_output[ROCKETNET_ACK_BLOCK_MAX_BYTES];
static double bench_ns[BENCH_BATCHES];

int main()
{
    printf( "ack_block_build_cost: %d batches of %d builds, capacity %d bytes\n", BENCH_BATCHES, BENCH_BUILDS_PER_BATCH, ROCKETNET_ACK_BLOCK_MAX_BYTES );
    printf( "%-22s %8s %6s %12s %12s\n", "shape", "bytes", "ranges", "median ns", "best ns" );

    unsigned long long sink = 0;

    for ( int shape = 0; shape < SHAPE_COUNT; shape++ )
    {
        bench_window( &bench_replay, shape );

        int bytes = 0;
        for ( int i = 0; i < 1000; i++ )
            bytes = rocketnet_ack_block_build( bench_output, ROCKETNET_ACK_BLOCK_MAX_BYTES, &bench_replay );

        for ( int b = 0; b < BENCH_BATCHES; b++ )
        {
            const auto start = std::chrono::steady_clock::now();
            for ( int i = 0; i < BENCH_BUILDS_PER_BATCH; i++ )
            {
                bytes = rocketnet_ack_block_build( bench_output, ROCKETNET_ACK_BLOCK_MAX_BYTES, &bench_replay );
                sink += bench_output[bytes - 1];
            }
            const auto finish = std::chrono::steady_clock::now();
            bench_ns[b] = std::chrono::duration<double, std::nano>( finish - start ).count() / BENCH_BUILDS_PER_BATCH;
        }

        std::sort( bench_ns, bench_ns + BENCH_BATCHES );
        printf( "%-22s %8d %6d %12.1f %12.1f\n", bench_shape_name[shape], bytes, ( bytes - ROCKETNET_ACK_BLOCK_MIN_BYTES ) / 4,
                bench_ns[BENCH_BATCHES / 2], bench_ns[0] );
    }

    printf( "sink %llu\n", sink );
    return 0;
}
