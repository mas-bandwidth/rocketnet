/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The server keeps per-slot statistics with no thread of its own: the running
// aggregates in sdk/rocketnet_ack.cpp (tla/RNServerStats.tla) move on every
// send, receive and ack, on the caller's thread inside update, and the stats
// read returns them. There is no background recompute and no schedule that
// reaches a few slots per update. The server runs on the in-memory loopback
// transport with step-driven clients and a test clock starting at 1000.0; no
// socket, no sleep and no real time.
//
// The create/update thread-count case moved to the functional suite
// (functional/process_thread_counts.cpp): reading the process's thread count
// from /proc/self/task is OS state. The world setup is shared from
// tests/no_worker_world.h.

#include "unit_shutdown.h"
#include "test_unit_registry.h"
#include "../sdk/rocketnet_connect_token.h"
#include "../sdk/rocketnet_base64.h"
#include "../sdk/rocketnet.h"
#include "rocketnet_internal.h"
#include "suite_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#include "no_worker_world.h"

// Every one of 1000 connected slots reads statistics that already reflect the
// update that just ran. The step shrinks from 100 ms to 10 ms for one round
// trip: the server sends to every slot, every client receives and acks on the
// next update, and on that same server update every slot's rtt (the minimum
// sample) must read the new 10 ms sample, with the send, receive and ack
// counts all moved. A schedule that recomputes a few slots per update leaves
// almost every slot at the old 100 ms.
static void test_server_stats_current_every_update()
{
    no_worker_world_t * world = &g_no_worker_world;
    no_worker_server_create( world, NO_WORKER_CLIENTS );
    no_worker_clients_create( world, NO_WORKER_CLIENTS );
    no_worker_connect_all( world );
    for ( int step = 0; step < 400 && no_worker_connected_slots( world ) < NO_WORKER_CLIENTS; step++ )
        no_worker_step( world, false, 0.1 );
    check( no_worker_connected_slots( world ) == NO_WORKER_CLIENTS );

    // steady traffic at 100 ms steps until every slot has samples
    for ( int step = 0; step < 10; step++ )
        no_worker_step( world, true, 0.1 );

    static struct rocketnet_server_client_stats_t before[NO_WORKER_CLIENTS];
    for ( int slot = 0; slot < NO_WORKER_CLIENTS; slot++ )
    {
        rocketnet_server_client_stats( world->server, slot, &before[slot] );
        check( before[slot].rtt_ms > 90.0 );
    }

    // the server's last send went out at a 10 ms step; this update's acks
    // carry 10 ms samples for every slot
    no_worker_step( world, true, 0.01 );
    no_worker_step( world, true, 0.01 );

    int current = 0;
    for ( int slot = 0; slot < NO_WORKER_CLIENTS; slot++ )
    {
        struct rocketnet_server_client_stats_t stats;
        rocketnet_server_client_stats( world->server, slot, &stats );
        check( stats.packets_sent > before[slot].packets_sent );
        check( stats.packets_received > before[slot].packets_received );
        check( stats.bandwidth_sent_kbps > 0.0 );
        check( stats.bandwidth_received_kbps > 0.0 );
        check( stats.bandwidth_acked_kbps > 0.0 );
        if ( stats.rtt_ms > 9.0 && stats.rtt_ms < 11.0 )
            current++;
    }
    check( current == NO_WORKER_CLIENTS );

    no_worker_world_destroy( world );
}

REGISTER_UNIT_TEST( test_server_stats_current_every_update );

// The resident bound of test_server_stats_current_every_update, from what it
// builds: one server of ROCKETNET_MAX_CLIENTS slots and NO_WORKER_CLIENTS
// connected clients, each with its slot. The SDK's plan for that world is
// about 36 GiB, almost all of it never touched, so a plan bound would hold
// nothing; the bound is on the pages the case touches instead.
//
// Each connected pair (a client and its slot) holds bytes it fills and tables
// it touches sparsely, one resident page per touch, so its share has a part
// that is the same on every page size and a part that is a count of pages.
// The server's build-wide tables (sized by ROCKETNET_MAX_CLIENTS) are filled
// densely. Measured 2026-10-24, Release, the case's peak resident set:
// 567 MiB at 4 KiB pages (amd-3, x86_64 Linux) and 1149 MiB at 16 KiB pages
// (mini-m5, arm64 macOS) with 1000 build slots; 641 MiB and 1228 MiB with
// 10000. That is about 367 KiB and 49 pages for each pair, and 9 KiB for
// each build slot. The allowances below sit 15 to 21% over every measurement.
// The SDK's allocations are the same on both systems (36625 MiB in 21061
// blocks at the peak, every one freed at destroy); only the page differs.
#define NO_WORKER_PAIR_BYTES        ( 416 * 1024 )
#define NO_WORKER_PAIR_PAGES        56
#define NO_WORKER_BUILD_SLOT_BYTES  ( 12 * 1024 )
#define NO_WORKER_MARGIN_MB         32

static long no_worker_rss_bound_mb_at( uint64_t page_bytes )
{
    const uint64_t pair_bytes = NO_WORKER_PAIR_BYTES + NO_WORKER_PAIR_PAGES * page_bytes;
    const uint64_t bytes = (uint64_t) NO_WORKER_CLIENTS * pair_bytes + (uint64_t) ROCKETNET_MAX_CLIENTS * NO_WORKER_BUILD_SLOT_BYTES;
    return suite_rss_bound_mb( bytes, NO_WORKER_MARGIN_MB );
}

static long no_worker_rss_bound_mb()
{
    return no_worker_rss_bound_mb_at( suite_page_bytes() );
}

SUITE_CASE_RSS_BOUND( test_server_stats_current_every_update, no_worker_rss_bound_mb );

// The bound grows with the page by NO_WORKER_PAIR_PAGES pages per pair and
// nothing else, and the runner fails a case whose peak is one MiB over it.
static void test_server_stats_rss_bound_scales_with_page()
{
    const long small = no_worker_rss_bound_mb_at( 4096 );
    const long large = no_worker_rss_bound_mb_at( 16384 );
    const uint64_t mib = 1024 * 1024;
    const long growth = (long) ( (uint64_t) NO_WORKER_CLIENTS * NO_WORKER_PAIR_PAGES * ( 16384 - 4096 ) / mib );
    check( large - small >= growth && large - small <= growth + 1 );    // each bound rounds up to whole MiB

    suite_result result = suite_result();
    result.name = "test_server_stats_current_every_update";
    result.peak_rss_mb = large;
    suite_check_rss( result, large );
    check( !result.failed );
    result.peak_rss_mb = large + 1;
    suite_check_rss( result, large );
    check( result.failed );
    check( result.detail.find( "peak_rss_mb=" ) != std::string::npos );
}

REGISTER_UNIT_TEST( test_server_stats_rss_bound_scales_with_page );
