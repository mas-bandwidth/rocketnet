/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */
#include "test_unit_registry.h"
#include "test/suite_runner.h"
#include "sdk/rocketnet_platform.h"

// Uses the existing private portable transport test-hook build. The handoff
// case drives FIFO/full/contention/partial/oversize transitions deterministically;
// the worker case verifies stop while RX is full and TX-only creation unwind.
bool rocketnet_portable_rx_test_handoff();
bool rocketnet_portable_rx_test_stop_and_cleanup();

static void test_portable_rx_handoff()
{
    rocketnet_unit_check( rocketnet_portable_rx_test_handoff() );
}

static void test_portable_rx_stop_and_cleanup()
{
    rocketnet_unit_check( rocketnet_portable_rx_test_stop_and_cleanup() );
}

REGISTER_UNIT_TEST( test_portable_rx_handoff );
REGISTER_UNIT_TEST( test_portable_rx_stop_and_cleanup );

#if ROCKETNET_PLATFORM == ROCKETNET_PLATFORM_MAC
bool rocketnet_msgx_test_pause_expiry_race();
static void test_msgx_pause_expiry_preserves_new_pause()
{
    rocketnet_unit_check( rocketnet_msgx_test_pause_expiry_race() );
}
REGISTER_UNIT_TEST( test_msgx_pause_expiry_preserves_new_pause );
#endif
