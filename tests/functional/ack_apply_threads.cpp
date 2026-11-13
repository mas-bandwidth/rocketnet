/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The deferred ack apply on real threads (sdk/rocketnet_ack.h,
// rocketnet_server_acks_t; proofs/tla/RNAckApplyPhase.tla). The trace and
// the comparison are the unit differential's (tests/ack_apply_differential.h):
// here each update's open clients go to four threads that every one of them
// asks for every client, each starting at a different place, so the claims
// are contested on every client. Exactly one taker applies a client; the
// batch, the counters and every endpoint must equal the inline apply's.
// Under TSan this is also the race check of the per-client ownership.

#include "rocketnet_ack.h"
#include "rocketnet_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <thread>
#include <vector>

static std::atomic<int> ack_threads_failures{ 0 };

#define ACK_APPLY_CHECK( condition )                                                                      \
    do                                                                                                    \
    {                                                                                                     \
        if ( !( condition ) )                                                                             \
        {                                                                                                 \
            printf( "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__ );                     \
            ack_threads_failures.fetch_add( 1 );                                                          \
        }                                                                                                 \
    } while ( 0 )

// every client of order[] is asked for, so the takes owe every queued block
// before close (ack_apply_differential.h)
#define ACK_APPLY_SCHEDULE_TAKES_ALL 1

#include "../ack_apply_differential.h"

#define ACK_APPLY_THREADS 4

static int ack_threads_taken_total;

static void ack_schedule_threads( struct rocketnet_server_acks_t * acks, const int * order, int count, uint64_t * rng )
{
    (void) rng;

    // every thread starts together, so the claims are contested
    std::atomic<int> ready{ 0 };
    std::atomic<int> taken{ 0 };
    std::vector<std::thread> threads;
    for ( int t = 0; t < ACK_APPLY_THREADS; t++ )
    {
        threads.emplace_back( [acks, order, count, t, &taken, &ready]()
                              {
                                  ready.fetch_add( 1 );
                                  while ( ready.load() < ACK_APPLY_THREADS )
                                  {
                                      std::this_thread::yield();
                                  }
                                  for ( int i = 0; i < count; i++ )
                                  {
                                      const int c = order[( i + t * count / ACK_APPLY_THREADS ) % count];
                                      if ( rocketnet_server_acks_apply_client( acks, c ) > 0 )
                                      {
                                          taken.fetch_add( 1 );
                                      }
                                  } } );
    }
    for ( std::thread & thread : threads )
    {
        thread.join();
    }
    ACK_APPLY_CHECK( taken.load() <= count );
    ack_threads_taken_total += taken.load();
}

void rocketnet_test_ack_apply_jobs_match_inline()
{
    const uint64_t seeds[] = { 0x7EAD5001ull, 0x7EAD5002ull };
    for ( uint64_t seed : seeds )
    {
        struct ack_apply_stats_t stats = {};
        ack_apply_differential( seed, 200, ack_schedule_threads, &stats );
        ACK_APPLY_CHECK( stats.blocks > 1000 );
        ACK_APPLY_CHECK( stats.early_gathers > 0 );
        ACK_APPLY_CHECK( stats.acked > 1000 );
    }
    ACK_APPLY_CHECK( ack_threads_taken_total > 0 );
    if ( ack_threads_failures.load() != 0 )
    {
        printf( "test_ack_apply_jobs_match_inline: %d checks failed\n", ack_threads_failures.load() );
        exit( 1 );
    }
}
