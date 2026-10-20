#!/bin/sh
# rocketnet transport bench runner (#81): one sender/receiver pair over loopback,
# per the bench discipline -- 30 s warm-up discarded, five 60 s windows.
#
#   ./run_pair.sh <binary> <port> <recv_core> <send_core> [sender-extra-arg]
#
# Cores are taskset-pinned; both sides run at nice 10 so resident services
# always win the core. The receiver's WINDOW lines are the result.

set -e

BINARY="$1"
PORT="$2"
RECV_CORE="$3"
SEND_CORE="$4"
EXTRA="$5"

WARMUP=30
WINDOW=60
WINDOWS=5
SEND_SECONDS=$(( WARMUP + WINDOW * WINDOWS + 2 ))

if [ -z "$BINARY" ] || [ -z "$PORT" ]; then
    echo "pair: binary and port are required" >&2
    exit 1
fi

. "$(dirname "$0")/pair_common.sh"
