# Shared process lifecycle for the pair runners. Both children must succeed.
# The caller sets BINARY, PORT, RECV_CORE, SEND_CORE, EXTRA and timing values.
#
# The sender is started for longer than the receiver measures, and is
# STOPPED when the receiver's last window closes (PROTOCOL.md), never left to
# run on into a closed port. The receiver stops it, before closing its own
# socket, through the pid written to BENCH_SENDER_PIDFILE (bench_common.h,
# bench_stop_sender); the sender then validates the sends it made up to the
# stop and exits. Its exit status is still checked, so a send refused at any
# point in the measured windows fails the pair; only sends refused after the
# stop are reported as late and left out of the sample.

RECV_PID=
SEND_PID=
BENCH_SENDER_PIDFILE=$( mktemp "${TMPDIR:-/tmp}/bench-pair.XXXXXX" ) || exit 1
export BENCH_SENDER_PIDFILE
cleanup()
{
    for pid in "$RECV_PID" "$SEND_PID"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -f "$BENCH_SENDER_PIDFILE"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

run_recv()
{
    if [ -n "$RECV_CORE" ] && command -v taskset >/dev/null 2>&1; then
        exec taskset -c "$RECV_CORE" nice -n 10 "./$BINARY" recv "$PORT" "$WARMUP" "$WINDOW" "$WINDOWS"
    fi
    exec nice -n 10 "./$BINARY" recv "$PORT" "$WARMUP" "$WINDOW" "$WINDOWS"
}
run_send()
{
    if [ -n "$RECV_CORE" ] && command -v taskset >/dev/null 2>&1; then
        exec taskset -c "${SEND_CORE:-$RECV_CORE}" nice -n 10 "./$BINARY" send 127.0.0.1 "$PORT" "$SEND_SECONDS" ${EXTRA:+"$EXTRA"}
    fi
    exec nice -n 10 "./$BINARY" send 127.0.0.1 "$PORT" "$SEND_SECONDS" ${EXTRA:+"$EXTRA"}
}

run_recv &
RECV_PID=$!
sleep 1
run_send &
SEND_PID=$!
echo "$SEND_PID" > "$BENCH_SENDER_PIDFILE"

RECV_STATUS=0
wait "$RECV_PID" || RECV_STATUS=$?
RECV_PID=
if [ "$RECV_STATUS" -ne 0 ]; then
    echo "pair: receiver failed ($RECV_STATUS)" >&2
    exit "$RECV_STATUS"
fi

# The receiver's last window has closed: stop the sender. The receiver has
# normally done this already; this covers a receiver that could not read the
# pid file. The sender validates its in-window sends and exits on its own, and
# that exit status is what decides the pair.
kill -USR1 "$SEND_PID" 2>/dev/null || true
SEND_STATUS=0
wait "$SEND_PID" || SEND_STATUS=$?
SEND_PID=
if [ "$SEND_STATUS" -ne 0 ]; then
    echo "pair: sender failed ($SEND_STATUS)" >&2
    exit "$SEND_STATUS"
fi
exit 0
