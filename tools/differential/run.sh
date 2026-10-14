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

# Build rocketnet once; the dev library carries the token minting used to
# construct the cases, and the admission path is the production code.
if [ ! -f build/librocketnet_dev.a ]; then
    nice -n 19 make
fi

CXX="${CXX:-c++}"
CXXFLAGS="-O2 -ffp-contract=off -std=c++11"
INCLUDES="-I sdk -I build/generated"
LIBS="build/librocketnet_dev.a -lbpf -lxdp -lpthread"

NETCODE_FLAGS=""
if [ -n "$NETCODE_DIR" ]; then
    if [ ! -d "$NETCODE_DIR" ]; then
        echo "run.sh: NETCODE_DIR does not exist: $NETCODE_DIR" >&2
        exit 1
    fi
    # netcode 1.02 is a two-file C core (netcode.c/netcode.h) over the same
    # sodium primitive set. Build the core in place and ask it live.
    NETCODE_OBJ="$(mktemp --tmpdir="$TMPDIR" --suffix=.o)"
    NETCODE_FLAGS="-DROCKETNET_DIFFERENTIAL_NETCODE_DIR -I \"$NETCODE_DIR\""
    # shellcheck disable=SC2086
    "$CXX" -O2 -c "$NETCODE_DIR/netcode.c" -o "$NETCODE_OBJ" -I "$NETCODE_DIR"
    LIBS="$LIBS $NETCODE_OBJ"
    echo "run.sh: netcode reference live from $NETCODE_DIR"
else
    echo "run.sh: no NETCODE_DIR; diffing against the cases.tsv reference column"
fi

BIN="$(mktemp --tmpdir="$TMPDIR" --suffix=.bin)"
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS $INCLUDES $NETCODE_FLAGS tools/differential/netcode_decisions.cpp -o "$BIN" $LIBS

"$BIN" tools/differential/cases.tsv

rm -f "$BIN" ${NETCODE_OBJ:-}
