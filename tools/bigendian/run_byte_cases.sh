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
filter=$(sed -n 's/^REGISTER_UNIT_TEST( *\(test_xdp_\(transport\|filter\)_host_[a-z0-9_]*\) *);.*/\1/p' tests/test_unit_xdp_filter_host.cpp | sort -u)

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

echo "byte cases: $count run, $failed failed ($(printf '%s\n' $vectors | wc -l) vector, $(printf '%s\n' $filter | wc -l) XDP filter)"
if [ "$failed" != 0 ]; then
    exit 1
fi
