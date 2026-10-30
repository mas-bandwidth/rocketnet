/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The suite runner's job count against its Windows wait set. The Windows
// runner waits on every running case from a fixed array in one
// WaitForMultipleObjects call, so the job count must never exceed that array.
// It once held 8 handles while the count followed the cores and the memory
// (about 30 on captain: 64 cores, 48 GiB free), and the ninth running case
// wrote past it: the parent ./bin/test died with 0xC0000409 partway through
// the suite. The count is a function of its inputs here; no time, sockets or
// processes.

#include "test_unit_registry.h"
#include "suite_runner.h"

#define check rocketnet_unit_check

static void test_suite_job_count_fits_the_windows_wait_set()
{
    const uint64_t gib = 1024ull * 1024 * 1024;

    // one job per core, at least one, as many as the memory holds

    check( suite_job_count_for( 4, 0, 0, SUITE_WAIT_HANDLES ) == 4 );
    check( suite_job_count_for( 0, 0, 0, SUITE_WAIT_HANDLES ) == 1 );
    check( suite_job_count_for( 64, 1, 0, SUITE_WAIT_HANDLES ) == 1 );
    check( suite_job_count_for( 64, 10 * gib, 0, SUITE_WAIT_HANDLES ) == 6 );

    // 64 cores and 50 GiB plan 31 jobs, far past the old 8

    check( suite_job_count_for( 64, 50 * gib, 0, SUITE_WAIT_HANDLES ) == 31 );

    // the ceiling holds against the cores and against a requested count

    check( suite_job_count_for( 128, 0, 0, SUITE_WAIT_HANDLES ) == SUITE_WAIT_HANDLES );
    check( suite_job_count_for( 64, 50 * gib, 1000, SUITE_WAIT_HANDLES ) == SUITE_WAIT_HANDLES );
    check( suite_job_count_for( 4, 0, 9, SUITE_WAIT_HANDLES ) == 9 );
    check( suite_job_count_for( 4, 0, -3, SUITE_WAIT_HANDLES ) == 4 );

    // without a ceiling (POSIX) the count is not capped

    check( suite_job_count_for( 128, 0, 0, 0 ) == 128 );
    check( suite_job_count_for( 4, 0, 1000, 0 ) == 1000 );

    // the live count on this host, whatever its cores, memory and
    // ROCKETNET_TEST_JOBS, fits the Windows wait set

#if defined( _WIN32 )
    check( suite_job_count() >= 1 );
    check( suite_job_count() <= SUITE_WAIT_HANDLES );
#endif // #if defined( _WIN32 )
}

REGISTER_UNIT_TEST( test_suite_job_count_fits_the_windows_wait_set );
