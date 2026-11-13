#!/usr/bin/env bash
# Self-test of tools/run_functional.sh: the exit is the suite's when the suite
# failed, the skip check's when it passed, and a planted skip problem is red.
# Fixtures only: fake suites (shell scripts printing a suite's tail and
# exiting with a chosen code) and a fake skip list, in a temporary directory.
# Run by the lint job of ci.yml.

set -euo pipefail

script="$(cd "$(dirname "$0")" && pwd)/run_functional.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

case "$(uname -s)" in
    Linux) os=Linux ;;
    Darwin) os=Darwin ;;
    *) os=Windows ;;
esac
printf '%s\n' "leg $(echo "$os" | sed -e 's/Linux/linux/' -e 's/Darwin/macos/' -e 's/Windows/windows/')-t $os auto" \
    'reason p_root needs root, which this host does not give' \
    "skip test_a p_root $(echo "$os" | sed -e 's/Linux/linux/' -e 's/Darwin/macos/' -e 's/Windows/windows/')-t" > "$work/list.txt"
export ROCKETNET_EXPECTED_SKIPS="$work/list.txt"
# The registry run_functional.sh's second check (check_test_environment.py
# --functional-log) holds the run to: test_a under p_root, test_b everywhere.
printf '%s\n' '{ "test_a", rocketnet_test_a, p_root },' '{ "test_b", rocketnet_test_b, NULL },' > "$work/registry.cpp"
export ROCKETNET_FUNCTIONAL_REGISTRY="$work/registry.cpp"
unset ROCKETNET_SERVER_TRANSPORT

# fake <name> <exit> <lines...>: a suite that prints the lines and exits
fake() {
    local name="$1" code="$2"
    shift 2
    {
        echo '#!/usr/bin/env bash'
        for line in "$@"; do
            printf 'echo %q\n' "$line"
        done
        echo "exit $code"
    } > "$work/$name"
    chmod +x "$work/$name"
}

fake good 0 'test_b' 'SKIPPED test_a: p_root' 'skipped: 1' '' '*** ALL TESTS PASSED ***' ''
fake red_suite 1 'check failed: test_b exit 1' 'SKIPPED test_a: p_root' 'skipped: 1'
fake unlisted 0 'SKIPPED test_a: p_root' 'SKIPPED test_c: p_root' 'skipped: 2' '*** ALL TESTS PASSED ***'
fake ran 0 'test_a' 'skipped: 0' '*** ALL TESTS PASSED ***'
fake crashed 139 'test_b'
fake crlf 0 $'test_b\r' $'SKIPPED test_a: p_root\r' $'skipped: 1\r' $'\r' $'*** ALL TESTS PASSED ***\r' $'\r'
fake lost 0 'SKIPPED test_a: p_root' 'skipped: 1' '' '*** ALL TESTS PASSED ***' ''
# test_a falls in shard 1 of 2 and test_b in shard 0 (tests/suite_runner.h suite_shard_of); every shard prints all SKIPPED lines
fake shard0 0 'shard 0/2: 1 of 2 cases' 'test_b' 'SKIPPED test_a: p_root' 'skipped: 1' '' '*** ALL TESTS PASSED ***' ''
fake shard1 0 'shard 1/2: 1 of 2 cases' 'SKIPPED test_a: p_root' 'skipped: 1' '' '*** ALL TESTS PASSED ***' ''
fake shard0_lost 0 'shard 0/2: 1 of 2 cases' 'SKIPPED test_a: p_root' 'skipped: 1' '' '*** ALL TESTS PASSED ***' ''
fake shard0_noline 0 'test_b' 'SKIPPED test_a: p_root' 'skipped: 1' '' '*** ALL TESTS PASSED ***' ''
fake report 0 'jobs: 4 (cpus this run may use: 4)' 'test_b' 'SKIPPED test_a: p_root' 'skipped: 1' '' 'slowest cases:' '    1.200s  test_b' 'peak rss:' '     10 MiB (bound 20)  test_b' '' '*** ALL TESTS PASSED ***' ''
fake stranger 0 'test_b' 'test_z' 'SKIPPED test_a: p_root' 'skipped: 1' '' '*** ALL TESTS PASSED ***' ''

failures=0
cases=0
# expect <want exit> <name> <args...>
expect() {
    local want="$1" name="$2"
    shift 2
    cases=$((cases + 1))
    set +e
    (cd "$work" && bash "$script" "$@") > "$work/out.txt" 2>&1
    local got=$?
    set -e
    if [ "$got" != "$want" ]; then
        echo "FAIL $name: want exit $want, got $got"
        sed 's/^/    /' "$work/out.txt"
        failures=$((failures + 1))
    fi
}

expect 0 "clean suite, exact skips" t "$work/good"
expect 0 "the ROCKETNET_TEST_REPORT lines are reports, not cases" t "$work/report"
expect 0 "carriage returns removed" t "$work/crlf"
expect 1 "suite failed: the suite's exit" t "$work/red_suite"
expect 1 "unlisted skip" t "$work/unlisted"
expect 1 "listed row ran" t "$work/ran"
expect 1 "a registered row neither ran nor skipped" t "$work/lost"
expect 1 "a printed name no registry holds" t "$work/stranger"
expect 139 "crash: the suite's exit, not the check's" t "$work/crashed"
expect 0 "shard 0/2: its rows only" t --shard 0/2 "$work/shard0"
expect 0 "shard 1/2: its skip only" t --shard 1/2 "$work/shard1"
expect 1 "shard 0/2: a row of the shard neither ran nor skipped" t --shard 0/2 "$work/shard0_lost"
expect 1 "shard run without its shard line" t --shard 0/2 "$work/shard0_noline"
expect 1 "whole-suite log must hold every row (a shard log is not whole)" t "$work/shard0"
expect 1 "undeclared variant" other "$work/good"
expect 2 "no command" t
expect 0 "--log names the log" t --log "$work/named.log" "$work/good"
cases=$((cases + 1))
if ! grep -qx 'skipped: 1' "$work/named.log"; then
    echo "FAIL --log: the suite's output is not in the named log"
    failures=$((failures + 1))
fi
export ROCKETNET_SERVER_TRANSPORT=portable
expect 1 "pin other than the leg's" t "$work/good"
unset ROCKETNET_SERVER_TRANSPORT

if [ "$failures" -ne 0 ]; then
    echo "run_functional.sh self-test: $failures of $cases cases failed"
    exit 1
fi
echo "ok: run_functional.sh self-test, $cases cases"
