#!/bin/sh
# rocketnet driver bench: the short pair (#98 item 7).
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
SEND_SECONDS=30

if [ -z "$BINARY" ] || [ -z "$PORT" ]; then
    echo "usage: ./smoke_pair.sh <binary> <port> [recv_core] [send_core] [sender-extra-arg]" >&2
    exit 1
fi

pin_recv=""
pin_send=""
if [ -n "$RECV_CORE" ] && command -v taskset > /dev/null 2>&1; then
    pin_recv="taskset -c $RECV_CORE"
    pin_send="taskset -c ${SEND_CORE:-$RECV_CORE}"
fi

$pin_recv nice -n 10 "./$BINARY" recv "$PORT" "$WARMUP" "$WINDOW" "$WINDOWS" &
RECV_PID=$!
sleep 1
$pin_send nice -n 10 "./$BINARY" send 127.0.0.1 "$PORT" "$SEND_SECONDS" $EXTRA &
SEND_PID=$!

wait "$RECV_PID"
RECV_STATUS=$?
kill "$SEND_PID" 2>/dev/null || true
wait "$SEND_PID" 2>/dev/null || true

# a harness that aborted said why on its way out; carry that up rather than
# letting a smoke run "pass" because the shell forgot the exit code

exit "$RECV_STATUS"
