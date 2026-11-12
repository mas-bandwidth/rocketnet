#!/bin/sh
# test_check_format.sh: verify check_format.sh passes the clean fixture, fails
# the dirty fixture with its name, reports the file count for a mixed run, and
# exits 2 when clang-format-18 is missing.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/tools/format/check_format.sh"
cd "$ROOT" || exit 1
CLEAN="tools/format/testdata/clean.c"
DIRTY="tools/format/testdata/dirty.c"

mkdir -p "$ROOT/build"
tmp="$(mktemp -d "$ROOT/build/format-test.XXXXXX")"
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
if ! grep -qx "tools/format/testdata/dirty.c: not formatted" "$tmp/dirty.out"; then
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

# zero inputs: a repository with no tracked source, and a git that cannot
# list, are both red (CLANG_FORMAT=true: no file reaches the formatter)
mkdir "$tmp/empty"
git -C "$tmp/empty" init -q
set +e
(cd "$tmp/empty" && CLANG_FORMAT=true sh "$SCRIPT") >"$tmp/empty.out" 2>&1
rc=$?
set -e
if [ "$rc" -ne 1 ] || ! grep -q "0 files to check" "$tmp/empty.out"; then
    echo "FAIL: zero tracked sources expected exit 1 with '0 files to check', got $rc" >&2
    cat "$tmp/empty.out" >&2
    exit 1
fi
set +e
GIT_DIR=/nonexistent CLANG_FORMAT=true sh "$SCRIPT" >"$tmp/nogit.out" 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo "FAIL: a git that cannot list passed the format check" >&2
    cat "$tmp/nogit.out" >&2
    exit 1
fi

echo "ok: check_format.sh fixture checks passed"
