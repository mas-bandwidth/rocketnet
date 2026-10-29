/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "suite_runner.h"
#include "../sdk/rocketnet_platform.h"

// Uses the private portable transport test-hook build (proofs/tla/RNPortableRx.tla).
// Both cases run on this thread with no socket read, no worker thread and no
// clock: the handoff case drives the FIFO, full, contended, partial and
// oversize transitions; the worker-steps case steps the worker pass through
// a full FIFO, the wake on a return to an empty free FIFO, and stop.
bool rocketnet_portable_rx_test_handoff();
bool rocketnet_portable_rx_test_worker_steps();

static void test_portable_rx_handoff()
{
    rocketnet_unit_check( rocketnet_portable_rx_test_handoff() );
}

static void test_portable_rx_worker_steps()
{
    rocketnet_unit_check( rocketnet_portable_rx_test_worker_steps() );
}

REGISTER_UNIT_TEST( test_portable_rx_handoff );
REGISTER_UNIT_TEST( test_portable_rx_worker_steps );
