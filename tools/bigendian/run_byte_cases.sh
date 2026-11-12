#!/usr/bin/env bash
# Runs the unit cases that judge bytes and need no socket, one process per
# case, under an optional runner: the standard's byte vectors
# (tests/test_unit_standard_*.cpp, tests/test_unit_connect_token_vectors.cpp)
# and the XDP filter compiled as host C (tests/xdp_host/,
# tests/test_unit_xdp_filter_host.cpp). The per-commit big-endian leg in
# .github/workflows/ci.yml runs them under qemu-user on the s390x build, so a
# field written in host order, or a conversion in the XDP program that swaps
# whatever the byte order is, fails on every commit.
#
# Every case named in those files must be registered in the binary: a case
# that a build condition dropped (the filter target not built, say) is a
# failure, never a quiet skip.
#
# A run that finds nothing to run is a failure, never a pass:
#   - either list (vectors, XDP filter cases) empty fails;
#   - a registration line in those files that the strict pattern does not
#     read (the macro spelled another way, a second argument) fails, so a
#     case cannot leave the run by being spelled differently;
#   - the counts must equal the ones docs/PLATFORMS.md states for this leg
#     (row "The standard's byte vectors, N cases ... and X XDP filter
#     cases"), so a drop in either count is red until the document says so.
#
# Usage: tools/bigendian/run_byte_cases.sh <unit binary> [runner...]
#        tools/bigendian/run_byte_cases.sh --count
#   tools/bigendian/run_byte_cases.sh bin/test qemu-s390x
#   tools/bigendian/run_byte_cases.sh bin/test
# --count reads the sources and the document only, prints the counts and
# applies every rule above except running the cases (the lint job runs it).
# Self-test: tools/bigendian/test_run_byte_cases.sh

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <unit binary> [runner...] | --count"
    exit 2
fi

count_only=0
if [ "$1" = "--count" ]; then
    count_only=1
else
    binary="$1"
    shift
fi

# the case runs in this process: no re-exec of an emulated binary
export ROCKETNET_TEST_SUITE_CAPTURE=1

vector_files=$(ls tests/test_unit_standard_*.cpp tests/test_unit_connect_token_vectors.cpp)
filter_file=tests/test_unit_xdp_filter_host.cpp

vectors=$(sed -nE 's/^REGISTER_UNIT_TEST\( *(test_[a-z0-9_]*) *\);.*/\1/p' $vector_files | sort -u)
filter=$(sed -nE 's/^REGISTER_UNIT_TEST\( *(test_xdp_(transport|filter)_host_[a-z0-9_]*) *\);.*/\1/p' "$filter_file" | sort -u)

# the loose reading: any line that registers something, however spelled; the
# filter file also registers one case that is not a filter case
# (test_xdp_transport_queue_default_matches_memory_plan), so there the loose
# reading keeps the host filter names only
loose_vectors=$(grep -ho 'REGISTER[A-Za-z_]*( *test_[a-z0-9_]*' $vector_files | sed 's/.*( *//' | sort -u || true)
loose_filter=$(grep -ho 'REGISTER[A-Za-z_]*( *test_xdp_[a-z]*_host_[a-z0-9_]*' "$filter_file" | sed 's/.*( *//' | sort -u || true)

lines() {
    if [ -z "$1" ]; then
        echo 0
    else
        printf '%s\n' "$1" | wc -l | tr -d ' '
    fi
}

nv=$(lines "$vectors")
nf=$(lines "$filter")

bad=0
if [ "$vectors" != "$loose_vectors" ]; then
    echo "check failed: a registration in the vector sources is not read by the strict pattern:"
    diff <(printf '%s\n' "$vectors") <(printf '%s\n' "$loose_vectors") || true
    bad=1
fi
if [ "$filter" != "$loose_filter" ]; then
    echo "check failed: a registration in $filter_file is not read by the strict pattern:"
    diff <(printf '%s\n' "$filter") <(printf '%s\n' "$loose_filter") || true
    bad=1
fi
if [ "$nv" = 0 ]; then
    echo "check failed: no byte-vector case found in the vector sources"
    bad=1
fi
if [ "$nf" = 0 ]; then
    echo "check failed: no XDP filter host case found in $filter_file"
    bad=1
fi

doc=docs/PLATFORMS.md
doc_row=$(grep -F "tools/bigendian/run_byte_cases.sh" "$doc" | grep -F "byte vectors" || true)
doc_vectors=$(printf '%s\n' "$doc_row" | sed -n 's/.*byte vectors, \([0-9][0-9]*\) cases.*/\1/p')
doc_filter=$(printf '%s\n' "$doc_row" | sed -n 's/.* \([0-9][0-9]*\) XDP filter cases.*/\1/p')
if [ -z "$doc_vectors" ] || [ -z "$doc_filter" ]; then
    echo "check failed: $doc has no row stating 'byte vectors, N cases' and 'X XDP filter cases' for this script"
    bad=1
elif [ "$doc_vectors" != "$nv" ] || [ "$doc_filter" != "$nf" ]; then
    echo "check failed: $doc states $doc_vectors vector and $doc_filter XDP filter cases; the sources register $nv and $nf"
    bad=1
fi

if [ "$bad" != 0 ]; then
    exit 1
fi

if [ "$count_only" = 1 ]; then
    echo "byte cases: $nv vector, $nf XDP filter (counted, not run)"
    exit 0
fi

listed=$("$@" "$binary" --list)

missing=0
for name in $vectors $filter; do
    if ! grep -qxF "$name" <<< "$listed"; then
        echo "check failed: $name is named in the source but not registered in $binary"
        missing=1
    fi
done
if [ "$missing" != 0 ]; then
    exit 1
fi

failed=0
count=0
for name in $vectors $filter; do
    count=$((count + 1))
    if ! "$@" "$binary" --run "$name" > case.out 2>&1; then
        echo "check failed: $name"
        cat case.out
        failed=$((failed + 1))
    fi
done
rm -f case.out

echo "byte cases: $count run, $failed failed ($nv vector, $nf XDP filter)"
if [ "$failed" != 0 ] || [ "$count" != $((nv + nf)) ] || [ "$count" = 0 ]; then
    exit 1
fi
