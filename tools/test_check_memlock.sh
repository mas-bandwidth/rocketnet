#!/usr/bin/env bash
# Self-test for tools/check_memlock.sh: a RLIMIT_MEMLOCK under runners times
# the per-case need exits nonzero, a limit that covers it exits 0, and a tree
# whose tests/suite_runner.h carries zero per-case need defines exits nonzero
# naming the count. Runs on any host: fake uname (Linux 6.12) and pgrep (one
# runner) come first on PATH, the /proc read is a fixture, the soft limit is
# lowered in the child shell only, and no transport_memory probe exists, so
# nothing creates a socket or a ring.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tool="$root/tools/check_memlock.sh"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
unset ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB

fail() { echo "check_memlock self-test FAIL: $*"; exit 1; }

mkdir -p "$tmp/bin" "$tmp/proc/sys/kernel"
# shellcheck disable=SC2016  # the fake's own shell expands $1
printf '#!/bin/sh\ncase "$1" in -r) echo 6.12.0 ;; *) echo Linux ;; esac\n' > "$tmp/bin/uname"
printf '#!/bin/sh\necho 1\n' > "$tmp/bin/pgrep"
chmod +x "$tmp/bin/uname" "$tmp/bin/pgrep"
echo 0 > "$tmp/proc/sys/kernel/io_uring_disabled"

# the soft limit the tool sees: 1024 KiB, or the hard limit when that is lower
limit=1024
hard=$(ulimit -H -l)
if [ "$hard" != unlimited ] && [ "$hard" -lt "$limit" ]; then
    limit=$hard
fi
[ "$limit" -ge 2 ] || fail "hard RLIMIT_MEMLOCK $hard KiB is too small to plant a fixture"

# fixture TREE NEED: a tree whose suite runner asks NEED KiB per case
fixture()
{
    mkdir -p "$tmp/$1/tests"
    printf '#define ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB %s\n#define ROCKETNET_TEST_URING_CASE_MEMLOCK_KIB_6_8 %s\n' \
        "$2" "$2" > "$tmp/$1/tests/suite_runner.h"
}
fixture covered $((limit / 2))
fixture short $((limit * 2))
mkdir -p "$tmp/empty"

# expect CASE STATUS TEXT TREE: the tool under the lowered limit exits STATUS and prints TEXT
expect()
{
    local status=0
    PATH="$tmp/bin:$PATH" ROCKETNET_MEMLOCK_PROC="$tmp/proc" \
        bash -c 'ulimit -S -l "$1" && exec bash "$2" --root "$3"' memlock "$limit" "$tool" "$4" \
        > "$tmp/$1.log" 2>&1 || status=$?
    [ "$status" = "$2" ] || fail "$1: exited $status, expected $2: $(cat "$tmp/$1.log")"
    grep -qF -- "$3" "$tmp/$1.log" || fail "$1: output lacks '$3': $(cat "$tmp/$1.log")"
}

expect covered 0 "memlock: $limit KiB covers $((limit / 2)) KiB: PASS" "$tmp/covered"
expect short 1 "memlock: FAIL: $limit KiB is under $((limit * 2)) KiB" "$tmp/short"
expect zero 1 'memlock: FAIL: 0 of 2 per-case need defines' "$tmp/empty"

echo "check_memlock self-test passed: covered, short limit ($limit KiB), and zero defines."
