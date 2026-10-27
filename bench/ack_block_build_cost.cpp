/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Measurement only, not a gate and not a unit test: times
// rocketnet_ack_block_build over fixed-seed replay windows. No allocation
// inside the timed loop: every replay window and the output buffer are
// built before timing. See ack_block_build_cost.md.
//
// Built as a CMake tool target in a Release build:
//
//   cmake -B build -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target ack_block_build_cost
//   ./bin/ack_block_build_cost

#include "rocketnet_ack.h"
#include "rocketnet_replay.h"

#include <chrono>
#include <stdio.h>
#include <string.h>

static uint64_t bench_rng_state;

static uint64_t bench_random()
{
    // splitmix64, fixed seeds per shape: the same windows on every run/host
    uint64_t z = ( bench_rng_state += 0x9E3779B97F4A7C15ull );
    z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
    z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
    return z ^ ( z >> 31 );
}

static void bench_set_bit( struct rocketnet_replay_protection_t * replay, uint64_t sequence )
{
    const uint64_t bit = sequence % ROCKETNET_REPLAY_WINDOW_BITS;
    replay->window[bit / 64] |= 1ull << ( bit % 64 );
}

static void bench_fill( struct rocketnet_replay_protection_t * replay, uint64_t highest, uint64_t seed, int percent_received, int alternating )
{
    memset( replay, 0, sizeof( *replay ) );
    replay->highest_opened = highest;
    if ( highest == 0 )
        return;
    uint64_t floor_sequence = 1;
    if ( highest >= ROCKETNET_REPLAY_WINDOW_BITS )
        floor_sequence = highest - ROCKETNET_REPLAY_WINDOW_BITS + 1;
    bench_rng_state = seed;
    for ( uint64_t s = floor_sequence; s <= highest; s++ )
    {
        int received;
        if ( alternating )
            received = ( ( s & 1 ) == ( highest & 1 ) );
        else if ( percent_received >= 100 )
            received = 1;
        else
            received = ( bench_random() % 100 ) < (uint64_t) percent_received;
        if ( received )
            bench_set_bit( replay, s );
    }
    bench_set_bit( replay, highest ); // largest itself is always marked received
}

struct bench_shape_t
{
    const char * name;
    struct rocketnet_replay_protection_t replay;
};

static bench_shape_t bench_shapes[7];

static void bench_build_shapes()
{
    bench_fill( &bench_shapes[0].replay, 20000, 0xA11CE001ull, 98, 0 );
    bench_shapes[0].name = "sparse_loss";
    bench_fill( &bench_shapes[1].replay, 20000, 0xA11CE002ull, 20, 0 );
    bench_shapes[1].name = "dense_loss";
    bench_fill( &bench_shapes[2].replay, 20000, 0xA11CE003ull, 0, 1 );
    bench_shapes[2].name = "alternating";
    bench_fill( &bench_shapes[3].replay, 20000, 0xA11CE004ull, 100, 0 );
    bench_shapes[3].name = "full_window";
    bench_fill( &bench_shapes[4].replay, 0, 0xA11CE005ull, 0, 0 );
    bench_shapes[4].name = "empty_window";
    // 4223 crosses the 4096-bit boundary: floor 128 wraps to bit 128 while
    // largest sits at bit 127, so the descending scan wraps the window.
    bench_fill( &bench_shapes[5].replay, 4223, 0xA11CE006ull, 50, 0 );
    bench_shapes[5].name = "wrapped_window";
    bench_fill( &bench_shapes[6].replay, 2000, 0xA11CE007ull, 50, 0 );
    bench_shapes[6].name = "largest_below_4096";
}

int main()
{
    bench_build_shapes();

    const int iterations = 20000;
    const int repetitions = 5;
    uint8_t output[ROCKETNET_ACK_BLOCK_MAX_BYTES];
    uint64_t checksum = 0;

    printf( "shape,ns_per_build_median,ns_per_build_min,bytes\n" );
    for ( int shape = 0; shape < 7; shape++ )
    {
        double best = 0, median_samples[5];
        for ( int rep = 0; rep < repetitions; rep++ )
        {
            // warm-up outside the timed window
            for ( int i = 0; i < 200; i++ )
                checksum += (uint64_t) rocketnet_ack_block_build( output, (int) sizeof( output ), &bench_shapes[shape].replay );
            const auto start = std::chrono::steady_clock::now();
            int bytes = 0;
            for ( int i = 0; i < iterations; i++ )
                bytes = rocketnet_ack_block_build( output, (int) sizeof( output ), &bench_shapes[shape].replay );
            const auto end = std::chrono::steady_clock::now();
            const double ns = std::chrono::duration<double, std::nano>( end - start ).count() / iterations;
            median_samples[rep] = ns;
            if ( rep == 0 || ns < best )
                best = ns;
            checksum += (uint64_t) bytes;
        }
        // median of the five repetitions (simple insertion sort)
        for ( int i = 1; i < repetitions; i++ )
        {
            const double v = median_samples[i];
            int j = i - 1;
            while ( j >= 0 && median_samples[j] > v ) { median_samples[j + 1] = median_samples[j]; j--; }
            median_samples[j + 1] = v;
        }
        const int bytes = rocketnet_ack_block_build( output, (int) sizeof( output ), &bench_shapes[shape].replay );
        printf( "%s,%.3f,%.3f,%d\n", bench_shapes[shape].name, median_samples[repetitions / 2], best, bytes );
    }
    printf( "checksum,%llu\n", (unsigned long long) checksum );
    return 0;
}
