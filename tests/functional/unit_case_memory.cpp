/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The functional memory row runs the complete unit binary in RSS-enforcement
    mode. The unit binary owns every case registration and bound declaration,
    so the functional tier consumes the same values without a copied table and
    also covers unit cases that use the suite-wide bound. The outer runner does
    not compare the unit controller's aggregate child usage with an unrelated
    case bound; the inner runner enforces each case and keeps its time limit.

    The row is the whole unit suite, so it has a limit of its own rather than
    a functional case's 120 s. Its wall is set by its slowest case: under
    ROCKETNET_TEST_NIGHTLY=1, test_server_per_client_mixed_subset_at_max_clients
    (34 to 36 s in Debug). Measured walls of `./bin/test --enforce-rss` with
    the nightly tier on, Debug, 2026-11-12, three runs each: bench-nyc-4, one
    nova-reserve slot of 5 cpus (15 jobs), gcc 45.6, 46.2, 47.6 s and clang
    48.0, 47.4, 47.2 s; bench-nyc-1, three 4-cpu slots running it at once (12
    jobs each, load 18 to 24), 47 to 59 s; mini-m5 (macOS, 18 cpus) 32.9,
    34.4, 32.2 s. Release 16.8 to 17.3 s (Linux), 12.2 s (macOS). The limit is
    the slowest, 59 s, at the repo's 60% margin rule: 59 / 0.6 = 98.3, so
    99 s. (An earlier 103 s macOS reading was of a tree whose many-client
    setup grew with the square of the client count; it is not this tree.)
    ROCKETNET_TEST_CASE_TIME_SCALE and ROCKETNET_NIGHTLY_NO_TIMEOUT apply to it
    as to every limit.

    The row runs where a bound is enforced. A sanitizer build enforces none
    unless ROCKETNET_TEST_MAX_CASE_RSS_MB sets one (suite_max_case_rss_mb:
    shadow memory is not a case's footprint), so there the row would check
    nothing and only repeat the unit suite, which every sanitizer job runs as
    its own step and which takes longer than the row's 120 s limit under a
    sanitizer. It is left out and printed as SKIPPED.
*/

#include "../suite_runner.h"

#define ROCKETNET_UNIT_CASE_MEMORY_MAX_SECONDS 99.0

suite_exec rocketnet_unit_case_memory_exec()
{
    const suite_exec exec = { "test_unit_case_memory", "test", "--enforce-rss", false, ROCKETNET_UNIT_CASE_MEMORY_MAX_SECONDS };
    return exec;
}

bool rocketnet_unit_case_memory_runs_here()
{
    if ( suite_max_case_rss_mb() > 0 )
    {
        return true;
    }
    suite_note_skipped( "test_unit_case_memory", "suite_max_case_rss_mb" );
    return false;
}
