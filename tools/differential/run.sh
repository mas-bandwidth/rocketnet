#!/usr/bin/env bash
# Cross-implementation differential: rocketnet vs netcode 1.02 token admission.
#
# Usage:
#   bash tools/differential/run.sh [NETCODE_DIR]
#
# With NETCODE_DIR (the mas-bandwidth/netcode checkout at the pinned tag) the
# harness is built against the netcode 1.02 source and asks the library live.
# Without it the harness diffs rocketnet's live decision against the reference
# column in cases.tsv and corpus_cases.tsv, which record what netcode 1.02
# decides per case. The build fetches nothing; the reference source is only
# ever read from the path given on the command line.
#
# Exit status: the gate's (netcode_decisions.cpp): 0 only when every row
# passes, 1 on any failing row (README.md, "Exit status"); any build failure
# also exits nonzero. The last line of output is the tally, PASS or FAIL.
#
# Environment: DIFFERENTIAL_BUILD_DIR (default <repo>/build) is the CMake
# build directory of the dev library; DIFFERENTIAL_JOBS (default 2) its
# compile parallelism. The library is rebuilt incrementally on every run, so
# the gate never judges a stale archive.
#
# This is the functional tier: it builds rocketnet and (optionally) netcode and
# drives the admission path in process. It is never part of ./bin/test.
# tools/differential/selftest.sh proves it can fail; the nightly differential
# workflow runs both.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NETCODE_DIR="${1:-}"
BUILD_DIR="${DIFFERENTIAL_BUILD_DIR:-$REPO_DIR/build}"
JOBS="${DIFFERENTIAL_JOBS:-2}"

cd "$REPO_DIR"

# The dev library carries the token minting used to construct the cases; the
# admission path is the production code.
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    nice -n 19 cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
fi
nice -n 19 cmake --build "$BUILD_DIR" --target rocketnet_dev -j "$JOBS"

CXX="${CXX:-c++}"
CXXFLAGS="-O2 -ffp-contract=off -std=c++11 -Wall -Wextra -Wpedantic -Werror"
INCLUDES="-I sdk -I $BUILD_DIR/generated -I sodium"

# Platform libraries. Linux links the XDP dataplane; darwin links the
# CoreFoundation/SystemConfiguration frameworks the dev library needs. The
# portable transport needs only pthreads everywhere.
case "$(uname -s)" in
    Linux)  PLATFORM_LIBS="-lbpf -lxdp -luring -lpthread" ;;
    Darwin) PLATFORM_LIBS="-lpthread -framework CoreFoundation -framework SystemConfiguration" ;;
    *)      PLATFORM_LIBS="-lpthread" ;;
esac
LIBS="$BUILD_DIR/librocketnet_dev.a $PLATFORM_LIBS"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/differential.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

NETCODE_OBJ=""
if [ -n "$NETCODE_DIR" ]; then
    if [ ! -f "$NETCODE_DIR/netcode.c" ] || [ ! -f "$NETCODE_DIR/netcode.h" ]; then
        echo "run.sh: NETCODE_DIR has no netcode.c/netcode.h: $NETCODE_DIR" >&2
        exit 1
    fi
    # The pinned tag is a two-file C core. Compile it against the vendored
    # sodium header so its crypto calls resolve to the symbols inside
    # librocketnet_dev.a. NDEBUG selects the library's release asserts.
    # The build fetches nothing.
    CC="${CC:-cc}"
    NETCODE_OBJ="$WORK/netcode.o"
    "$CC" -O2 -DNDEBUG -ffp-contract=off -c "$NETCODE_DIR/netcode.c" -o "$NETCODE_OBJ" \
        -I "$NETCODE_DIR" -I "$REPO_DIR/sodium"
    echo "run.sh: netcode reference live from $NETCODE_DIR"
else
    echo "run.sh: no NETCODE_DIR; diffing against the reference columns"
fi

BIN="$WORK/netcode_decisions"
if [ -n "$NETCODE_DIR" ]; then
    # netcode.o before the archive: the linker pulls the vendored sodium
    # members for netcode's calls only if the object is seen first.
    # shellcheck disable=SC2086
    "$CXX" $CXXFLAGS $INCLUDES -DROCKETNET_DIFFERENTIAL_NETCODE_DIR -I "$NETCODE_DIR" \
        tools/differential/netcode_decisions.cpp "$NETCODE_OBJ" -o "$BIN" $LIBS -lm
else
    # shellcheck disable=SC2086
    "$CXX" $CXXFLAGS $INCLUDES tools/differential/netcode_decisions.cpp -o "$BIN" $LIBS
fi

"$BIN" tools/differential/cases.tsv --corpus tools/differential/corpus_cases.tsv tests/fuzz/corpus_connect_token/*
