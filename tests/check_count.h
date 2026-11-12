/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// Every check macro of the unit binary (rocketnet_unit_check, rocketnet_check,
// channels_check, the channels file's check) counts itself here each time it
// runs, pass or fail. The unit worker fails a case that returned having run
// no check at all: a body compiled out on one platform, or a case that
// returns before its checks, would otherwise pass there having verified
// nothing. One counter for the process, shared by every translation unit
// (an inline function's static), atomic because cases check on threads too.

#pragma once

#ifndef ROCKETNET_TESTS_CHECK_COUNT_H
#define ROCKETNET_TESTS_CHECK_COUNT_H

#include <atomic>
#include <cstdint>

inline std::atomic<uint64_t> & rocketnet_test_checks_run()
{
    static std::atomic<uint64_t> checks( 0 );
    return checks;
}

#define ROCKETNET_TEST_COUNT_CHECK() rocketnet_test_checks_run().fetch_add( 1, std::memory_order_relaxed )

#endif // ROCKETNET_TESTS_CHECK_COUNT_H
