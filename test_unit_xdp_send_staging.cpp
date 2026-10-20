/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "sdk/rocketnet_send_handoff.h"
#include "test_unit_registry.h"

// The send worker does not retire a generation until the test tells it to.
// No sockets, worker threads or clock take part in this interleaving.
static void xdp_send_staging_retains_producer()
{
    rocketnet_send_handoff_t<2> handoff;
    const int first = handoff.acquire();
    rocketnet_unit_check(first >= 0);
    handoff.publish();
    const int second = handoff.acquire();
    rocketnet_unit_check(second >= 0 && second != first);
    handoff.publish();
    // A second flush while the worker is behind keeps producer storage.
    rocketnet_unit_check(handoff.acquire() >= 0);
    rocketnet_unit_check(handoff.filling == second);
    rocketnet_unit_check(handoff.count == 1 && handoff.front() == first);
    for (int flush = 0; flush < 4; ++flush)
    {
        handoff.publish();
        rocketnet_unit_check(handoff.acquire() == second);
        rocketnet_unit_check(handoff.front() == first && handoff.count == 1);
    }
    handoff.retire();
    handoff.publish();
    rocketnet_unit_check(handoff.front() == second);
    rocketnet_unit_check(handoff.acquire() == first);
    handoff.retire();
    rocketnet_unit_check(handoff.count == 0);
}
REGISTER_UNIT_TEST(xdp_send_staging_retains_producer);

// The fixture calls the actual transport send_packets with a fake staged
// backend. After the first call the staged and flushed counters agree.
int rocketnet_server_transport_test_xdp_reflush();
static void xdp_send_staging_reflushes_without_commit()
{
    rocketnet_unit_check(rocketnet_server_transport_test_xdp_reflush() == 2);
}
REGISTER_UNIT_TEST(xdp_send_staging_reflushes_without_commit);
