#!/bin/sh
# rocketnet transport bench: the short pair (#98 item 7).
#
#   ./smoke_pair.sh <binary> <port> [recv_core] [send_core] [sender-extra-arg]
#
# The same shape as run_pair.sh at a fraction of the time: 3 s warm-up, one
# 5 s window. It is not a measurement -- 5 s does not clear the variance floor
# and no number from it belongs in a table. It answers one question, which is
# whether this binary on this host produces packets at all, and it is what you
# run before committing a machine to the 5x60 s campaign or before trusting a
# freshly built harness.
#
# Core pinning is optional so this runs on hosts without taskset (macOS has no
# userspace CPU affinity API at all); when cores are given it pins exactly as
# run_pair.sh does.

set -e

BINARY="$1"
PORT="$2"
RECV_CORE="$3"
SEND_CORE="$4"
EXTRA="$5"

WARMUP=3
WINDOW=5
WINDOWS=1
SEND_SECONDS=$(( WARMUP + WINDOW * WINDOWS + 2 ))

if [ -z "$BINARY" ] || [ -z "$PORT" ]; then
    echo "pair: binary and port are required" >&2
    exit 1
fi

. "$(dirname "$0")/pair_common.sh"
