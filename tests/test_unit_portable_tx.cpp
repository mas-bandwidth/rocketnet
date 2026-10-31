/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "suite_runner.h"

// Needs ROCKETNET_DEV. No socket or wall-clock schedule:
// the adapter drives borrow, contested publish, full-bank drop and both stop
// interleavings at exact index transitions; one worker uses a paused seam.
bool rocketnet_portable_tx_test_handoff();

static void test_portable_tx_handoff()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_handoff() );
}

REGISTER_UNIT_TEST( test_portable_tx_handoff );

// Stop keeps a producer it cannot publish and publishes it on a later poll,
// before quit (proofs/tla/RNPortableTx.tla MStopRetry); nothing is dropped.
bool rocketnet_portable_tx_test_stop_publishes_across_polls();

static void test_portable_tx_stop_publishes_across_polls()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_stop_publishes_across_polls() );
}

REGISTER_UNIT_TEST( test_portable_tx_stop_publishes_across_polls );
