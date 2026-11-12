#!/usr/bin/env bash
# Self-test for tools/check_make_jobs.sh: a child make with a bare -j exits
# nonzero, a bounded -j exits 0, and a tree with zero build files exits
# nonzero naming the count. Fixtures live in a temporary directory.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tool="$root/tools/check_make_jobs.sh"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

fail() { echo "check_make_jobs self-test FAIL: $*"; exit 1; }

# expect CASE STATUS TEXT DIR: the tool on DIR exits STATUS and prints TEXT
expect()
{
    local status=0
    bash "$tool" "$4" > "$tmp/$1.log" 2>&1 || status=$?
    [ "$status" = "$2" ] || fail "$1: exited $status, expected $2: $(cat "$tmp/$1.log")"
    grep -qF "$3" "$tmp/$1.log" || fail "$1: output lacks '$3': $(cat "$tmp/$1.log")"
}

mkdir -p "$tmp/clean/benchmarks" "$tmp/bad" "$tmp/empty"
# shellcheck disable=SC2016  # the fixtures are make text, not shell expansions
printf 'all:\n\t$(MAKE) -j$(JOBS) -C sub\n' > "$tmp/clean/Makefile"
# shellcheck disable=SC2016
printf 'bench:\n\t${MAKE} -j4 run\n' > "$tmp/clean/benchmarks/Makefile"
# shellcheck disable=SC2016
printf 'all:\n\t$(MAKE) -j -C sub\n' > "$tmp/bad/Makefile"

expect clean 0 'ok: every child make has a bounded -j (2 files)' "$tmp/clean"
expect bare-j 1 'Makefile:2: unbounded -j in a child make' "$tmp/bad"
expect zero 1 'FAIL: 0 files checked' "$tmp/empty"

echo 'check_make_jobs self-test passed: bounded, bare -j, and zero files.'
