/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "../sdk/rocketnet_send_handoff.h"
#include "../sdk/rocketnet_driver.h"
#include "test_unit_registry.h"

// the AF_XDP code is Linux only, so its tests are built there only
#if defined(__linux__)

// The send worker does not retire a generation until the test tells it to.
// No sockets, worker threads or clock take part in this interleaving.
static void test_xdp_transport_send_staging_retains_producer()
{
    rocketnet_send_handoff_t<2> handoff;
    const int first = handoff.acquire();
    rocketnet_unit_check(first >= 0);
    rocketnet_unit_check(handoff.publish());
    const int second = handoff.acquire();
    rocketnet_unit_check(second >= 0 && second != first);
    rocketnet_unit_check(!handoff.publish());
    // A second flush while the worker is behind keeps producer storage.
    rocketnet_unit_check(handoff.acquire() >= 0);
    rocketnet_unit_check(handoff.filling == second);
    rocketnet_unit_check(handoff.count == 1 && handoff.front() == first);
    for (int flush = 0; flush < 4; ++flush)
    {
        rocketnet_unit_check(!handoff.publish());
        rocketnet_unit_check(handoff.acquire() == second);
        rocketnet_unit_check(handoff.front() == first && handoff.count == 1);
    }
    handoff.retire();
    rocketnet_unit_check(handoff.publish());
    rocketnet_unit_check(handoff.front() == second);
    rocketnet_unit_check(handoff.acquire() == first);
    handoff.retire();
    rocketnet_unit_check(handoff.count == 0);
}
REGISTER_UNIT_TEST(test_xdp_transport_send_staging_retains_producer);

// The fixture calls the actual transport send_packets with a fake staged
// backend. After the first call the staged and flushed counters agree.
int rocketnet_server_transport_test_xdp_reflush();
static void test_xdp_transport_send_staging_reflushes_without_commit()
{
    rocketnet_unit_check(rocketnet_server_transport_test_xdp_reflush() == 2);
}
REGISTER_UNIT_TEST(test_xdp_transport_send_staging_reflushes_without_commit);

// gso retains its producer on a contended or full handoff and io_uring keeps
// its index while sends wait for CQEs: the next send boundary retries both,
// with no new commit, as it does for XDP. A synchronous staged transport
// (iocp here) flushes only for a new commit.
int rocketnet_server_transport_test_reflush( int transport_kind );
static void test_gso_transport_send_packets_reflushes_without_commit()
{
    rocketnet_unit_check(rocketnet_server_transport_test_reflush(ROCKETNET_TRANSPORT_GSO) == 2);
}
REGISTER_UNIT_TEST(test_gso_transport_send_packets_reflushes_without_commit);

static void test_io_uring_transport_send_packets_reflushes_without_commit()
{
    rocketnet_unit_check(rocketnet_server_transport_test_reflush(ROCKETNET_TRANSPORT_IO_URING) == 2);
}
REGISTER_UNIT_TEST(test_io_uring_transport_send_packets_reflushes_without_commit);

static void test_staged_transport_send_packets_flushes_once_per_commit()
{
    rocketnet_unit_check(rocketnet_server_transport_test_reflush(ROCKETNET_TRANSPORT_IOCP) == 1);
}
REGISTER_UNIT_TEST(test_staged_transport_send_packets_flushes_once_per_commit);

int rocketnet_server_transport_test_staged_simulator_flush();
static void test_xdp_transport_send_packets_flushes_simulator_window()
{
    rocketnet_unit_check(rocketnet_server_transport_test_staged_simulator_flush() == 1);
}
REGISTER_UNIT_TEST(test_xdp_transport_send_packets_flushes_simulator_window);

// A publish without a filling generation is refused: the release build must
// not queue -1, and the refused call leaves the queue as it was. The
// refusal does not wedge the handoff: the next acquire and publish work.
static void test_send_handoff_publish_without_acquire_refuses()
{
    rocketnet_send_handoff_t<2> handoff;
    rocketnet_unit_check(!handoff.publish());
    rocketnet_unit_check(handoff.count == 0);
    rocketnet_unit_check(handoff.head == 0);
    rocketnet_unit_check(handoff.filling == -1);
    rocketnet_unit_check(handoff.front() == -1);
    rocketnet_unit_check(handoff.free[0] && handoff.free[1]);

    // the refused publish left the handoff usable
    const int generation = handoff.acquire();
    rocketnet_unit_check(generation >= 0);
    rocketnet_unit_check(handoff.publish());
    rocketnet_unit_check(handoff.count == 1 && handoff.front() == generation);
}
REGISTER_UNIT_TEST(test_send_handoff_publish_without_acquire_refuses);

// A retire on an empty queue changes nothing: the release build must not
// free a stale generation, advance the head or leave the count at -1. The
// refusal does not wedge the handoff: the next acquire, publish and retire
// work.
static void test_send_handoff_retire_empty_changes_nothing()
{
    rocketnet_send_handoff_t<2> handoff;
    const int before_count = handoff.count;
    const int before_head = handoff.head;
    const bool before_free_0 = handoff.free[0];
    const bool before_free_1 = handoff.free[1];
    handoff.retire();
    rocketnet_unit_check(handoff.count == before_count);
    rocketnet_unit_check(handoff.head == before_head);
    rocketnet_unit_check(handoff.free[0] == before_free_0);
    rocketnet_unit_check(handoff.free[1] == before_free_1);

    // the refused retire left the handoff usable
    const int generation = handoff.acquire();
    rocketnet_unit_check(generation >= 0);
    rocketnet_unit_check(handoff.publish());
    handoff.retire();
    rocketnet_unit_check(handoff.count == 0);
    rocketnet_unit_check(handoff.head == (before_head + 1) % 2);
    rocketnet_unit_check(handoff.free[generation]);
}
REGISTER_UNIT_TEST(test_send_handoff_retire_empty_changes_nothing);

// A start past the send queue size is refused and counted, so the flush
// names the queue and the count once per flush instead of the start path
// logging a line for every refused packet. No clock, socket or thread.
static void test_xdp_transport_send_overflow_counted_once_per_flush()
{
    const int limit = 1024;
    std::atomic<int> num_packets( 0 );
    std::atomic<uint64_t> refused( 0 );

    // the first limit claims take an index of their own, the next five are
    // refused and counted
    for ( int i = 0; i < limit + 5; ++i )
    {
        const int index = rocketnet_xdp_send_claim( num_packets, limit, refused );
        if ( i < limit )
            rocketnet_unit_check( index == i );
        else
            rocketnet_unit_check( index == -1 );
    }
    rocketnet_unit_check( refused.load( std::memory_order_relaxed ) == 5 );

    // a queue that starts over claims from zero again and counts again
    num_packets.store( 0, std::memory_order_relaxed );
    refused.store( 0, std::memory_order_relaxed );
    for ( int i = 0; i < limit + 5; ++i )
    {
        const int index = rocketnet_xdp_send_claim( num_packets, limit, refused );
        if ( i < limit )
            rocketnet_unit_check( index == i );
        else
            rocketnet_unit_check( index == -1 );
    }
    rocketnet_unit_check( refused.load( std::memory_order_relaxed ) == 5 );
}
REGISTER_UNIT_TEST(test_xdp_transport_send_overflow_counted_once_per_flush);

#endif // #if defined(__linux__)
