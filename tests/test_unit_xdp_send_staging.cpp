/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "../sdk/rocketnet_send_handoff.h"
#include "../sdk/rocketnet_driver.h"
#include "test_unit_registry.h"

// Counts misuse asserts so the refusal cases run in every build, including
// ones where rocketnet_assert is compiled in.
static int send_handoff_assert_count = 0;

static void send_handoff_assert_recorder( const char * condition, const char * function, const char * file, int line )
{
    (void) condition;
    (void) function;
    (void) file;
    (void) line;
    send_handoff_assert_count++;
}

// tla/SendHandoff.tla Publish needs a filling generation: a publish with no
// prior acquire refuses and leaves the queue empty instead of queueing -1.
static void test_send_handoff_publish_without_acquire_refuses()
{
    rocketnet_assert_function( send_handoff_assert_recorder );
    send_handoff_assert_count = 0;
    rocketnet_send_handoff_t<2> handoff;
    rocketnet_unit_check(!handoff.publish());
    rocketnet_unit_check(handoff.count == 0 && handoff.head == 0);
    rocketnet_unit_check(handoff.filling == -1 && handoff.front() == -1);
    rocketnet_unit_check(handoff.free[0] && handoff.free[1]);
    // the assert stays: it fires wherever asserts are compiled in
    rocketnet_unit_check(send_handoff_assert_count == (ROCKETNET_ASSERTS ? 1 : 0));
    // after the refusal the handoff still cycles a generation normally
    const int first = handoff.acquire();
    rocketnet_unit_check(first >= 0);
    rocketnet_unit_check(handoff.publish());
    rocketnet_unit_check(handoff.count == 1 && handoff.front() == first);
    handoff.retire();
    rocketnet_unit_check(handoff.count == 0 && handoff.free[first]);
    rocketnet_assert_function( NULL );
}
REGISTER_UNIT_TEST(test_send_handoff_publish_without_acquire_refuses);

// tla/SendHandoff.tla Retire needs a non-empty queue: a retire on an empty
// queue changes nothing, so count never goes negative.
static void test_send_handoff_retire_empty_changes_nothing()
{
    rocketnet_assert_function( send_handoff_assert_recorder );
    send_handoff_assert_count = 0;
    rocketnet_send_handoff_t<2> handoff;
    // two generations queued and retired leave every queued entry stale
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        rocketnet_unit_check(handoff.acquire() >= 0);
        rocketnet_unit_check(handoff.publish());
        handoff.retire();
    }
    const int head = handoff.head;
    rocketnet_unit_check(handoff.count == 0);
    const int second = handoff.acquire();
    rocketnet_unit_check(second >= 0);
    handoff.retire();
    rocketnet_unit_check(handoff.count == 0 && handoff.head == head);
    rocketnet_unit_check(handoff.filling == second && handoff.front() == -1);
    rocketnet_unit_check(!handoff.free[second]);
    for (int i = 0; i < 2; ++i)
        if (i != second) rocketnet_unit_check(handoff.free[i]);
    // the assert stays: it fires wherever asserts are compiled in
    rocketnet_unit_check(send_handoff_assert_count == (ROCKETNET_ASSERTS ? 1 : 0));
    // the handoff still publishes and retires the filling generation
    rocketnet_unit_check(handoff.publish());
    rocketnet_unit_check(handoff.count == 1 && handoff.front() == second);
    handoff.retire();
    rocketnet_unit_check(handoff.count == 0 && handoff.free[second]);
    rocketnet_assert_function( NULL );
}
REGISTER_UNIT_TEST(test_send_handoff_retire_empty_changes_nothing);

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

#endif // #if defined(__linux__)
