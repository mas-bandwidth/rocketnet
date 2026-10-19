#!/bin/sh
# test_check_format.sh: verify check_format.sh passes the clean fixture, fails
# the dirty fixture with its name, reports the file count for a mixed run, and
# exits 2 when clang-format-18 is missing.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/tools/format/check_format.sh"
CLEAN="$ROOT/tools/format/testdata/clean.c"
DIRTY="$ROOT/tools/format/testdata/dirty.c"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! sh "$SCRIPT" "$CLEAN" >"$tmp/clean.out" 2>&1; then
    echo "FAIL: check_format.sh did not pass clean.c" >&2
    exit 1
fi

set +e
sh "$SCRIPT" "$DIRTY" >"$tmp/dirty.out" 2>&1
rc=$?
set -e
if [ "$rc" -ne 1 ]; then
    echo "FAIL: dirty.c expected exit 1, got $rc" >&2
    exit 1
fi
if ! grep -q "dirty.c: not formatted" "$tmp/dirty.out"; then
    echo "FAIL: dirty.c output does not name the file" >&2
    exit 1
fi

set +e
sh "$SCRIPT" "$CLEAN" "$DIRTY" >"$tmp/both.out" 2>&1
rc=$?
set -e
if [ "$rc" -ne 1 ]; then
    echo "FAIL: mixed run expected exit 1, got $rc" >&2
    exit 1
fi
if ! grep -q "1 of 2 files not formatted" "$tmp/both.out"; then
    echo "FAIL: mixed run does not print 1 of 2 files not formatted" >&2
    exit 1
fi

set +e
CLANG_FORMAT=/nonexistent sh "$SCRIPT" "$CLEAN" >"$tmp/missing.out" 2>&1
rc=$?
set -e
if [ "$rc" -ne 2 ]; then
    echo "FAIL: missing clang-format expected exit 2, got $rc" >&2
    exit 1
fi
if ! grep -q "clang-format-18 not found" "$tmp/missing.out"; then
    echo "FAIL: missing clang-format message absent" >&2
    exit 1
fi

echo "ok: check_format.sh fixture checks passed"
