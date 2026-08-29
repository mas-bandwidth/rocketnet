#!/bin/sh
# rocketnet driver bench runner, darwin (#100): one sender/receiver pair over
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
SEND_SECONDS=340        # outlives warm-up + windows; killed when the receiver exits

nice -n 10 "./$BINARY" recv "$PORT" "$WARMUP" "$WINDOW" "$WINDOWS" &
RECV_PID=$!
sleep 1
nice -n 10 "./$BINARY" send 127.0.0.1 "$PORT" "$SEND_SECONDS" $EXTRA &
SEND_PID=$!

wait "$RECV_PID"
kill "$SEND_PID" 2>/dev/null || true
wait "$SEND_PID" 2>/dev/null || true
