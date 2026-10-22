#!/usr/bin/env bash
# Cross-implementation differential: rocketnet vs netcode 1.02 token admission.
#
# Usage:
#   bash tools/differential/run.sh [NETCODE_DIR]
#
# With NETCODE_DIR (the mas-bandwidth/netcode checkout at the pinned tag) the
# harness is built against the netcode 1.02 source and asks the library live.
# Without it the harness diffs rocketnet's live decision against the reference
# column in cases.tsv, which records what netcode 1.02 decides per case. The
# build fetches nothing; the reference source is only ever read from the path
# given on the command line.
#
# This is the functional tier: it builds rocketnet and (optionally) netcode and
# drives the admission path in process. It is never part of ./bin/test.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NETCODE_DIR="${1:-}"

cd "$REPO_DIR"

# Build the dev library once. It carries the token minting used to construct
# the cases; the admission path is the production code. Two compile jobs, not
# the unbounded inner job of the top-level Makefile.
if [ ! -f build/librocketnet_dev.a ]; then
    nice -n 19 cmake -S "$REPO_DIR" -B "$REPO_DIR/build" -DCMAKE_BUILD_TYPE=Release
    nice -n 19 cmake --build "$REPO_DIR/build" --target rocketnet_dev -j 2
fi

CXX="${CXX:-c++}"
CXXFLAGS="-O2 -ffp-contract=off -std=c++11 -Wall -Wextra -Wpedantic -Werror"
INCLUDES="-I sdk -I build/generated -I sodium"

# Platform libraries. Linux links the XDP dataplane; darwin links the
# CoreFoundation/SystemConfiguration frameworks the dev library needs. The
# portable transport needs only pthreads everywhere.
case "$(uname -s)" in
    Linux)  PLATFORM_LIBS="-lbpf -lxdp -luring -lpthread" ;;
    Darwin) PLATFORM_LIBS="-lpthread -framework CoreFoundation -framework SystemConfiguration" ;;
    *)      PLATFORM_LIBS="-lpthread" ;;
esac
LIBS="build/librocketnet_dev.a $PLATFORM_LIBS"

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
    NETCODE_OBJ="$(mktemp "${TMPDIR:-/tmp}/netcode.XXXXXX")"
    "$CC" -O2 -DNDEBUG -ffp-contract=off -c "$NETCODE_DIR/netcode.c" -o "$NETCODE_OBJ" \
        -I "$NETCODE_DIR" -I "$REPO_DIR/sodium"
    echo "run.sh: netcode reference live from $NETCODE_DIR"
else
    echo "run.sh: no NETCODE_DIR; diffing against the cases.tsv reference column"
fi

BIN="$(mktemp "${TMPDIR:-/tmp}/netcode_decisions.XXXXXX")"
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

"$BIN" tools/differential/cases.tsv --corpus tools/differential/corpus_cases.tsv fuzz/corpus_connect_token/*

rm -f "$BIN" ${NETCODE_OBJ:-}
