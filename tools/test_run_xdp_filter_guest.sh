#!/usr/bin/env bash
# Self-test for tools/run_xdp_filter_guest.sh's log validation, through its
# --verify-log mode (no guest, no kernel traffic): a log with a skipped case
# and a log with a FAILED case each exit nonzero by name, a log with every
# required case ok and the exact summary exits
# 0, and an empty log or a log with zero case lines exits nonzero. Logs and
# artifacts live in a temporary directory.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tool="$root/tools/run_xdp_filter_guest.sh"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

fail() { echo "run_xdp_filter_guest self-test FAIL: $*"; exit 1; }

# expect CASE STATUS TEXT LOG: --verify-log LOG exits STATUS and prints TEXT
expect()
{
    local status=0
    bash "$tool" --verify-log "$4" --artifact-dir "$tmp/artifacts-$1" > "$tmp/$1.out" 2>&1 || status=$?
    [ "$status" = "$2" ] || fail "$1: exited $status, expected $2: $(cat "$tmp/$1.out")"
    grep -qF -- "$3" "$tmp/$1.out" || fail "$1: output lacks '$3': $(cat "$tmp/$1.out")"
}

# The required cases, read from the tool so the fixture follows its list.
sed -n '/^REQUIRED_CASES=(/,/^)/p' "$tool" | sed -n 's/^ *"\([a-z0-9_]*\)"$/\1/p' > "$tmp/cases"
count=$(wc -l < "$tmp/cases" | tr -d ' ')
[ "$count" -gt 0 ] || fail "read 0 required cases from $tool"
summary="test_xdp_filter: $count cases passed"

{
    echo "verifier: server_xdp.o loaded"
    sed 's/$/: ok/' "$tmp/cases"
    echo "$summary"
} > "$tmp/clean.log"
sed '3s/: ok$/: skipped (no XDP support)/' "$tmp/clean.log" > "$tmp/skipped.log"
sed '3s/: ok$/: FAILED/' "$tmp/clean.log" > "$tmp/failed.log"
: > "$tmp/empty.log"
printf 'verifier: server_xdp.o loaded\n%s\n' "$summary" > "$tmp/nocases.log"

expect clean 0 "XDP-FILTER-GUEST PASS (verify-log): $summary" "$tmp/clean.log"
expect skipped 1 'skipped case(s) detected' "$tmp/skipped.log"
expect failed 1 'failed case(s) detected' "$tmp/failed.log"
expect empty 1 'XDP-FILTER-GUEST FAIL' "$tmp/empty.log"
expect zero-cases 1 'ran 0 case lines' "$tmp/nocases.log"

echo "run_xdp_filter_guest self-test passed: $count cases ok, skipped case, failed case, empty log, and zero case lines."
