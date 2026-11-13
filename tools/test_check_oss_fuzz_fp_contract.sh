#!/usr/bin/env bash
# Self-test for tools/check_oss_fuzz_fp_contract.cmake: a $CC/$CXX compile
# line without -ffp-contract=off exits nonzero (the flag in a trailing comment
# does not count, and a one-step compile-and-link is a compile line), a script
# whose every compile line carries it exits 0 (a commented-out command is no
# compile line), and an empty build script (zero compile lines)
# exits nonzero naming the count. Runs cmake -P on fixtures in a temporary
# directory; nothing is built.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tool="$root/tools/check_oss_fuzz_fp_contract.cmake"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

fail() { echo "check_oss_fuzz_fp_contract self-test FAIL: $*"; exit 1; }

# expect CASE STATUS-KIND TEXT SCRIPT: STATUS-KIND is zero or nonzero
expect()
{
    local status=0
    cmake -DROCKETNET_OSS_FUZZ_BUILD_SCRIPT="$4" -P "$tool" > "$tmp/$1.log" 2>&1 || status=$?
    if [ "$2" = zero ]; then
        [ "$status" = 0 ] || fail "$1: exited $status, expected 0: $(cat "$tmp/$1.log")"
    else
        [ "$status" != 0 ] || fail "$1: exited 0, expected nonzero: $(cat "$tmp/$1.log")"
    fi
    tr -s " \n" "  " < "$tmp/$1.log" | grep -qF -- "$3" || fail "$1: output lacks '$3': $(cat "$tmp/$1.log")"
}

# shellcheck disable=SC2016  # the fixtures are build-script text
printf '#!/bin/bash\n$CC $CFLAGS -ffp-contract=off -c a.c -o a.o\n$CXX $CXXFLAGS -ffp-contract=off -c b.cpp -o b.o\n$CXX a.o b.o -o fuzz\n' > "$tmp/clean.sh"
# shellcheck disable=SC2016
printf '#!/bin/bash\n$CC $CFLAGS -ffp-contract=off -c a.c -o a.o\n$CXX $CXXFLAGS -c b.cpp -o b.o\n$CXX a.o b.o -o fuzz\n' > "$tmp/bad.sh"
# the flag only in a trailing shell comment
# shellcheck disable=SC2016
printf '#!/bin/bash\n$CC $CFLAGS -c a.c -o a.o # -ffp-contract=off\n' > "$tmp/comment.sh"
# a one-step compile-and-link names its source and passes no -c
# shellcheck disable=SC2016
printf '#!/bin/bash\n$CC $CFLAGS -ffp-contract=off -c a.c -o a.o\n$CXX $CXXFLAGS fuzz.cpp a.o -o fuzz\n' > "$tmp/onestep.sh"
# a commented-out command is no compile line, and a link of objects needs no flag
# shellcheck disable=SC2016
printf '#!/bin/bash\n$CC $CFLAGS -ffp-contract=off -c a.c -o a.o\n# $CXX $CXXFLAGS -c old.cpp -o old.o\n$CXX a.o -o fuzz\n' > "$tmp/commented.sh"
: > "$tmp/empty.sh"

expect clean zero 'builds every compile line (2) with -ffp-contract=off' "$tmp/clean.sh"
expect missing-flag nonzero 'does not build with -ffp-contract=off' "$tmp/bad.sh"
expect comment-only nonzero 'does not build with -ffp-contract=off' "$tmp/comment.sh"
expect one-step nonzero 'compile line lacks -ffp-contract=off: $CXX $CXXFLAGS fuzz.cpp a.o -o fuzz' "$tmp/onestep.sh"
expect commented-out zero 'builds every compile line (1) with -ffp-contract=off' "$tmp/commented.sh"
expect zero nonzero '0 compile lines' "$tmp/empty.sh"

echo 'check_oss_fuzz_fp_contract self-test passed: clean, missing flag, flag only in a comment, a one-step compile-and-link, a commented-out command, and zero compile lines.'
