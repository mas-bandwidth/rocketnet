/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The packet builder's write cost against the plan's size (roadmap row 417,
// the builder half): one rn_packet_builder_write over plans of 32, 128 and
// 512 items, 8 channels, the four classes and the four priorities, one packet
// per call (the endpoint's shape) and 16. Every plan and frame is allocated
// before the timed window and nothing is allocated inside it; the items are
// put back between calls outside the window. Prints the median and the best
// nanoseconds per write.
//
// Built by hand against a Release build (no CMake target yet). On Linux,
// after make:
//
//   c++ -O2 -std=c++11 -Isdk benchmarks/packet_builder_cost.cpp build/librocketnet_dev.a -luring -lxdp -lbpf -lpthread -o bin/packet_builder_cost
//   ./bin/packet_builder_cost

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <string.h>

#include "rocketnet.h"
#include "rocketnet_allocator.h"
#include "rocketnet_counters.h"
#include "rocketnet_message.h"
#include "rocketnet_packet_builder.h"
#include "rocketnet_send_budget.h"

#define BENCH_MAX_ITEMS         512
#define BENCH_PAYLOAD_BYTES     1191
#define BENCH_CHANNELS          8
#define BENCH_CALLS             20001

static uint8_t bench_data[BENCH_MAX_ITEMS][64];
static uint8_t bench_frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS][BENCH_PAYLOAD_BYTES + 1];
static struct rn_packet_item_t bench_plan[BENCH_MAX_ITEMS];
static struct rn_packet_item_t bench_items[BENCH_MAX_ITEMS];
static double bench_ns[BENCH_CALLS];
static rocketnet_counters_t bench_counters;

static uint32_t bench_random_state = 0x2545F491u;

static uint32_t bench_random()
{
    bench_random_state ^= bench_random_state << 13;
    bench_random_state ^= bench_random_state >> 17;
    bench_random_state ^= bench_random_state << 5;
    return bench_random_state;
}

static void bench_fill( int num_items )
{
    for ( int i = 0; i < num_items; i++ )
    {
        struct rn_packet_item_t * item = &bench_plan[i];
        memset( item, 0, sizeof( *item ) );
        item->data = bench_data[i];
        item->bytes = 8 + bench_random() % 57;
        item->number = ROCKETNET_PACKET_NUMBER_NONE;
        item->descriptor = RN_DESCRIPTOR_NONE;
        item->channel = (uint16_t) ( bench_random() % BENCH_CHANNELS );
        item->code = ROCKETNET_MESSAGE_CODE_UNRELIABLE;
        item->send_class = (uint8_t) ( bench_random() % 4 );
        item->priority = (uint8_t) ( bench_random() % 4 );
    }
}

static void bench_run( rn_region_t * region, int num_items, int packets_per_update )
{
    bench_fill( num_items );

    uint8_t * frames[ROCKETNET_PACKET_BUILDER_MAX_PACKETS];

    for ( int p = 0; p < ROCKETNET_PACKET_BUILDER_MAX_PACKETS; p++ )
    {
        frames[p] = bench_frames[p];
    }

    struct rn_packet_builder_t builder;
    if ( !rn_packet_builder_init( &builder, region, NULL, NULL, packets_per_update, BENCH_PAYLOAD_BYTES ) )
    {
        fprintf( stderr, "packet_builder_cost: init refused\n" );
        return;
    }

    int packets = 0;

    for ( int call = 0; call < BENCH_CALLS; call++ )
    {
        memcpy( bench_items, bench_plan, sizeof( bench_items[0] ) * (size_t) num_items );

        const auto start = std::chrono::steady_clock::now();

        packets = rn_packet_builder_write( &builder, bench_items, num_items, frames, ROCKETNET_PACKET_BUILDER_MAX_PACKETS );

        const auto finish = std::chrono::steady_clock::now();
        bench_ns[call] = (double) std::chrono::duration_cast<std::chrono::nanoseconds>( finish - start ).count();
    }

    int taken = 0;

    for ( int i = 0; i < num_items; i++ )
    {
        taken += bench_items[i].taken ? 1 : 0;
    }

    std::sort( bench_ns, bench_ns + BENCH_CALLS );
    printf( "items %4d  packets_per_update %2d  packets %2d  taken %3d  median_ns %9.0f  best_ns %9.0f\n", num_items, packets_per_update, packets, taken, bench_ns[BENCH_CALLS / 2], bench_ns[0] );
}

int main()
{
    if ( rocketnet_init( NULL, NULL, NULL ) != ROCKETNET_STATUS_OK )
    {
        fprintf( stderr, "packet_builder_cost: rocketnet_init failed\n" );
        return 1;
    }

    rn_arena_t * arena = rn_arena_create( NULL, NULL, 1, &bench_counters );
    if ( !arena )
    {
        fprintf( stderr, "packet_builder_cost: rn_arena_create failed\n" );
        return 1;
    }

    rn_region_t * region = rn_arena_region( arena, 0 );

    static const int sizes[] = { 32, 128, 512 };
    static const int packet_counts[] = { 1, 16 };

    for ( int c = 0; c < 2; c++ )
        for ( int s = 0; s < 3; s++ )
        {
            bench_run( region, sizes[s], packet_counts[c] );
        }

    rn_arena_destroy( arena );
    rocketnet_term();

    return 0;
}
