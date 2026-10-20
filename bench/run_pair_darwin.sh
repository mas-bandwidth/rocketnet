#!/bin/sh
# rocketnet transport bench runner, darwin (#100): one sender/receiver pair over
# loopback, the run_pair.sh protocol minus core pinning -- macOS has no
# userspace CPU affinity API, so both sides run at nice 10 unpinned and the
# report says so. Loopback magnitudes are loopback; ordering is the product.
#
#   ./run_pair_darwin.sh <binary> <port> [sender-extra-arg]

set -e

BINARY="$1"
PORT="$2"
EXTRA="$3"

WARMUP=30
WINDOW=60
WINDOWS=5
RECV_CORE=
SEND_CORE=
SEND_SECONDS=$(( WARMUP + WINDOW * WINDOWS + 2 ))

if [ -z "$BINARY" ] || [ -z "$PORT" ]; then
    echo "pair: binary and port are required" >&2
    exit 1
fi

. "$(dirname "$0")/pair_common.sh"
