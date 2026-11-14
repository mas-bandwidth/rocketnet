#!/usr/bin/env bash
# Self-test of scripts/channel-seed-sweep's FAIL reasons: the reason is the
# measurement line's shape ("<case>: <shape> delivered=...") when a case
# prints one, else the failing check line, and never a word of the case's
# table banner ("<case>: one 262144-byte block ...", which once gave
# reason=one for seed 4914435); a green run that took its seed passes.
# Fixtures only: a fake functional binary that records its seed, prints
# $FAKE_OUT and exits $FAKE_EXIT, in a temporary directory. Linux only (the
# sweep runs on the Linux nightly runner). Run by the lint-tools job of ci.yml.

set -euo pipefail

script="$(cd "$(dirname "$0")/.." && pwd)/scripts/channel-seed-sweep"
if [ "$(uname -s)" != Linux ]; then
    echo "channel-seed-sweep self-test: Linux only (this host is $(uname -s))"
    exit 2
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

printf '#!/bin/sh\necho "$ROCKETNET_CHANNEL_SEED" >> "$ROCKETNET_CHANNEL_SEED_RECORD"\nprintf "%%b" "$FAKE_OUT"\nexit "$FAKE_EXIT"\n' > "$work/functional"
chmod +x "$work/functional"

cases=0
failures=0
# expect <exit> <name> <grep -E pattern> <fake output> <fake exit>
expect() {
    local want="$1" name="$2" pattern="$3" out="$4" code="$5"
    cases=$((cases + 1))
    local rc=0
    FAKE_OUT="$out" FAKE_EXIT="$code" "$script" --cases test_fake --seeds 7 --jobs 1 --timeout 30 --bin "$work/functional" > "$work/out" 2>&1 || rc=$?
    if [ "$rc" != "$want" ] || ! grep -Eq -- "$pattern" "$work/out"; then
        echo "FAIL $name: exit $rc (want $want), output:"
        sed 's/^/    /' "$work/out"
        failures=$((failures + 1))
    else
        echo "ok   $name"
    fi
}

expect 1 "a banner then a failed check gives the check, not the banner's word" '^FAIL seed=7 case=test_fake reason=rows\[4\]\.small_held == 0$' \
    'test_fake: one 262144-byte reliable-ordered block, rtt 200 ms\nresend timing check failed: rows[4].small_held == 0 at line 782\ncheck failed: test_fake left through exit() before it returned\n' 1
expect 1 "a measurement line gives its shape" '^FAIL seed=7 case=test_fake reason=budget_multiple$' \
    'seed=0x7\ntest_fake: budget_multiple delivered=10/10 completion_ms=1.0\nordered check failed at line 311: within_budget\n' 1
expect 1 "a check macro gives its condition" '^FAIL seed=7 case=test_fake reason=a == b$' \
    'test_fake: a table banner\ncheck failed: ( a == b ), function f, file x.cpp, line 3\n' 1
expect 1 "a failed-at-line check gives its condition" '^FAIL seed=7 case=test_fake reason=got == want$' \
    'channel receipts failed at line 9: got == want\n' 1
expect 1 "no check line gives the exit status" '^FAIL seed=7 case=test_fake reason=exit_3$' \
    'test_fake: a table banner\n' 3
expect 0 "a green run that took its seed passes" '^sweep: 1 runs, 0 failures$' \
    'test_fake\n' 0

if [ "$failures" -ne 0 ]; then
    echo "channel-seed-sweep self-test: $failures of $cases cases failed"
    exit 1
fi
echo "ok: channel-seed-sweep self-test, $cases cases"
