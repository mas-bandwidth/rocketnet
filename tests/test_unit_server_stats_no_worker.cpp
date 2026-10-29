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
#include "msan_scale_allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define check rocketnet_unit_check

#if ROCKETNET_DEV
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
    msan_scale_allocator_scope_t scale_allocator; // tests/msan_scale_allocator.h
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
#endif // #if ROCKETNET_DEV

// The resident bound of test_server_stats_current_every_update is what the
// library allocates for the world it builds: one server of ROCKETNET_MAX_CLIENTS
// slots and NO_WORKER_CLIENTS clients, by their memory plans, plus the suite's
// margin. How many of those pages the OS keeps resident is the OS's business
// (4 KiB or 16 KiB pages, its allocator, its accounting), so it is not bounded
// here; the plans themselves are checked against the allocator elsewhere.

#if ROCKETNET_DEV
static long no_worker_rss_bound_mb()
{
    rocketnet_server_config_t server;
    rocketnet_client_config_t client;
    if ( rocketnet_server_default_config( &server ) != ROCKETNET_STATUS_OK || rocketnet_client_default_config( &client ) != ROCKETNET_STATUS_OK )
        return 0;
    server.max_clients = ROCKETNET_MAX_CLIENTS;
    server.transport = "loopback";
    client.transport = "loopback";
    no_worker_message_config( &server, &client );
    const uint64_t planned = (uint64_t) rocketnet_server_required_memory( &server ) +
                             (uint64_t) NO_WORKER_CLIENTS * (uint64_t) rocketnet_client_required_memory( &client );
    return suite_rss_bound_mb( planned, 32 );
}

SUITE_CASE_RSS_BOUND( test_server_stats_current_every_update, no_worker_rss_bound_mb );
#endif // #if ROCKETNET_DEV

// A peak one MiB over a case's bound is recorded in both tiers; the unit tier
// only reports it, and the functional tier (--enforce-rss) fails it.
static void test_server_stats_rss_bound_reported_then_enforced()
{
    const long bound = 700;

    suite_result result = suite_result();
    result.name = "test_server_stats_current_every_update";
    result.peak_rss_mb = bound;
    suite_check_rss( result, bound );
    check( !result.failed );
    check( !result.rss_over_bound );

    result.peak_rss_mb = bound + 1;
    suite_check_rss( result, bound, false );
    check( !result.failed && result.rss_over_bound );
    check( result.detail.find( "peak_rss_mb=" ) != std::string::npos );

    result.rss_over_bound = false;
    suite_check_rss( result, bound, true );
    check( result.failed && result.rss_over_bound );
    check( result.detail.find( "peak_rss_mb=" ) != std::string::npos );
}

REGISTER_UNIT_TEST( test_server_stats_rss_bound_reported_then_enforced );
