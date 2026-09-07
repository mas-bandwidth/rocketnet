# Shared process lifecycle for the pair runners. Both children must succeed.
# The caller sets BINARY, PORT, RECV_CORE, SEND_CORE, EXTRA and timing values.

RECV_PID=
SEND_PID=
cleanup()
{
    for pid in "$RECV_PID" "$SEND_PID"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
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

RECV_STATUS=0
wait "$RECV_PID" || RECV_STATUS=$?
RECV_PID=
if [ "$RECV_STATUS" -ne 0 ]; then
    echo "pair: receiver failed ($RECV_STATUS)" >&2
    exit "$RECV_STATUS"
fi

# Let the bounded sender finish naturally, preserving its actual exit status.
SEND_STATUS=0
wait "$SEND_PID" || SEND_STATUS=$?
SEND_PID=
if [ "$SEND_STATUS" -ne 0 ]; then
    echo "pair: sender failed ($SEND_STATUS)" >&2
    exit "$SEND_STATUS"
fi
exit 0
