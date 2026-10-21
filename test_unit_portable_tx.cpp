/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "test/suite_runner.h"

// Compiled only into rocketnet_test_hooks. No socket, worker or wall clock:
// the adapter drives borrow, contested publish, full-bank drop and stop order
// at exact index transitions.
bool rocketnet_portable_tx_test_handoff();

static void test_portable_tx_handoff()
{
    rocketnet_unit_check( rocketnet_portable_tx_test_handoff() );
}

REGISTER_UNIT_TEST( test_portable_tx_handoff );
