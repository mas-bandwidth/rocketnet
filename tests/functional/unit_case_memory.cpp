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
*/

#include "../suite_runner.h"

suite_exec rocketnet_unit_case_memory_exec()
{
    const suite_exec exec = { "test_unit_case_memory", "test", "--enforce-rss", false };
    return exec;
}
