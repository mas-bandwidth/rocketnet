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
# Usage: tools/bigendian/run_byte_cases.sh <unit binary> [runner...]
#   tools/bigendian/run_byte_cases.sh bin/test qemu-s390x
#   tools/bigendian/run_byte_cases.sh bin/test

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <unit binary> [runner...]"
    exit 2
fi

binary="$1"
shift

# the case runs in this process: no re-exec of an emulated binary
export ROCKETNET_TEST_SUITE_CAPTURE=1

vectors=$(sed -n 's/^REGISTER_UNIT_TEST( *\(test_[a-z0-9_]*\) *);.*/\1/p' tests/test_unit_standard_*.cpp tests/test_unit_connect_token_vectors.cpp | sort -u)
filter=$(sed -n -E 's/^REGISTER_UNIT_TEST\( *(test_xdp_(transport|filter)_host_[a-z0-9_]*) *\);.*/\1/p' tests/test_unit_xdp_filter_host.cpp | sort -u)

# Zero or short is red: a registration spelled otherwise, or a moved file,
# would empty the list and the run below would pass having run nothing.
# The floors are the counts on 2026-11-12 (docs/PLATFORMS.md names them);
# raise them when cases are added, never lower them to pass.
VECTOR_FLOOR=58
FILTER_FLOOR=30
vector_count=$(printf '%s' "$vectors" | grep -c . || [ $? -eq 1 ])
filter_count=$(printf '%s' "$filter" | grep -c . || [ $? -eq 1 ])
if [ "$vector_count" -lt "$VECTOR_FLOOR" ] || [ "$filter_count" -lt "$FILTER_FLOOR" ]; then
    echo "check failed: the sources name $vector_count vector and $filter_count XDP filter cases; the floor is $VECTOR_FLOOR and $FILTER_FLOOR"
    exit 1
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

echo "byte cases: $count run, $failed failed ($vector_count vector, $filter_count XDP filter)"
if [ "$count" -ne $((vector_count + filter_count)) ] || [ "$count" -eq 0 ]; then
    echo "check failed: $count cases run; the sources name $((vector_count + filter_count))"
    exit 1
fi
if [ "$failed" != 0 ]; then
    exit 1
fi
