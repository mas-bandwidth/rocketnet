#!/usr/bin/env bash
# Self-test of scripts/transport-cells-repeat's verdicts: a clean run passes;
# a case list of blanks is refused (it once ran nothing and passed); an
# io_uring cell that scripts/io-uring-gate refuses fails the run, and one it
# skips (a listed machine) does not; a failing case fails the run. Fixtures
# only, in a temporary directory: a fake ci.yml, a fake functional binary
# (prints the case's name; FAKE_FAIL_CASE exits 1 instead), and a copy of the
# script beside a fake io-uring-gate that answers as FAKE_URING says (on,
# listed or refused), so the real kernel's switch is never read. Linux only
# (the cells are the Linux matrix's). Run by the lint job of ci.yml.

set -euo pipefail

if [ "$(uname -s)" != Linux ]; then
    echo "transport-cells-repeat self-test: Linux only (this host is $(uname -s))"
    exit 2
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir "$work/scripts"
cp "$(cd "$(dirname "$0")/.." && pwd)/scripts/transport-cells-repeat" "$work/scripts/"
script="$work/scripts/transport-cells-repeat"
{
    echo '#!/bin/sh'
    echo 'case "${FAKE_URING:-on}" in'
    echo '  on) exit 0 ;;'
    echo '  listed) echo "$1 [SKIPPED io_uring absent on fake: listed]"; exit 3 ;;'
    echo '  *) echo "$1 FAIL this kernel disables io_uring (io_uring_disabled=2) on fake"; exit 1 ;;'
    echo 'esac'
} > "$work/scripts/io-uring-gate"
chmod +x "$work/scripts/io-uring-gate"

printf '#!/bin/sh\nif [ "$2" = "${FAKE_FAIL_CASE:-}" ]; then echo "check failed: $2"; exit 1; fi\necho "$2"\n' > "$work/functional"
chmod +x "$work/functional"

# ci <file> <socket_cases value>: a one-step workflow with the Linux matrix
ci() {
    {
        echo 'jobs:'
        echo '  matrix:'
        echo '    steps:'
        echo '      - name: Transport family matrix'
        echo '        env:'
        echo '          TRANSPORT_MATRIX_LINUX: |'
        echo '            portable portable'
        echo '            io_uring portable'
        echo '        run: |'
        echo "          socket_cases=\"$2\""
    } > "$1"
}
ci "$work/ci.yml" "test_a test_b"
ci "$work/ci-blank.yml" "   "

cases=0
failures=0
# expect <exit> <name> <grep -E pattern> <env assignments and args...>
expect() {
    local want="$1" name="$2" pattern="$3"
    shift 3
    cases=$((cases + 1))
    local rc=0
    ( cd "$work" && env "$@" ) > "$work/out" 2>&1 || rc=$?
    if [ "$rc" != "$want" ] || ! grep -Eq -- "$pattern" "$work/out"; then
        echo "FAIL $name: exit $rc (want $want), output:"
        sed 's/^/    /' "$work/out"
        failures=$((failures + 1))
    else
        echo "ok   $name"
    fi
}

common=(--os linux --runs 2 --functional "$work/functional" --out "$work/out-dir")
expect 0 "every case of every cell passes" 'CELL io_uring/portable test_b runs=2 pass=2 fail=0' \
    FAKE_URING=on "$script" "${common[@]}" --ci "$work/ci.yml"
expect 2 "a case list of blanks is refused" 'names no cases' \
    FAKE_URING=on "$script" "${common[@]}" --ci "$work/ci-blank.yml"
expect 1 "an io_uring cell the gate refuses fails the run" 'CELL io_uring/portable FAIL this kernel disables io_uring' \
    FAKE_URING=refused "$script" "${common[@]}" --ci "$work/ci.yml"
expect 0 "an io_uring cell on a listed machine is skipped by name" 'CELL io_uring/portable \[SKIPPED io_uring absent on fake: listed\]' \
    FAKE_URING=listed "$script" "${common[@]}" --ci "$work/ci.yml"
expect 1 "a failing case fails the run" 'CELL portable/portable test_a runs=2 pass=0 fail=2' \
    FAKE_FAIL_CASE=test_a FAKE_URING=on "$script" "${common[@]}" --ci "$work/ci.yml"

if [ "$failures" -ne 0 ]; then
    echo "transport-cells-repeat self-test: $failures of $cases cases failed"
    exit 1
fi
echo "ok: transport-cells-repeat self-test, $cases cases"
