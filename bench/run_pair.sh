#!/bin/sh
# rocketnet driver bench runner (#81): one sender/receiver pair over loopback,
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
SEND_SECONDS=340        # outlives warm-up + windows; killed when the receiver exits

taskset -c "$RECV_CORE" nice -n 10 "./$BINARY" recv "$PORT" "$WARMUP" "$WINDOW" "$WINDOWS" &
RECV_PID=$!
sleep 1
taskset -c "$SEND_CORE" nice -n 10 "./$BINARY" send 127.0.0.1 "$PORT" "$SEND_SECONDS" $EXTRA &
SEND_PID=$!

wait "$RECV_PID"
kill "$SEND_PID" 2>/dev/null || true
wait "$SEND_PID" 2>/dev/null || true
