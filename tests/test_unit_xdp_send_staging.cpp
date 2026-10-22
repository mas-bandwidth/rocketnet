/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "../sdk/rocketnet_send_handoff.h"
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

int rocketnet_server_transport_test_staged_simulator_flush();
static void test_xdp_transport_send_packets_flushes_simulator_window()
{
    rocketnet_unit_check(rocketnet_server_transport_test_staged_simulator_flush() == 1);
}
REGISTER_UNIT_TEST(test_xdp_transport_send_packets_flushes_simulator_window);

#endif // #if defined(__linux__)
