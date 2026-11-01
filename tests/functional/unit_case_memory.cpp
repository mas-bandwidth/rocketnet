/*
    RocketNet. Copyright 2026 Mas Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0

    The functional memory row runs the complete unit binary in RSS-enforcement
    mode. The unit binary owns every case registration and bound declaration,
    so the functional tier consumes the same values without a copied table and
    also covers unit cases that use the suite-wide bound. The outer runner does
    not compare the unit controller's aggregate child usage with an unrelated
    case bound; the inner runner enforces each case, and both runners keep their
    existing time limits.

    The row runs where a bound is enforced. A sanitizer build enforces none
    unless ROCKETNET_TEST_MAX_CASE_RSS_MB sets one (suite_max_case_rss_mb:
    shadow memory is not a case's footprint), so there the row would check
    nothing and only repeat the unit suite, which every sanitizer job runs as
    its own step and which takes longer than the row's 120 s limit under a
    sanitizer. It is left out and printed as SKIPPED.
*/

#include "../suite_runner.h"

suite_exec rocketnet_unit_case_memory_exec()
{
    const suite_exec exec = { "test_unit_case_memory", "test", "--enforce-rss", false };
    return exec;
}

bool rocketnet_unit_case_memory_runs_here()
{
    if ( suite_max_case_rss_mb() > 0 )
        return true;
    suite_note_skipped( "test_unit_case_memory", "suite_max_case_rss_mb" );
    return false;
}
